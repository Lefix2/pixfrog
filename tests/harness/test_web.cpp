// web_config.cpp — the REST API contract through the real handlers: routes,
// JSON shapes, validation, auth + CSRF gate, backup/restore, OTA, scenes,
// FSEQ, logs, core dump, peers. Real config_store/dmx_manager/sacn beneath.

#include <string>
#include <sys/stat.h>
#include <vector>

#include "cJSON.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
#include "harness.h"
#include "hub_election.h"
#include "sacn.h"
#include "shim_control.h"
#include "web_config.h"

using namespace pixfrog;
using Headers = std::map<std::string, std::string>;

namespace {

std::string b64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t val = 0;
    int bits     = -6;
    for (unsigned char c : in) {
        val   = ((val << 8) + c) & 0xFFFFFF;  // only the last 3 bytes are ever read
        bits += 8;
        while (bits >= 0) {
            out.push_back(t[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

shim::HttpResponse get(const std::string& uri) {
    return shim::http_request("GET", uri);
}
shim::HttpResponse post(const std::string& uri, const std::string& body = "{}",
                        const Headers& h = {}) {
    return shim::http_request("POST", uri, body, h);
}

// Parsed JSON body, freed at the end of the full-expression's scope owner.
struct Json {
    cJSON* j;
    explicit Json(const std::string& s) : j(cJSON_Parse(s.c_str())) {}
    ~Json() { cJSON_Delete(j); }
    const cJSON* operator[](const char* k) const { return cJSON_GetObjectItemCaseSensitive(j, k); }
};

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        web::start();
        once = true;
    }
    fake::reset_modules();
    fseq::fake::set(fseq::Status::Idle, 0);
    shim::http_recv_chunk(0);
    shim::ota_fail_validation(false);
    shim::coredump_set({});
    shim::mdns_reset();
    if (config::web_password_set()) config::set_web_password("");
}

}  // namespace

TEST(every_route_fits_the_handler_table) {
    // esp_http_server refuses handlers past max_uri_handlers and start()
    // ignores the result: an overflow would silently lose the last routes.
    EXPECT_TRUE(shim::http_running());
    EXPECT_EQ(shim::http_routes(), 34);
    EXPECT_TRUE(post("/api/loglevel", "{\"level\":\"info\"}").handled);  // the last one
}

TEST(root_page_is_gzipped_with_etag_revalidation) {
    const auto r = get("/");
    EXPECT_EQ(r.status, 200);
    EXPECT_STREQ(r.headers.at("Content-Encoding").c_str(), "gzip");
    EXPECT_TRUE(r.body.size() > 50000 && r.body.size() < 150000);
    EXPECT_EQ(static_cast<uint8_t>(r.body[0]), 0x1f);  // gzip magic
    const std::string etag = r.headers.at("ETag");
    const auto again       = shim::http_request("GET", "/", "", { { "If-None-Match", etag } });
    EXPECT_EQ(again.status, 304);
    EXPECT_EQ(again.body.size(), 0);
    EXPECT_EQ(shim::http_request("GET", "/", "", { { "If-None-Match", "\"stale\"" } }).status, 200);
    EXPECT_EQ(get("/?embed=1").status, 200);
}

TEST(config_json_shape_and_cors) {
    const auto r = get("/api/config");
    EXPECT_EQ(r.status, 200);
    EXPECT_STREQ(r.headers.at("Access-Control-Allow-Origin").c_str(), "*");
    Json j(r.body);
    EXPECT_TRUE(cJSON_IsObject(j["global"]));
    EXPECT_EQ(cJSON_GetArraySize(j["channels"]), 8);
    EXPECT_EQ(cJSON_GetArraySize(j["scenes"]), static_cast<int>(config::num_scenes()));
    const cJSON* ch0 = cJSON_GetArrayItem(j["channels"], 0);
    for (const char* k :
         { "protocol", "pixel_count", "max_pixels", "universe_start", "gaps", "wb" })
        EXPECT_TRUE(cJSON_GetObjectItemCaseSensitive(ch0, k) != nullptr);
}

TEST(status_and_diag_report_the_rollback_until_acknowledged) {
    Json s(get("/api/status").body);
    EXPECT_TRUE(s["fps"] != nullptr);
    EXPECT_TRUE(s["rollback"] == nullptr);
    config::RollbackRecord r{};
    std::strcpy(r.rejected_version, "v9.9.9");
    std::strcpy(r.rejected_slot, "ota_1");
    config::set_rollback(r);
    Json s2(get("/api/status").body);
    EXPECT_TRUE(s2["rollback"] != nullptr);
    EXPECT_EQ(post("/api/rollback/ack").status, 200);
    Json s3(get("/api/status").body);
    EXPECT_TRUE(s3["rollback"] == nullptr);
    Json d(get("/api/diag").body);
    const cJSON* sys = d["sys"];
    EXPECT_TRUE(cJSON_GetObjectItemCaseSensitive(sys, "last_rollback") != nullptr);
}

TEST(post_global_applies_and_validates) {
    EXPECT_EQ(post("/api/global", "{\"refresh_hz\":45,\"short_name\":\"rig-a\"}").status, 200);
    EXPECT_EQ(config::get_global().refresh_rate_hz, 45);
    EXPECT_STREQ(config::get_global().short_name, "rig-a");
    post("/api/global", "{\"refresh_hz\":200}");  // out of range: ignored
    EXPECT_EQ(config::get_global().refresh_rate_hz, 45);
    EXPECT_EQ(post("/api/global", "{not json").status, 400);
    EXPECT_EQ(post("/api/global", "").status, 400);
    post("/api/global", "{\"web_enabled\":true,\"sacn_enabled\":true}");
    EXPECT_TRUE(shim::task_created("sacn_rx"));
    post("/api/global", "{\"refresh_hz\":60,\"sacn_enabled\":false}");
}

TEST(auth_gate_and_brute_force_delay) {
    EXPECT_EQ(post("/api/global", "{\"web_password\":\"pa55\"}").status, 200);
    EXPECT_TRUE(config::web_password_set());
    const int64_t t0 = shim::now_us();
    auto denied      = post("/api/scenes/stop");
    EXPECT_EQ(denied.status, 401);
    EXPECT_TRUE(denied.headers.count("WWW-Authenticate") == 1);
    EXPECT_TRUE(shim::now_us() - t0 >= 500000);  // flat 500 ms
    EXPECT_EQ(post("/api/scenes/stop", "{}", { { "Authorization", "Basic " + b64("admin:nope") } })
                  .status,
              401);
    const Headers ok = { { "Authorization", "Basic " + b64("anyone:pa55") } };
    EXPECT_EQ(post("/api/scenes/stop", "{}", ok).status, 200);
    EXPECT_EQ(get("/api/config").status, 200);  // reads stay open…
    // …except raw RAM and the log ring, which may hold a password or show data.
    EXPECT_EQ(get("/api/logs").status, 401);
    EXPECT_EQ(get("/api/coredump").status, 401);
    EXPECT_EQ(shim::http_request("GET", "/api/logs", "", ok).status, 200);
    EXPECT_EQ(shim::http_request("GET", "/api/coredump", "", ok).status, 404);  // none stored
    EXPECT_EQ(post("/api/global", "{\"web_password\":\"\"}", ok).status, 200);
    EXPECT_FALSE(config::web_password_set());
}

TEST(longest_password_works_one_more_is_refused) {
    // Was truncated to 63 bytes before hashing while the check hashed what the
    // browser sent: a longer password locked the user out (UART recovery only).
    const std::string too_long(64, 'p');
    EXPECT_EQ(post("/api/global", "{\"web_password\":\"" + too_long + "\"}").status, 400);
    EXPECT_FALSE(config::web_password_set());
    EXPECT_FALSE(config::set_web_password(too_long.c_str()));

    const std::string longest(63, 'q');
    EXPECT_EQ(post("/api/global", "{\"web_password\":\"" + longest + "\"}").status, 200);
    EXPECT_TRUE(config::web_password_set());
    // A long user name as well: the Authorization header must still fit.
    const Headers ok = { { "Authorization",
                           "Basic " + b64(std::string(40, 'u') + ":" + longest) } };
    EXPECT_EQ(post("/api/scenes/stop", "{}", ok).status, 200);
    EXPECT_EQ(post("/api/global", "{\"web_password\":\"\"}", ok).status, 200);
}

TEST(cross_origin_writes_are_rejected) {
    const Headers foreign = { { "Origin", "http://evil.example" }, { "Host", "192.168.2.50" } };
    EXPECT_EQ(post("/api/scenes/stop", "{}", foreign).status, 403);
    const Headers same = { { "Origin", "http://192.168.2.50" }, { "Host", "192.168.2.50" } };
    EXPECT_EQ(post("/api/scenes/stop", "{}", same).status, 200);
}

TEST(post_channel_updates_and_ignores_bad_fields) {
    const std::string body = "{\"protocol\":\"WS2815\",\"pixel_count\":144,\"universe_start\":7,"
                             "\"gaps\":[[1,1],[60,2]],\"wb\":\"#ff8000\",\"brightness\":999}";
    EXPECT_EQ(post("/api/channel/1", body).status, 200);
    const auto& c = config::get_channel(1);
    EXPECT_EQ(c.pixel_count, 144);
    EXPECT_EQ(c.universe_start, 7);
    EXPECT_EQ(c.gaps[1].pos, 59);
    EXPECT_EQ(c.wb_g, 0x80);
    EXPECT_TRUE(c.brightness != 999 % 256);  // out of range: untouched
    EXPECT_EQ(post("/api/channel/9", "{}").status, 400);
    EXPECT_EQ(post("/api/channel/1/identify").status, 200);
    EXPECT_EQ(dmx::identify_channel(), 1);
    dmx::identify_stop();
}

// Fixtures: [first LED, count] pairs like the gaps; two sharing an LED are
// refused whole, the stored list kept.
TEST(channel_fixtures_round_trip_and_overlaps_are_refused) {
    EXPECT_EQ(post("/api/channel/4", "{\"pixel_count\":295,\"gaps\":[[60,1],[120,1]],"
                                     "\"fixtures\":[[1,59],[61,59],[121,59]]}")
                  .status,
              200);
    const auto& c = config::get_channel(4);
    EXPECT_EQ(config::fixture_count(c.fixtures, config::kMaxFixtures), 3u);
    EXPECT_EQ(c.fixtures[1].pos, 60);
    EXPECT_EQ(c.fixtures[1].len, 59);
    Json cfg(get("/api/config").body);
    const cJSON* jf = cJSON_GetObjectItem(cJSON_GetArrayItem(cfg["channels"], 4), "fixtures");
    EXPECT_EQ(cJSON_GetArraySize(jf), 3);
    EXPECT_EQ(cJSON_GetArrayItem(cJSON_GetArrayItem(jf, 2), 0)->valueint, 121);
    const auto overlap = post("/api/channel/4", "{\"fixtures\":[[1,59],[50,20]]}");
    EXPECT_EQ(overlap.status, 400);
    EXPECT_TRUE(overlap.body.find("fixtures 1 and 2 overlap") != std::string::npos);
    EXPECT_EQ(post("/api/channel/4", "{\"fixtures\":[[0,5]]}").status, 400);      // 1-based
    EXPECT_EQ(post("/api/channel/4", "{\"fixtures\":[[1020,10]]}").status, 400);  // past 1024
    EXPECT_EQ(post("/api/channel/4", "{\"fixtures\":{\"a\":1}}").status, 400);    // not a list
    EXPECT_EQ(config::fixture_count(c.fixtures, config::kMaxFixtures), 3u);       // kept
    // 32 fixtures (the most) fit one request, and a full backup restores.
    config::ChannelConfig saved[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        saved[ch] = config::get_channel(ch);
    std::string many = "{\"pixel_count\":1024,\"fixtures\":[";
    for (int k = 0; k < 32; ++k)
        many += (k ? ",[" : "[") + std::to_string(1 + k * 32) + ",31]";
    many += "]}";
    for (int ch = 0; ch < 8; ++ch)
        EXPECT_EQ(post("/api/channel/" + std::to_string(ch), many).status, 200);
    const std::string backup = get("/api/backup").body;
    config::reset_to_defaults();
    EXPECT_EQ(post("/api/restore", backup).status, 200);
    EXPECT_EQ(config::fixture_count(config::get_channel(7).fixtures, config::kMaxFixtures), 32u);
    EXPECT_EQ(post("/api/channel/0", "{\"fixtures\":[]}").status, 200);  // empty: none
    EXPECT_EQ(config::fixture_count(config::get_channel(0).fixtures, config::kMaxFixtures), 0u);
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        config::set_channel(ch, saved[ch]);
}

// The dashboard button: every configured output, or the outputs asked for.
TEST(speaker_test_endpoint_and_status) {
    Json s(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsTrue(s["audio"]));
    EXPECT_EQ(post("/api/audio/test").status, 200);
    EXPECT_EQ(fake::modules().audio_tests, 1);
    fake::modules().audio_ready = false;  // no codec answering
    EXPECT_EQ(post("/api/audio/test").status, 409);
    Json off(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsFalse(off["audio"]));
}

TEST(identify_endpoint_runs_the_configured_outputs_in_turn) {
    Json all(post("/api/identify", "").body);
    EXPECT_EQ(cJSON_GetObjectItem(all.j, "outputs")->valueint, dmx::identify_configured_outputs());
    Json some(post("/api/identify", "{\"outputs\":5}").body);
    EXPECT_EQ(cJSON_GetObjectItem(some.j, "outputs")->valueint, 5);
    EXPECT_EQ(dmx::identify_channel(), 0);
    EXPECT_EQ(post("/api/identify", "{nope").status, 400);
    dmx::identify_stop();
}

TEST(fragmented_request_bodies_are_reassembled) {
    shim::http_recv_chunk(7);
    EXPECT_EQ(post("/api/channel/2", "{\"pixel_count\":321,\"protocol\":\"WS2812B\"}").status, 200);
    EXPECT_EQ(config::get_channel(2).pixel_count, 321);
}

TEST(out_of_range_numbers_are_ignored_not_wrapped) {
    // Found by fuzz_web_api: the value was cast to uint32_t before the range
    // check (UB; on the RISC-V target -5 saturated to 0 and was accepted).
    const auto before = config::get_channel(1);
    post("/api/channel/1", "{\"brightness\":-5,\"pixel_count\":1e40,\"grouping\":-1}");
    const auto after = config::get_channel(1);
    EXPECT_EQ(after.brightness, before.brightness);
    EXPECT_EQ(after.pixel_count, before.pixel_count);
    EXPECT_EQ(after.grouping, before.grouping);
    const auto g = config::get_global();
    post("/api/global", "{\"refresh_hz\":-60}");
    EXPECT_EQ(config::get_global().refresh_rate_hz, g.refresh_rate_hz);
}

TEST(scene_endpoints_manage_the_list) {
    const size_t n = config::num_scenes();
    Json added(post("/api/scenes/add", "{\"name\":\"Web\",\"effect\":3}").body);
    EXPECT_EQ(added["index"]->valueint, static_cast<int>(n));
    EXPECT_EQ(
        post("/api/scene/" + std::to_string(n), "{\"colors\":[\"#ff0000\",\"#00ff00\"]}").status,
        200);
    EXPECT_EQ(config::scene_num_colors(config::get_scene(n)), 2);
    EXPECT_EQ(config::get_scene(n).fixture_mode, config::kFixtureModeStrip);  // the default
    post("/api/scene/" + std::to_string(n), "{\"fixture_mode\":\"mirror\"}");
    EXPECT_EQ(config::get_scene(n).fixture_mode, config::kFixtureModeMirror);
    post("/api/scene/" + std::to_string(n), "{\"fixture_mode\":\"spiral\"}");  // unknown: kept
    EXPECT_EQ(config::get_scene(n).fixture_mode, config::kFixtureModeMirror);
    Json listed(get("/api/config").body);
    EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(listed["scenes"], static_cast<int>(n)),
                                     "fixture_mode")
                     ->valuestring,
                 "mirror");
    EXPECT_EQ(post("/api/scenes/move", "{\"from\":" + std::to_string(n) + ",\"to\":0}").status,
              200);
    EXPECT_STREQ(config::get_scene(0).name, "Web");
    EXPECT_EQ(post("/api/scene/0/play").status, 200);
    EXPECT_EQ(dmx::active_scene(), 0);
    EXPECT_EQ(post("/api/scene/0/delete").status, 200);
    EXPECT_EQ(dmx::active_scene(), -1);
    EXPECT_EQ(post("/api/scene/99/play").status, 404);
    EXPECT_EQ(post("/api/scenes/move", "{\"from\":0,\"to\":99}").status, 400);
    EXPECT_EQ(config::num_scenes(), n);
}

// Several boxes' backups must not collide: the file is named after the box
// and the browser's date (the box has no clock).
TEST(backup_file_is_named_after_the_box_and_the_date) {
    auto name = [](const std::string& uri) { return get(uri).headers.at("Content-Disposition"); };
    post("/api/global", "{\"short_name\":\"Rack 2 / Cour\"}");
    EXPECT_STREQ(name("/api/backup?date=2026-10-01").c_str(),
                 "attachment; filename=\"pixfrog-rack-2-cour-2026-10-01.json\"");
    EXPECT_STREQ(name("/api/backup").c_str(), "attachment; filename=\"pixfrog-rack-2-cour.json\"");
    EXPECT_STREQ(name("/api/backup?date=..%2Fetc").c_str(),
                 "attachment; filename=\"pixfrog-rack-2-cour.json\"");  // not a date: dropped
    post("/api/global", "{\"short_name\":\"***\"}");
    EXPECT_STREQ(name("/api/backup?date=2026-10-01").c_str(),
                 "attachment; filename=\"pixfrog-2026-10-01.json\"");
    post("/api/global", "{\"short_name\":\"pixfrog\"}");
    EXPECT_STREQ(name("/api/backup?date=2026-10-01").c_str(),
                 "attachment; filename=\"pixfrog-2026-10-01.json\"");  // not pixfrog-pixfrog
}

// POST and restore share one parser per object: the same bounds, the same
// bools (true/false or 0/1). A field POST refuses fails the whole request;
// restore skips it and keeps the rest.
TEST(post_and_restore_parse_the_same_fields) {
    const auto before = config::get_global();
    EXPECT_EQ(post("/api/global", "{\"refresh_hz\":30,\"ip\":\"nope\"}").status, 400);
    EXPECT_EQ(config::get_global().refresh_rate_hz, before.refresh_rate_hz);  // nothing saved
    EXPECT_EQ(post("/api/channel/2", "{\"pixel_count\":33,\"color_order\":\"XYZ\"}").status, 400);
    EXPECT_TRUE(config::get_channel(2).pixel_count != 33);

    Json backup(get("/api/backup").body);
    cJSON* g = cJSON_GetObjectItem(backup.j, "global");
    cJSON_ReplaceItemInObject(g, "refresh_hz", cJSON_CreateNumber(30));
    cJSON_ReplaceItemInObject(g, "ip", cJSON_CreateString("nope"));
    cJSON_ReplaceItemInObject(g, "reply_unicast", cJSON_CreateNumber(1));  // 0/1 as in POST
    cJSON* c2 = cJSON_GetArrayItem(cJSON_GetObjectItem(backup.j, "channels"), 2);
    cJSON_ReplaceItemInObject(c2, "pixel_count", cJSON_CreateNumber(33));
    cJSON_ReplaceItemInObject(c2, "color_order", cJSON_CreateString("XYZ"));
    cJSON_ReplaceItemInObject(c2, "invert", cJSON_CreateNumber(1));
    char* body = cJSON_PrintUnformatted(backup.j);
    EXPECT_EQ(post("/api/restore", body).status, 200);
    cJSON_free(body);
    EXPECT_EQ(config::get_global().refresh_rate_hz, 30);
    EXPECT_EQ(config::get_global().static_ip, before.static_ip);  // the bad one skipped
    EXPECT_TRUE(config::get_global().artnet_poll_reply_unicast);
    EXPECT_EQ(config::get_channel(2).pixel_count, 33);
    EXPECT_TRUE(config::get_channel(2).invert_direction);
    post("/api/global", "{\"refresh_hz\":60,\"reply_unicast\":false}");
    post("/api/channel/2", "{\"invert\":false}");
}

TEST(backup_then_restore_round_trips) {
    post("/api/channel/3", "{\"protocol\":\"APA102\",\"pixel_count\":77,\"gaps\":[[5,1]]}");
    post("/api/global", "{\"short_name\":\"before\"}");
    const auto backup = get("/api/backup");
    EXPECT_EQ(backup.status, 200);
    EXPECT_TRUE(backup.headers.at("Content-Disposition").find("attachment") != std::string::npos);
    const std::string snapshot = get("/api/config").body;

    config::reset_to_defaults();
    EXPECT_TRUE(get("/api/config").body != snapshot);
    EXPECT_EQ(post("/api/restore", backup.body).status, 200);
    Json a(snapshot), b(get("/api/config").body);
    EXPECT_TRUE(cJSON_Compare(a["channels"], b["channels"], true));
    EXPECT_TRUE(cJSON_Compare(a["scenes"], b["scenes"], true));
    EXPECT_STREQ(config::get_global().short_name, "before");
    EXPECT_EQ(post("/api/restore", "garbage").status, 400);

    // Every exported global field comes back, and the receivers follow.
    Json exported(backup.body);
    EXPECT_TRUE(cJSON_GetObjectItemCaseSensitive(exported["global"], "fpp_remote") != nullptr);
    std::string flipped = backup.body;
    auto swap           = [&](const char* from, const char* to) {
        const size_t at = flipped.find(from);
        EXPECT_TRUE(at != std::string::npos);
        if (at != std::string::npos) flipped.replace(at, std::strlen(from), to);
    };
    swap("\"fpp_remote\":false", "\"fpp_remote\":true");
    swap("\"lang\":0", "\"lang\":1");
    const int fpp_starts = ::fake::modules().fpp_starts;
    EXPECT_EQ(post("/api/restore", flipped).status, 200);
    EXPECT_TRUE(config::get_global().fpp_remote);
    EXPECT_EQ(config::get_global().language, 1);
    EXPECT_EQ(::fake::modules().fpp_starts, fpp_starts + 1);  // started now, not at reboot
    EXPECT_EQ(post("/api/restore", backup.body).status, 200);
    EXPECT_FALSE(config::get_global().fpp_remote);
    EXPECT_FALSE(::fake::modules().fpp_running);

    // A backup from the DMX512-output era: that channel comes back disabled.
    std::string old = backup.body;
    const size_t at = old.find("\"protocol\":\"APA102\"");
    EXPECT_TRUE(at != std::string::npos);
    old.replace(at, std::strlen("\"protocol\":\"APA102\""), "\"protocol\":\"DMX512\"");
    EXPECT_EQ(post("/api/restore", old).status, 200);
    EXPECT_TRUE(config::get_channel(3).protocol == led::Protocol::Off);
    EXPECT_EQ(post("/api/channel/3", "{\"protocol\":\"DMX512\"}").status, 400);
}

TEST(ota_error_paths_report_and_release_the_lock) {
    // Runs before the successful upload below, which leaves the lock taken.
    std::string img(4096, '\x5A');
    img[0] = static_cast<char>(0xE9);
    shim::fail_next(shim::Fault::OtaNoTarget);
    EXPECT_TRUE(post("/api/ota", img).body.find("no OTA partition") != std::string::npos);
    shim::fail_next(shim::Fault::OtaBegin);
    EXPECT_TRUE(post("/api/ota", img).body.find("esp_ota_begin failed") != std::string::npos);
    shim::fail_next(shim::Fault::OtaWrite);
    EXPECT_TRUE(post("/api/ota", img).body.find("flash write failed") != std::string::npos);
    shim::fail_next(shim::Fault::OtaSetBoot);
    EXPECT_TRUE(post("/api/ota", img).body.find("set boot partition failed") != std::string::npos);
    EXPECT_TRUE(post("/api/ota", std::string(8u << 20, '\xE9')).body.find("image too large") !=
                std::string::npos);  // larger than the 7 MB slot
    shim::faults_clear();
    EXPECT_FALSE(shim::ota_boot_switched());
}

TEST(ota_rejects_a_bad_image_and_boots_a_good_one) {
    const int restarts0 = shim::restarts();
    EXPECT_EQ(post("/api/ota", "").status, 400);
    auto bad = post("/api/ota", std::string(4096, '\x00'));  // no 0xE9 magic
    EXPECT_EQ(bad.status, 400);
    EXPECT_FALSE(shim::ota_boot_switched());
    // An earlier rollback, acknowledged, of the very build about to be sent.
    config::RollbackRecord rb{};
    std::strcpy(rb.rejected_version, "v1.2.3");
    std::memset(rb.rejected_sha, 0xAB, sizeof(rb.rejected_sha));
    rb.acknowledged = 1;
    config::set_rollback(rb);
    std::string img(10000, '\x5A');
    img[0] = static_cast<char>(0xE9);
    shim::http_recv_chunk(1000);  // arrives in pieces, like TCP
    auto good = post("/api/ota", img);
    EXPECT_EQ(good.status, 200);
    // The upload overwrote the rejected slot: the record no longer names that
    // image, so a rollback of this upload (same SHA) counts as a new one.
    config::RollbackRecord after{};
    EXPECT_TRUE(config::get_rollback(after));
    EXPECT_EQ(after.rejected_sha[0], 0);
    EXPECT_STREQ(after.rejected_version, "v1.2.3");
    EXPECT_TRUE(shim::ota_boot_switched());
    EXPECT_EQ(shim::ota_image().size(), img.size());
    EXPECT_EQ(shim::restarts(), restarts0 + 1);
    // Last: a successful upload restarts the device, so the handler never
    // clears its in-progress flag (esp_restart does not return on hardware).
}

TEST(fseq_endpoints_drive_the_player) {
    Json files(get("/api/fseq/files").body);
    EXPECT_EQ(cJSON_GetArraySize(files["files"]), 2);
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":\"show.fseq\"}").status, 200);
    EXPECT_STREQ(fake::modules().fseq_started.c_str(), "show.fseq");
    EXPECT_EQ(post("/api/fseq/stop").status, 200);
    EXPECT_TRUE(fake::modules().fseq_stopped);
}

