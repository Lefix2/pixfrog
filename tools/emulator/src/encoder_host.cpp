// Host implementation of the encoder driver.
//
// Drop-in replacement for components/ui/src/encoder_seesaw.cpp: provides the
// same encoder_init / encoder_poll symbols the UI task calls. Instead of an
// I2C seesaw, events are pushed into a thread-safe queue by the SDL keyboard
// handler and the stdin agent API (emu_push_event), then drained by
// encoder_poll() exactly like the real driver drains the seesaw FIFO.

#include "encoder_emu.h"
#include "ui_internal.h"

#include <chrono>
#include <deque>
#include <mutex>

namespace pixfrog::ui::detail {
namespace {
std::mutex& queue_mutex() {
    static std::mutex m;
    return m;
}
std::deque<Event>& queue() {
    static std::deque<Event> q;
    return q;
}
}  // namespace

bool encoder_init(i2c_master_bus_handle_t /*bus*/, uint8_t /*addr*/, int /*int_gpio*/) {
    return true;
}

Event encoder_poll() {
    std::lock_guard<std::mutex> lk(queue_mutex());
    if (queue().empty()) return Event::None;
    Event e = queue().front();
    queue().pop_front();
    return e;
}

void emu_push_event_internal(Event e) {
    std::lock_guard<std::mutex> lk(queue_mutex());
    queue().push_back(e);
}

// ── Platform hooks (firmware versions live in ui.cpp) ───────────────────────

uint32_t now_ms() {
    using clock                       = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
}

const char* fw_version() {
    return "emulator";
}

const char* fw_build_info() {
    return "host build  " __DATE__;
}

}  // namespace pixfrog::ui::detail

void emu_push_event(EmuEvent e) {
    using pixfrog::ui::detail::Event;
    Event ev = Event::None;
    switch (e) {
    case EmuEvent::RotateLeft: ev = Event::RotateLeft; break;
    case EmuEvent::RotateRight: ev = Event::RotateRight; break;
    case EmuEvent::Click: ev = Event::Click; break;
    case EmuEvent::LongPress: ev = Event::LongPress; break;
    }
    if (ev != Event::None) pixfrog::ui::detail::emu_push_event_internal(ev);
}
