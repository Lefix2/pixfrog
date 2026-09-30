// The real web_config handlers: byte 0 picks a route, byte 1 the index for
// the wildcard routes and the receive chunk size, the rest is the request
// body (JSON, raw upload bytes). Each input starts from factory settings.
#include <cstddef>
#include <cstdint>
#include <string>

#include "config_store.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
#include "shim_control.h"
#include "web_config.h"

using namespace pixfrog;

namespace {

struct Route {
    const char* method;
    const char* uri;
    bool indexed;  // append "/<n>"
};
const Route kRoutes[] = {
    { "POST", "/api/config", false },      { "POST", "/api/global", false },
    { "POST", "/api/channel", true },      { "POST", "/api/restore", false },
    { "POST", "/api/scene", true },        { "DELETE", "/api/scene", true },
    { "POST", "/api/scenes/add", false },  { "POST", "/api/scenes/move", false },
    { "POST", "/api/scenes/stop", false }, { "POST", "/api/rollback/ack", false },
    { "POST", "/api/autopatch", false },   { "POST", "/api/fseq/play", false },
    { "POST", "/api/fseq/stop", false },   { "POST", "/api/loglevel", false },
    { "POST", "/api/ota", false },         { "GET", "/api/config", false },
    { "GET", "/api/status", false },       { "GET", "/api/backup", false },
    { "GET", "/api/fseq/files", false },   { "GET", "/api/logs", false },
};
constexpr size_t kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        web::start();
        once = true;
    }
    if (size < 2) return 0;
    shim::nvs_wipe();
    config::init();  // factory state: inputs stay independent
    ::fake::reset_modules();
    fseq::fake::set(fseq::Status::Idle, 0);
    const Route& r  = kRoutes[data[0] % kRouteCount];
    std::string uri = r.uri;
    if (r.indexed) uri += "/" + std::to_string(data[1] % 40);
    shim::http_recv_chunk(data[1] >> 6 ? size_t(1) << (data[1] >> 6) : 0);
    const std::string body(reinterpret_cast<const char*>(data + 2), size - 2);
    shim::http_request(r.method, uri, body, { { "Content-Type", "application/json" } });
    dmx::handle_pending_remaps();
    return 0;
}
