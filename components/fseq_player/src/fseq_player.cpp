// FSEQ v1/v2 player backed by SDMMC 4-bit + FATFS.
//
// Design notes:
//  • FSEQ byte offset B → universe (universe_base + B/512), slot (B%512).
//    This matches the default xLights Art-Net output layout (universes
//    starting at 1, consecutive).  Sparse-range files are mapped correctly
//    via the range→absolute-channel→universe chain.
//  • Compressed blocks (zstd only; lz4/none also supported) are decompressed
//    into a PSRAM staging buffer one block at a time; frames are then sliced
//    from the decompressed output without further copies.
//  • All large buffers live in PSRAM; the per-frame injection loop is the
//    only hot path and contains no allocation.
//  • The playback task runs on core 0 at priority 5, below the ArtNet /
//    sACN receivers (~10) so network traffic preempts file I/O.

#include "fseq_player.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "dmx_manager.h"
#include "fseq_format.h"

// Forward-declare the zstd functions from third_party/zstddeclib.c.
extern "C" {
typedef unsigned long long ZSTD_ULL;
size_t ZSTD_decompress(void* dst, size_t dstCap, const void* src, size_t srcSz);
ZSTD_ULL ZSTD_getFrameContentSize(const void* src, size_t srcSz);
unsigned ZSTD_isError(size_t result);
const char* ZSTD_getErrorName(size_t result);
}

namespace pixfrog::fseq {

namespace {

constexpr const char* TAG      = "FSEQ";
constexpr const char* kMntPath = "/sdcard";

// Universe number of FSEQ byte 0 (xLights default = 1).
constexpr uint16_t kUniverseBase = 1;

// Per-frame buffer caps.
constexpr size_t kMaxFrameBytes     = 512 * 1024;       // 512 KB; truncates huge shows safely
constexpr size_t kMaxCompBlockBytes = 1024 * 1024;      // 1 MB compressed input per block
constexpr size_t kMaxDecompBytes    = 2 * 1024 * 1024;  // 2 MB decompressed per block

sdmmc_card_t* g_card = nullptr;
std::atomic<SdState> g_sd_state{ SdState::Absent };
InitConfig g_init_cfg = {};
bool g_init_done      = false;

// Playback state
char g_active_file[kMaxNameLen] = {};
Status g_status                 = Status::Idle;
char g_error[80]                = {};

std::atomic<bool> g_run{ false };
TaskHandle_t g_task_handle = nullptr;

// Per-play heap allocations (PSRAM, freed when task exits).
struct Buffers {
    uint8_t* frame;   // channel_count bytes per frame
    uint8_t* comp;    // compressed block input
    uint8_t* decomp;  // decompressed block output
};

// ── SD card hot-plug ─────────────────────────────────────────────────────────

// Reconstruct host+slot config from g_init_cfg and attempt a fresh mount.
// Called from the monitor task; returns true on success.
static bool do_mount() {
    sdmmc_host_t host               = SDMMC_HOST_DEFAULT();
    host.max_freq_khz               = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot        = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk                        = static_cast<gpio_num_t>(g_init_cfg.clk_gpio);
    slot.cmd                        = static_cast<gpio_num_t>(g_init_cfg.cmd_gpio);
    slot.d0                         = static_cast<gpio_num_t>(g_init_cfg.d0_gpio);
    slot.d1                         = static_cast<gpio_num_t>(g_init_cfg.d1_gpio);
    slot.d2                         = static_cast<gpio_num_t>(g_init_cfg.d2_gpio);
    slot.d3                         = static_cast<gpio_num_t>(g_init_cfg.d3_gpio);
    slot.width                      = 4;
    slot.flags                      = 0;
    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed     = false;
    mcfg.max_files                  = 5;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMntPath, &host, &slot, &mcfg, &g_card);
    if (err != ESP_OK) return false;
    g_sd_state.store(SdState::Mounted, std::memory_order_release);
    ESP_LOGI(TAG, "SD card mounted at %s", kMntPath);
    return true;
}

// Kill any running playback task and reset playback state.
// Shared between the public stop() and do_unmount().
static void stop_task() {
    if (!g_run.load(std::memory_order_acquire)) return;
    g_run.store(false, std::memory_order_release);
    dmx::fseq_set_active(false);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (g_task_handle && xTaskGetTickCount() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    g_status         = Status::Idle;
    g_active_file[0] = '\0';
}

// Stop any running playback, unmount the FAT volume, and transition to Absent.
// Called only from sd_monitor_task.
static void do_unmount() {
    stop_task();
    esp_vfs_fat_sdcard_unmount(kMntPath, g_card);
    g_card = nullptr;
    g_sd_state.store(SdState::Absent, std::memory_order_release);
    ESP_LOGI(TAG, "SD card unmounted");
}

// Background task: polls every 1 s for card insertion / removal.
static void sd_monitor_task(void* /*arg*/) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (g_sd_state.load(std::memory_order_acquire) == SdState::Absent) {
            do_mount();
        } else if (sdmmc_get_status(g_card) != ESP_OK) {
            ESP_LOGW(TAG, "SD card removed");
            do_unmount();
        }
    }
}

