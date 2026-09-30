// web_config.cpp — the REST API contract through the real handlers: routes,
// JSON shapes, validation, auth + CSRF gate, backup/restore, OTA, scenes,
// FSEQ, logs, core dump, peers. Real config_store/dmx_manager/sacn beneath.

#include <string>

#include "cJSON.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
#include "harness.h"
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
    EXPECT_EQ(shim::http_routes(), 29);
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
    EXPECT_EQ(get("/api/config").status, 200);  // reads stay open
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
    EXPECT_EQ(post("/api/factory-reset").status, 200);
    EXPECT_TRUE(config::get_channel(0).pixel_count != 999);
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

TEST(backup_restore_carries_the_control_mode_and_fade) {
    post("/api/control", "{\"preset\":\"full\",\"enabled\":true,\"universe\":55,\"address\":7}");
    post("/api/global", "{\"scene_fade_ms\":2500}");
    const std::string backup = get("/api/backup").body;
    config::reset_to_defaults();
    EXPECT_EQ(config::get_control().universe, config::kDefaultControlUniverse);
    EXPECT_EQ(post("/api/restore", backup).status, 200);
    EXPECT_EQ(config::get_control().universe, 55);
    EXPECT_EQ(config::get_control().address, 7);
    EXPECT_EQ(config::get_control().count, 15);
    EXPECT_EQ(config::get_global().scene_fade_ms, 2500);
    // A malformed control object is skipped, the rest still restores.
    std::string bad       = backup;
    const std::string key = "\"fn\":\"master\"";
    bad.replace(bad.find(key), key.size(), "\"fn\":\"laser\"");
    config::reset_to_defaults();
    EXPECT_EQ(post("/api/restore", bad).status, 200);
    EXPECT_EQ(config::get_control().universe, config::kDefaultControlUniverse);
    EXPECT_EQ(config::get_global().scene_fade_ms, 2500);
    post("/api/global", "{\"scene_fade_ms\":0}");
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
