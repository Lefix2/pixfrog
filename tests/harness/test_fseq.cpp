// fseq_player.cpp on a host directory standing for the SD card: hot-plug,
// file listing, uncompressed / zstd / sparse playback down to the universe
// banks, seek, every malformed-file path, and fpp_sync.cpp driving it from
// FPP MultiSync packets. zstd frames are built with raw blocks (no compressor
// needed; the real decoder parses them).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fpp_sync_parser.h"
#include "fseq_format.h"
#include "fseq_player.h"
#include "harness.h"
#include "shim_control.h"

using namespace pixfrog;
using Bytes = std::vector<uint8_t>;

namespace {

std::string g_dir;  // the card

void put_le16(Bytes& b, size_t at, uint16_t v) {
    b[at]     = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void put_le32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// A v2 header; tables and data are appended by the callers.
Bytes header(uint32_t channels, uint32_t frames, uint8_t comp = 0, uint8_t nblocks = 0,
             uint8_t nranges = 0, uint16_t data_off = 0) {
    Bytes h(32, 0);
    std::memcpy(h.data(), "PSEQ", 4);
    const uint16_t off = data_off ? data_off
                                  : static_cast<uint16_t>(32 + nblocks * 8 + nranges * 6);
    put_le16(h, 4, off);
    h[7] = 2;  // major
    put_le16(h, 8, off);
    put_le32(h, 10, channels);
    put_le32(h, 14, frames);
    h[18] = 25;  // 40 fps
    h[20] = comp;
    h[21] = nblocks;
    h[22] = nranges;
    return h;
}

// Frame n of a test file: every byte = 10 * (n + 1) + channel index % 10.
Bytes frames_data(uint32_t channels, uint32_t frames) {
    Bytes d;
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < channels; ++c)
            d.push_back(static_cast<uint8_t>(10 * (f + 1) + c % 10));
    return d;
}

// One zstd frame of raw (stored) blocks holding `data`.
Bytes zstd_raw(const Bytes& data) {
    Bytes z = { 0x28, 0xB5, 0x2F, 0xFD, 0xA0 };  // magic; single segment, 4-byte content size
    for (int i = 0; i < 4; ++i)
        z.push_back(static_cast<uint8_t>(data.size() >> (8 * i)));
    const uint32_t hdr = 1u | (0u << 1) | (static_cast<uint32_t>(data.size()) << 3);  // last, raw
    z.push_back(static_cast<uint8_t>(hdr));
    z.push_back(static_cast<uint8_t>(hdr >> 8));
    z.push_back(static_cast<uint8_t>(hdr >> 16));
    z.insert(z.end(), data.begin(), data.end());
    return z;
}

void write_file(const char* name, const Bytes& b) {
    FILE* f = fopen((g_dir + "/" + name).c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

Bytes cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Universe-1 bytes seen at every frame boundary of a playback.
std::vector<Bytes> g_seen;
void capture() {
    const uint8_t* u = dmx::universe_front_buffer_for(1);
    if (u) g_seen.push_back(Bytes(u, u + 3));
}

// Plays `name` to its end (or `max_frames`), capturing universe 1 per frame.
bool play(const char* name, int max_frames = 1000) {
    g_seen.clear();
    if (!fseq::start(name)) return false;
    shim::run_task_for("fseq_play", max_frames, capture);
    return true;
}

void map_universes() {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch < 2 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = 4;
        c.universe_start = static_cast<uint16_t>(1 + ch);  // channel 0: U1, channel 1: U2
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    dmx::handle_pending_remaps();
}

void setup() {
    static bool once = false;
    if (!once) {
        char tmpl[] = "/tmp/pixfrog-sd-XXXXXX";
        g_dir       = mkdtemp(tmpl);
        shim::sd_root(g_dir);
        shim::sd_insert(true);
        shim::nvs_wipe();
        config::init();
        dmx::init();
        map_universes();
        fseq::InitConfig cfg{};
        fseq::init(cfg);
        once = true;
    }
    shim::faults_clear();
    shim::sd_insert(true);
}

}  // namespace

TEST(card_mounts_at_init_and_lists_only_fseq_files) {
    EXPECT_TRUE(fseq::sd_state() == fseq::SdState::Mounted);
    EXPECT_TRUE(fseq::init(fseq::InitConfig{}));  // second call: no-op
    write_file("a.fseq", header(3, 1));
    write_file("B.FSEQ", header(3, 1));
    write_file("notes.txt", Bytes(4, 0));
    write_file("x", Bytes(1, 0));
    mkdir((g_dir + "/dir.fseq").c_str(), 0700);
    char names[fseq::kMaxFiles][fseq::kMaxNameLen];
    const size_t n = fseq::list_files(names, fseq::kMaxFiles);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(fseq::list_files(names, 0), 0u);
}

TEST(uncompressed_file_plays_every_frame_into_the_banks) {
    write_file("lin.fseq", cat(header(12, 3), frames_data(12, 3)));
    EXPECT_TRUE(play("lin.fseq"));
    EXPECT_EQ(g_seen.size(), 3u);
    if (g_seen.size() == 3) {
        EXPECT_EQ(g_seen[0][0], 10);
        EXPECT_EQ(g_seen[1][0], 20);
        EXPECT_EQ(g_seen[2][2], 32);
    }
    EXPECT_TRUE(fseq::status() == fseq::Status::Idle);  // ran to its end
    EXPECT_FALSE(dmx::fseq_is_active());
    EXPECT_TRUE(fseq::active_file() == nullptr);
}

namespace {
int g_tick = 0;
void seek_on_first_frame() {
    capture();
    if (++g_tick == 1) {
        EXPECT_EQ(fseq::duration_ms(), 250u);  // 10 frames × 25 ms
        EXPECT_TRUE(fseq::seek_ms(200));       // frame 8
    }
}
}  // namespace

TEST(seek_jumps_to_the_requested_frame) {
    write_file("ten.fseq", cat(header(3, 10), frames_data(3, 10)));
    EXPECT_FALSE(fseq::seek_ms(10));  // nothing playing
    g_seen.clear();
    g_tick = 0;
    EXPECT_TRUE(fseq::start("ten.fseq"));
    shim::run_task_for("fseq_play", 100, seek_on_first_frame);
    // frame 0, then the seek lands on frame 8: 8, 9.
    EXPECT_EQ(g_seen.size(), 3u);
    if (g_seen.size() == 3) EXPECT_EQ(g_seen[1][0], 90);
}

TEST(zstd_blocks_decompress_and_bad_blocks_are_skipped) {
    // Block 0: frames 0-1 (good). Block 1: frame 2, corrupt data.
    const Bytes good = zstd_raw(frames_data(6, 2));
    const Bytes bad  = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
    Bytes f          = header(6, 3, fseq::kCompZstd, 2);
    Bytes table(16, 0);
    put_le32(table, 0, 0);
    put_le32(table, 4, static_cast<uint32_t>(good.size()));
    put_le32(table, 8, 2);
    put_le32(table, 12, static_cast<uint32_t>(bad.size()));
    write_file("z.fseq", cat(cat(cat(f, table), good), bad));
    EXPECT_TRUE(play("z.fseq"));
    EXPECT_EQ(g_seen.size(), 3u);  // every frame period still elapses
    if (!g_seen.empty()) EXPECT_EQ(g_seen[0][0], 10);
    if (g_seen.size() > 1) EXPECT_EQ(g_seen[1][0], 20);
}

TEST(zstd_corner_cases_skip_frames) {
    // A zero-size block, a short read, frames past the decompressed data,
    // and frames with no block at all.
    const Bytes one = zstd_raw(frames_data(6, 1));  // holds 1 frame, claims 2
    Bytes f         = header(6, 4, fseq::kCompZstd, 3);
    Bytes table(24, 0);
    put_le32(table, 0, 0);  // block 0: frames 0-1 → only 1 decompresses
    put_le32(table, 4, static_cast<uint32_t>(one.size()));
    put_le32(table, 8, 2);  // block 1: frame 2, size 0
    put_le32(table, 12, 0);
    put_le32(table, 16, 3);  // block 2: frame 3, larger than the file
    put_le32(table, 20, 4096);
    write_file("zc.fseq", cat(cat(f, table), one));
    EXPECT_TRUE(play("zc.fseq"));
    EXPECT_EQ(g_seen.size(), 4u);
    Bytes nb = header(6, 2, fseq::kCompZstd, 0);  // compressed but no block table
    write_file("znb.fseq", cat(nb, frames_data(6, 2)));
    EXPECT_TRUE(play("znb.fseq"));
    EXPECT_EQ(g_seen.size(), 2u);
}

TEST(sparse_ranges_land_on_their_universes) {
    // Range A: 3 bytes at channel 0 (U1 slot 0); range B: 3 bytes at channel
    // 512 (U2 slot 0); range C: past the addressable universes (dropped).
    Bytes f = header(9, 1, 0, 0, 3);
    Bytes ranges(18, 0);
    put_le32(ranges, 0, 0);
    put_le16(ranges, 4, 3);
    put_le32(ranges, 6, 512);
    put_le16(ranges, 10, 3);
    put_le32(ranges, 12, 0x7FFFu * 512u + 1024u);
    put_le16(ranges, 16, 3);
    write_file("sp.fseq", cat(cat(f, ranges), Bytes{ 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
    EXPECT_TRUE(play("sp.fseq"));
    const uint8_t* u2 = dmx::universe_front_buffer_for(2);
    EXPECT_EQ(dmx::universe_front_buffer_for(1)[0], 1);
    if (u2) EXPECT_EQ(u2[2], 6);
}

TEST(malformed_files_report_an_error) {
    struct Case {
        const char* name;
        Bytes bytes;
        const char* error;
    };
    Bytes bad_magic    = header(3, 1);
    bad_magic[0]       = 'X';
    Bytes short_tbl    = header(3, 1, fseq::kCompZstd, 4);  // 4 blocks announced, none written
    Bytes short_sp     = header(3, 1, 0, 0, 2);             // 2 ranges announced, none written
    const Case cases[] = {
        { "short.fseq", Bytes(10, 0), "Short read: header" },
        { "magic.fseq", bad_magic, "Bad FSEQ header" },
        { "empty.fseq", header(0, 0), "Empty FSEQ file" },
        { "tbl.fseq", short_tbl, "Short read: comp table" },
        { "sp2.fseq", short_sp, "Short read: sparse table" },
        { "lz4.fseq", cat(header(3, 1, fseq::kCompLz4), Bytes(3, 0)), "lz4 compression" },
    };
    for (const auto& c : cases) {
        write_file(c.name, c.bytes);
        EXPECT_TRUE(play(c.name));
        EXPECT_TRUE(fseq::status() == fseq::Status::Error);
        EXPECT_TRUE(std::strstr(fseq::error_string(), c.error) != nullptr);
    }
    EXPECT_TRUE(play("missing.fseq"));
    EXPECT_TRUE(std::strstr(fseq::error_string(), "Cannot open") != nullptr);
}

TEST(start_failures_and_stop_before_the_first_frame) {
    EXPECT_FALSE(fseq::start(""));
    shim::fail_next(shim::Fault::HeapCaps, 1, 1);  // the second PSRAM buffer
    EXPECT_FALSE(fseq::start("lin.fseq"));
    EXPECT_TRUE(std::strstr(fseq::error_string(), "PSRAM") != nullptr);
    shim::fail_next(shim::Fault::TaskCreate);
    EXPECT_FALSE(fseq::start("lin.fseq"));
    EXPECT_TRUE(std::strstr(fseq::error_string(), "Task create") != nullptr);
    EXPECT_TRUE(fseq::start("lin.fseq"));
    EXPECT_STREQ(fseq::active_file(), "lin.fseq");
    fseq::stop();                 // the task has not run yet: stop waits it out
    shim::run_task("fseq_play");  // it sees the stop at once and cleans up
    EXPECT_TRUE(fseq::status() == fseq::Status::Idle);
    fseq::stop();  // idle: no-op
}

namespace {
// Full control preset on universe 100: slot 16 is the FSEQ band.
void desk_fseq(uint8_t value) {
    uint8_t u[16] = { 0xFF, 0xFF };
    u[15]         = value;
    dmx::write_universe_from_source(100, u, sizeof(u), 0x0A000001, dmx::kArtnetMergeTimeoutUs);
    dmx::swap_universes();
    dmx::update_show_control();
}

int g_mon = 0;
void monitor_script() {
    ++g_mon;
    if (g_mon == 2) desk_fseq(8);  // first frame of the desk: band 1 = file 1
    if (g_mon == 4) {
        EXPECT_TRUE(fseq::active_file() != nullptr);  // served by the monitor
        shim::run_task("fseq_play");
        desk_fseq(0);  // band back to 0: stop
    }
    if (g_mon == 7) shim::sd_insert(false);  // pulled: noticed at the 1 s check
    if (g_mon == 15) {
        EXPECT_TRUE(fseq::sd_state() == fseq::SdState::Absent);
        char names[4][fseq::kMaxNameLen];
        EXPECT_EQ(fseq::list_files(names, 4), 0u);
        EXPECT_FALSE(fseq::start("lin.fseq"));  // no card
        shim::sd_insert(true);
    }
}
}  // namespace

TEST(monitor_serves_desk_requests_and_hot_plug) {
    auto c     = config::default_control();
    c.enabled  = 1;
    c.universe = 100;
    config::control_apply_preset(c, config::ControlPreset::Full);
    config::set_control(c);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    g_mon = 0;
    EXPECT_TRUE(shim::run_task_for("sd_mon", 25, monitor_script));
    EXPECT_TRUE(fseq::sd_state() == fseq::SdState::Mounted);  // re-inserted, re-mounted
    EXPECT_TRUE(fseq::active_file() == nullptr);              // the desk stopped it
    config::set_control(config::default_control());
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    dmx::update_show_control();
}

// ── FPP MultiSync ───────────────────────────────────────────────────────────

namespace {
constexpr uint16_t kFpp = fpp::parser::kPort;

Bytes fpp_sync(uint8_t action, const char* file, float secs, uint8_t file_type = 0) {
    Bytes b = { 'F', 'P', 'P', 'D', fpp::parser::kPktSync, 0, 0, action, file_type };
    for (int i = 0; i < 4; ++i)
        b.push_back(0);  // frame number
    uint8_t f[4];
    std::memcpy(f, &secs, 4);
    b.insert(b.end(), f, f + 4);
    b.insert(b.end(), file, file + std::strlen(file) + 1);
    b[5] = static_cast<uint8_t>(b.size() - 7);
    return b;
}

void pump_fpp() {
    shim::net_on_idle(kFpp, [] { fpp::stop(); });
    fpp::start();
    shim::run_task("fpp_sync");
}
}  // namespace

TEST(fpp_start_sync_and_stop_drive_the_player) {
    // One pump per started file: each playback task runs (and frees its
    // buffers) before the next start, as it would on the device.
    shim::net_reset();
    shim::tasks_forget();
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncOpen, "lin.fseq", 0));  // preload: ignored
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncStart, "lin.fseq", 0));
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncSync, "lin.fseq", 0.02f));  // in tolerance
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncSync, "lin.fseq", 5.0f));   // drifted: seek
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncStart, "song.mp3", 0, 1));  // media: ignored
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncSync, "", 1.0f));           // no name
    shim::net_push(kFpp, Bytes{ 'F', 'P', 'P', 'D', 9, 0, 0 });                 // other packet
    pump_fpp();
    EXPECT_STREQ(fseq::active_file(), "lin.fseq");
    EXPECT_FALSE(fpp::is_running());
    shim::run_task("fseq_play");

    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncSync, "ten.fseq", 1.0f));  // hot-join
    pump_fpp();
    EXPECT_STREQ(fseq::active_file(), "ten.fseq");
    shim::run_task("fseq_play");

    // Hot-join of a file that cannot be read: the start succeeds, the
    // playback task reports the error.
    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncSync, "nope.fseq", 1.0f));
    pump_fpp();
    shim::run_task("fseq_play");
    EXPECT_TRUE(fseq::status() == fseq::Status::Error);

    shim::net_push(kFpp, fpp_sync(fpp::parser::kSyncStop, "", 0));
    pump_fpp();
    EXPECT_TRUE(fseq::status() != fseq::Status::Playing);
}

TEST(fpp_socket_bind_and_join_failures) {
    shim::net_reset();
    shim::tasks_forget();
    shim::fail_next(shim::Fault::Socket);
    fpp::start();
    shim::run_task("fpp_sync");
    EXPECT_FALSE(fpp::is_running());
    shim::fail_next(shim::Fault::Bind);
    fpp::start();
    shim::run_task("fpp_sync");
    EXPECT_FALSE(fpp::is_running());
    shim::fail_next(shim::Fault::Join);  // non-fatal: broadcast still works
    pump_fpp();
    EXPECT_FALSE(fpp::is_running());
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
