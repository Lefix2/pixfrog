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
}  // namespace pixfrog::ui::detail
