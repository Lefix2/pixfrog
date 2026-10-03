// Stand-in for menu.cpp (which needs every module) when the UI task loop and
// the display/encoder drivers are under test: records what the loop asks.
#pragma once
#include <vector>

#include "ui_internal.h"

namespace fake {
struct Menu {
    bool inited          = false;
    bool home            = true;  // menu_is_home()
    int renders          = 0;
    int timeouts         = 0;
    uint32_t fingerprint = 0;      // menu_fingerprint()
    bool moves           = false;  // a dispatch changes it (the step moved something)
    bool speaker         = false;  // ui::set_speaker_present()
    int press            = 1;      // menu_press_kind()
    float gauge          = -1.0f;  // menu_gauge_level()
    std::vector<pixfrog::ui::detail::Event> events;
};
Menu& menu();
}  // namespace fake