TEST(logs_capture_and_level) {
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();  // logs "remap applied"
    const auto logs = get("/api/logs");
    EXPECT_EQ(logs.status, 200);
    EXPECT_TRUE(logs.body.find("remap applied") != std::string::npos);
    EXPECT_EQ(post("/api/loglevel", "{\"level\":\"warn\"}").status, 200);
    EXPECT_EQ(shim::log_level(), 2);
    EXPECT_EQ(post("/api/loglevel", "{\"level\":\"loud\"}").status, 400);
    post("/api/loglevel", "{\"level\":\"info\"}");
}

TEST(coredump_download_and_erase) {
    EXPECT_EQ(get("/api/coredump").status, 404);
    shim::coredump_set(std::vector<uint8_t>(300, 0xCD));
    const auto r = get("/api/coredump");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.size(), 300);
    EXPECT_EQ(shim::http_request("DELETE", "/api/coredump").status, 200);
    EXPECT_EQ(get("/api/coredump").status, 404);
}

TEST(peers_lists_self_and_pixfrog_siblings_only) {
    shim::mdns_add_peer("rig-b", 0xC0A80233, "pixfrog", "rig-b", "v1");
    shim::mdns_add_peer("printer", 0xC0A80240, "other", nullptr, nullptr);
    shim::mdns_add_peer("rig-b-again", 0xC0A80233, "pixfrog", "dup", "v1");
    Json peers(get("/api/peers").body);
    EXPECT_EQ(cJSON_GetArraySize(peers.j), 2);  // self + rig-b
    const cJSON* self = cJSON_GetArrayItem(peers.j, 0);
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(self, "self")));
    const cJSON* sib = cJSON_GetArrayItem(peers.j, 1);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(sib, "ip")->valuestring, "192.168.2.51");
}

