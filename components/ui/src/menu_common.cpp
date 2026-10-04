// Menu helpers shared by every screen: drawing primitives, the status strip,
// value formatting, the menu state and the scrolling list renderer.

#include "menu_internal.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

// ── Canvas text helpers ───────────────────────────────────────────────────────
// Thin wrapper mapping OLED-style (row, col) coordinates to pixel coords. Every
// call site is in an OLED branch, so the TFT build has no use for it.

[[maybe_unused]] void draw_row(uint8_t row, uint8_t col, const char* str) {
    canvas_draw_text(col * kFontCellWidth, row * kFontHeight, str, color::White);
}

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
void draw_icon_layer(int x, int y, const uint8_t* alpha, Color fg) {
    canvas_draw_mask_aa(x, y, kIconSize, kIconSize, alpha, fg);
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// Network-state icon: one per NetState, themed to the design palette.
//   Disconnected → red X over a dim grey cable
//   Acquiring     → dim grey cable + orange dots
//   Connected     → green cable
//   Error         → orange "?" over a dim grey cable
void draw_net_icon(int x, int y, ui::NetState st) {
    using ui::NetState;
    const Color dim = color::DimGreen;  // muted base (grey-green)
    switch (st) {
    case NetState::Disconnected:
        draw_icon_layer(x, y, kIcon_net_disconnected_dim, dim);
        draw_icon_layer(x, y, kIcon_net_disconnected_main, color::BadCoral);
        break;
    case NetState::Acquiring:
        draw_icon_layer(x, y, kIcon_net_acquiring_dim, dim);
        draw_icon_layer(x, y, kIcon_net_acquiring_main, color::Orange);
        break;
    case NetState::Connected:
        draw_icon_layer(x, y, kIcon_net_connected_main, color::FrogLine);
        break;
    case NetState::Error:
        draw_icon_layer(x, y, kIcon_net_no_route_dim, dim);
        draw_icon_layer(x, y, kIcon_net_no_route_main, color::Orange);
        break;
    }
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
void draw_data_icon(int x, int y, DataFlow f) {
    const Color active = color::GoodBright;
    const Color muted  = color::DimGreen;
    switch (f) {
    case DataFlow::None: draw_icon_layer(x, y, kIcon_data_idle_dim, muted); break;
    case DataFlow::Receive:
        draw_icon_layer(x, y, kIcon_data_receive_dim, muted);
        draw_icon_layer(x, y, kIcon_data_receive_main, active);
        break;
    case DataFlow::Transmit:
        draw_icon_layer(x, y, kIcon_data_transmit_dim, muted);
        draw_icon_layer(x, y, kIcon_data_transmit_main, active);
        break;
    case DataFlow::Both: draw_icon_layer(x, y, kIcon_data_txrx_main, active); break;
    }
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// Channel-number badge: a rounded square filled with the wiring-family colour
// when the channel is in use, or a hollow 2px ring of that colour when it is
// Off. The digit is the homogeneous Body face — big enough to fill the square.
// `behind` is the row tint the AA edges and the hollow centre blend into.
//
// Body figures don't fill their 9×16 advance cell: the ink box spans roughly
// cols 0–6 / rows 2–11 (caps hug the baseline at row 12, the descender rows stay
// blank), so centring on the advance drifts the digit up and to the left. Offset
// the draw origin so the digit's *ink* box lands on the badge centre instead.
void draw_chan_badge(int x, int y, int side, int number, Color family, bool filled, Color behind) {
    canvas_fill_round_rect_aa(x, y, side, side, 4, family, behind);
    if (!filled)  // carve the centre back to the row tint → a family-colour ring
        canvas_fill_round_rect_aa(x + 2, y + 2, side - 4, side - 4, 3, behind, family);
    char n[4];
    std::snprintf(n, sizeof(n), "%d", number);
    const Color ink      = filled ? color::Black : family;
    constexpr int kInkCx = 3;  // Body digit ink centre column within the cell
    constexpr int kInkCy = 6;  // Body digit ink centre row within the cell (≈6.5)
    canvas_draw_text_f(x + (side - 1) / 2 - kInkCx, y + (side - 1) / 2 - kInkCy, n, ink,
                       color::Transparent, FontId::Body);
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// Number of DMX universes a channel occupies, derived from its pixel
// byte-footprint (pixels × bytes/px, offset by dmx_start); a disabled channel
// none. Lets HOME show the real
// addressing span (U1-2, U8-10) instead of only the start universe.
int channel_universe_span(const config::ChannelConfig& cc) {
    if (led::is_off(cc.protocol)) return 0;
    const uint32_t offset = (cc.dmx_start > 0) ? (cc.dmx_start - 1u) : 0u;
    const uint32_t bytes  = static_cast<uint32_t>(cc.pixel_count) *
                           led::bytes_per_pixel(cc.protocol);
    const uint32_t total = offset + bytes;
    if (total == 0) return 1;
    return static_cast<int>((total + dmx::kUniverseSize - 1) / dmx::kUniverseSize);
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// Edit-screen hint bar: green keycaps + dim actions, three segments (design
// Hints "TURN · PRESS · HOLD"). Drawn flush to the bottom over a top hairline.
void draw_hint_bar(const char* v_turn, const char* v_press, const char* v_hold) {
    const int y = kTH - kFootH;
    canvas_hline(0, y, kTW, color::Hair);
    const char* keys[3] = { "TURN", "PRESS", "HOLD" };
    const char* vals[3] = { v_turn, v_press, v_hold };
    for (int i = 0; i < 3; ++i) {
        const int w  = body_w(keys[i]) + 5 + body_w(vals[i]);
        const int cx = kTW * (2 * i + 1) / 6;
        int x        = cx - w / 2;
        text_body(x, y, kFootH, keys[i], color::FrogLine);
        text_body(x + body_w(keys[i]) + 5, y, kFootH, vals[i], color::DimGreen);
    }
}
#endif
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// Shared screen header: dark bar + signature-green accent line + title.
void draw_tft_header(const char* title, Color title_col) {
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    canvas_fill_rect(0, 0, kTW, kHdrH - 1, color::Black);
    canvas_fill_rect(0, kHdrH - 1, kTW, 1, color::FrogLine);
    text_body(kPadX, 0, kHdrH, title, title_col);
#else
    canvas_fill_rect(0, 0, kTW, kHdrH - 2, color::HeaderBg);
    canvas_fill_rect(0, kHdrH - 2, kTW, 2, color::FrogLine);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, title, title_col, color::HeaderBg, kTxtSc);
#endif
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
int pill_width(const char* txt) {
    return static_cast<int>(std::strlen(txt)) * kFontCellWidth + 12;
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
[[maybe_unused]] int draw_pill(int x, int y, const char* txt, Color bg, Color fg, Color behind) {
    const int w = pill_width(txt);
    canvas_fill_round_rect_aa(x, y, w, kPillH, kPillH / 2, bg, behind);
    // Small-font caps ink spans rows 0..5 of the 8px cell: +4 centres it.
    canvas_draw_text(x + 6, y + 4, txt, fg, bg, 1);
    return x + w + 6;
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// Channel-number badge: AA rounded square with the digit drawn on a transparent
// background so its ink composites over the badge and never squares off the
// rounded corners.
[[maybe_unused]] void draw_badge(int x, int y, int side, int number, Color badge_col, Color num_col,
                                 Color behind) {
    canvas_fill_round_rect_aa(x, y, side, side, 4, badge_col, behind);
    char num[8];
    std::snprintf(num, sizeof(num), "%d", number);
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // Scale 1: a digit is 6×8 — centred in the 14px badge.
    const int cw = kFontCellWidth;
    const int ch = kFontHeight;
    const int tx = x + (side - cw) / 2;
    const int ty = y + (side - ch) / 2;
    canvas_draw_text(tx, ty, num, num_col, color::Transparent, 1);
#else
    // Scale 2: digit ink sits in the upper ~13 rows of the 12×16 cell, bias down.
    const int cw = kFontCellWidth * kTxtSc;  // 12 px wide
    const int tx = x + (side - cw) / 2;
    const int ty = y + (side - kTxtH) / 2 + 2;
    canvas_draw_text(tx, ty, num, num_col, color::Transparent, kTxtSc);
#endif
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// Right-aligned standard-scale text; returns the x where the text starts.
[[maybe_unused]] int draw_text_r(int x_end, int y, const char* txt, Color fg, Color bg) {
    const int x = x_end - static_cast<int>(std::strlen(txt)) * kFontCellWidth * kTxtSc;
    canvas_draw_text(x, y, txt, fg, bg, kTxtSc);
    return x;
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// Packet counters outgrow their column fast; keep them to ≤6 glyphs.
[[maybe_unused]] void fmt_count(char* out, size_t cap, uint64_t v) {
    if (v < 1'000'000ull)
        std::snprintf(out, cap, "%llu", static_cast<unsigned long long>(v));
    else if (v < 1'000'000'000ull)
        std::snprintf(out, cap, "%llu.%lluM", static_cast<unsigned long long>(v / 1'000'000ull),
                      static_cast<unsigned long long>((v / 100'000ull) % 10ull));
    else
        std::snprintf(out, cap, "%lluG", static_cast<unsigned long long>(v / 1'000'000'000ull));
}
#endif

uint8_t g_ctl_slot = 0;

char g_ctl_slot_title[12];

void save_control(const config::ControlConfig& c) {
    config::set_control(c);
    dmx::mark_global_dirty();  // maps (or drops) the control universe
}

uint8_t g_gap_index = 0;

// Pick-from-a-list kinds (wheel) vs numeric ones (gauge).
bool is_enum_kind(ValueKind k) {
    return k == ValueKind::Protocol || k == ValueKind::ColorOrder || k == ValueKind::Failsafe ||
           k == ValueKind::CtlFn || k == ValueKind::Preset || k == ValueKind::IpFallback;
}

bool is_gauge_kind(ValueKind k) {
    return k == ValueKind::Int || k == ValueKind::ClockHz || k == ValueKind::Mask ||
           k == ValueKind::Tenths;
}

// TFT labels of the control functions, indexed by config::CtlFn.
const char* ctl_fn_label(uint8_t fn) {
    static const char* const kLabels[] = {
        "Spare", "Master", "Blackout", "Strobe", "Scene", "Speed",     "Param",   "Red",
        "Green", "Blue",   "Effect",   "Fade",   "FSEQ",  "Direction", "Fix mode"
    };
    static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == static_cast<size_t>(config::CtlFn::Count),
                  "one label per control function");
    return fn < static_cast<uint8_t>(config::CtlFn::Count) ? kLabels[fn] : "Spare";
}

// "12------" style: one digit per output in the mask.
void format_mask(uint8_t mask, char* out, size_t cap) {
    if (cap < 9) return;
    for (int o = 0; o < 8; ++o)
        out[o] = ((mask >> o) & 1) ? static_cast<char>('1' + o) : '-';
    out[8] = '\0';
}

State s;

char g_fseq_names[kFseqMenuMaxFiles][fseq::kMaxNameLen];

uint8_t g_fseq_file_count = 0;

RotationAccel g_accel;

void accel_reset() {
    g_accel.reset();
}

// Call on every rotation detent; returns the step multiplier to apply.
int32_t accel_note_rotation(Event e) {
    return g_accel.note(now_ms(), e == Event::RotateRight);
}

// last slot = end marker

int char_to_idx(char c) {
    if (c == kStrEnd) return kAlphabetLen;
    for (int i = 0; i < kAlphabetLen; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return 0;  // unknown → space
}

char idx_to_char(int i) {
    i = ((i % kStrCycle) + kStrCycle) % kStrCycle;
    return (i == kAlphabetLen) ? kStrEnd : kAlphabet[i];
}

// ── Helpers: enum → name ────────────────────────────────────────────────────

const char* protocol_name(led::Protocol p) {
    switch (p) {
    case led::Protocol::WS2815: return "WS2815";
    case led::Protocol::WS2812B: return "WS2812B";
    case led::Protocol::WS2811: return "WS2811";
    case led::Protocol::SK6812: return "SK6812";
    case led::Protocol::WS2814: return "WS2814";
    case led::Protocol::APA102: return "APA102";
    case led::Protocol::SK9822: return "SK9822";
    case led::Protocol::LPD8806: return "LPD8806";
    case led::Protocol::Off: return "Off";
    default: return "?";
    }
}

// Channel badge colour mirrors the wiring family: clocked SPI strips stand out
// from the common NRZ pixels; a disabled channel is greyed out.
Color badge_color(led::Protocol p) {
    if (led::is_off(p)) return color::DarkGray;
    if (led::is_clocked(p)) return color::BadgePurple;
    return color::BadgeGreen;
}

const char* color_order_name(led::ColorOrder o) {
    switch (o) {
    case led::ColorOrder::RGB: return "RGB";
    case led::ColorOrder::RBG: return "RBG";
    case led::ColorOrder::GRB: return "GRB";
    case led::ColorOrder::GBR: return "GBR";
    case led::ColorOrder::BRG: return "BRG";
    case led::ColorOrder::BGR: return "BGR";
    case led::ColorOrder::RGBW: return "RGBW";
    case led::ColorOrder::GRBW: return "GRBW";
    default: return "?";
    }
}

void truncate(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

const char* failsafe_name(uint8_t m) {
    switch (m) {
    case config::kFailsafeBlackout: return "Black";
    case config::kFailsafeColor: return "Color";
    case config::kFailsafeScene: return "Scene";
    default: return "Hold";
    }
}

// Index of the choice nearest to `hz` (legacy/odd values snap to a real rate).
int clock_choice_index(int32_t hz) {
    int best          = 0;
    int32_t best_dist = 0x7fffffff;
    for (int i = 0; i < kClockChoiceCount; ++i) {
        const int32_t d = hz > kClockChoices[i] ? hz - kClockChoices[i] : kClockChoices[i] - hz;
        if (d < best_dist) {
            best_dist = d;
            best      = i;
        }
    }
    return best;
}

void format_clock_mhz(int32_t hz, char* out, size_t cap) {
    std::snprintf(out, cap, "%ld.%01ldMHz", static_cast<long>(hz / 1'000'000),
                  static_cast<long>((hz % 1'000'000) / 100'000));
}

void format_value(const EditCtx& e, int32_t v, char* out, size_t cap) {
    switch (e.kind) {
    case ValueKind::Int: std::snprintf(out, cap, "%ld", static_cast<long>(v)); return;
    case ValueKind::Bool: std::snprintf(out, cap, "%s", v ? "ON" : "OFF"); return;
    case ValueKind::Failsafe:
        std::snprintf(out, cap, "%s", failsafe_name(static_cast<uint8_t>(v)));
        return;
    case ValueKind::Protocol:
        std::snprintf(out, cap, "%s", protocol_name(static_cast<led::Protocol>(v)));
        return;
    case ValueKind::ColorOrder:
        std::snprintf(out, cap, "%s", color_order_name(static_cast<led::ColorOrder>(v)));
        return;
    case ValueKind::ClockHz: format_clock_mhz(v, out, cap); return;
    case ValueKind::CtlFn:
        std::snprintf(out, cap, "%s", ctl_fn_label(static_cast<uint8_t>(v)));
        return;
    case ValueKind::Preset: std::snprintf(out, cap, "%s", v ? "Full" : "Simple"); return;
    case ValueKind::IpFallback:
        std::snprintf(out, cap, "%s", v == config::kIpFallbackArtnet ? "Art-Net 2.x" : "169.254");
        return;
    case ValueKind::Mask: format_mask(static_cast<uint8_t>(v), out, cap); return;
    case ValueKind::Tenths:
        std::snprintf(out, cap, "%ld.%lds", static_cast<long>(v / 10), static_cast<long>(v % 10));
        return;
    }
}

// the longest list: DMX CONTROL

// A "return to the parent menu" row. Rendered with a left back-arrow glyph
// instead of bracketed text; `label` lets Main say "HOME" and the test-pattern
// node say "Stop & back".
ListItem back_item(const char* label) {
    ListItem it{};
    it.label = label;
    it.value = "";
    it.back  = true;
    return it;
}

// Free-roaming scroll viewport: the cursor moves anywhere within the visible
// window and only drags the scroll offset when it would leave the top or bottom
// edge. The offset persists in s.scroll across renders, so coming back up
// un-sticks the cursor from the last row instead of dragging the whole list.
// Returns the index of the first visible row.
[[maybe_unused]] uint8_t viewport_first(uint8_t cursor, uint8_t count, uint8_t visible) {
    if (count <= visible) {
        s.scroll = 0;
        return 0;
    }
    if (cursor < s.scroll)
        s.scroll = cursor;
    else if (cursor >= s.scroll + visible)
        s.scroll = static_cast<uint8_t>(cursor - visible + 1);
    if (s.scroll + visible > count) s.scroll = static_cast<uint8_t>(count - visible);
    return s.scroll;
}

// Off-focus rows fade with their distance from the cursor (1 = nearest).
[[maybe_unused]] Color fade_gray(int d) {
    static const Color g[] = { color::LightGray, Color{ 0x8C71 }, Color{ 0x5AEB },
                               color::DarkGray };
    if (d < 1) d = 1;
    if (d > 4) d = 4;
    return g[d - 1];
}

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// A small left-pointing arrow ("◀—") vertically centred in an `h`-tall row at
// (x, y) — the prettier replacement for a "[Back]" label.
void draw_back_arrow(int x, int y, int h, Color c) {
    const int mid  = y + h / 2;
    const int head = 5;  // triangle half-height (apex at x, base at x+head)
    for (int d = 0; d <= head; ++d)
        canvas_vline(x + d, mid - d, 2 * d + 1, c);  // widens rightward → points left
    canvas_fill_rect(x + head, mid - 1, 7, 3, c);    // tail
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// One list row inside a column (left edge x0, width colW, top ry). Matches
// pixfrog-screen.jsx ListRow: optional chip, label, optional gold value, chevron.
void draw_list_item_col(const ListItem& it, bool sel, int x0, int colW, int ry, int row) {
    // Zebra bands (selection > alt-row tint > black) replace the row hairlines
    // and the column divider — softer, and consistent with the HOME table.
    const bool alt = (row & 1) != 0;
    const Color bg = sel ? color::SelBg : (alt ? color::RowAlt : color::Black);
    canvas_fill_rect(x0, ry, colW, kRowH, bg);
    if (sel) canvas_fill_round_rect(x0, ry + 3, 3, kRowH - 6, 1, color::GoodBright);

    const bool terminal = it.back || (it.label[0] == '[');
    int x               = x0 + kIndent;

    if (it.back) {
        draw_back_arrow(x, ry, kRowH, color::FrogLine);
        text_body(x + 13, ry, kRowH, it.label, color::FrogLine, bg);
        return;
    }
    if (it.badge >= 0) {
        // Same badge as HOME: filled when the channel is in use, hollow (a
        // family-colour ring) when Off — a disabled channel reads as DarkGray.
        const int cy      = ry + (kRowH - kChip) / 2;
        const bool filled = (it.badge_col != color::DarkGray);
        draw_chan_badge(x, cy, kChip, it.badge + 1, it.badge_col, filled, bg);
        x += kChip + 7;
    }

    // Right edge: chevron, then value to its left.
    int rx = x0 + colW - kIndent;
    if (!terminal) {
        text_body(rx - canvas_font_adv(FontId::Body), ry, kRowH, ">", color::DimGreen, bg);
        rx -= canvas_font_adv(FontId::Body) + 4;
    }
    if (it.value && it.value[0]) {
        const int vw = body_w(it.value);
        text_body(rx - vw, ry, kRowH, it.value, it.value_col, bg);
        rx -= vw + 6;
    }
    // Label fills the gap; truncate to the remaining width.
    const Color lc = sel ? color::Cream : color::Cream;
    char lbl[24];
    truncate(lbl, sizeof(lbl), it.label);
    const int maxchars = (rx - x) / canvas_font_adv(FontId::Body);
    if (maxchars > 0 && static_cast<int>(std::strlen(lbl)) > maxchars)
        lbl[maxchars > 0 ? maxchars : 0] = '\0';
    text_body(x, ry, kRowH, lbl, lc, bg);
}
#endif

void render_list(const char* title, const ListItem* items, uint8_t count, uint8_t cursor) {
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 landscape: row-major two-column list (1 2 / 3 4 / …) ────────────
    // Cursor advances left→right then down; scrolling moves whole rows.
    canvas_clear(color::Black);
    draw_tft_header(title);

    const int totalRows = (count + 1) / 2;
    // Free-roaming row viewport: the cursor row moves freely within the visible
    // window and only drags the scroll offset when it would leave the top or
    // bottom edge. The offset persists in s.scroll across renders, so coming
    // back up un-sticks the cursor from the last row instead of dragging the
    // whole list (mirrors viewport_first() used by the TFT/OLED paths).
    int firstRow = static_cast<int>(s.scroll);
    if (totalRows <= kListRows) {
        firstRow = 0;
    } else {
        const int crow = cursor / 2;
        if (crow < firstRow)
            firstRow = crow;
        else if (crow >= firstRow + kListRows)
            firstRow = crow - (kListRows - 1);
        if (firstRow + kListRows > totalRows) firstRow = totalRows - kListRows;
        if (firstRow < 0) firstRow = 0;
    }
    s.scroll        = static_cast<uint8_t>(firstRow);
    const int first = firstRow * 2;
    for (int p = 0; p < kMaxVis && first + p < count; ++p) {
        const int idx = first + p;
        const int col = p % 2;  // left / right
        const int row = p / 2;  // top → bottom
        const int x0  = col * kColW;
        const int ry  = kHdrH + row * kRowH;
        draw_list_item_col(items[idx], idx == cursor, x0, kColW, ry, row);
    }
    // Thin divider between the two columns (only when the right column is used).
    if (count > 1) canvas_vline(kColW, kHdrH, kTH - kHdrH, color::Hair);
    // Scrollbar (row-based): track + proportional thumb on the far right.
    if (totalRows > kListRows) {
        const int trackH = kTH - kHdrH - 6;
        int thumbH       = trackH * kListRows / totalRows;
        if (thumbH < 12) thumbH = 12;
        const int travel = totalRows - kListRows;
        const int y      = kHdrH + 3 + (travel > 0 ? (trackH - thumbH) * firstRow / travel : 0);
        canvas_fill_round_rect(kTW - 5, kHdrH + 3, 3, trackH, 1, color::Hair);
        canvas_fill_round_rect(kTW - 5, y, 3, thumbH, 1, color::FrogLine);
    }
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    // ── TFT: dark list with rounded cursor highlight ──────────────────────────
    canvas_clear(color::Black);
    draw_tft_header(title);

    const int visible = kMaxVis;
    const int first   = viewport_first(cursor, count, static_cast<uint8_t>(visible));
    for (int i = 0; i < visible && first + i < count; ++i) {
        const int idx      = first + i;
        const int ry       = kHdrH + i * kItemH;
        const ListItem& it = items[idx];
        const bool sel     = (idx == cursor);
        // Terminal rows (back rows + bracketed actions like "[Stop]") carry no
        // ">" sub-menu chevron.
        const bool no_chevron = it.back || (it.label[0] == '[');
        const Color text_bg   = sel ? color::CursorBg : color::Black;

        if (sel) {
            // Rounded highlight with a signature-green bar on the left edge.
            canvas_fill_round_rect_aa(3, ry + 1, kTW - 6, kItemH - 2, 6, color::CursorBg,
                                      color::Black);
            canvas_fill_round_rect_aa(5, ry + 4, 4, kItemH - 8, 2, color::FrogLine,
                                      color::CursorBg);
        }
        if (it.back) {
            // Back-arrow glyph in the badge column, label in the signature green.
            draw_back_arrow(kGutter, ry, kItemH, color::FrogLine);
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
            canvas_draw_text(kGutter + 10, ry + kPad, it.label, color::FrogLine, text_bg, kLstSc);
#else
            canvas_draw_text(kGutter + 18, ry + kPad, it.label, color::FrogLine, text_bg, kTxtSc);
#endif
            continue;
        }
        if (it.badge >= 0) {
            const Color num_col = (it.badge_col == color::DarkGray) ? color::LightGray
                                                                    : color::Black;
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
            draw_badge(kIndent, ry + (kItemH - kBadge) / 2, kBadge, it.badge + 1, it.badge_col,
                       num_col, text_bg);
#else
            draw_badge(5, ry + 3, kBadge, it.badge + 1, it.badge_col, num_col, text_bg);
#endif
        }
        canvas_draw_text(kGutter, ry + kPad, it.label, color::Cream, text_bg, kLstSc);

        const int chev_x = kTW - kChevW - kIndent;
        if (!no_chevron) canvas_draw_text(chev_x, ry + kPad, ">", color::DarkGray, text_bg, kLstSc);
        if (it.value && it.value[0]) {
            const int vw   = static_cast<int>(std::strlen(it.value)) * kFontCellWidth * kLstSc;
            const int vend = no_chevron ? (kTW - kIndent) : (chev_x - 4);
            canvas_draw_text(vend - vw, ry + kPad, it.value, it.value_col, text_bg, kLstSc);
        }
    }
    // Scrollbar: a full-height track with a proportional thumb so the list
    // length and position are obvious at a glance (not just "more exists").
    if (count > static_cast<uint8_t>(visible)) {
        const int trackY = kHdrH + 2;
        const int trackH = kTH - trackY - 2;
        canvas_fill_round_rect(kTW - 4, trackY, 3, trackH, 1, color::CursorBg);
        int thumbH = trackH * visible / count;
        if (thumbH < 12) thumbH = 12;
        const int travel = count - visible;
        const int thumbY = trackY + (travel > 0 ? (trackH - thumbH) * first / travel : 0);
        canvas_fill_round_rect(kTW - 4, thumbY, 3, thumbH, 1, color::FrogLine);
    }
#else
    // ── OLED: classic scrolling text list ────────────────────────────────────
    canvas_clear();
    draw_row(0, 0, title);
    const uint8_t visible = kRows - 1;
    const uint8_t first   = viewport_first(cursor, count, visible);
    for (uint8_t i = 0; i < visible && first + i < count; ++i) {
        const uint8_t idx = first + i;
        char line[kOledCols + 1];
        const char prefix = (idx == cursor) ? '>' : ' ';
        if (items[idx].back) {
            // Back row: a "<" return indicator instead of bracketed text (the
            // 5x7 OLED font has no arrow glyph).
            std::snprintf(line, sizeof(line), "%c< %s", prefix, items[idx].label);
        } else if (items[idx].value && items[idx].value[0]) {
            std::snprintf(line, sizeof(line), "%c%s:%s", prefix, items[idx].label,
                          items[idx].value);
        } else {
            std::snprintf(line, sizeof(line), "%c%s", prefix, items[idx].label);
        }
        draw_row(static_cast<uint8_t>(1 + i), 0, line);
    }
#endif
}

}  // namespace pixfrog::ui::detail::menu_impl