// Inject one flat frame (no sparse ranges) into the universe back-buffers.
void inject_linear_frame(const uint8_t* data, uint32_t channel_count) {
    uint32_t remaining = channel_count;
    uint16_t uni       = kUniverseBase;
    while (remaining > 0) {
        const size_t chunk = remaining < 512 ? remaining : 512;
        dmx::inject_universe(uni, 0, data, chunk);
        data      += chunk;
        remaining -= static_cast<uint32_t>(chunk);
        ++uni;
    }
}

// Inject one sparse frame into the universe back-buffers.
// Each SparseRange maps a contiguous run of FSEQ bytes to an absolute
// channel offset; that offset is split into (universe, slot) pairs.
void inject_sparse_frame(const uint8_t* data, const SparseRange* ranges, uint8_t num_ranges) {
    const uint8_t* p = data;
    for (uint8_t r = 0; r < num_ranges; ++r) {
        uint32_t ch_abs    = ranges[r].start_channel;
        uint32_t remaining = ranges[r].length;
        while (remaining > 0) {
            const uint16_t uni   = channel_to_universe(ch_abs, kUniverseBase);
            const uint16_t slot  = channel_to_slot(ch_abs);
            const uint32_t avail = 512u - slot;
            const uint32_t chunk = remaining < avail ? remaining : avail;
            dmx::inject_universe(uni, slot, p, static_cast<size_t>(chunk));
            p         += chunk;
            ch_abs    += chunk;
            remaining -= chunk;
        }
    }
}

// ── Playback task ─────────────────────────────────────────────────────────

struct TaskArg {
    char filename[kMaxNameLen];
    Buffers bufs;
};

