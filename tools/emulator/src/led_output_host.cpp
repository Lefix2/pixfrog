// Host stub for led_output: the UI's test-pattern menu reads/writes the
// calibration mode. The emulator has no LED output, so this is just a latch
// the menu can toggle and read back (the marker '*' next to the active pattern).

#include "led_output.h"

namespace pixfrog::output {

namespace {
int8_t g_cal_mode = -1;  // -1 = off
}

void set_calibration_mode(int8_t pattern_id) {
    g_cal_mode = pattern_id;
}

int8_t get_calibration_mode() {
    return g_cal_mode;
}

}  // namespace pixfrog::output
