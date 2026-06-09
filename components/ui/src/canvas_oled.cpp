#include "ui_internal.h"

namespace pixfrog::ui::detail {

int canvas_width() {
    return 128;
}
int canvas_height() {
    return 64;
}

void canvas_clear(Color /*bg*/) {
    oled_clear();
}

void canvas_fill_rect(int /*x*/, int /*y*/, int /*w*/, int /*h*/, Color /*c*/) {}

void canvas_hline(int /*x*/, int /*y*/, int /*w*/, Color /*c*/) {}
void canvas_vline(int /*x*/, int /*y*/, int /*h*/, Color /*c*/) {}
void canvas_fill_round_rect(int /*x*/, int /*y*/, int /*w*/, int /*h*/, int /*r*/, Color /*c*/) {}
void canvas_draw_mask(int /*x*/, int /*y*/, int /*w*/, int /*h*/, const uint8_t* /*mask*/,
                      Color /*fg*/, Color /*bg*/) {}

void canvas_draw_text(int x, int y, const char* str, Color fg, Color /*bg*/, uint8_t /*scale*/) {
    if (!str || fg == color::Black) return;
    oled_draw_text(static_cast<uint8_t>(y / kFontHeight), static_cast<uint8_t>(x / kFontCellWidth),
                   str);
}

void canvas_flush() {
    oled_flush();
}

}  // namespace pixfrog::ui::detail