void playback_task(void* arg_ptr) {
    TaskArg* arg = static_cast<TaskArg*>(arg_ptr);
    Buffers& buf = arg->bufs;

    char path[kMaxNameLen + 16];
    snprintf(path, sizeof(path), "%s/%s", kMntPath, arg->filename);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        snprintf(g_error, sizeof(g_error), "Cannot open %s", arg->filename);
        ESP_LOGE(TAG, "%s", g_error);
        g_status = Status::Error;
        goto done;
    }

    {
        // ── Parse header ──────────────────────────────────────────────────
        uint8_t hdr_buf[sizeof(Header)];
        if (fread(hdr_buf, 1, sizeof(hdr_buf), fp) != sizeof(hdr_buf)) {
            snprintf(g_error, sizeof(g_error), "Short read: header");
            g_status = Status::Error;
            fclose(fp);
            goto done;
        }

        Header hdr;
        if (parse_header(hdr_buf, sizeof(hdr_buf), hdr) != ParseResult::Ok) {
            snprintf(g_error, sizeof(g_error), "Bad FSEQ header");
            g_status = Status::Error;
            fclose(fp);
            goto done;
        }

        const uint32_t frame_bytes = (hdr.channel_count > kMaxFrameBytes)
                                       ? static_cast<uint32_t>(kMaxFrameBytes)
                                       : hdr.channel_count;

        if (frame_bytes == 0 || hdr.frame_count == 0) {
            snprintf(g_error, sizeof(g_error), "Empty FSEQ file");
            g_status = Status::Error;
            fclose(fp);
            goto done;
        }

        // ── Read comp-block table ─────────────────────────────────────────
        CompBlock blocks[256]  = {};
        SparseRange ranges[64] = {};

        if (hdr.num_comp_blocks > 0) {
            fseek(fp, static_cast<long>(kCompBlockTableOffset), SEEK_SET);
            const size_t bytes = static_cast<size_t>(hdr.num_comp_blocks) * sizeof(CompBlock);
            if (fread(blocks, 1, bytes, fp) != bytes) {
                snprintf(g_error, sizeof(g_error), "Short read: comp table");
                g_status = Status::Error;
                fclose(fp);
                goto done;
            }
        }

        // ── Read sparse-range table ───────────────────────────────────────
        if (hdr.num_sparse_ranges > 0 &&
            hdr.num_sparse_ranges <= static_cast<uint8_t>(sizeof(ranges) / sizeof(ranges[0]))) {
            fseek(fp, static_cast<long>(sparse_range_file_offset(hdr)), SEEK_SET);
            const size_t bytes = static_cast<size_t>(hdr.num_sparse_ranges) * sizeof(SparseRange);
            if (fread(ranges, 1, bytes, fp) != bytes) {
                snprintf(g_error, sizeof(g_error), "Short read: sparse table");
                g_status = Status::Error;
                fclose(fp);
                goto done;
            }
        }

        if (hdr.compression_type == kCompLz4) {
            snprintf(g_error, sizeof(g_error), "lz4 compression not supported");
            g_status = Status::Error;
            fclose(fp);
            goto done;
        }

        // ── Playback loop ─────────────────────────────────────────────────
        const TickType_t frame_period = pdMS_TO_TICKS(hdr.step_time_ms ? hdr.step_time_ms : 25u);
        TickType_t last_wake          = xTaskGetTickCount();

        // For zstd: track which block is currently decompressed.
        int32_t decomp_block_idx     = -1;  // -1 = none
        uint32_t decomp_block_first  = 0;   // first frame of decomp'd block
        uint32_t decomp_block_frames = 0;   // frames in decomp'd block
        uint32_t decomp_block_offset = 0;   // file offset of this block's compressed data

        g_status = Status::Playing;
        dmx::fseq_set_active(true);

        for (uint32_t fn = 0; fn < hdr.frame_count && g_run.load(std::memory_order_acquire);) {
            if (hdr.compression_type == kCompNone) {
                // ── Uncompressed: seek directly to frame ──────────────────
                const long off = static_cast<long>(uncompressed_frame_offset(hdr, fn));
                fseek(fp, off, SEEK_SET);
                const size_t got = fread(buf.frame, 1, frame_bytes, fp);
                if (got < frame_bytes) memset(buf.frame + got, 0, frame_bytes - got);

            } else {
                // ── zstd block: locate or (re)decompress the block ────────
                int32_t block_idx = -1;
                if (hdr.num_comp_blocks > 0) {
                    // Find which block contains frame fn.
                    for (int32_t b = 0; b < static_cast<int32_t>(hdr.num_comp_blocks); ++b) {
                        const uint32_t last_frame = (b + 1 <
                                                     static_cast<int32_t>(hdr.num_comp_blocks))
                                                      ? blocks[b + 1].first_frame - 1
                                                      : hdr.frame_count - 1;
                        if (fn >= blocks[b].first_frame && fn <= last_frame) {
                            block_idx = b;
                            break;
                        }
                    }
                }

                if (block_idx < 0) {
                    // No block table or block not found — skip frame
                    ++fn;
                    vTaskDelayUntil(&last_wake, frame_period);
                    continue;
                }

                // Decompress block if needed.
                if (block_idx != decomp_block_idx) {
                    // Compute file offset of this block.
                    uint32_t block_file_off = hdr.channel_data_offset;
                    for (int32_t b = 0; b < block_idx; ++b)
                        block_file_off += blocks[b].data_size;

                    const size_t comp_sz = blocks[block_idx].data_size;
                    if (comp_sz == 0 || comp_sz > kMaxCompBlockBytes) {
                        ESP_LOGW(TAG, "block %d size %u out of range", block_idx,
                                 static_cast<unsigned>(comp_sz));
                        ++fn;
                        vTaskDelayUntil(&last_wake, frame_period);
                        continue;
                    }

                    fseek(fp, static_cast<long>(block_file_off), SEEK_SET);
                    if (fread(buf.comp, 1, comp_sz, fp) != comp_sz) {
                        ESP_LOGW(TAG, "short read on comp block %d", block_idx);
                        ++fn;
                        vTaskDelayUntil(&last_wake, frame_period);
                        continue;
                    }

                    const size_t decomp_sz = ZSTD_decompress(buf.decomp, kMaxDecompBytes, buf.comp,
                                                             comp_sz);
                    if (ZSTD_isError(decomp_sz)) {
                        ESP_LOGW(TAG, "zstd error block %d: %s", block_idx,
                                 ZSTD_getErrorName(decomp_sz));
                        ++fn;
                        vTaskDelayUntil(&last_wake, frame_period);
                        continue;
                    }

                    decomp_block_idx    = block_idx;
                    decomp_block_first  = blocks[block_idx].first_frame;
                    decomp_block_frames = static_cast<uint32_t>(decomp_sz / hdr.channel_count);
                    decomp_block_offset = block_file_off;
                    (void)decomp_block_offset;
                }

                // Slice frame from decompressed block.
                const uint32_t frame_in_block = fn - decomp_block_first;
                if (frame_in_block >= decomp_block_frames) {
                    ++fn;
                    vTaskDelayUntil(&last_wake, frame_period);
                    continue;
                }
                const uint8_t* src = buf.decomp + frame_in_block * hdr.channel_count;
                memcpy(buf.frame, src, frame_bytes);
            }

            // ── Inject frame into universe back-buffers ───────────────────
            if (hdr.num_sparse_ranges > 0) {
                inject_sparse_frame(buf.frame, ranges, hdr.num_sparse_ranges);
            } else {
                inject_linear_frame(buf.frame, frame_bytes);
            }

            ++fn;
            vTaskDelayUntil(&last_wake, frame_period);
        }

        fclose(fp);
    }

