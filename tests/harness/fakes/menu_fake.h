// Stand-in for menu.cpp (which needs every module) when the UI task loop and
// the display/encoder drivers are under test: records what the loop asks.
#pragma once
#include <vector>

#include "ui_internal.h"

namespace fake {
struct Menu {
    bool inited  = false;
    bool home    = true;  // menu_is_home()
    int renders  = 0;
    int timeouts = 0;
    std::vector<pixfrog::ui::detail::Event> events;
};
Menu& menu();
}  // namespace fake
