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
#include <strings.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <dirent.h>

#include "config_store.h"
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
constexpr const char* kMntPath = kMountPath;

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

// What happens when a file ends. Written by start*() before the task exists,
// then read by the task only.
enum class Mode : uint8_t { Once, Loop, Playlist };
Mode g_mode = Mode::Once;
config::FseqPlaylist g_list{};      // snapshot of the playlist being played
std::atomic<int> g_list_idx{ -1 };  // item playing, -1 when not a playlist
uint8_t g_repeat_left = 0;          // further plays of the current item
bool g_autostart_done = false;      // the boot autostart is tried once

// Seek/position channel between the API and the playback task.
// g_seek_frame: target frame requested by seek_ms(), -1 when none pending.
std::atomic<int64_t> g_seek_frame{ -1 };
std::atomic<uint32_t> g_cur_frame{ 0 };    // last frame injected
std::atomic<uint32_t> g_step_ms{ 0 };      // step_time of the active file
std::atomic<uint32_t> g_frame_count{ 0 };  // frame count of the active file

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
    // The first mount since boot plays the playlist when it is set to.
    if (!g_autostart_done) {
        g_autostart_done = true;
        if (config::get_playlist().autostart) start_playlist();
    }
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

// Plays/stops what the DMX control universe asked for (the render task only
// posts the request: starting a file must not block it).
static void serve_desk_request() {
    const int16_t req = dmx::take_fseq_request();
    if (req == dmx::kFseqNoRequest) return;
    if (req == dmx::kFseqStopRequest) {
        stop();
        return;
    }
    static char names[16][kMaxNameLen];
    const size_t n = list_files(names, 16);
    if (static_cast<size_t>(req) < n) start(names[req]);
}

// Background task: polls every 1 s for card insertion / removal, and every
// 100 ms for desk FSEQ requests.
static void sd_monitor_task(void* /*arg*/) {
    for (uint32_t tick = 1;; ++tick) {
        vTaskDelay(pdMS_TO_TICKS(100));
        serve_desk_request();
        if (tick % 10) continue;
        if (g_sd_state.load(std::memory_order_acquire) == SdState::Absent) {
            do_mount();
        } else if (sdmmc_get_status(g_card) != ESP_OK) {
            ESP_LOGW(TAG, "SD card removed");
            do_unmount();
        }
    }
}

// Inject one flat frame (no sparse ranges) into the universe back-buffers.
// `base` = universe of FSEQ byte 0 (config::fseq_universe).
void inject_linear_frame(const uint8_t* data, uint32_t channel_count, uint16_t base) {
    uint32_t remaining = channel_count;
    uint32_t uni       = base;
    while (remaining > 0 && uni <= dmx::kMaxUniverseNumber) {
        const size_t chunk = remaining < 512 ? remaining : 512;
        dmx::inject_frame_universe(static_cast<uint16_t>(uni), 0, data, chunk);
        data      += chunk;
        remaining -= static_cast<uint32_t>(chunk);
        ++uni;
    }
}