done:
    heap_caps_free(buf.frame);
    heap_caps_free(buf.comp);
    heap_caps_free(buf.decomp);
    delete arg;

    dmx::fseq_set_active(false);
    if (g_status == Status::Playing) {
        g_status         = Status::Idle;
        g_active_file[0] = '\0';
    }
    g_run.store(false, std::memory_order_release);
    g_task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool init(const InitConfig& cfg) {
    if (g_init_done) return true;
    g_init_cfg  = cfg;
    g_init_done = true;

    // Try an immediate mount; the monitor task will retry every second if absent.
    do_mount();

    const BaseType_t ok = xTaskCreate(sd_monitor_task, "sd_mon", 4096, nullptr, 2, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SD monitor task");
        return false;
    }
    return true;
}

size_t list_files(char names[][kMaxNameLen], size_t max) {
    if (g_sd_state.load(std::memory_order_acquire) != SdState::Mounted || !names || max == 0)
        return 0;

    DIR* dir = opendir(kMntPath);
    if (!dir) return 0;

    size_t count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr && count < max) {
        if (ent->d_type != DT_REG) continue;
        const char* name = ent->d_name;
        const size_t len = strlen(name);
        if (len < 5) continue;
        const char* ext = name + len - 5;
        // Match .fseq or .FSEQ (case-insensitive last 5 chars check)
        if ((ext[0] == '.' || ext[0] == '.') && (ext[1] == 'f' || ext[1] == 'F') &&
            (ext[2] == 's' || ext[2] == 'S') && (ext[3] == 'e' || ext[3] == 'E') &&
            (ext[4] == 'q' || ext[4] == 'Q')) {
            strncpy(names[count], name, kMaxNameLen - 1);
            names[count][kMaxNameLen - 1] = '\0';
            ++count;
        }
    }
    closedir(dir);
    return count;
}