TEST(reboot_factory_reset_autopatch) {
    const int r0 = shim::restarts();
    EXPECT_EQ(post("/api/reboot").status, 200);
    EXPECT_EQ(shim::restarts(), r0 + 1);
    EXPECT_EQ(post("/api/autopatch", "{\"base\":0}").status, 200);
    post("/api/channel/0", "{\"pixel_count\":999}");
    const int r1  = shim::restarts();
    const auto fr = post("/api/factory-reset");
    EXPECT_EQ(fr.status, 200);
    EXPECT_TRUE(fr.body.find("\"rebooting\":true") != std::string::npos);
    EXPECT_EQ(shim::restarts(), r1 + 1);  // as the SPA announces: services follow the defaults
    EXPECT_TRUE(config::get_channel(0).pixel_count != 999);
}

// The server cannot stop itself inside a handler: it answers, then a
// short-lived task stops it. It used to keep serving until a reboot.
TEST(turning_the_web_ui_off_from_it_stops_the_server) {
    EXPECT_TRUE(shim::http_running());
    const auto on = post("/api/global", "{\"web_enabled\":true}");
    EXPECT_TRUE(on.body.find("web_stopping") == std::string::npos);
    EXPECT_FALSE(shim::task_created("web_stop"));
    const auto off = post("/api/global", "{\"web_enabled\":false}");
    EXPECT_EQ(off.status, 200);
    EXPECT_TRUE(off.body.find("\"web_stopping\":true") != std::string::npos);
    EXPECT_TRUE(shim::http_running());  // the answer went out first
    EXPECT_TRUE(shim::run_task("web_stop"));
    EXPECT_FALSE(shim::http_running());
    EXPECT_FALSE(config::get_global().web_enabled);
    web::start();  // the next cases need it
    EXPECT_TRUE(shim::http_running());
}

