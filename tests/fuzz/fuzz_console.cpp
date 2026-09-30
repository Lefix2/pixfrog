// The real UART console command table: the input is one command line (cut at
// the first NUL or newline), run as the REPL would.
#include <cstddef>
#include <cstdint>
#include <string>

#include "config_store.h"
#include "control_console.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
#include "shim_control.h"

using namespace pixfrog;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        console::start();
        once = true;
    }
    std::string line;
    for (size_t i = 0; i < size && data[i] && data[i] != '\n' && line.size() < 256; ++i)
        line += static_cast<char>(data[i]);
    shim::nvs_wipe();
    config::init();
    ::fake::reset_modules();
    fseq::fake::set(fseq::Status::Idle, 0);
    shim::tasks_forget();
    dmx::scene_stop();
    shim::console_exec(line.c_str());
    dmx::handle_pending_remaps();
    return 0;
}