// Inject one sparse frame into the universe back-buffers.
// Each SparseRange maps a contiguous run of FSEQ bytes to an absolute
// channel offset; that offset is split into (universe, slot) pairs.
void inject_sparse_frame(const uint8_t* data, const SparseRange* ranges, uint8_t num_ranges,
                         uint16_t base) {
    const uint8_t* p = data;
    for (uint8_t r = 0; r < num_ranges; ++r) {
        uint32_t ch_abs    = ranges[r].start_channel;
        uint32_t remaining = ranges[r].length;
        while (remaining > 0) {
            const uint16_t slot  = channel_to_slot(ch_abs);
            const uint32_t avail = 512u - slot;
            const uint32_t chunk = remaining < avail ? remaining : avail;
            uint16_t uni         = 0;
            // A range reaching past the addressable universe range only walks
            // further out, so drop the rest of it — but still step `p` over
            // those bytes, or every later range would read the wrong offset.
            if (!channel_to_universe_checked(ch_abs, base, dmx::kMaxUniverseNumber, &uni)) {
                p += remaining;
                break;
            }
            dmx::inject_frame_universe(uni, slot, p, static_cast<size_t>(chunk));
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

enum class FileResult : uint8_t { Ended, Stopped, Failed };

// Plays one file to its last frame, or until stop(). On Failed, g_status and
// g_error say why.
FileResult play_file(const char* filename, Buffers& buf) {
    // Read once per file: a change applies from the next file (or loop).
    const uint16_t base = config::fseq_universe(config::get_global());
    char path[kMaxNameLen + 16];
    snprintf(path, sizeof(path), "%s/%s", kMntPath, filename);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        snprintf(g_error, sizeof(g_error), "Cannot open %s", filename);
        ESP_LOGE(TAG, "%s", g_error);
        g_status = Status::Error;
        return FileResult::Failed;
    }

    {
        // ── Parse header ──────────────────────────────────────────────────
        uint8_t hdr_buf[sizeof(Header)];
        if (fread(hdr_buf, 1, sizeof(hdr_buf), fp) != sizeof(hdr_buf)) {
            snprintf(g_error, sizeof(g_error), "Short read: header");
            g_status = Status::Error;
            fclose(fp);
            return FileResult::Failed;
        }

        Header hdr;
        if (parse_header(hdr_buf, sizeof(hdr_buf), hdr) != ParseResult::Ok) {
            snprintf(g_error, sizeof(g_error), "Bad FSEQ header");
            g_status = Status::Error;
            fclose(fp);
            return FileResult::Failed;
        }

        const uint32_t frame_bytes = (hdr.channel_count > kMaxFrameBytes)
                                       ? static_cast<uint32_t>(kMaxFrameBytes)
                                       : hdr.channel_count;

        if (frame_bytes == 0 || hdr.frame_count == 0) {
            snprintf(g_error, sizeof(g_error), "Empty FSEQ file");
            g_status = Status::Error;
            fclose(fp);
            return FileResult::Failed;
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
                return FileResult::Failed;
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
                return FileResult::Failed;
            }
        }

        if (hdr.compression_type == kCompLz4) {
            snprintf(g_error, sizeof(g_error), "lz4 compression not supported");
            g_status = Status::Error;
            fclose(fp);
            return FileResult::Failed;
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
        g_step_ms.store(hdr.step_time_ms ? hdr.step_time_ms : 25u, std::memory_order_release);
        g_frame_count.store(hdr.frame_count, std::memory_order_release);
        g_seek_frame.store(-1, std::memory_order_release);
        dmx::fseq_set_active(true);

        for (uint32_t fn = 0; fn < hdr.frame_count && g_run.load(std::memory_order_acquire);) {
            // Consume a pending seek at the frame boundary.
            const int64_t seek = g_seek_frame.exchange(-1, std::memory_order_acq_rel);
            if (seek >= 0) {
                fn = (seek < hdr.frame_count) ? static_cast<uint32_t>(seek) : hdr.frame_count - 1;
                last_wake = xTaskGetTickCount();  // re-anchor the pacing clock
            }
            g_cur_frame.store(fn, std::memory_order_release);

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

            // ── Inject frame into the universe back bank ──────────────────
            // One lock for the whole frame: the render task's next swap
            // publishes every universe of it together (no torn frame).
            dmx::inject_frame_begin();
            if (hdr.num_sparse_ranges > 0) {
                inject_sparse_frame(buf.frame, ranges, hdr.num_sparse_ranges, base);
            } else {
                inject_linear_frame(buf.frame, frame_bytes, base);
            }
            dmx::inject_frame_end();

            ++fn;
            vTaskDelayUntil(&last_wake, frame_period);
        }

        fclose(fp);
    }
    return g_run.load(std::memory_order_acquire) ? FileResult::Ended : FileResult::Stopped;
}

// After a file: the name of the next one to play, false when done.
bool next_item(char* name) {
    switch (g_mode) {
    case Mode::Once: return false;
    case Mode::Loop: return true;  // the same file again
    case Mode::Playlist:
        if (g_repeat_left > 0) {
            --g_repeat_left;
            return true;
        }
        {
            int idx = g_list_idx.load(std::memory_order_relaxed) + 1;
            if (idx >= g_list.count) {
                if (!g_list.loop) return false;
                idx = 0;
            }
            g_list_idx.store(idx, std::memory_order_relaxed);
            g_repeat_left = static_cast<uint8_t>(g_list.items[idx].repeat - 1);
            strncpy(name, g_list.items[idx].name, kMaxNameLen - 1);
            name[kMaxNameLen - 1] = '\0';
        }
        return true;
    }
    return false;
}

void playback_task(void* arg_ptr) {
    TaskArg* arg = static_cast<TaskArg*>(arg_ptr);
    Buffers& buf = arg->bufs;
    char name[kMaxNameLen];
    strncpy(name, arg->filename, sizeof(name));

    size_t failures = 0;  // in a row
    for (;;) {
        strncpy(g_active_file, name, kMaxNameLen - 1);
        g_active_file[kMaxNameLen - 1] = '\0';
        const FileResult r             = play_file(name, buf);
        if (r == FileResult::Stopped || !g_run.load(std::memory_order_acquire)) break;
        if (r == FileResult::Failed) {
            // A playlist skips a broken item (and its repeats); a list made of
            // nothing but broken items stops instead of spinning.
            if (g_mode != Mode::Playlist || ++failures >= g_list.count) break;
            g_repeat_left = 0;
        } else {
            failures = 0;
        }
        if (!next_item(name)) break;
        g_status = Status::Playing;
    }

    heap_caps_free(buf.frame);
    heap_caps_free(buf.comp);
    heap_caps_free(buf.decomp);
    delete arg;

    dmx::fseq_set_active(false);
    g_step_ms.store(0, std::memory_order_release);
    g_frame_count.store(0, std::memory_order_release);
    g_cur_frame.store(0, std::memory_order_release);
    g_seek_frame.store(-1, std::memory_order_release);
    g_list_idx.store(-1, std::memory_order_relaxed);
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

    // Card supply (active low): on before the first mount, and let it settle —
    // an SD card needs ~1 ms after VDD before it answers CMD0.
    if (cfg.power_gpio >= 0) {
        const auto pin = static_cast<gpio_num_t>(cfg.power_gpio);
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Try an immediate mount; the monitor task will retry every second if absent.
    // The driver's per-attempt errors when no card is present are demoted to
    // debug by the web log tee (web_config log_vprintf), not muted here.
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
        // name.fseq in any case; a bare ".fseq" is a hidden file, not a show.
        if (len <= 5 || strcasecmp(name + len - 5, ".fseq") != 0) continue;
        strncpy(names[count], name, kMaxNameLen - 1);
        names[count][kMaxNameLen - 1] = '\0';
        ++count;
    }
    closedir(dir);
    return count;
}

namespace {
// Starts the playback task on `filename` with the mode already chosen.
bool launch(const char* filename) {
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

bool card_ready() {
    if (g_sd_state.load(std::memory_order_acquire) == SdState::Mounted) return true;
    snprintf(g_error, sizeof(g_error), "No SD card");
    g_status = Status::Error;
    return false;
}
}  // namespace

bool start(const char* filename, bool loop) {
    if (!filename || !filename[0]) return false;
    if (!card_ready()) return false;
    stop();  // stop any running playback first
    g_mode = loop ? Mode::Loop : Mode::Once;
    g_list_idx.store(-1, std::memory_order_relaxed);
    return launch(filename);
}

bool start_playlist() {
    if (!card_ready()) return false;
    const config::FseqPlaylist& pl = config::get_playlist();
    if (pl.count == 0) {
        snprintf(g_error, sizeof(g_error), "Empty playlist");
        g_status = Status::Error;
        return false;
    }
    stop();
    g_list        = pl;
    g_mode        = Mode::Playlist;
    g_repeat_left = static_cast<uint8_t>(g_list.items[0].repeat - 1);
    g_list_idx.store(0, std::memory_order_relaxed);
    if (launch(g_list.items[0].name)) return true;
    g_list_idx.store(-1, std::memory_order_relaxed);
    return false;
}

bool looping() {
    return g_run.load(std::memory_order_acquire) &&
           (g_mode == Mode::Loop || (g_mode == Mode::Playlist && g_list.loop));
}

int playlist_index() {
    return g_list_idx.load(std::memory_order_relaxed);
}

void stop() {
    if (!g_run.load(std::memory_order_acquire)) return;
    stop_task();
    ESP_LOGI(TAG, "playback stopped");
}

bool seek_ms(uint32_t ms) {
    if (!g_run.load(std::memory_order_acquire)) return false;
    const uint32_t step = g_step_ms.load(std::memory_order_acquire);
    if (step == 0) return false;
    g_seek_frame.store(static_cast<int64_t>(ms / step), std::memory_order_release);
    return true;
}

uint32_t position_ms() {
    return g_cur_frame.load(std::memory_order_acquire) * g_step_ms.load(std::memory_order_acquire);
}

uint32_t duration_ms() {
    return g_frame_count.load(std::memory_order_acquire) *
           g_step_ms.load(std::memory_order_acquire);
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