// The SPA's live status: once a WebSocket client is on /api/ws, the push task
// sends the /api/status JSON (typed "status") to it every second.
TEST(status_is_pushed_to_websocket_clients) {
    EXPECT_EQ(shim::ws_open("/api/nope"), -1);
    const int fd = shim::ws_open("/api/ws");
    EXPECT_TRUE(fd >= 0);
    EXPECT_TRUE(shim::ws_frames(fd).empty());
    shim::run_task_for("web_push", 11, [] {});  // ten 200 ms ticks
    std::vector<std::string> status, preview;
    for (const auto& f : shim::ws_frames(fd))
        (f[0] == '{' ? status : preview).push_back(f);
    EXPECT_EQ(status.size(), 2u);    // 1 Hz
    EXPECT_EQ(preview.size(), 10u);  // 5 Hz
    if (!status.empty()) {
        Json j(status.back());
        EXPECT_STREQ(j["type"]->valuestring, "status");
        EXPECT_TRUE(cJSON_IsNumber(j["fps"]));
        EXPECT_TRUE(cJSON_IsArray(j["channels"]));
        EXPECT_TRUE(cJSON_IsObject(j["fseq"]));
    }
    if (!preview.empty()) {
        // 'P', the output count, then per output n + n RGB triplets.
        const std::string& p = preview.back();
        EXPECT_EQ(p[0], 'P');
        EXPECT_EQ(static_cast<size_t>(p[1]), config::kNumChannels);
        size_t len = 2;
        for (size_t ch = 0; ch < config::kNumChannels && len < p.size(); ++ch)
            len += 1 + static_cast<uint8_t>(p[len]) * 3;
        EXPECT_EQ(len, p.size());
    }
    shim::ws_close(fd);
}

TEST(unknown_route_is_404) {
    EXPECT_FALSE(get("/api/nope").handled);
    EXPECT_EQ(get("/api/nope").status, 404);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}

// ── Show control, DMX control universe, zones ───────────────────────────────

TEST(show_endpoint_sets_master_blackout_strobe) {
    Json r(post("/api/show", "{\"master\":50}").body);
    EXPECT_EQ(dmx::master_local(0), 32768);
    EXPECT_TRUE(cJSON_GetArrayItem(r["master"], 0)->valuedouble == 50.0);
    EXPECT_STREQ(r["control"]->valuestring, "off");
    EXPECT_EQ(post("/api/show", "{\"outputs\":3,\"blackout\":true}").status, 200);
    EXPECT_EQ(dmx::blackout_local(), 0x03);
    EXPECT_EQ(post("/api/show", "{\"blackout\":\"toggle\"}").status, 200);
    EXPECT_EQ(dmx::blackout_local(), 0xFF);
    EXPECT_EQ(post("/api/show", "{\"blackout\":false,\"strobe_hz\":12,\"master\":100}").status,
              200);
    EXPECT_EQ(dmx::blackout_local(), 0);
    EXPECT_EQ(dmx::strobe_local(5), 120);
    EXPECT_EQ(post("/api/show", "{\"master\":101}").status, 400);
    EXPECT_EQ(post("/api/show", "{\"outputs\":0,\"master\":10}").status, 400);
    EXPECT_EQ(post("/api/show", "{\"blackout\":\"maybe\"}").status, 400);
    EXPECT_EQ(post("/api/show", "{\"strobe_hz\":-1}").status, 400);
    EXPECT_EQ(dmx::master_local(0), dmx::kMasterFull);  // the refused ones changed nothing
    Json st(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsObject(st["show"]));
    EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(st["show"], "scenes")), 8);
    post("/api/show", "{\"strobe_hz\":0}");
}

