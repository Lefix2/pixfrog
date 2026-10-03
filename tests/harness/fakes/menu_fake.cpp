#include "menu_fake.h"

namespace fake {
Menu& menu() {
    static Menu m;
    return m;
}
}  // namespace fake

namespace pixfrog::ui::detail {
void menu_init() {
    fake::menu().inited = true;
}
void menu_dispatch(Event e) {
    fake::menu().events.push_back(e);
    if (fake::menu().moves) ++fake::menu().fingerprint;
}
void menu_render() {
    ++fake::menu().renders;
    canvas_draw_text(0, 0, "MENU", color::White);
}
void menu_on_idle_timeout() {
    ++fake::menu().timeouts;
}
bool menu_is_home() {
    return fake::menu().home;
}
float menu_gauge_level() {
    return fake::menu().gauge;
}
int menu_press_kind(Event) {
    return fake::menu().press;
}
uint32_t menu_fingerprint() {
    return fake::menu().fingerprint;
}
}  // namespace pixfrog::ui::detail

namespace pixfrog::ui {
void set_speaker_present(bool present) {
    fake::menu().speaker = present;
}
}  // namespace pixfrog::ui