bool start(const char* filename) {
    if (!filename || !filename[0]) return false;
    if (g_sd_state.load(std::memory_order_acquire) != SdState::Mounted) {
        snprintf(g_error, sizeof(g_error), "No SD card");
        g_status = Status::Error;
        return false;
    }

    stop();  // stop any running playback first

    // Allocate PSRAM buffers for the new playback session.
    Buffers bufs;
    bufs.frame  = static_cast<uint8_t*>(heap_caps_malloc(kMaxFrameBytes, MALLOC_CAP_SPIRAM));
    bufs.comp   = static_cast<uint8_t*>(heap_caps_malloc(kMaxCompBlockBytes, MALLOC_CAP_SPIRAM));
    bufs.decomp = static_cast<uint8_t*>(heap_caps_malloc(kMaxDecompBytes, MALLOC_CAP_SPIRAM));
    if (!bufs.frame || !bufs.comp || !bufs.decomp) {
        ESP_LOGE(TAG, "PSRAM alloc failed for FSEQ buffers");
        heap_caps_free(bufs.frame);
        heap_caps_free(bufs.comp);
        heap_caps_free(bufs.decomp);
        snprintf(g_error, sizeof(g_error), "Out of PSRAM");
        g_status = Status::Error;
        return false;
    }

    // Pass arguments + buffers to the task via a heap-allocated struct so
    // start() can return before the task uses them.
    TaskArg* arg = new (std::nothrow) TaskArg;
    if (!arg) {
        heap_caps_free(bufs.frame);
        heap_caps_free(bufs.comp);
        heap_caps_free(bufs.decomp);
        snprintf(g_error, sizeof(g_error), "OOM");
        g_status = Status::Error;
        return false;
    }
    strncpy(arg->filename, filename, kMaxNameLen - 1);
    arg->filename[kMaxNameLen - 1] = '\0';
    arg->bufs                      = bufs;

    strncpy(g_active_file, filename, kMaxNameLen - 1);
    g_active_file[kMaxNameLen - 1] = '\0';
    g_error[0]                     = '\0';
    g_status                       = Status::Playing;
    g_run.store(true, std::memory_order_release);

    const BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "fseq_play", 8192, arg, 5,
                                                  &g_task_handle, 0);
    if (ok != pdPASS) {
        g_run.store(false, std::memory_order_release);
        delete arg;
        heap_caps_free(bufs.frame);
        heap_caps_free(bufs.comp);
        heap_caps_free(bufs.decomp);
        snprintf(g_error, sizeof(g_error), "Task create failed");
        g_status      = Status::Error;
        g_task_handle = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "playing %s", filename);
    return true;
}

void stop() {
    if (!g_run.load(std::memory_order_acquire)) return;
    stop_task();
    ESP_LOGI(TAG, "playback stopped");
}

const char* active_file() {
    return g_active_file[0] ? g_active_file : nullptr;
}

SdState sd_state() {
    return g_sd_state.load(std::memory_order_acquire);
}

Status status() {
    return g_status;
}

const char* error_string() {
    return g_error;
}

}  // namespace pixfrog::fseq