TEST(control_endpoint_validates_and_applies) {
    Json full(post("/api/control", "{\"preset\":\"full\",\"enabled\":true,\"universe\":77}").body);
    EXPECT_EQ(static_cast<int>(full["footprint"]->valuedouble), 16);
    EXPECT_EQ(dmx::control_universe(), 77);
    const std::string zones =
        "{\"address\":10,\"slots\":[{\"fn\":\"scene\",\"mask\":15},{\"fn\":\"scene\",\"mask\":240},"
        "{\"fn\":\"master\",\"fine\":true},{\"fn\":\"green\",\"index\":3},{\"fn\":\"none\"}]}";
    EXPECT_EQ(post("/api/control", zones).status, 200);
    const auto& c = config::get_control();
    EXPECT_EQ(c.count, 5);
    EXPECT_EQ(c.address, 10);
    EXPECT_EQ(c.slots[1].mask, 240);
    EXPECT_EQ(c.slots[2].flags, config::kCtlFlagFine);
    EXPECT_EQ(c.slots[3].index, 3);
    EXPECT_EQ(post("/api/control", "{\"slots\":[{\"fn\":\"laser\"}]}").status, 400);
    EXPECT_EQ(post("/api/control", "{\"slots\":[{\"fn\":\"master\",\"mask\":0}]}").status, 400);
    EXPECT_EQ(post("/api/control", "{\"address\":512,\"preset\":\"full\"}").status, 400);
    EXPECT_EQ(post("/api/control", "{\"universe\":40000}").status, 400);
    EXPECT_EQ(post("/api/control", "{\"preset\":\"huge\"}").status, 400);
    EXPECT_EQ(config::get_control().count, 5);  // refused bodies changed nothing
    Json cfg(get("/api/config").body);
    EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cfg["control"], "slots")), 5);
    post("/api/control", "{\"preset\":\"simple\",\"enabled\":false,\"address\":1}");
}

// The profile a desk imports: one mode, the slots in order, every channel's
// capabilities covering 0-255 without a gap (what OFL/QLC+ require).
TEST(fixture_profile_matches_the_control_mode) {
    post("/api/control", "{\"preset\":\"full\"}");
    post("/api/control",
         "{\"slots\":[{\"fn\":\"master\",\"fine\":true},{\"fn\":\"scene\",\"mask\":15},"
         "{\"fn\":\"scene\",\"mask\":240},{\"fn\":\"none\"},{\"fn\":\"effect\"},{\"fn\":\"red\"},"
         "{\"fn\":\"blackout\"},{\"fn\":\"strobe\"},{\"fn\":\"fseq\"}]}");
    const auto res = get("/api/control/fixture");
    EXPECT_EQ(res.status, 200);
    EXPECT_TRUE(res.headers.at("Content-Disposition").find("pixfrog-control.json") !=
                std::string::npos);
    Json f(res.body);
    EXPECT_TRUE(f.j != nullptr);
    const cJSON* mode  = cJSON_GetArrayItem(f["modes"], 0);
    const cJSON* chans = cJSON_GetObjectItemCaseSensitive(mode, "channels");
    EXPECT_EQ(cJSON_GetArraySize(chans), 10);  // 9 slots, the master is 16-bit
    EXPECT_STREQ(cJSON_GetArrayItem(chans, 0)->valuestring, "Master");
    EXPECT_STREQ(cJSON_GetArrayItem(chans, 1)->valuestring, "Master fine");
    EXPECT_STREQ(cJSON_GetArrayItem(chans, 2)->valuestring, "Scene (out 1-4)");
    EXPECT_STREQ(cJSON_GetArrayItem(chans, 3)->valuestring, "Scene (out 5-8)");
    EXPECT_TRUE(cJSON_IsNull(cJSON_GetArrayItem(chans, 4)));  // the spare channel
    const cJSON* avail = f["availableChannels"];
    int checked        = 0;
    for (const cJSON* ch = avail->child; ch; ch = ch->next) {
        const cJSON* caps = cJSON_GetObjectItemCaseSensitive(ch, "capabilities");
        if (!caps) {
            EXPECT_TRUE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(ch, "capability")));
            continue;
        }
        int next = 0;
        for (const cJSON* cap = caps->child; cap; cap = cap->next) {
            const cJSON* r = cJSON_GetObjectItemCaseSensitive(cap, "dmxRange");
            EXPECT_EQ(static_cast<int>(cJSON_GetArrayItem(r, 0)->valuedouble), next);
            next = static_cast<int>(cJSON_GetArrayItem(r, 1)->valuedouble) + 1;
        }
        EXPECT_EQ(next, 256);
        ++checked;
    }
    EXPECT_EQ(checked, 6);  // two scenes, effect, blackout, strobe, fseq
    const cJSON* scene = cJSON_GetObjectItemCaseSensitive(avail, "Scene (out 1-4)");
    EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(scene, "capabilities")),
              static_cast<int>(config::num_scenes()) + 2);
    post("/api/control", "{\"preset\":\"simple\"}");
}

// The remaining functions: overrides (speed/param/colour), fade, a coarse
// master — ranged channels still gapless, the others a single capability.
TEST(fixture_profile_covers_every_control_function) {
    post("/api/control", "{\"preset\":\"full\"}");
    EXPECT_EQ(post("/api/control",
                   "{\"slots\":[{\"fn\":\"master\"},{\"fn\":\"speed\"},{\"fn\":\"param\"},"
                   "{\"fn\":\"red\",\"index\":1},{\"fn\":\"green\"},{\"fn\":\"blue\"},"
                   "{\"fn\":\"fade\"},{\"fn\":\"scene\"}]}")
                  .status,
              200);
    EXPECT_EQ(post("/api/control", "{\"address\":0}").status, 400);
    EXPECT_EQ(post("/api/control", "{\"slots\":[{\"fn\":\"red\",\"index\":9}]}").status, 400);
    Json f(get("/api/control/fixture").body);
    const cJSON* avail = f["availableChannels"];
    int gapless = 0, single = 0;
    for (const cJSON* ch = avail->child; ch; ch = ch->next) {
        const cJSON* caps = cJSON_GetObjectItemCaseSensitive(ch, "capabilities");
        if (!caps) {
            ++single;
            continue;
        }
        int next = 0;
        for (const cJSON* cap = caps->child; cap; cap = cap->next) {
            const cJSON* r = cJSON_GetObjectItemCaseSensitive(cap, "dmxRange");
            EXPECT_EQ(static_cast<int>(cJSON_GetArrayItem(r, 0)->valuedouble), next);
            next = static_cast<int>(cJSON_GetArrayItem(r, 1)->valuedouble) + 1;
        }
        EXPECT_EQ(next, 256);
        ++gapless;
    }
    EXPECT_EQ(single, 5);   // master, red, green, blue (intensities), fade (a time)
    EXPECT_EQ(gapless, 3);  // speed, param, scene
    const cJSON* fade = cJSON_GetObjectItemCaseSensitive(avail, "Fade time");
    EXPECT_TRUE(fade != nullptr);
    post("/api/control", "{\"preset\":\"simple\"}");
}

TEST(backup_restore_carries_the_control_mode_and_fade) {
    post("/api/control", "{\"preset\":\"full\",\"enabled\":true,\"universe\":55,\"address\":7}");
    EXPECT_EQ(config::get_global().speaker_volume, 0);  // the default: speaker off
    post("/api/global", "{\"scene_fade_ms\":2500,\"hub_preferred\":true,\"fseq_universe\":9,"
                        "\"speaker_volume\":60}");
    EXPECT_EQ(post("/api/global", "{\"fseq_universe\":0,\"speaker_volume\":101}").status,
              200);  // out of range: kept
    EXPECT_EQ(config::get_global().fseq_universe, 9);
    EXPECT_EQ(config::get_global().speaker_volume, 60);
    EXPECT_EQ(config::get_global().fseq_universe, 9);
    const std::string backup = get("/api/backup").body;
    config::reset_to_defaults();
    EXPECT_EQ(config::get_control().universe, config::kDefaultControlUniverse);
    EXPECT_EQ(post("/api/restore", backup).status, 200);
    EXPECT_EQ(config::get_control().universe, 55);
    EXPECT_EQ(config::get_control().address, 7);
    EXPECT_EQ(config::get_control().count, 15);
    EXPECT_EQ(config::get_global().scene_fade_ms, 2500);
    EXPECT_EQ(config::get_global().hub_preferred, 1);
    EXPECT_EQ(config::get_global().fseq_universe, 9);
    EXPECT_EQ(config::get_global().speaker_volume, 60);
    // A malformed control object is skipped, the rest still restores.
    std::string bad       = backup;
    const std::string key = "\"fn\":\"master\"";
    bad.replace(bad.find(key), key.size(), "\"fn\":\"laser\"");
    config::reset_to_defaults();
    EXPECT_EQ(post("/api/restore", bad).status, 200);
    EXPECT_EQ(config::get_control().universe, config::kDefaultControlUniverse);
    EXPECT_EQ(config::get_global().scene_fade_ms, 2500);
    post("/api/global", "{\"scene_fade_ms\":0,\"hub_preferred\":false,\"fseq_universe\":1,"
                        "\"speaker_volume\":0}");
    post("/api/control", "{\"enabled\":false}");
}

TEST(scene_play_takes_an_output_zone_and_stop_is_per_scene) {
    EXPECT_EQ(post("/api/scene/1/play", "{\"outputs\":15}").status, 200);
    EXPECT_EQ(dmx::scene_outputs(1), 0x0F & config::get_scene(1).channel_mask);
    EXPECT_EQ(post("/api/scene/2/play", "").status, 200);  // no body: its own mask
    EXPECT_EQ(post("/api/scene/1/play", "{\"outputs\":0}").status, 400);
    EXPECT_EQ(post("/api/scene/2/stop", "").status, 200);
    EXPECT_EQ(dmx::scene_outputs(2), 0);
    post("/api/scenes/stop");
    EXPECT_EQ(dmx::active_scene(), -1);
}

// ── Coverage: logs, diag, every global/channel field, OTA and upload errors ──

TEST(log_ring_demotes_sd_spam_strips_colours_and_wraps) {
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGE("sdmmc_cmd", "sdmmc_card_init failed");  // below debug: dropped
    ESP_LOGW("x", "\x1b[0;33mcoloured\x1b[0m line");
    auto logs = get("/api/logs").body;
    EXPECT_TRUE(logs.find("sdmmc_card_init") == std::string::npos);
    EXPECT_TRUE(logs.find("coloured line") != std::string::npos);
    EXPECT_TRUE(logs.find('\x1b') == std::string::npos);
    esp_log_level_set("*", ESP_LOG_DEBUG);
    ESP_LOGE("sdmmc_cmd", "sdmmc retry");  // at debug: kept, relabelled
    logs = get("/api/logs").body;
    EXPECT_TRUE(logs.find("sdmmc retry") != std::string::npos);
    for (int i = 0; i < 400; ++i)  // more than the ring holds
        ESP_LOGI("fill", "line %03d of a long burst of log output to wrap the ring", i);
    logs = get("/api/logs").body;
    EXPECT_TRUE(logs.find("line 399") != std::string::npos);
    EXPECT_TRUE(logs.find("line 000") == std::string::npos);  // overwritten
    esp_log_level_set("*", ESP_LOG_INFO);
}

TEST(loglevel_accepts_every_level) {
    for (const char* l : { "none", "error", "warn", "info", "debug", "verbose" }) {
        EXPECT_EQ(post("/api/loglevel", std::string("{\"level\":\"") + l + "\"}").status, 200);
    }
    EXPECT_EQ(shim::log_level(), ESP_LOG_VERBOSE);
    post("/api/loglevel", "{\"level\":\"info\"}");
}

// A frozen UI task (screen + knob) shows over HTTP, no UART needed.
TEST(diag_reports_the_ui_task_health) {
    fake::modules().ui_loop_age_ms = 42;
    fake::modules().display_stalls = 3;
    Json d(get("/api/diag").body);
    const cJSON* u = d["ui"];
    EXPECT_EQ(cJSON_GetObjectItem(u, "loop_age_ms")->valueint, 42);
    EXPECT_EQ(cJSON_GetObjectItem(u, "display_stalls")->valueint, 3);
}

TEST(diag_names_every_reset_reason) {
    const int reasons[]    = { ESP_RST_EXT,      ESP_RST_SW,     ESP_RST_PANIC,     ESP_RST_INT_WDT,
                               ESP_RST_TASK_WDT, ESP_RST_WDT,    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT,
                               ESP_RST_SDIO,     ESP_RST_UNKNOWN };
    const char* expected[] = { "external", "software",   "panic",    "int-wdt", "task-wdt",
                               "wdt",      "deep-sleep", "brownout", "sdio" };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        shim::set_reset_reason(reasons[i]);
        const std::string body = get("/api/diag").body;
        if (i < sizeof(expected) / sizeof(expected[0]))
            EXPECT_TRUE(body.find(std::string("\"") + expected[i] + "\"") != std::string::npos);
    }
    shim::set_reset_reason(ESP_RST_POWERON);
}

TEST(global_post_sets_network_display_failsafe_and_services) {
    const auto before = config::get_global();
    Json r(post("/api/global",
                "{\"dhcp\":0,\"ip\":\"10.1.2.3\",\"mask\":\"255.255.0.0\",\"gw\":\"10.1.0.1\","
                "\"long_name\":\"A long name\",\"tft_brightness\":55,\"tft_dim_delay_s\":80,"
                "\"failsafe_scene\":1,\"failsafe_color\":\"#102030\",\"sacn_enabled\":1,"
                "\"fpp_remote\":true}")
               .body);
    EXPECT_STREQ(r["note"]->valuestring, "network_changes_apply_after_reboot");
    const auto& g = config::get_global();
    EXPECT_FALSE(g.use_dhcp);
    EXPECT_EQ(g.static_ip, 0x0A010203u);
    EXPECT_EQ(g.static_mask, 0xFFFF0000u);
    EXPECT_EQ(g.static_gateway, 0x0A010001u);
    EXPECT_STREQ(g.long_name, "A long name");
    EXPECT_EQ(g.tft_brightness, 55);
    EXPECT_EQ(g.tft_dim_delay_s, 80);
    EXPECT_EQ(g.failsafe_scene, 1);
    EXPECT_EQ(g.failsafe_g, 0x20);
    EXPECT_TRUE(sacn::is_running());
    EXPECT_TRUE(::fake::modules().fpp_running);
    post("/api/global", "{\"sacn_enabled\":false,\"fpp_remote\":false}");
    shim::run_task("sacn_rx");  // the stopped task runs to its end
    EXPECT_FALSE(sacn::is_running());
    EXPECT_FALSE(::fake::modules().fpp_running);
    EXPECT_EQ(post("/api/global", "{\"ip\":\"1.2.3\"}").status, 400);
    EXPECT_EQ(post("/api/global", "{\"mask\":\"x\"}").status, 400);
    EXPECT_EQ(post("/api/global", "{\"gw\":\"300.1.1.1\"}").status, 400);
    config::set_global(before);
}

TEST(channel_post_colour_order_and_numeric_bools) {
    EXPECT_EQ(post("/api/channel/4", "{\"color_order\":\"BGR\",\"invert\":1}").status, 200);
    EXPECT_TRUE(config::get_channel(4).color_order == led::ColorOrder::BGR);
    EXPECT_TRUE(config::get_channel(4).invert_direction);
    EXPECT_EQ(post("/api/channel/4", "{\"color_order\":\"XYZ\"}").status, 400);
    post("/api/channel/4", "{\"invert\":false}");
}

TEST(restore_accepts_a_single_legacy_scene_colour) {
    const std::string backup = get("/api/backup").body;
    std::string one = "{\"scenes\":[{\"name\":\"Old\",\"effect\":0,\"color\":\"#0a0b0c\"}]}";
    EXPECT_EQ(post("/api/restore", one).status, 200);
    EXPECT_EQ(config::get_scene(0).r, 0x0A);
    EXPECT_EQ(config::get_scene(0).b, 0x0C);
    post("/api/restore", backup);
}

TEST(fseq_upload_rejects_before_touching_the_card) {
    fseq::fake::set(fseq::Status::Idle, 0);
    ::fake::modules().sd_mounted = false;
    EXPECT_EQ(post("/api/fseq/upload?name=show.fseq", "data").status, 500);  // no SD card
    ::fake::modules().sd_mounted = true;
    EXPECT_EQ(post("/api/fseq/upload", "data").status, 400);                   // no name
    EXPECT_EQ(post("/api/fseq/upload?name=..%2Fx.fseq", "data").status, 400);  // traversal
    EXPECT_EQ(post("/api/fseq/upload?name=show.txt", "data").status, 400);
    EXPECT_EQ(post("/api/fseq/upload?name=show+2.fseq", "").status, 400);  // empty body
    EXPECT_EQ(post("/api/fseq/play", "{}").status, 400);                   // missing filename
}

// play takes the same names upload writes: nothing that leaves the mount root,
// nothing truncated into another file's name; a fragmented body still parses.
TEST(ip_fallback_is_set_shown_and_restored) {
    EXPECT_EQ(post("/api/global", "{\"ip_fallback\":\"artnet\"}").status, 200);
    EXPECT_EQ(config::get_global().ip_fallback, config::kIpFallbackArtnet);
    Json c(get("/api/config").body);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(c["global"], "ip_fallback")->valuestring,
                 "artnet");
    EXPECT_EQ(post("/api/global", "{\"ip_fallback\":\"10.x\"}").status, 400);
    const std::string backup = get("/api/backup").body;
    EXPECT_EQ(post("/api/global", "{\"ip_fallback\":\"linklocal\"}").status, 200);
    EXPECT_EQ(post("/api/restore", backup).status, 200);
    EXPECT_EQ(config::get_global().ip_fallback, config::kIpFallbackArtnet);
    post("/api/global", "{\"ip_fallback\":\"linklocal\"}");
}

TEST(fseq_playlist_round_trips_and_validates) {
    EXPECT_EQ(post("/api/fseq/playlist",
                   "{\"loop\":true,\"autostart\":true,\"items\":[{\"name\":\"intro.fseq\","
                   "\"repeat\":2},{\"name\":\"show.fseq\"}]}")
                  .status,
              200);
    const auto& p = config::get_playlist();
    EXPECT_EQ(p.count, 2);
    EXPECT_EQ(p.loop, 1);
    EXPECT_EQ(p.autostart, 1);
    EXPECT_STREQ(p.items[0].name, "intro.fseq");
    EXPECT_EQ(p.items[0].repeat, 2);
    EXPECT_EQ(p.items[1].repeat, 1);  // default
    Json g(get("/api/fseq/playlist").body);
    EXPECT_EQ(cJSON_GetArraySize(g["items"]), 2);
    EXPECT_TRUE(cJSON_IsTrue(g["loop"]));
    // Partial update: the list stays.
    EXPECT_EQ(post("/api/fseq/playlist", "{\"autostart\":false}").status, 200);
    EXPECT_EQ(config::get_playlist().count, 2);
    EXPECT_EQ(config::get_playlist().autostart, 0);
    // Refused bodies change nothing.
    for (const char* bad : { "{\"loop\":1}", "{\"autostart\":\"yes\"}", "{\"items\":{}}",
                             "{\"items\":[{\"name\":\"../x.fseq\"}]}", "{\"items\":[{\"name\":7}]}",
                             "{\"items\":[{\"name\":\"a.fseq\",\"repeat\":0}]}", "[1]", "nope" })
        EXPECT_EQ(post("/api/fseq/playlist", bad).status, 400);
    std::string many = "{\"items\":[";
    for (int i = 0; i < 17; ++i)
        many += std::string(i ? "," : "") + "{\"name\":\"f" + std::to_string(i) + ".fseq\"}";
    EXPECT_EQ(post("/api/fseq/playlist", many + "]}").status, 400);
    EXPECT_EQ(config::get_playlist().count, 2);
    // Play it, or a file in a loop.
    const int starts = ::fake::modules().playlist_starts;
    EXPECT_EQ(post("/api/fseq/play", "{\"playlist\":true}").status, 200);
    EXPECT_EQ(::fake::modules().playlist_starts, starts + 1);
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":\"show.fseq\",\"loop\":true}").status, 200);
    EXPECT_TRUE(::fake::modules().fseq_loop);
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":\"show.fseq\",\"loop\":\"y\"}").status, 400);
    Json st(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st["fseq"], "loop")));
    // In the backup, and restored all or nothing.
    const std::string backup = get("/api/backup").body;
    EXPECT_TRUE(backup.find("\"playlist\"") != std::string::npos);
    config::set_playlist(config::FseqPlaylist{});
    EXPECT_EQ(post("/api/restore", backup).status, 200);
    EXPECT_EQ(config::get_playlist().count, 2);
    std::string bad = backup;
    bad.replace(bad.find("intro.fseq"), 10, "../x.fseq!");
    config::set_playlist(config::FseqPlaylist{});
    EXPECT_EQ(post("/api/restore", bad).status, 200);
    EXPECT_EQ(config::get_playlist().count, 0);
    EXPECT_EQ(post("/api/fseq/play", "{\"playlist\":true}").status, 409);  // empty
    ::fake::modules().fseq_loop = false;
}

TEST(fseq_play_refuses_names_outside_the_card_root) {
    for (const char* bad :
         { "../etc/x.fseq", "dir/x.fseq", "..\\\\x.fseq", ".hidden.fseq", "show.txt", "" }) {
        const std::string body = std::string("{\"filename\":\"") + bad + "\"}";
        EXPECT_EQ(post("/api/fseq/play", body).status, 400);
    }
    const std::string longname(fseq::kMaxNameLen + 4, 'a');
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":\"" + longname + ".fseq\"}").status, 400);
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":42}").status, 400);
    EXPECT_EQ(post("/api/fseq/play", "not json").status, 400);
    shim::http_recv_chunk(5);  // the body arrives in 5-byte pieces
    EXPECT_EQ(post("/api/fseq/play", "{\"filename\":\"show.fseq\"}").status, 200);
    shim::http_recv_chunk(0);
    EXPECT_TRUE(::fake::modules().fseq_started == "show.fseq");
}

TEST(peers_fall_back_to_the_instance_name) {
    shim::mdns_reset();
    shim::advance_ms(6000);  // past the 5 s peers cache
    shim::mdns_add_peer("anon-box", 0xC0A80244, "pixfrog", nullptr, nullptr);
    shim::mdns_add_peer("", 0xC0A80245, "pixfrog", nullptr, nullptr);
    const std::string body = get("/api/peers").body;
    EXPECT_TRUE(body.find("\"anon-box\"") != std::string::npos);
    EXPECT_TRUE(body.find("\"name\":\"pixfrog\"") != std::string::npos);
    shim::mdns_reset();
}

TEST(server_restart_and_start_failures) {
    web::stop();
    EXPECT_FALSE(web::is_running());
    web::stop();  // twice is harmless
    shim::fail_next(shim::Fault::HttpdStart);
    web::start();
    EXPECT_FALSE(web::is_running());
    shim::fail_next(shim::Fault::MdnsInit);
    web::start();  // up without mDNS
    EXPECT_TRUE(web::is_running());
    web::stop();
    web::start();
    EXPECT_TRUE(web::is_running());
}

TEST(fseq_upload_writes_the_file_and_handles_card_errors) {
    char tmpl[]           = "/tmp/pixfrog-web-sd-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    shim::sd_root(dir);
    ::fake::modules().sd_mounted = true;
    const std::string body(3000, 'Q');  // several receive chunks
    EXPECT_EQ(post("/api/fseq/upload?name=show%201.fseq", body).status, 200);
    FILE* f = fopen((dir + "/show 1.fseq").c_str(), "rb");
    EXPECT_TRUE(f != nullptr);
    if (f) {
        std::fseek(f, 0, SEEK_END);
        EXPECT_EQ(std::ftell(f), 3000);
        std::fclose(f);
    }
    // Re-uploading the file being played stops it first, then replaces it.
    ::fake::modules().fseq_started = "show 1.fseq";
    EXPECT_EQ(post("/api/fseq/upload?name=show+1.fseq", "new").status, 200);
    EXPECT_TRUE(::fake::modules().fseq_stopped);
    // A directory where the file should go: the rename fails, nothing is left.
    mkdir((dir + "/dir.fseq").c_str(), 0700);
    mkdir((dir + "/dir.fseq/x").c_str(), 0700);
    EXPECT_EQ(post("/api/fseq/upload?name=dir.fseq", "abc").status, 500);
    // The card's directory gone: the temporary file cannot be created.
    shim::sd_root(dir + "/missing");
    EXPECT_EQ(post("/api/fseq/upload?name=x.fseq", "abc").status, 500);
    shim::sd_root("");
}

// ── pixfrog.local with several boxes (hub_election.h + web_hub task) ───────

namespace {
void tick() {
    shim::advance_ms(1000);
}
// The hub task for `seconds` of fake time (one loop per second); each run
// starts the task from the top, so it browses at once.
void run_hub(int seconds) {
    shim::run_task_for("web_hub", seconds + 1, tick, true);
}
// A clean roster: every sibling a previous case left behind has expired.
void hub_fresh() {
    shim::mdns_reset();
    shim::advance_ms(60'000);
    run_hub(1);
}
constexpr uint32_t kSelfIp = 0xC0A80232;
}  // namespace

TEST(hub_election_prefers_the_marked_box_then_the_lowest_mac) {
    using web::hub::Candidate;
    const Candidate low{ { 0, 0, 0, 0, 0, 1 }, false }, high{ { 0, 0, 0, 0, 0, 9 }, false },
        high_pref{ { 0, 0, 0, 0, 0, 9 }, true };
    EXPECT_TRUE(web::hub::outranks(low, high));
    EXPECT_FALSE(web::hub::outranks(high, low));
    EXPECT_TRUE(web::hub::outranks(high_pref, low));
    EXPECT_FALSE(web::hub::outranks(low, low));  // a tie is no win: one holder

    uint8_t mac[6];
    EXPECT_TRUE(web::hub::parse_mac("30EDa0123456", mac));
    EXPECT_EQ(mac[0], 0x30);
    EXPECT_EQ(mac[5], 0x56);
    EXPECT_FALSE(web::hub::parse_mac("30eda012345", mac));
    EXPECT_FALSE(web::hub::parse_mac("30eda012345g", mac));
    EXPECT_FALSE(web::hub::parse_mac(nullptr, mac));
    char host[web::hub::kHostnameMax];
    web::hub::hostname_for(mac, host, sizeof(host));
    EXPECT_STREQ(host, "pixfrog-3456");
}

TEST(roster_keeps_a_sibling_through_a_missed_browse_then_forgets_it) {
    web::hub::Roster r;
    const web::hub::Candidate self{ { 0, 0, 0, 0, 0, 5 }, false },
        sib{ { 0, 0, 0, 0, 0, 1 }, false };
    EXPECT_TRUE(r.holds_alias(self, 0));
    r.saw(sib, 1000);
    EXPECT_FALSE(r.holds_alias(self, 1000));
    EXPECT_FALSE(r.holds_alias(self, 1000 + web::hub::kPeerTtlMs));  // still within the TTL
    EXPECT_EQ(r.live(1000 + web::hub::kPeerTtlMs), 1u);
    EXPECT_TRUE(r.holds_alias(self, 1001 + web::hub::kPeerTtlMs));  // gone silent
    EXPECT_EQ(r.live(1001 + web::hub::kPeerTtlMs), 0u);
    r.saw(self, 2000);  // our own answer never outranks us
    EXPECT_TRUE(r.holds_alias(self, 2000));
    // A full roster replaces its stalest entry.
    for (uint8_t i = 0; i < web::hub::kMaxPeers + 2; ++i)
        r.saw({ { 1, 0, 0, 0, 0, i }, false }, 3000 + i);
    EXPECT_EQ(r.live(3000 + web::hub::kMaxPeers + 2), web::hub::kMaxPeers);
}

TEST(a_lone_box_has_its_own_name_and_answers_pixfrog_local) {
    hub_fresh();
    EXPECT_TRUE(shim::task_created("web_hub"));
    EXPECT_TRUE(shim::mdns_hostname() == "pixfrog-3456");
    EXPECT_TRUE(shim::mdns_txt("mac") == "30eda0123456");
    EXPECT_TRUE(shim::mdns_txt("hub") == "0");
    EXPECT_TRUE(shim::mdns_txt("product") == "pixfrog");
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), kSelfIp);
    Json s(get("/api/status").body);
    EXPECT_STREQ(s["host"]->valuestring, "pixfrog-3456");
    EXPECT_TRUE(cJSON_IsTrue(s["alias"]));
    EXPECT_EQ(s["siblings"]->valueint, 0);
}

TEST(a_lower_mac_sibling_takes_pixfrog_local_until_it_goes_silent) {
    hub_fresh();
    shim::mdns_add_peer("rig-a", 0xC0A80233, "pixfrog", "rig-a", "v1", "30eda0000001", "0");
    shim::mdns_add_peer("old-box", 0xC0A80234, "pixfrog", "old", "v0");  // no MAC: no vote
    run_hub(16);                                                         // the next browse
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), 0u);
    Json s(get("/api/status").body);
    EXPECT_FALSE(cJSON_IsTrue(s["alias"]));
    EXPECT_EQ(s["siblings"]->valueint, 1);

    shim::advance_ms(6000);  // past the peers cache
    Json peers(get("/api/peers").body);
    EXPECT_EQ(cJSON_GetArraySize(peers.j), 3);
    const cJSON* me  = cJSON_GetArrayItem(peers.j, 0);
    const cJSON* sib = cJSON_GetArrayItem(peers.j, 1);
    const cJSON* old = cJSON_GetArrayItem(peers.j, 2);
    EXPECT_STREQ(cJSON_GetObjectItem(me, "host")->valuestring, "pixfrog-3456");
    EXPECT_STREQ(cJSON_GetObjectItem(sib, "host")->valuestring, "pixfrog-0001");
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sib, "alias")));
    EXPECT_TRUE(cJSON_GetObjectItem(me, "alias") == nullptr);
    EXPECT_TRUE(cJSON_GetObjectItem(old, "host") == nullptr);

    shim::mdns_reset();  // the holder dies (last seen at most 22 s ago)
    run_hub(10);
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), 0u);  // a missed browse is not enough
    run_hub(50);
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), kSelfIp);
}

TEST(the_preferred_box_holds_pixfrog_local_whatever_its_mac) {
    hub_fresh();
    shim::mdns_add_peer("rig-a", 0xC0A80233, "pixfrog", "rig-a", "v1", "30eda0000001", "0");
    run_hub(16);
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), 0u);
    EXPECT_TRUE(post("/api/global", "{\"hub_preferred\":true}").status == 200);
    Json c(get("/api/config").body);
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(c["global"], "hub_preferred")));
    run_hub(2);
    EXPECT_TRUE(shim::mdns_txt("hub") == "1");
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), kSelfIp);
    // Two preferred boxes: back to the lowest MAC among them.
    shim::mdns_reset();
    shim::mdns_add_peer("rig-a", 0xC0A80233, "pixfrog", "rig-a", "v1", "30eda0000001", "1");
    run_hub(16);
    EXPECT_EQ(shim::mdns_delegate_ip("pixfrog"), 0u);
    post("/api/global", "{\"hub_preferred\":false}");
    run_hub(1);
    EXPECT_TRUE(shim::mdns_txt("hub") == "0");
}

TEST(a_renamed_box_republishes_its_txt_name) {
    hub_fresh();
    post("/api/global", "{\"short_name\":\"Rack 2\"}");
    run_hub(1);
    EXPECT_TRUE(shim::mdns_txt("node") == "Rack 2");
}

// Kept last: it re-initialises the config store on a dead NVS.
TEST(status_tells_when_settings_no_longer_persist) {
    Json ok(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsTrue(ok["persist_ok"]));
    shim::nvs_wipe();
    shim::nvs_fail_init(5);
    shim::nvs_fail_erase(5);
    config::init();
    Json bad(get("/api/status").body);
    EXPECT_TRUE(cJSON_IsFalse(bad["persist_ok"]));
    shim::nvs_fail_init(0);
    shim::nvs_fail_erase(0);
    shim::nvs_wipe();
    config::init();
}
