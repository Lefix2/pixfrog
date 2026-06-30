// Menu state machine for pixfrog's OLED UI.
//
// Conventions:
//   Rotate-right = cursor-down / value-up.
//   Rotate-left  = cursor-up   / value-down.
//   Click on a list entry = enter edit or sub-screen.
//   Click in edit mode = commit (writes NVS, marks dmx dirty), return.
//   Click on a "[Back]" entry = return one level.
//   Idle timeout → HOME.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>

#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_output.h"
#include "led_protocols.h"
#include "sacn.h"
#include "ui.h"
#include "web_config.h"

namespace pixfrog::ui::detail {

namespace {

// ── Local constants ──────────────────────────────────────────────────────────

// kCols is used for buffer sizing (worst-case OLED width 21 chars).
// For TFT, kRows/kCols from ui_internal.h govern the grid, but local
// buffers use kOledCols which covers both backends safely.
constexpr uint8_t kOledCols = 21;

// ── Canvas text helpers ───────────────────────────────────────────────────────
// Thin wrappers that map OLED-style (row, col) coordinates to pixel coords.

static void draw_row(uint8_t row, uint8_t col, const char* str) {
    canvas_draw_text(col * kFontCellWidth, row * kFontHeight, str, color::White);
}

// ── TFT layout ────────────────────────────────────────────────────────────────
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// NV3007 rotated 90° → 428×142 landscape. Pixel-exact to pixfrog-screen.jsx:
// 22px header, 18px hint bar, two-column lists/home, native fonts (no scaling).
constexpr int kTW       = 428;                        // TFT width  (landscape, rotated)
constexpr int kTH       = 142;                        // TFT height (landscape, rotated)
constexpr int kHdrH     = 22;                         // header bar height (design HDR)
constexpr int kFootH    = 18;                         // edit-screen hint bar (design FOOT)
constexpr int kPadX     = 12;                         // header / status side padding
constexpr int kIndent   = 10;                         // list-row side padding
constexpr int kCols2    = 2;                          // two side-by-side columns
constexpr int kColW     = kTW / kCols2;               // 214px per column
constexpr int kListRows = 4;                          // design ROWS = 4 per column
constexpr int kMaxVis   = kCols2 * kListRows;         // 8 visible list items
constexpr int kRowH     = (kTH - kHdrH) / kListRows;  // 30px list row
constexpr int kChip     = 15;                         // 15×15 channel chip
// kTxtSc/kBadge/kGutter kept for code paths still shared with the splash helpers.
constexpr int kTxtSc = 2;
constexpr int kTxtH  = 8 * kTxtSc;
#else
// ST7789 landscape 320×240 layout
constexpr int kTW    = 320;         // TFT width  (landscape)
constexpr int kTH    = 240;         // TFT height (landscape)
constexpr int kHdrH  = 28;          // header bar height
constexpr int kItemH = 24;          // list item height
constexpr int kTxtSc = 2;           // standard text scale (12x16 per char)
constexpr int kLstSc = 2;           // same as kTxtSc for landscape
constexpr int kTxtH  = 8 * kTxtSc;  // 16px
// +2: glyph ink sits in the upper ~12 of the 16px cell (descender space below),
// so centring the full cell reads high — nudge text down to centre the ink.
constexpr int kTxtYBias = 2;
constexpr int kPad      = (kItemH - kTxtH) / 2 + kTxtYBias;  // vertical text offset in item
constexpr int kIndent   = 6;                                 // left margin
constexpr int kMaxVis   = (kTH - kHdrH) / kItemH;            // 8 visible items
constexpr int kBadge    = kItemH - 6;                        // square channel-badge side (18px)
constexpr int kGutter   = 5 + kBadge + 6;                    // label x: clears the badge column
constexpr int kChevW    = kFontCellWidth * kTxtSc;           // chevron glyph width (12px)
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// ── NV3007 design helpers ────────────────────────────────────────────────────
// Vertically-centre Body-font text in an h-tall band starting at y.
inline void text_body(int x, int y, int h, const char* s, Color fg, Color bg = color::Transparent) {
    canvas_draw_text_f(x, y + (h - canvas_font_h(FontId::Body)) / 2, s, fg, bg, FontId::Body);
}
inline int body_w(const char* s) {
    return canvas_text_w(s, FontId::Body);
}
inline int small_w(const char* s) {
    return canvas_text_w(s, FontId::Small);
}

// Frog mark (two eyes + smile), scaled from the 24px design glyph.
void draw_frog(int x, int y, int s, Color col, Color behind) {
    const int r   = s * 34 / 240;  // eye radius (design r=3.4 in vb 24)
    const int ex1 = x + s * 7 / 24, ex2 = x + s * 17 / 24, ey = y + s * 9 / 24;
    canvas_fill_round_rect_aa(ex1 - r, ey - r, 2 * r, 2 * r, r, col, behind);
    canvas_fill_round_rect_aa(ex2 - r, ey - r, 2 * r, 2 * r, r, col, behind);
    const int pr = s * 13 / 240;  // pupil
    if (pr >= 1) {
        canvas_fill_round_rect_aa(ex1 - pr, ey - pr, 2 * pr, 2 * pr, pr, color::Black, col);
        canvas_fill_round_rect_aa(ex2 - pr, ey - pr, 2 * pr, 2 * pr, pr, color::Black, col);
    }
    // Smile: a shallow arc just below the eyes (thicker centre).
    const int my = y + s * 13 / 24, mx0 = x + s * 5 / 24, mw = s * 14 / 24;
    const int th = s >= 30 ? 2 : 1;
    canvas_fill_rect(mx0 + mw / 6, my, mw - mw / 3, th, col);
    canvas_fill_rect(mx0, my - th, mw / 6 + 1, th, col);
    canvas_fill_rect(mx0 + mw - mw / 6, my - th, mw / 6 + 1, th, col);
}

// Outlined status chip: 1px rounded border (accent if on, hairline if off),
// small-font caps inside. Returns the x just past the chip + gap.
int draw_chip(int x, int y, const char* txt, bool on, Color accent, Color behind) {
    const int tw = small_w(txt), w = tw + 8, h = 12;
    const Color border = on ? accent : color::Hair;
    const Color ink    = on ? accent : color::DimGreen;
    canvas_fill_round_rect(x, y, w, h, 3, border);
    canvas_fill_round_rect(x + 1, y + 1, w - 2, h - 2, 2, behind);
    canvas_draw_text_f(x + 4, y + (h - kFontHeight) / 2, txt, ink, behind, FontId::Small);
    return x + w + 4;
}

// Edit-screen hint bar: green keycaps + dim actions, three segments (design
// Hints "TURN · PRESS · HOLD"). Drawn flush to the bottom over a top hairline.
void draw_hint_bar(const char* v_turn, const char* v_press, const char* v_hold) {
    const int y = kTH - kFootH;
    canvas_hline(0, y, kTW, color::Hair);
    const int ty        = y + (kFootH - kFontHeight) / 2 + 1;
    const char* keys[3] = { "TURN", "PRESS", "HOLD" };
    const char* vals[3] = { v_turn, v_press, v_hold };
    for (int i = 0; i < 3; ++i) {
        const int w  = small_w(keys[i]) + 5 + small_w(vals[i]);
        const int cx = kTW * (2 * i + 1) / 6;
        int x        = cx - w / 2;
        canvas_draw_text_f(x, ty, keys[i], color::FrogLine, color::Black, FontId::Small);
        canvas_draw_text_f(x + small_w(keys[i]) + 5, ty, vals[i], color::DimGreen, color::Black,
                           FontId::Small);
    }
}
#endif

// Shared screen header: dark bar + signature-green accent line + title.
void draw_tft_header(const char* title, Color title_col = color::Cream) {
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

// Small-font status capsule over a solid backdrop; returns the x past it.
constexpr int kPillH = 14;

int pill_width(const char* txt) {
    return static_cast<int>(std::strlen(txt)) * kFontCellWidth + 12;
}

int draw_pill(int x, int y, const char* txt, Color bg, Color fg, Color behind) {
    const int w = pill_width(txt);
    canvas_fill_round_rect_aa(x, y, w, kPillH, kPillH / 2, bg, behind);
    // Small-font caps ink spans rows 0..5 of the 8px cell: +4 centres it.
    canvas_draw_text(x + 6, y + 4, txt, fg, bg, 1);
    return x + w + 6;
}

// Channel-number badge: AA rounded square with the digit drawn on a transparent
// background so its ink composites over the badge and never squares off the
// rounded corners.
void draw_badge(int x, int y, int side, int number, Color badge_col, Color num_col, Color behind) {
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

// Right-aligned standard-scale text; returns the x where the text starts.
int draw_text_r(int x_end, int y, const char* txt, Color fg, Color bg) {
    const int x = x_end - static_cast<int>(std::strlen(txt)) * kFontCellWidth * kTxtSc;
    canvas_draw_text(x, y, txt, fg, bg, kTxtSc);
    return x;
}

// Packet counters outgrow their column fast; keep them to ≤6 glyphs.
void fmt_count(char* out, size_t cap, uint64_t v) {
    if (v < 1'000'000ull)
        std::snprintf(out, cap, "%llu", static_cast<unsigned long long>(v));
    else if (v < 1'000'000'000ull)
        std::snprintf(out, cap, "%llu.%lluM", static_cast<unsigned long long>(v / 1'000'000ull),
                      static_cast<unsigned long long>((v / 100'000ull) % 10ull));
    else
        std::snprintf(out, cap, "%lluG", static_cast<unsigned long long>(v / 1'000'000'000ull));
}
#endif

// ── Screens ─────────────────────────────────────────────────────────────────
// All the scrolling list menus collapse into a single Screen::Menu driven by the
// declarative node engine (see NodeId / kNodes below). Only the screens with
// bespoke rendering/input stay distinct: the Home dashboard, the About info
// page, and the four value editors.
enum class Screen : uint8_t {
    Home,
    Menu,  // node engine active — the current list is s.node
    About,
    EditValue,
    EditString,
    EditIp,
    EditUni,
};

// ── Menu tree nodes ─────────────────────────────────────────────────────────
// Every scrolling list is a node. The engine handles cursor, scroll, enter and
// back (via each node's parent) generically, so navigation carries no hardcoded
// cursor indices. Indexed by value into kNodes[].
enum class NodeId : uint8_t {
    Main,
    Inputs,
    Network,
    Output,
    Playback,
    Channel,
    Scenes,
    Fseq,
    TestPattern,
    Count,
};

// Forward declarations: build_* lambdas wire these as click actions.
void go(NodeId n);
void go_back();
void open_channel(uint8_t idx);

// ── Editable fields ─────────────────────────────────────────────────────────
enum class Field : uint8_t {
    None,
    ArtnetNet,
    ArtnetSubnet,
    ArtnetReplyUnicast,
    ArtnetSacn,
    ArtnetFpp,
    ArtnetFailsafeMode,
    ArtnetFailsafeTimeout,
    GlobalRefresh,
    NetworkDhcp,
    NetworkWebEnabled,
    ChProtocol,
    ChColorOrder,
    ChUniverse,
    ChDmx,
    ChPixels,
    ChBrightness,
    ChGamma,
    ChGrouping,
    ChInvert,
    ChClock,
    AutoPatch,
};

enum class StringField : uint8_t {
    ArtnetShort,
    ArtnetLong,
};

enum class IpField : uint8_t {
    StaticIp,
    StaticMask,
    StaticGw,
};

enum class ValueKind : uint8_t {
    Int,
    Bool,
    Protocol,
    ColorOrder,
    Failsafe,
    ClockHz,  // clocked-SPI rate, picked from kClockChoices (achievable divisors)
};

struct EditCtx {
    Field field          = Field::None;
    ValueKind kind       = ValueKind::Int;
    int32_t current      = 0;
    int32_t original     = 0;  // captured at enter_edit; shown when current differs
    int32_t step         = 1;
    int32_t min          = 0;
    int32_t max          = 0;
    uint8_t channel      = 0;
    Screen return_screen = Screen::Menu;
    const char* label    = "";
};

struct EditStringCtx {
    char buf[65]         = {};  // max(short=18, long=64) + null
    uint8_t max_len      = 0;
    uint8_t cursor       = 0;  // 0..max_len; max_len = DONE position
    StringField field    = StringField::ArtnetShort;
    Screen return_screen = Screen::Menu;
    const char* label    = "";
};

struct EditIpCtx {
    uint32_t value       = 0;  // host-order
    uint8_t cursor       = 0;  // 0..4; 4 = DONE position
    IpField field        = IpField::StaticIp;
    Screen return_screen = Screen::Menu;
    const char* label    = "";
};

// Art-Net Port-Address edited as net.sub.uni (like an IP). The packed 15-bit
// value is net(7):sub(4):uni(4) — what config::ChannelConfig::universe_start
// stores; segment maxima double as the field bitmasks (127, 15, 15).
struct EditUniCtx {
    uint16_t value       = 0;  // packed 15-bit port-address
    uint8_t cursor       = 0;  // 0..3; 3 = DONE position
    uint8_t channel      = 0;
    Screen return_screen = Screen::Menu;
};

struct State {
    Screen screen  = Screen::Home;
    NodeId node    = NodeId::Main;  // current list when screen == Menu
    uint8_t cursor = 0;             // active cursor for the current node
    uint8_t scroll = 0;             // active scroll offset for the current node
    // Per-node memory so back/forward restores where you were in each list.
    uint8_t cur[static_cast<uint8_t>(NodeId::Count)] = {};
    uint8_t scr[static_cast<uint8_t>(NodeId::Count)] = {};
    uint8_t channel_index                            = 0;
    EditCtx edit;
    EditStringCtx str_edit;
    EditIpCtx ip_edit;
    EditUniCtx uni_edit;
};

State s;

// Cached FSEQ file list — populated when entering FSeqMenu.
constexpr uint8_t kFseqMenuMaxFiles = 8;
static char g_fseq_names[kFseqMenuMaxFiles][fseq::kMaxNameLen];
static uint8_t g_fseq_file_count = 0;

// ── Rotation acceleration ───────────────────────────────────────────────────
// Sustained rotation escalates the per-detent step ×10 then ×100, whatever
// the direction (the streak counts detents, not displacement). A short pause
// drops back to ×1. Tuned for ~20 detents/s on a fast flick: ×10 after about
// half a turn, ×100 after roughly two seconds of continuous spinning.

constexpr uint32_t kAccelResetMs    = 350;
constexpr uint16_t kAccelTensAt     = 10;
constexpr uint16_t kAccelHundredsAt = 36;

uint32_t g_accel_last_ms = 0;
uint16_t g_accel_streak  = 0;

void accel_reset() {
    g_accel_streak = 0;
}

// Call on every rotation detent; returns the step multiplier to apply.
int32_t accel_note_rotation() {
    const uint32_t now = now_ms();
    if (now - g_accel_last_ms > kAccelResetMs) g_accel_streak = 0;
    if (g_accel_streak < UINT16_MAX) g_accel_streak++;
    g_accel_last_ms = now;
    if (g_accel_streak >= kAccelHundredsAt) return 100;
    if (g_accel_streak >= kAccelTensAt) return 10;
    return 1;
}

// ── Alphabet for string editing ─────────────────────────────────────────────
constexpr const char kAlphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz"
                                   "0123456789"
                                   "-_.";
constexpr int kAlphabetLen       = sizeof(kAlphabet) - 1;  // 66

// Transient sentinel that rotates in after the last real glyph: choosing it
// ends (and saves) the string at the cursor. Never stored in a committed name.
constexpr char kStrEnd  = '\x01';
constexpr int kStrCycle = kAlphabetLen + 1;  // last slot = end marker

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
    case led::Protocol::DMX512: return "DMX512";
    case led::Protocol::Off: return "Off";
    default: return "?";
    }
}

// Channel badge colour mirrors the wiring family: clocked SPI strips and DMX512
// stand out from the common NRZ pixels; a disabled channel is greyed out.
Color badge_color(led::Protocol p) {
    if (led::is_off(p)) return color::DarkGray;
    if (led::is_dmx(p)) return color::Orange;
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

// Clocked-SPI rates the 16 MHz bus can actually realise (f = 16 MHz / spc, spc
// even): 8/4/2/1 MHz are the clean integer-divisor steps (spc 2/4/8/16). The
// picker offers only these so every shown value maps exactly to itself instead
// of being silently rounded by led::timing_for. APA102/SK9822 ≤30 MHz, LPD8806
// ≤20 MHz per the datasheets, so all four are in spec.
constexpr int32_t kClockChoices[] = { 1'000'000, 2'000'000, 4'000'000, 8'000'000 };
constexpr int kClockChoiceCount   = static_cast<int>(sizeof(kClockChoices) /
                                                     sizeof(kClockChoices[0]));

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
    }
}

// ── List rendering with scrolling viewport ─────────────────────────────────

struct ListItem {
    const char* label;
    const char* value;
    int8_t badge    = -1;  // ≥0 → draw a numbered badge (badge+1)
    Color badge_col = color::BadgeGreen;
    Color value_col = color::Gold;
    bool back       = false;  // "return to parent" row → back-arrow glyph, no brackets
};

// A node row's click action. Non-capturing lambdas convert to this pointer, so
// each build_* wires actions inline without a giant union. `idx` is the row
// index within the node (used by the dynamic lists: channels, scenes, files).
using OnClick = void (*)(uint8_t idx);

// Widest node: the main menu (8 channels + 6 entries). Build buffers size to it.
constexpr uint8_t kMaxRows = 16;

// A "return to the parent menu" row. Rendered with a left back-arrow glyph
// instead of bracketed text; `label` lets Main say "HOME" and the test-pattern
// node say "Stop & back".
ListItem back_item(const char* label = "Back") {
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
uint8_t viewport_first(uint8_t cursor, uint8_t count, uint8_t visible) {
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
Color fade_gray(int d) {
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
void draw_list_item_col(const ListItem& it, bool sel, int x0, int colW, int ry, bool lastRow) {
    const Color bg = sel ? color::SelBg : color::Black;
    if (sel) canvas_fill_rect(x0, ry, colW, kRowH, color::SelBg);
    if (!lastRow) canvas_hline(x0, ry + kRowH - 1, colW, color::Hair);
    if (sel) canvas_fill_round_rect(x0, ry + 3, 3, kRowH - 6, 1, color::GoodBright);

    const bool terminal = it.back || (it.label[0] == '[');
    int x               = x0 + kIndent;

    if (it.back) {
        draw_back_arrow(x, ry, kRowH, color::FrogLine);
        text_body(x + 13, ry, kRowH, it.label, color::FrogLine, bg);
        return;
    }
    if (it.badge >= 0) {
        const int cy        = ry + (kRowH - kChip) / 2;
        const Color num_col = (it.badge_col == color::DarkGray) ? color::DimGreen : color::Black;
        canvas_fill_round_rect_aa(x, cy, kChip, kChip, 4, it.badge_col, bg);
        char n[4];
        std::snprintf(n, sizeof(n), "%d", it.badge + 1);
        canvas_draw_text_f(x + (kChip - small_w(n)) / 2, cy + (kChip - kFontHeight) / 2, n, num_col,
                           color::Transparent, FontId::Small);
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
    // ── NV3007 landscape: two-column list (4 rows × 2 cols, 8 visible) ─────────
    canvas_clear(color::Black);
    draw_tft_header(title);

    // Scroll so the cursor stays in view (design ListView window logic).
    int first = 0;
    if (count > kMaxVis) {
        if (cursor >= kMaxVis) first = cursor - (kMaxVis - 1);
        if (first + kMaxVis > count) first = count - kMaxVis;
    }
    for (int p = 0; p < kMaxVis && first + p < count; ++p) {
        const int idx = first + p;
        const int col = p / kListRows;
        const int row = p % kListRows;
        const int x0  = col * kColW;
        const int ry  = kHdrH + row * kRowH;
        draw_list_item_col(items[idx], idx == cursor, x0, kColW, ry, row == kListRows - 1);
    }
    // Divider between columns.
    canvas_vline(kColW, kHdrH, kTH - kHdrH, color::Hair);
    // Scrollbar (design ScrollBar): track + proportional thumb on the far right.
    if (count > kMaxVis) {
        const int trackH = kTH - kHdrH - 6;
        int thumbH       = trackH * kMaxVis / count;
        if (thumbH < 12) thumbH = 12;
        const int travel = count - kMaxVis;
        const int y      = kHdrH + 3 + (travel > 0 ? (trackH - thumbH) * first / travel : 0);
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

// ── HOME ────────────────────────────────────────────────────────────────────

void render_home() {
    char line[48];
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 landscape 428×142 dashboard (pixfrog-screen.jsx Home) ──────────
    // Status block (2 rows) | green rule | 8 channels in 2 columns × 4 rows.
    constexpr int kStatusH = 41;                  // status block incl. rule
    constexpr int kChTop   = kStatusH;            // channel grid top
    constexpr int kChH     = (kTH - kChTop) / 4;  // 25px per channel row
    constexpr int kCellW   = kTW / 2;             // 214px per channel column
    const int sfh          = kFontHeight;         // small-font height (8)

    canvas_clear(color::Black);
    const auto stats = dmx::get_stats();
    const auto& g    = config::get_global();
    const bool nvs   = config::is_persistence_ok();

    // ── Status row 1: frog + pixfrog + 8CH NODE … fps + LINK ──────────────────
    {
        const int band = 3, h = 15;
        draw_frog(kPadX, band + (h - 14) / 2, 14, nvs ? color::FrogLine : color::BadCoral,
                  color::Black);
        int x = kPadX + 14 + 7;
        text_body(x, band, h, "pixfrog", color::White);
        x += body_w("pixfrog") + 7;
        canvas_draw_text_f(x, band + (h - sfh) / 2, "8CH NODE", color::DimGreen, color::Black,
                           FontId::Small);
        // Right: LINK/DOWN chip, fps to its left.
        const bool up        = ui::is_link_up();
        const char* link_txt = up ? "LINK" : "DOWN";
        const int chipw      = small_w(link_txt) + 8;
        int rx               = kTW - kPadX - chipw;
        draw_chip(rx, band + (h - 12) / 2, link_txt, true, up ? color::FrogLine : color::BadCoral,
                  color::Black);
        std::snprintf(line, sizeof(line), "%lu", static_cast<unsigned long>(stats.current_fps));
        rx -= 6 + small_w("fps");
        canvas_draw_text_f(rx, band + (h - sfh) / 2, "fps", color::DimGreen, color::Black,
                           FontId::Small);
        rx -= 2 + body_w(line);
        text_body(rx, band, h, line, color::Gold);
    }

    // ── Status row 2: IP + chips … RX <pkts> ──────────────────────────────────
    {
        const int band = 20, h = 14;
        int x             = kPadX;
        const uint32_t ip = ui::get_ip();
        if (ip == 0) {
            text_body(x, band, h, "0.0.0.0", color::White);
            x += body_w("0.0.0.0") + 7;
        } else {
            std::snprintf(
                line, sizeof(line), "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
                static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
                static_cast<unsigned>(ip & 0xFFu));
            text_body(x, band, h, line, color::White);
            x += body_w(line) + 7;
        }
        const int cy = band + (h - 12) / 2;
        x = draw_chip(x, cy, g.use_dhcp ? "DHCP" : "STATIC", true, color::DimGreen, color::Black);
        x = draw_chip(x, cy, "sACN", g.sacn_enabled, color::FrogLine, color::Black);
        x = draw_chip(x, cy, "WEB", g.web_enabled, color::FrogLine, color::Black);
        std::snprintf(line, sizeof(line), "%uHz", g.refresh_rate_hz);
        draw_chip(x, cy, line, true, color::Gold, color::Black);
        // Right: RX <pkts>
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%llu",
                      static_cast<unsigned long long>(stats.artnet_packets_rx));
        int rx = kTW - kPadX - body_w(cnt);
        text_body(rx, band, h, cnt, color::Cream);
        rx -= 4 + small_w("RX");
        canvas_draw_text_f(rx, band + (h - sfh) / 2, "RX", color::DimGreen, color::Black,
                           FontId::Small);
    }
    canvas_hline(0, kStatusH - 1, kTW, color::FrogLine);

    // ── Channel grid: 8 single-line cells, 2 columns × 4 rows ─────────────────
    for (int i = 0; i < 8; ++i) {
        const int col  = i / 4;
        const int row  = i % 4;
        const int x0   = col * kCellW;
        const int cy   = kChTop + row * kChH;
        const auto& cc = config::get_channel(i);
        const bool off = led::is_off(cc.protocol);
        const bool dmx = led::is_dmx(cc.protocol);
        const bool ok  = dmx::is_channel_capacity_ok(i);

        if (row != 3) canvas_hline(x0, cy + kChH - 1, kCellW, color::Hair);
        const int syc = cy + (kChH - sfh) / 2;  // small-font y, centred

        // Chip with channel number.
        const int chcy      = cy + (kChH - kChip) / 2;
        const Color fam     = badge_color(cc.protocol);
        const Color num_col = off ? color::DimGreen : color::Black;
        canvas_fill_round_rect_aa(x0 + 9, chcy, kChip, kChip, 4, fam, color::Black);
        char n[4];
        std::snprintf(n, sizeof(n), "%d", i + 1);
        canvas_draw_text_f(x0 + 9 + (kChip - small_w(n)) / 2, chcy + (kChip - sfh) / 2, n, num_col,
                           color::Transparent, FontId::Small);

        // Protocol name (Body).
        text_body(x0 + 9 + kChip + 6, cy, kChH, protocol_name(cc.protocol),
                  off ? color::DimGreen : color::Cream);

        // Right-aligned columns: dot/! | bri | pixels | universe.
        int rx = x0 + kCellW - 9;
        // activity dot or conflict mark
        if (!ok) {
            canvas_draw_text_f(rx - small_w("!"), syc, "!", color::BadCoral, color::Black,
                               FontId::Small);
        } else if (!off) {
            const bool act     = dmx::is_channel_active(i);
            const bool fsafe   = dmx::is_channel_failsafe(i);
            const Color dotcol = fsafe ? color::Orange : act ? color::GoodBright : color::IdleGreen;
            canvas_fill_round_rect_aa(rx - 7, cy + (kChH - 7) / 2, 7, 7, 3, dotcol, color::Black);
        }
        rx -= 9;
        if (!off) {
            if (!dmx) {
                std::snprintf(line, sizeof(line), "%u%%",
                              (static_cast<unsigned>(cc.brightness) * 100u + 127u) / 255u);
                canvas_draw_text_f(rx - small_w(line), syc, line, color::Cream, color::Black,
                                   FontId::Small);
            }
            rx -= 32;
            std::snprintf(line, sizeof(line), "%u", cc.pixel_count);
            canvas_draw_text_f(rx - small_w(line), syc, line, color::DimGreen, color::Black,
                               FontId::Small);
            rx -= 34;
            std::snprintf(line, sizeof(line), "%u", cc.universe_start);
            canvas_draw_text_f(rx - small_w(line), syc, line, ok ? color::Gold : color::BadCoral,
                               color::Black, FontId::Small);
        }
    }
    canvas_vline(kCellW, kChTop, kTH - kChTop, color::Hair);
#else
    // ── TFT dashboard ────────────────────────────────────────────────────────
    // Header 28 | network 24 | services 18 | column header 14 | 8×19 channels.
    constexpr int kInfoH   = 24;
    constexpr int kSvcH    = 18;
    constexpr int kColHdrH = 14;
    constexpr int kChH     = 19;

    // Channel-table columns (shared by the header labels and the rows).
    constexpr int kColBadge  = 6;
    constexpr int kColProto  = 30;
    constexpr int kColUniEnd = 158;
    constexpr int kColPixEnd = 222;
    constexpr int kColBriEnd = 286;
    constexpr int kDotD      = 10;
    constexpr int kColDot    = kTW - kIndent - kDotD - 4;

    canvas_clear(color::Black);

    const auto stats = dmx::get_stats();
    const auto& g    = config::get_global();

    // ── Header: green wordmark + version, FPS + link pill on the right ──────
    canvas_fill_rect(0, 0, kTW, kHdrH - 2, color::HeaderBg);
    canvas_fill_rect(0, kHdrH - 2, kTW, 2, color::FrogLine);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, "pixfrog", color::FrogLine, color::HeaderBg,
                     kTxtSc);
    const int after_mark = kIndent + 7 * kFontCellWidth * kTxtSc + 8;
    if (!config::is_persistence_ok()) {
        draw_pill(after_mark, (kHdrH - 2 - kPillH) / 2, "NVS!", color::Red, color::Black,
                  color::HeaderBg);
    } else {
        char ver[13];
        truncate(ver, sizeof(ver), fw_version());
        // Small version string, baseline-aligned with the wordmark.
        canvas_draw_text(after_mark, (kHdrH - kTxtH) / 2 + 6, ver, color::DarkGray, color::HeaderBg,
                         1);
    }
    int hx = kTW - kIndent;
    if (ui::is_link_up()) {
        hx -= pill_width("LINK");
        draw_pill(hx, (kHdrH - 2 - kPillH) / 2, "LINK", color::BadgeGreen, color::Black,
                  color::HeaderBg);
    } else {
        hx -= pill_width("NO LINK");
        draw_pill(hx, (kHdrH - 2 - kPillH) / 2, "NO LINK", color::Red, color::Black,
                  color::HeaderBg);
    }
    std::snprintf(line, sizeof(line), "%lufps", static_cast<unsigned long>(stats.current_fps));
    draw_text_r(hx - 8, (kHdrH - kTxtH) / 2, line, color::Gold, color::HeaderBg);

    // ── Network row: IP + addressing mode, ArtNet packet counter ────────────
    int ry = kHdrH;
    canvas_fill_rect(0, ry, kTW, kInfoH, color::AltRowBg);
    const int iy      = ry + (kInfoH - kTxtH) / 2;
    const uint32_t ip = ui::get_ip();
    int ip_end        = kIndent;
    if (ip == 0) {
        canvas_draw_text(kIndent, iy, "no address", color::DarkGray, color::AltRowBg, kTxtSc);
        ip_end += 10 * kFontCellWidth * kTxtSc;
    } else {
        std::snprintf(line, sizeof(line), "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
                      static_cast<unsigned>((ip >> 16) & 0xFFu),
                      static_cast<unsigned>((ip >> 8) & 0xFFu), static_cast<unsigned>(ip & 0xFFu));
        canvas_draw_text(kIndent, iy, line, color::White, color::AltRowBg, kTxtSc);
        ip_end += static_cast<int>(std::strlen(line)) * kFontCellWidth * kTxtSc;
    }
    canvas_draw_text(ip_end + 8, iy + 6, g.use_dhcp ? "DHCP" : "STATIC", color::DarkGray,
                     color::AltRowBg, 1);
    char cnt[24];
    fmt_count(cnt, sizeof(cnt), stats.artnet_packets_rx);
    const int cnt_x = draw_text_r(kTW - kIndent, iy, cnt, color::LightGray, color::AltRowBg);
    canvas_draw_text(cnt_x - 2 * kFontCellWidth - 6, iy + 6, "RX", color::DarkGray, color::AltRowBg,
                     1);

    // ── Services row: opt-in listeners + the live playback source ───────────
    ry           += kInfoH;
    int px        = kIndent;
    const int py  = ry + (kSvcH - kPillH) / 2;
    px            = draw_pill(px, py, "sACN", g.sacn_enabled ? color::BadgeGreen : color::HeaderBg,
                   g.sacn_enabled ? color::Black : color::DarkGray, color::Black);
    px            = draw_pill(px, py, "WEB", g.web_enabled ? color::BadgeGreen : color::HeaderBg,
                   g.web_enabled ? color::Black : color::DarkGray, color::Black);
    std::snprintf(line, sizeof(line), "%uHz", g.refresh_rate_hz);
    draw_pill(px, py, line, color::HeaderBg, color::DarkGray, color::Black);
    const int scene = dmx::active_scene();
    if (output::get_calibration_mode() >= 0) {
        draw_text_r(kTW - kIndent, ry, "TEST PATTERN", color::Orange, color::Black);
    } else if (dmx::fseq_is_active()) {
        draw_text_r(kTW - kIndent, ry, "FSEQ PLAYING", color::Gold, color::Black);
    } else if (scene >= 0) {
        char nm[13];
        truncate(nm, sizeof(nm), config::get_scene(static_cast<size_t>(scene)).name);
        std::snprintf(line, sizeof(line), ">%s", nm);
        draw_text_r(kTW - kIndent, ry, line, color::Gold, color::Black);
    }

    // ── Channel table ────────────────────────────────────────────────────────
    ry           += kSvcH;
    const int ly  = ry + (kColHdrH - 8) / 2 - 1;
    canvas_draw_text(kColBadge, ly, "CH", color::DarkGray, color::Black, 1);
    canvas_draw_text(kColProto, ly, "PROTOCOL", color::DarkGray, color::Black, 1);
    canvas_draw_text(kColUniEnd - 3 * kFontCellWidth, ly, "UNI", color::DarkGray, color::Black, 1);
    canvas_draw_text(kColPixEnd - 3 * kFontCellWidth, ly, "PIX", color::DarkGray, color::Black, 1);
    canvas_draw_text(kColBriEnd - 3 * kFontCellWidth, ly, "BRI", color::DarkGray, color::Black, 1);
    canvas_draw_text(kColDot + kDotD - 3 * kFontCellWidth, ly, "ACT", color::DarkGray, color::Black,
                     1);
    canvas_hline(0, ry + kColHdrH - 1, kTW, color::DarkGray);

    ry += kColHdrH;
    for (int i = 0; i < 8; ++i) {
        const int cy       = ry + i * kChH;
        const Color row_bg = (i & 1) ? color::AltRowBg : color::Black;
        canvas_fill_rect(0, cy, kTW, kChH, row_bg);

        const auto& cc = config::get_channel(i);
        const bool off = led::is_off(cc.protocol);
        const int ty   = cy + (kChH - kTxtH) / 2 + kTxtYBias;

        // Numbered badge, colour-coded by wiring family (grey when disabled).
        draw_badge(kColBadge, cy + 1, kChH - 3, i + 1, badge_color(cc.protocol),
                   off ? color::LightGray : color::Black, row_bg);

        canvas_draw_text(kColProto, ty, protocol_name(cc.protocol),
                         off ? color::DarkGray : color::Cream, row_bg, kTxtSc);

        const bool ok    = dmx::is_channel_capacity_ok(i);
        const bool fsafe = dmx::is_channel_failsafe(i);
        if (!off) {
            std::snprintf(line, sizeof(line), "%u", cc.universe_start);
            draw_text_r(kColUniEnd, ty, line, ok ? color::Gold : color::Red, row_bg);
            std::snprintf(line, sizeof(line), "%u", cc.pixel_count);
            draw_text_r(kColPixEnd, ty, line, color::LightGray, row_bg);
            if (!led::is_dmx(cc.protocol)) {
                std::snprintf(line, sizeof(line), "%u%%",
                              (static_cast<unsigned>(cc.brightness) * 100u + 127u) / 255u);
                draw_text_r(kColBriEnd, ty, line, color::LightGray, row_bg);
            }
        }

        // Activity LED: red = over capacity, orange = failsafe, green = live DMX.
        const Color act_col = off                       ? color::HeaderBg
                            : !ok                       ? color::Red
                            : fsafe                     ? color::Orange
                            : dmx::is_channel_active(i) ? color::FrogLine
                                                        : color::DarkGray;
        canvas_fill_round_rect_aa(kColDot, cy + (kChH - kDotD) / 2, kDotD, kDotD, kDotD / 2,
                                  act_col, row_bg);
    }
#endif  // !CONFIG_PIXFROG_DISPLAY_NV3007
#else
    // ── OLED classic home ─────────────────────────────────────────────────────
    canvas_clear();

    // Item B6: warn loudly when NVS is broken (config does NOT persist).
    if (!config::is_persistence_ok()) {
        draw_row(0, 0, "pixfrog       [!NVS]");
    } else {
        draw_row(0, 0, "pixfrog");
    }

    const uint32_t ip = ui::get_ip();
    if (ip == 0) {
        draw_row(1, 0, "IP   : ---.---.---.---");
    } else {
        std::snprintf(line, sizeof(line), "IP   : %u.%u.%u.%u",
                      static_cast<unsigned>((ip >> 24) & 0xFFu),
                      static_cast<unsigned>((ip >> 16) & 0xFFu),
                      static_cast<unsigned>((ip >> 8) & 0xFFu), static_cast<unsigned>(ip & 0xFFu));
        draw_row(1, 0, line);
    }

    // Item B4: link state, distinct from IP since DHCP can be pending.
    draw_row(2, 0, ui::is_link_up() ? "LINK : UP" : "LINK : DOWN");

    const auto stats = dmx::get_stats();
    std::snprintf(line, sizeof(line), "FPS  : %lu", static_cast<unsigned long>(stats.current_fps));
    draw_row(3, 0, line);
    std::snprintf(line, sizeof(line), "Pkts : %llu",
                  static_cast<unsigned long long>(stats.artnet_packets_rx));
    draw_row(4, 0, line);

    draw_row(5, 0, "CH 1 2 3 4 5 6 7 8");
    char ch_line[kOledCols + 1];
    ch_line[0] = ' ';
    ch_line[1] = ' ';
    ch_line[2] = ' ';
    for (int i = 0; i < 8; ++i) {
        // Priority: o (disabled) > ! (over-capacity) > * (recent DMX) > - (idle).
        char glyph = '-';
        if (led::is_off(config::get_channel(i).protocol))
            glyph = 'o';
        else if (!dmx::is_channel_capacity_ok(i))
            glyph = '!';
        else if (dmx::is_channel_failsafe(i))
            glyph = 'F';
        else if (dmx::is_channel_active(i))
            glyph = '*';
        ch_line[3 + i * 2]     = glyph;
        ch_line[3 + i * 2 + 1] = ' ';
    }
    ch_line[3 + 8 * 2 - 1] = '\0';
    draw_row(6, 0, ch_line);
#endif
}

// ── MAIN MENU ───────────────────────────────────────────────────────────────

// Main menu layout: the 8 channels first (direct access, with protocol badges),
// then the grouped config sub-menus, About, and the way back to HOME.
constexpr uint8_t kChannelRows = config::kNumChannels;  // 8

uint8_t build_main(ListItem* items, OnClick* fns) {
    static char names[kChannelRows][12];
    for (uint8_t i = 0; i < kChannelRows; ++i) {
        // Channel rows: show the configured protocol + a numbered badge.
        // A disabled channel ("Off") is greyed out instead of gold.
        std::snprintf(names[i], sizeof(names[i]), "Channel %u", i + 1);
        const auto& cc     = config::get_channel(i);
        items[i]           = {};
        items[i].label     = names[i];
        items[i].value     = protocol_name(cc.protocol);  // points to static string
        items[i].badge     = static_cast<int8_t>(i);
        items[i].badge_col = badge_color(cc.protocol);
        items[i].value_col = led::is_off(cc.protocol) ? color::DarkGray : color::Gold;
        fns[i]             = open_channel;
    }
    uint8_t n = kChannelRows;
    items[n]  = { "Inputs", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Inputs); };
    items[n]  = { "Network", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Network); };
    items[n]  = { "Output", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Output); };
    items[n]  = { "Playback", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Playback); };
    items[n]  = { "About", "" };
    fns[n++]  = [](uint8_t) { s.screen = Screen::About; };
    items[n]  = back_item("HOME");
    fns[n++]  = [](uint8_t) { go_back(); };
    return n;
}

// ── ABOUT ───────────────────────────────────────────────────────────────────

void render_about() {
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: frog mark + wordmark + one-liners ──────────────────────
    canvas_clear(color::Black);
    draw_tft_header("ABOUT");
    const int cy = (kHdrH + kTH) / 2;
    draw_frog(kPadX + 6, cy - 23, 46, color::FrogLine, color::Black);
    const int tx = kPadX + 6 + 46 + 22;
    canvas_draw_text_f(tx, cy - 22, "pixfrog", color::FrogLine, color::Black, FontId::Mega);
    text_body(tx, cy + 8, 14, "8-channel ArtNet . sACN node", color::Cream);
    canvas_draw_text_f(tx, cy + 24, fw_build_info(), color::DimGreen, color::Black, FontId::Small);
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    canvas_clear(color::Black);
    draw_tft_header("ABOUT");

    int y = kHdrH + 20;
    canvas_draw_text(kIndent, y, "pixfrog", color::FrogLine, color::Black, kTxtSc);
    y += kTxtH + 10;
    canvas_draw_text(kIndent, y, fw_version(), color::Gold, color::Black, kTxtSc);
    y += kTxtH + 10;
    canvas_draw_text(kIndent, y, fw_build_info(), color::LightGray, color::Black, 1);
#else
    canvas_clear();
    draw_row(0, 0, "ABOUT");
    draw_row(2, 0, "pixfrog");
    char line[kOledCols + 1];
    truncate(line, sizeof(line), fw_version());
    draw_row(3, 0, line);
    truncate(line, sizeof(line), fw_build_info());
    draw_row(5, 0, line);
#endif
}

void dispatch_about(Event e) {
    // Return to the main menu list; the node engine restores the cursor on the
    // "About" row automatically.
    if (e == Event::Click) s.screen = Screen::Menu;
}

// ── TEST PATTERN NODE (TODO B5) ─────────────────────────────────────────────

constexpr const char* kTestPatternLabels[3] = {
    "Pat 0 sq 1kHz",
    "Pat 1 walk-1",
    "Pat 2 altern.",
};

uint8_t build_testpattern(ListItem* items, OnClick* fns) {
    static char marked[3][24];
    const int8_t active = output::get_calibration_mode();
    for (uint8_t i = 0; i < 3; ++i) {
        if (active == static_cast<int8_t>(i)) {
            std::snprintf(marked[i], sizeof(marked[i]), "%s *", kTestPatternLabels[i]);
            items[i] = { marked[i], "" };
        } else {
            items[i] = { kTestPatternLabels[i], "" };
        }
        // Activate the chosen pattern; stay in the node so the user can switch
        // patterns without leaving.
        fns[i] = [](uint8_t idx) { output::set_calibration_mode(static_cast<int8_t>(idx)); };
    }
    items[3] = back_item("Stop & back");
    fns[3]   = [](uint8_t) {
        output::set_calibration_mode(-1);
        go_back();
    };
    return 4;
}

// ── SCENES NODE ──────────────────────────────────────────────────────────────
// Play/stop only — editing colours and masks on a rotary encoder is web/UART
// territory. The active scene is starred.

uint8_t build_scenes(ListItem* items, OnClick* fns) {
    static char marked[config::kNumScenes][kOledCols + 1];
    const int active = dmx::active_scene();
    for (uint8_t i = 0; i < config::kNumScenes; ++i) {
        const auto& sc = config::get_scene(i);
        if (active == static_cast<int>(i)) {
            std::snprintf(marked[i], sizeof(marked[i]), "%s *", sc.name);
            items[i] = { marked[i], "" };
        } else {
            items[i] = { sc.name, "" };
        }
        fns[i] = [](uint8_t idx) { dmx::scene_start(idx); };  // stay to switch scenes
    }
    uint8_t n = config::kNumScenes;
    items[n]  = { "[Stop]", "" };
    fns[n++]  = [](uint8_t) { dmx::scene_stop(); };
    items[n]  = back_item();
    fns[n++]  = [](uint8_t) { go_back(); };
    return n;
}

// ── FSEQ NODE ────────────────────────────────────────────────────────────────
// Shows .fseq files on the SD card. Clicking a filename starts playback. The
// file list is cached (g_fseq_names) when the node is opened from Playback.

uint8_t build_fseq(ListItem* items, OnClick* fns) {
    const bool no_card  = (fseq::sd_state() == fseq::SdState::Absent);
    const uint8_t n     = no_card ? 0 : g_fseq_file_count;
    const char* playing = fseq::active_file();
    if (n == 0) {
        items[0] = { no_card ? "No SD card" : "No .fseq files", "" };
        fns[0]   = nullptr;  // inert note
        items[1] = back_item();
        fns[1]   = [](uint8_t) { go_back(); };
        return 2;
    }
    static char marked[kFseqMenuMaxFiles][fseq::kMaxNameLen + 3];
    for (uint8_t i = 0; i < n; ++i) {
        if (playing && strcmp(g_fseq_names[i], playing) == 0) {
            snprintf(marked[i], sizeof(marked[i]), "%.*s *",
                     static_cast<int>(fseq::kMaxNameLen) - 1, g_fseq_names[i]);
            items[i] = { marked[i], "" };
        } else {
            items[i] = { g_fseq_names[i], "" };
        }
        fns[i] = [](uint8_t idx) { fseq::start(g_fseq_names[idx]); };
    }
    items[n]     = { "[Stop]", "" };
    fns[n]       = [](uint8_t) { fseq::stop(); };
    items[n + 1] = back_item();
    fns[n + 1]   = [](uint8_t) { go_back(); };
    return static_cast<uint8_t>(n + 2);
}

// ── EDIT VALUE — generic int / bool / enum ──────────────────────────────────

void enter_edit(Field field, ValueKind kind, int32_t cur, int32_t mn, int32_t mx, int32_t step,
                const char* label, Screen return_screen, uint8_t channel = 0xFF) {
    s.edit.field         = field;
    s.edit.kind          = kind;
    s.edit.current       = cur;
    s.edit.original      = cur;
    s.edit.min           = mn;
    s.edit.max           = mx;
    s.edit.step          = step;
    s.edit.label         = label;
    s.edit.return_screen = return_screen;
    s.edit.channel       = channel;
    s.screen             = Screen::EditValue;
    accel_reset();
}

void render_edit_value() {
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: EDIT · <label> with a Mega cyan value ──────────────────
    canvas_clear(color::Black);
    char hdr[40];
    if (s.edit.channel != 0xFF)
        std::snprintf(hdr, sizeof(hdr), "CH%u  %s", s.edit.channel + 1, s.edit.label);
    else
        std::snprintf(hdr, sizeof(hdr), "EDIT  %s", s.edit.label);
    draw_tft_header(hdr);

    const bool gauge = (s.edit.kind == ValueKind::Int || s.edit.kind == ValueKind::ClockHz);
    const bool boolf = (s.edit.kind == ValueKind::Bool);
    const bool enumf = (s.edit.kind == ValueKind::Protocol ||
                        s.edit.kind == ValueKind::ColorOrder || s.edit.kind == ValueKind::Failsafe);
    const int mh     = canvas_font_h(FontId::Mega);

    if (enumf) {
        // Horizontal wheel: centre option big (Mega cyan in a pill), neighbours
        // shrink outward. Gold dot above the option equal to the original value.
        const int cy  = (kHdrH + (kTH - kFootH)) / 2;
        const int gap = 18;
        struct Cell {
            char txt[24];
            FontId f;
            int w;
            int32_t v;
        };
        Cell cells[5];
        int nc = 0;
        for (int off = -2; off <= 2; ++off) {
            const int32_t v = s.edit.current + off;
            if (v < s.edit.min || v > s.edit.max) continue;
            Cell& c = cells[nc];
            format_value(s.edit, v, c.txt, sizeof(c.txt));
            c.f = (off == 0)              ? FontId::Mega
                : (off == -1 || off == 1) ? FontId::Body
                                          : FontId::Small;
            c.w = canvas_text_w(c.txt, c.f);
            c.v = v;
            ++nc;
        }
        int total = 0;
        for (int i = 0; i < nc; ++i)
            total += cells[i].w + (i ? gap : 0);
        int x = (kTW - total) / 2;
        for (int i = 0; i < nc; ++i) {
            Cell& c        = cells[i];
            const int chh  = canvas_font_h(c.f);
            const int yy   = cy - chh / 2;
            const bool ctr = (c.f == FontId::Mega);
            if (ctr) {
                canvas_fill_round_rect_aa(x - 12, yy - 5, c.w + 24, chh + 10, 8, color::SelBg,
                                          color::Black);
                canvas_draw_text_f(x, yy, c.txt, color::EditCyan, color::Transparent, c.f);
            } else {
                const Color col = (c.f == FontId::Body) ? color::Cream : color::DimGreen;
                canvas_draw_text_f(x, yy, c.txt, col, color::Black, c.f);
            }
            if (c.v == s.edit.original)
                canvas_fill_round_rect_aa(x + c.w / 2 - 3, yy - 11, 6, 6, 3, color::Gold,
                                          color::Black);
            x += c.w + gap;
        }
        draw_hint_bar("adjust", "apply", "cancel");
    } else if (boolf) {
        const char* v = s.edit.current ? "ON" : "OFF";
        const int w   = canvas_text_w(v, FontId::Mega);
        canvas_draw_text_f((kTW - w) / 2, (kHdrH + (kTH - kFootH)) / 2 - mh / 2, v, color::EditCyan,
                           color::Black, FontId::Mega);
        draw_hint_bar("toggle", "apply", "cancel");
    } else {
        // Numeric gauge.
        char val[24];
        format_value(s.edit, s.edit.current, val, sizeof(val));
        const int vw = canvas_text_w(val, FontId::Mega);
        canvas_draw_text_f((kTW - vw) / 2, kHdrH + 6, val, color::EditCyan, color::Black,
                           FontId::Mega);
        if (gauge) {
            const int gx = 30, gw = kTW - 60, gy = kHdrH + 6 + mh + 4, gh = 10;
            canvas_fill_round_rect(gx, gy, gw, gh, 5, color::IdleGreen);
            long span = static_cast<long>(s.edit.max) - s.edit.min;
            if (span < 1) span = 1;
            long fw = static_cast<long>(gw) * (s.edit.current - s.edit.min) / span;
            if (fw < 4) fw = 4;
            if (fw > gw) fw = gw;
            canvas_fill_round_rect(gx, gy, static_cast<int>(fw), gh, 5, color::FrogLine);
            if (s.edit.current != s.edit.original) {
                long ofw = static_cast<long>(gw) * (s.edit.original - s.edit.min) / span;
                if (ofw < 0) ofw = 0;
                if (ofw > gw) ofw = gw;
                const int ox = gx + static_cast<int>(ofw);
                for (int r = 0; r < 5; ++r)
                    canvas_hline(ox - r, gy + gh + 1 + r, 2 * r + 1, color::Gold);
            }
            char lo[16], hi[16];
            format_value(s.edit, s.edit.min, lo, sizeof(lo));
            format_value(s.edit, s.edit.max, hi, sizeof(hi));
            canvas_draw_text_f(gx, gy + gh + 8, lo, color::DimGreen, color::Black, FontId::Small);
            canvas_draw_text_f(gx + gw - small_w(hi), gy + gh + 8, hi, color::DimGreen,
                               color::Black, FontId::Small);
            if (s.edit.current != s.edit.original) {
                char orig[24], was[32];
                format_value(s.edit, s.edit.original, orig, sizeof(orig));
                std::snprintf(was, sizeof(was), "was: %s", orig);
                canvas_draw_text_f((kTW - small_w(was)) / 2, gy + gh + 8, was, color::Gold,
                                   color::Black, FontId::Small);
            }
        }
        draw_hint_bar("adjust", "apply", "cancel");
    }
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    canvas_clear(color::Black);
    char hdr[32];
    if (s.edit.channel != 0xFF)
        std::snprintf(hdr, sizeof(hdr), "CH%u: %s", s.edit.channel + 1, s.edit.label);
    else
        std::snprintf(hdr, sizeof(hdr), "EDIT %s", s.edit.label);
    draw_tft_header(hdr);

    const bool gauge = (s.edit.kind == ValueKind::Int || s.edit.kind == ValueKind::ClockHz);
    const bool enumf = (s.edit.kind == ValueKind::Protocol ||
                        s.edit.kind == ValueKind::ColorOrder || s.edit.kind == ValueKind::Failsafe);

    if (enumf) {
        // A list-of-values picker rendered as a spinning "wheel": the current
        // choice is centred in the XL cell (font 2), its immediate neighbours in
        // the large cell (font 1) and the rest in the small cell (font 0) — sizes
        // run 0-0-1-2-1-0-0 top-to-bottom, so the selection reads big like the
        // detent of a physical wheel. A small orange dot flags the original value
        // so the pending change stays clear.
        const int cx   = kTW / 2;
        const int midY = (kHdrH + (kTH - 20)) / 2;
        // Row-centre y by |distance| from the selection: gaps shrink outward so
        // the rows pack toward the rim like a wheel curving away.
        auto row_center_y = [&](int off) {
            const int a = off < 0 ? -off : off;
            const int d = (a == 0) ? 0 : (a == 1) ? 38 : (a == 2) ? 56 : 68;
            return midY + (off < 0 ? -d : d);
        };
        for (int off = -3; off <= 3; ++off) {
            const int32_t v = s.edit.current + off;
            if (v < s.edit.min || v > s.edit.max) continue;
            char buf[24];
            format_value(s.edit, v, buf, sizeof(buf));
            const int len = static_cast<int>(std::strlen(buf));
            const int y   = row_center_y(off);
            const int a   = off < 0 ? -off : off;
            if (a == 0) {
                // Font 2 (XL 18×24), highlighted on a rounded pill. Drawn with a
                // transparent bg so the glyphs composite over the pill's corners.
                const int w = canvas_text_xl_width(buf);
                canvas_fill_round_rect_aa(cx - w / 2 - 14, y - kFontXLHeight / 2 - 4, w + 28,
                                          kFontXLHeight + 8, 8, color::CursorBg, color::Black);
                canvas_draw_text_xl(cx - w / 2, y - kFontXLHeight / 2, buf, color::Cyan,
                                    color::Transparent);
            } else if (a == 1) {
                // Font 1 (large 12×16).
                const int w  = len * kFontCellWidth * kTxtSc;
                const int ty = y - kTxtH / 2;
                canvas_draw_text(cx - w / 2, ty, buf, fade_gray(a), color::Black, kTxtSc);
                if (v == s.edit.original)
                    canvas_fill_round_rect(cx - w / 2 - 14, ty + 5, 6, 6, 3, color::Orange);
            } else {
                // Font 0 (small 6×8).
                const int w  = len * kFontCellWidth;
                const int ty = y - kFontHeight / 2;
                canvas_draw_text(cx - w / 2, ty, buf, fade_gray(a), color::Black, 1);
                if (v == s.edit.original)
                    canvas_fill_round_rect(cx - w / 2 - 13, ty, 6, 6, 3, color::Orange);
            }
        }
    } else {
        // Numeric / bool: a large value, plus a fill gauge for numeric ranges.
        // Use the natively-rasterised 18x24 XL cell (crisp) rather than upscaling
        // the small cell, which looked pixelated.
        char val[24];
        format_value(s.edit, s.edit.current, val, sizeof(val));
        const int val_w = canvas_text_xl_width(val);
        canvas_draw_text_xl((kTW - val_w) / 2, kHdrH + 26, val, color::Cyan, color::Black);
        if (gauge) {
            const int gx = 44, gw = kTW - 88, gy = kHdrH + 78, gh = 12;
            canvas_fill_round_rect(gx, gy, gw, gh, 5, color::CursorBg);
            long span = static_cast<long>(s.edit.max) - s.edit.min;
            if (span < 1) span = 1;
            long fw = static_cast<long>(gw) * (s.edit.current - s.edit.min) / span;
            if (fw < 4) fw = 4;
            if (fw > gw) fw = gw;
            canvas_fill_round_rect(gx, gy, static_cast<int>(fw), gh, 5, color::FrogLine);
            // Orange up-triangle under the original value (mirrors the dropdown's
            // original-value dot) so you can see where you started from.
            if (s.edit.current != s.edit.original) {
                long ofw = static_cast<long>(gw) * (s.edit.original - s.edit.min) / span;
                if (ofw < 0) ofw = 0;
                if (ofw > gw) ofw = gw;
                const int ox = gx + static_cast<int>(ofw);
                for (int r = 0; r < 4; ++r)
                    canvas_hline(ox - r, gy + gh + 1 + r, 2 * r + 1, color::Orange);
            }
            char lo[16], hi[16];
            format_value(s.edit, s.edit.min, lo, sizeof(lo));
            format_value(s.edit, s.edit.max, hi, sizeof(hi));
            canvas_draw_text(gx, gy + gh + 10, lo, color::DarkGray, color::Black, 1);
            const int hiw = static_cast<int>(std::strlen(hi)) * kFontCellWidth;
            canvas_draw_text(gx + gw - hiw, gy + gh + 10, hi, color::DarkGray, color::Black, 1);
        }
        if (s.edit.current != s.edit.original) {
            char orig[24];
            format_value(s.edit, s.edit.original, orig, sizeof(orig));
            char was[32];
            std::snprintf(was, sizeof(was), "was: %s", orig);
            const int was_w = static_cast<int>(std::strlen(was)) * kFontCellWidth * kTxtSc;
            canvas_draw_text((kTW - was_w) / 2, kHdrH + 118, was, color::Orange, color::Black,
                             kTxtSc);
        }
    }

    // Footer: the encoder controls, so they're discoverable on every edit.
    canvas_fill_rect(0, kTH - 20, kTW, 20, color::HeaderBg);
    const char* hint = "TURN  adjust     PRESS  apply     HOLD  cancel";
    const int hw     = static_cast<int>(std::strlen(hint)) * kFontCellWidth;
    canvas_draw_text((kTW - hw) / 2, kTH - 14, hint, color::SplashSub, color::HeaderBg, 1);
#else
    canvas_clear();
    // Buffers are sized to hold the prefix plus a full kOledCols-wide value.
    char line[kOledCols + 4];
    if (s.edit.channel != 0xFF)
        std::snprintf(line, sizeof(line), "CH%u: %s", s.edit.channel + 1, s.edit.label);
    else
        std::snprintf(line, sizeof(line), "EDIT %s", s.edit.label);
    draw_row(0, 0, line);

    char val[kOledCols + 1];
    format_value(s.edit, s.edit.current, val, sizeof(val));
    std::snprintf(line, sizeof(line), "  %s", val);
    draw_row(3, 0, line);

    // Item B3: show the original (pre-edit) value when the user has moved
    // away from it, so it's clear the change is not yet committed.
    if (s.edit.current != s.edit.original) {
        char orig[kOledCols + 1];
        format_value(s.edit, s.edit.original, orig, sizeof(orig));
        char was[kOledCols + 8];
        std::snprintf(was, sizeof(was), "  was: %s", orig);
        draw_row(4, 0, was);
    }

    // Reachable range (int/clock) or list position (enum), then the controls.
    char ctx[40];  // wider than the row; draw_row clips to the panel
    if (s.edit.kind == ValueKind::Int || s.edit.kind == ValueKind::ClockHz) {
        char lo[16], hi[16];
        format_value(s.edit, s.edit.min, lo, sizeof(lo));
        format_value(s.edit, s.edit.max, hi, sizeof(hi));
        std::snprintf(ctx, sizeof(ctx), "  %s..%s", lo, hi);
        draw_row(5, 0, ctx);
    } else if (s.edit.kind == ValueKind::Protocol || s.edit.kind == ValueKind::ColorOrder ||
               s.edit.kind == ValueKind::Failsafe) {
        std::snprintf(ctx, sizeof(ctx), "  %ld / %ld",
                      static_cast<long>(s.edit.current - s.edit.min + 1),
                      static_cast<long>(s.edit.max - s.edit.min + 1));
        draw_row(5, 0, ctx);
    }
    draw_row(7, 0, "turn=adj press=ok");
#endif
}

void commit_edit() {
    const int32_t v = s.edit.current;
    switch (s.edit.field) {
    case Field::ArtnetNet: {
        auto g       = config::get_global();
        g.artnet_net = static_cast<uint8_t>(v);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::ArtnetSubnet: {
        auto g          = config::get_global();
        g.artnet_subnet = static_cast<uint8_t>(v);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::ArtnetReplyUnicast: {
        auto g                      = config::get_global();
        g.artnet_poll_reply_unicast = (v != 0);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::ArtnetFailsafeMode: {
        auto g          = config::get_global();
        g.failsafe_mode = static_cast<uint8_t>(v);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::ArtnetFailsafeTimeout: {
        auto g               = config::get_global();
        g.failsafe_timeout_s = static_cast<uint16_t>(v);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::ArtnetSacn: {
        auto g         = config::get_global();
        g.sacn_enabled = (v != 0);
        config::set_global(g);
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
        break;
    }
    case Field::ArtnetFpp: {
        auto g       = config::get_global();
        g.fpp_remote = (v != 0);
        config::set_global(g);
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
        break;
    }
    case Field::AutoPatch:
        // Re-address every channel contiguously from the chosen base universe.
        // auto_patch_universes persists each channel and marks it dirty itself.
        dmx::auto_patch_universes(static_cast<uint16_t>(v));
        break;
    case Field::GlobalRefresh: {
        auto g            = config::get_global();
        g.refresh_rate_hz = static_cast<uint8_t>(v);
        config::set_global(g);
        // A higher refresh shrinks every channel's pixel budget — truncate any
        // strip that no longer fits (e.g. 800 px valid at 30 Hz, not at 60 Hz).
        dmx::clamp_pixel_counts();
        dmx::mark_global_dirty();
        break;
    }
    case Field::NetworkDhcp: {
        auto g     = config::get_global();
        g.use_dhcp = (v != 0);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::NetworkWebEnabled: {
        auto g        = config::get_global();
        g.web_enabled = (v != 0);
        config::set_global(g);
        if (g.web_enabled)
            web::start();
        else
            web::stop();
        break;
    }

    case Field::ChProtocol: {
        auto c     = config::get_channel(s.edit.channel);
        c.protocol = static_cast<led::Protocol>(v);
        config::set_channel(s.edit.channel, c);
        // New protocol → new per-pixel cost (and DMX caps at one universe);
        // truncate the pixel count if it no longer fits.
        dmx::clamp_pixel_counts();
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChColorOrder: {
        auto c        = config::get_channel(s.edit.channel);
        c.color_order = static_cast<led::ColorOrder>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChUniverse: {
        auto c           = config::get_channel(s.edit.channel);
        c.universe_start = static_cast<uint16_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChDmx: {
        auto c      = config::get_channel(s.edit.channel);
        c.dmx_start = static_cast<uint16_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChPixels: {
        auto c        = config::get_channel(s.edit.channel);
        c.pixel_count = static_cast<uint16_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChBrightness: {
        auto c       = config::get_channel(s.edit.channel);
        c.brightness = static_cast<uint8_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChGamma: {
        auto c      = config::get_channel(s.edit.channel);
        c.gamma_x10 = static_cast<uint8_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChGrouping: {
        auto c     = config::get_channel(s.edit.channel);
        c.grouping = static_cast<uint8_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChInvert: {
        auto c             = config::get_channel(s.edit.channel);
        c.invert_direction = (v != 0);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChClock: {
        auto c     = config::get_channel(s.edit.channel);
        c.clock_hz = static_cast<uint32_t>(v);
        config::set_channel(s.edit.channel, c);
        // A slower clock stretches the frame → fewer pixels fit the budget.
        dmx::clamp_pixel_counts();
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }

    default: break;
    }
}

void dispatch_edit_value(Event e) {
    if (e == Event::RotateLeft || e == Event::RotateRight) {
        if (s.edit.kind == ValueKind::ClockHz) {
            // Step through the discrete achievable rates, not raw Hz.
            int idx = clock_choice_index(s.edit.current) + (e == Event::RotateRight ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx >= kClockChoiceCount) idx = kClockChoiceCount - 1;
            s.edit.current = kClockChoices[idx];
            return;
        }
        // Acceleration only makes sense for plain integers; enums/bools have
        // tiny ranges where a ×10 jump would just slam into the clamp.
        const int32_t mult  = (s.edit.kind == ValueKind::Int) ? accel_note_rotation() : 1;
        const int32_t step  = s.edit.step * mult;
        s.edit.current     += (e == Event::RotateRight) ? step : -step;
        if (s.edit.current < s.edit.min) s.edit.current = s.edit.min;
        if (s.edit.current > s.edit.max) s.edit.current = s.edit.max;
        // Live strip preview while the pixel count moves.
        if (s.edit.field == Field::ChPixels && dmx::pixel_preview_channel() == s.edit.channel) {
            dmx::set_pixel_preview(s.edit.channel, static_cast<uint16_t>(s.edit.current));
        }
    }
    if (e == Event::Click) {
        dmx::clear_pixel_preview();
        commit_edit();
        s.screen = s.edit.return_screen;
    }
}

// ── EDIT STRING — char-by-char ──────────────────────────────────────────────

void enter_edit_string(StringField field, const char* source, uint8_t max_len, const char* label,
                       Screen return_screen) {
    s.str_edit.field         = field;
    s.str_edit.max_len       = max_len;
    s.str_edit.cursor        = 0;
    s.str_edit.return_screen = return_screen;
    s.str_edit.label         = label;
    std::memset(s.str_edit.buf, ' ', sizeof(s.str_edit.buf));
    s.str_edit.buf[max_len] = '\0';
    for (uint8_t i = 0; i < max_len && source[i] != '\0'; ++i) {
        s.str_edit.buf[i] = source[i];
    }
    s.screen = Screen::EditString;
}

void render_edit_string() {
    char title[48];
    if (s.str_edit.cursor < s.str_edit.max_len) {
        std::snprintf(title, sizeof(title), "EDIT %s [%u/%u]", s.str_edit.label,
                      static_cast<unsigned>(s.str_edit.cursor + 1),
                      static_cast<unsigned>(s.str_edit.max_len));
    } else {
        std::snprintf(title, sizeof(title), "EDIT %s [end]", s.str_edit.label);
    }

    constexpr int kWin   = 20;
    const uint8_t cursor = s.str_edit.cursor;
    const uint8_t len    = s.str_edit.max_len;
    int win_start        = static_cast<int>(cursor) - 10;
    if (win_start < 0) win_start = 0;
    if (win_start + kWin > len) win_start = len - kWin;
    if (win_start < 0) win_start = 0;

    char window[kWin + 1];
    std::memset(window, 0, sizeof(window));
    for (int i = 0; i < kWin && win_start + i < len; ++i) {
        const char c = s.str_edit.buf[win_start + i];
        window[i]    = (c == ' ') ? '_' : (c == kStrEnd ? ' ' : c);  // end marker drawn separately
    }
    const bool end_marker = (cursor < len && s.str_edit.buf[cursor] == kStrEnd);

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: cell strip, active char cyan + underline ───────────────
    canvas_clear(color::Black);
    draw_tft_header(title);
    const int cy = (kHdrH + (kTH - kFootH)) / 2;
    if (end_marker || cursor >= len) {
        const char* msg = "ready";
        canvas_draw_text_f((kTW - canvas_text_w(msg, FontId::Mega)) / 2,
                           cy - canvas_font_h(FontId::Mega) / 2, msg, color::FrogLine, color::Black,
                           FontId::Mega);
    } else {
        const int adv = canvas_font_adv(FontId::Body) + 3;
        int n         = 0;
        while (window[n])
            ++n;
        int x        = (kTW - n * adv) / 2;
        const int ty = cy - canvas_font_h(FontId::Body) / 2;
        for (int i = 0; window[i]; ++i) {
            const bool act   = (win_start + i == cursor);
            const bool blank = (window[i] == '_');
            char ch[2]       = { window[i], 0 };
            const Color col  = act ? color::EditCyan : blank ? color::IdleGreen : color::Cream;
            canvas_draw_text_f(x, ty, ch, col, color::Black, FontId::Body);
            if (act)
                canvas_fill_rect(x, ty + canvas_font_h(FontId::Body) + 1, adv - 3, 2,
                                 color::EditCyan);
            x += adv;
        }
    }
    draw_hint_bar("letter", (end_marker || cursor >= len) ? "apply" : "next", "cancel");
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    canvas_clear(color::Black);
    draw_tft_header(title);

    canvas_draw_text(kIndent, kHdrH + 20, window, color::Cyan, color::Black, kTxtSc);

    if (end_marker) {
        // The validate glyph: a green "save here" cell + spelled-out action.
        const int cx = kIndent + (cursor - win_start) * kFontCellWidth * kTxtSc;
        canvas_fill_round_rect(cx - 1, kHdrH + 18, kFontCellWidth * kTxtSc + 2, kTxtH + 4, 3,
                               color::FrogLine);
        canvas_draw_text(kIndent, kHdrH + 20 + kTxtH + 12, "SAVE & FINISH", color::FrogLine,
                         color::Black, kTxtSc);
    } else if (cursor < len) {
        const int high_x = kIndent + (cursor - win_start) * kFontCellWidth * kTxtSc;
        canvas_fill_rect(high_x, kHdrH + 20 + kTxtH + 4, kFontCellWidth * kTxtSc, 4, color::Cyan);
    } else {
        canvas_draw_text(kIndent, kHdrH + 20 + kTxtH + 12, "SAVE & FINISH", color::FrogLine,
                         color::Black, kTxtSc);
    }

    canvas_fill_rect(0, kTH - 20, kTW, 20, color::HeaderBg);
    const char* hint = end_marker ? "TURN  back        PRESS  save        HOLD  cancel"
                                  : "TURN  letter      PRESS  next        HOLD  cancel";
    const int hw     = static_cast<int>(std::strlen(hint)) * kFontCellWidth;
    canvas_draw_text((kTW - hw) / 2, kTH - 14, hint, color::SplashSub, color::HeaderBg, 1);
#else
    canvas_clear();
    draw_row(0, 0, title);
    draw_row(2, 0, window);

    if (end_marker) {
        draw_row(3, 0, "  >> SAVE & FINISH");
    } else if (cursor < len) {
        char caret[kWin + 1];
        std::memset(caret, 0, sizeof(caret));
        const int col = cursor - win_start;
        for (int i = 0; i < col && i < kWin; ++i)
            caret[i] = ' ';
        if (col >= 0 && col < kWin) caret[col] = '^';
        draw_row(3, 0, caret);
    } else {
        draw_row(3, 0, ">> SAVE & FINISH");
    }
    draw_row(7, 0, "press=ok hold=cancel");
#endif
}

void commit_edit_string() {
    // Trim trailing spaces.
    int end = s.str_edit.max_len;
    while (end > 0 && s.str_edit.buf[end - 1] == ' ')
        --end;
    s.str_edit.buf[end] = '\0';

    auto g = config::get_global();
    if (s.str_edit.field == StringField::ArtnetShort) {
        const size_t n = std::min(std::strlen(s.str_edit.buf), sizeof(g.short_name) - 1);
        std::memset(g.short_name, 0, sizeof(g.short_name));
        std::memcpy(g.short_name, s.str_edit.buf, n);
    } else {
        const size_t n = std::min(std::strlen(s.str_edit.buf), sizeof(g.long_name) - 1);
        std::memset(g.long_name, 0, sizeof(g.long_name));
        std::memcpy(g.long_name, s.str_edit.buf, n);
    }
    config::set_global(g);
    dmx::mark_global_dirty();
}

void dispatch_edit_string(Event e) {
    const uint8_t len = s.str_edit.max_len;
    if (s.str_edit.cursor < len) {
        char& cur = s.str_edit.buf[s.str_edit.cursor];
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            cur = idx_to_char(char_to_idx(cur) + (e == Event::RotateRight ? 1 : -1));
        } else if (e == Event::Click) {
            if (cur == kStrEnd) {
                // Validate-and-finish: drop this cell and everything after it.
                for (uint8_t i = s.str_edit.cursor; i < len; ++i)
                    s.str_edit.buf[i] = ' ';
                commit_edit_string();
                s.screen = s.str_edit.return_screen;
            } else {
                s.str_edit.cursor++;
            }
        }
    } else {
        // At DONE position: rotate-left returns to last char; click commits.
        if (e == Event::RotateLeft) {
            if (s.str_edit.cursor > 0) s.str_edit.cursor--;
        } else if (e == Event::Click) {
            commit_edit_string();
            s.screen = s.str_edit.return_screen;
        }
    }
}

// ── Art-Net Port-Address segments (net.sub.uni) ─────────────────────────────
// Per-segment right-shift, and maximum which equals the field bitmask.
constexpr uint8_t kUniShift[3]  = { 8, 4, 0 };
constexpr uint16_t kUniMax[3]   = { 127, 15, 15 };
constexpr uint16_t kUniFMask[3] = { 0x7F00, 0x00F0, 0x000F };

void format_uni(char* buf, size_t cap, uint16_t v) {
    std::snprintf(buf, cap, "%u.%u.%u", static_cast<unsigned>((v >> 8) & 0x7F),
                  static_cast<unsigned>((v >> 4) & 0x0F), static_cast<unsigned>(v & 0x0F));
}

// ── EDIT IP — 4 octets ──────────────────────────────────────────────────────

void enter_edit_ip(IpField field, uint32_t current, const char* label, Screen return_screen) {
    s.ip_edit.field         = field;
    s.ip_edit.value         = current;
    s.ip_edit.cursor        = 0;
    s.ip_edit.label         = label;
    s.ip_edit.return_screen = return_screen;
    s.screen                = Screen::EditIp;
    accel_reset();
}

void render_edit_ip() {
    char title[32];
    std::snprintf(title, sizeof(title), "EDIT %s", s.ip_edit.label);

    // Render as "192.[168].001.123" with brackets around current octet.
    const uint32_t v     = s.ip_edit.value;
    const uint8_t oct[4] = {
        static_cast<uint8_t>((v >> 24) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>(v & 0xFF),
    };
    char ip_line[32];
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == s.ip_edit.cursor)
            pos += std::snprintf(ip_line + pos, sizeof(ip_line) - pos, "[%u]", oct[i]);
        else
            pos += std::snprintf(ip_line + pos, sizeof(ip_line) - pos, "%u", oct[i]);
        if (i < 3) pos += std::snprintf(ip_line + pos, sizeof(ip_line) - pos, ".");
        if (pos >= static_cast<int>(sizeof(ip_line)) - 1) break;
    }

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: big octets, active one cyan + bracketed ────────────────
    canvas_clear(color::Black);
    draw_tft_header(title);
    {
        const int mh   = canvas_font_h(FontId::Mega);
        const int dotw = canvas_font_adv(FontId::Mega);
        const int cy   = (kHdrH + (kTH - kFootH)) / 2;
        char seg[4][8];
        int w[4], total = 0;
        for (int i = 0; i < 4; ++i) {
            if (i == s.ip_edit.cursor)
                std::snprintf(seg[i], sizeof(seg[i]), "[%u]", oct[i]);
            else
                std::snprintf(seg[i], sizeof(seg[i]), "%u", oct[i]);
            w[i]   = canvas_text_w(seg[i], FontId::Mega);
            total += w[i] + (i < 3 ? dotw : 0);
        }
        int x        = (kTW - total) / 2;
        const int ty = cy - mh / 2;
        for (int i = 0; i < 4; ++i) {
            const bool act = (i == s.ip_edit.cursor);
            canvas_draw_text_f(x, ty, seg[i], act ? color::EditCyan : color::Cream, color::Black,
                               FontId::Mega);
            x += w[i];
            if (i < 3) {
                canvas_draw_text_f(x, ty, ".", color::DimGreen, color::Black, FontId::Mega);
                x += dotw;
            }
        }
    }
    draw_hint_bar("adjust", s.ip_edit.cursor >= 4 ? "apply" : "next", "cancel");
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    canvas_clear(color::Black);
    draw_tft_header(title);

    canvas_draw_text(kIndent, kHdrH + 30, ip_line, color::Cyan, color::Black, kTxtSc);

    if (s.ip_edit.cursor >= 4) {
        canvas_draw_text(kIndent, kHdrH + 30 + kTxtH + 16, "[ready to commit]", color::Orange,
                         color::Black, kTxtSc);
    }
#else
    canvas_clear();
    draw_row(0, 0, title);
    draw_row(3, 0, ip_line);

    if (s.ip_edit.cursor >= 4) {
        draw_row(5, 0, "[ready to commit]");
    }
#endif
}

void commit_edit_ip() {
    auto g = config::get_global();
    switch (s.ip_edit.field) {
    case IpField::StaticIp: g.static_ip = s.ip_edit.value; break;
    case IpField::StaticMask: g.static_mask = s.ip_edit.value; break;
    case IpField::StaticGw: g.static_gateway = s.ip_edit.value; break;
    }
    config::set_global(g);
    dmx::mark_global_dirty();
}

void dispatch_edit_ip(Event e) {
    if (s.ip_edit.cursor < 4) {
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            const int32_t step  = accel_note_rotation();
            const int shift     = (3 - s.ip_edit.cursor) * 8;
            int32_t oct         = (s.ip_edit.value >> shift) & 0xFF;
            oct                += (e == Event::RotateRight) ? step : -step;
            if (oct < 0) oct = 0;
            if (oct > 255) oct = 255;
            const uint32_t mask = ~(0xFFu << shift);
            s.ip_edit.value     = (s.ip_edit.value & mask) | (static_cast<uint32_t>(oct) << shift);
        } else if (e == Event::Click) {
            s.ip_edit.cursor++;
            accel_reset();
        }
    } else {
        if (e == Event::RotateLeft) {
            if (s.ip_edit.cursor > 0) s.ip_edit.cursor--;
        } else if (e == Event::Click) {
            commit_edit_ip();
            s.screen = s.ip_edit.return_screen;
        }
    }
}

// ── EDIT UNI — net.sub.uni (3 segments) ─────────────────────────────────────

void enter_edit_uni(uint8_t channel, uint16_t current, Screen return_screen) {
    s.uni_edit.value         = current & 0x7FFF;
    s.uni_edit.cursor        = 0;
    s.uni_edit.channel       = channel;
    s.uni_edit.return_screen = return_screen;
    s.screen                 = Screen::EditUni;
    accel_reset();
}

void render_edit_uni() {
    char title[24];
    std::snprintf(title, sizeof(title), "EDIT UNI C%u", s.uni_edit.channel + 1);

    // Render as "0.[2].1 = U513" with brackets around the current segment.
    const uint16_t v      = s.uni_edit.value;
    const uint16_t seg[3] = {
        static_cast<uint16_t>((v >> 8) & 0x7F),
        static_cast<uint16_t>((v >> 4) & 0x0F),
        static_cast<uint16_t>(v & 0x0F),
    };
    char line[40];
    int pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (i == s.uni_edit.cursor)
            pos += std::snprintf(line + pos, sizeof(line) - pos, "[%u]", seg[i]);
        else
            pos += std::snprintf(line + pos, sizeof(line) - pos, "%u", seg[i]);
        if (i < 2) pos += std::snprintf(line + pos, sizeof(line) - pos, ".");
    }
    std::snprintf(line + pos, sizeof(line) - pos, " = U%u", static_cast<unsigned>(v));

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: net.sub.uni big, "= U<abs>" in green ───────────────────
    canvas_clear(color::Black);
    draw_tft_header(title);
    {
        const int mh   = canvas_font_h(FontId::Mega);
        const int dotw = canvas_font_adv(FontId::Mega);
        const int cy   = (kHdrH + (kTH - kFootH)) / 2 - 4;
        char part[3][8], tail[12];
        std::snprintf(tail, sizeof(tail), "=U%u", static_cast<unsigned>(v));
        int w[3], total = 0;
        for (int i = 0; i < 3; ++i) {
            if (i == s.uni_edit.cursor)
                std::snprintf(part[i], sizeof(part[i]), "[%u]", seg[i]);
            else
                std::snprintf(part[i], sizeof(part[i]), "%u", seg[i]);
            w[i]   = canvas_text_w(part[i], FontId::Mega);
            total += w[i] + (i < 2 ? dotw : 0);
        }
        const int tw  = canvas_text_w(tail, FontId::Mega);
        total        += dotw + tw;  // spacer + tail
        int x         = (kTW - total) / 2;
        const int ty  = cy - mh / 2;
        for (int i = 0; i < 3; ++i) {
            const bool act = (i == s.uni_edit.cursor);
            canvas_draw_text_f(x, ty, part[i], act ? color::EditCyan : color::Cream, color::Black,
                               FontId::Mega);
            x += w[i];
            if (i < 2) {
                canvas_draw_text_f(x, ty, ".", color::DimGreen, color::Black, FontId::Mega);
                x += dotw;
            }
        }
        x += dotw;
        canvas_draw_text_f(x, ty, tail, color::FrogLine, color::Black, FontId::Mega);
        // net/sub/uni captions under the segments.
        canvas_draw_text_f((kTW - total) / 2, ty + mh + 2, "net sub uni", color::DimGreen,
                           color::Black, FontId::Small);
    }
    draw_hint_bar("adjust", s.uni_edit.cursor >= 3 ? "apply" : "next", "cancel");
#elif defined(CONFIG_PIXFROG_DISPLAY_TFT)
    canvas_clear(color::Black);
    draw_tft_header(title);
    canvas_draw_text(kIndent, kHdrH + 30, line, color::Cyan, color::Black, kTxtSc);
    canvas_draw_text(kIndent, kHdrH + 30 + kTxtH + 6, "net . sub . uni", color::DarkGray,
                     color::Black, 1);
    if (s.uni_edit.cursor >= 3) {
        canvas_draw_text(kIndent, kHdrH + 30 + kTxtH + 22, "[ready to commit]", color::Orange,
                         color::Black, kTxtSc);
    }
#else
    canvas_clear();
    draw_row(0, 0, title);
    draw_row(3, 0, line);
    draw_row(4, 0, "net . sub . uni");
    if (s.uni_edit.cursor >= 3) {
        draw_row(6, 0, "[ready to commit]");
    }
#endif
}

void commit_edit_uni() {
    auto c           = config::get_channel(s.uni_edit.channel);
    c.universe_start = s.uni_edit.value;
    config::set_channel(s.uni_edit.channel, c);
    dmx::mark_channel_dirty(s.uni_edit.channel);
}

void dispatch_edit_uni(Event e) {
    if (s.uni_edit.cursor < 3) {
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            const int32_t step   = accel_note_rotation();
            const uint8_t shift  = kUniShift[s.uni_edit.cursor];
            const int32_t max    = kUniMax[s.uni_edit.cursor];
            int32_t seg          = (s.uni_edit.value >> shift) & max;
            seg                 += (e == Event::RotateRight) ? step : -step;
            if (seg < 0) seg = 0;
            if (seg > max) seg = max;
            const uint16_t fmask = kUniFMask[s.uni_edit.cursor];
            s.uni_edit.value     = static_cast<uint16_t>(
                (s.uni_edit.value & ~fmask) | ((static_cast<uint16_t>(seg) << shift) & fmask));
        } else if (e == Event::Click) {
            s.uni_edit.cursor++;
            accel_reset();
        }
    } else {
        if (e == Event::RotateLeft) {
            if (s.uni_edit.cursor > 0) s.uni_edit.cursor--;
        } else if (e == Event::Click) {
            commit_edit_uni();
            s.screen = s.uni_edit.return_screen;
        }
    }
}

// ── INPUTS NODE ──────────────────────────────────────────────────────────────
// DMX-over-IP reception: Art-Net addressing + alternative input protocols
// (sACN, FPP) + the universe auto-patch helper.

uint8_t build_inputs(ListItem* items, OnClick* fns) {
    static char vnet[8], vsub[8], vunicast[8], vsacn[8], vfpp[8];
    const auto& g = config::get_global();
    std::snprintf(vnet, sizeof(vnet), "%u", g.artnet_net);
    std::snprintf(vsub, sizeof(vsub), "%u", g.artnet_subnet);
    std::snprintf(vunicast, sizeof(vunicast), "%s", g.artnet_poll_reply_unicast ? "ON" : "OFF");
    std::snprintf(vsacn, sizeof(vsacn), "%s", g.sacn_enabled ? "ON" : "OFF");
    std::snprintf(vfpp, sizeof(vfpp), "%s", g.fpp_remote ? "ON" : "OFF");

    items[0] = { "Net", vnet };
    fns[0]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetNet, ValueKind::Int, g.artnet_net, 0, 127, 1, "Net", Screen::Menu);
    };
    items[1] = { "Sub", vsub };
    fns[1]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetSubnet, ValueKind::Int, g.artnet_subnet, 0, 15, 1, "Sub",
                     Screen::Menu);
    };
    items[2] = { "Unicast", vunicast };
    fns[2]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetReplyUnicast, ValueKind::Bool, g.artnet_poll_reply_unicast ? 1 : 0,
                     0, 1, 1, "Unicast", Screen::Menu);
    };
    items[3] = { "sACN", vsacn };
    fns[3]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetSacn, ValueKind::Bool, g.sacn_enabled ? 1 : 0, 0, 1, 1, "sACN",
                     Screen::Menu);
    };
    items[4] = { "FPP", vfpp };
    fns[4]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetFpp, ValueKind::Bool, g.fpp_remote ? 1 : 0, 0, 1, 1, "FPP",
                     Screen::Menu);
    };
    items[5] = { "Auto-patch", "" };
    fns[5]   = [](uint8_t) {
        // Cascade all channels from a base universe; seed with channel 0's so
        // re-patching in place is one click.
        enter_edit(Field::AutoPatch, ValueKind::Int, config::get_channel(0).universe_start, 0,
                     32767, 1, "Patch", Screen::Menu);
    };
    items[6] = back_item();
    fns[6]   = [](uint8_t) { go_back(); };
    return 7;
}

// ── OUTPUT NODE ──────────────────────────────────────────────────────────────
// Global output behaviour: the LED refresh rate and what the strips do when the
// live input drops (failsafe mode + timeout).

uint8_t build_output(ListItem* items, OnClick* fns) {
    static char vrefresh[8], vfsm[8], vfst[8];
    const auto& g = config::get_global();
    std::snprintf(vrefresh, sizeof(vrefresh), "%uHz", g.refresh_rate_hz);
    std::snprintf(vfsm, sizeof(vfsm), "%s", failsafe_name(g.failsafe_mode));
    std::snprintf(vfst, sizeof(vfst), "%us", g.failsafe_timeout_s);

    items[0] = { "Refresh", vrefresh };
    fns[0]   = [](uint8_t) {
        // Refresh: step 30 → only [30, 60] reachable; satisfies spec §4.
        const auto& g = config::get_global();
        enter_edit(Field::GlobalRefresh, ValueKind::Int, g.refresh_rate_hz, 30, 60, 30, "Refresh",
                     Screen::Menu);
    };
    items[1] = { "Failsafe", vfsm };
    fns[1]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetFailsafeMode, ValueKind::Failsafe, g.failsafe_mode, 0, 3, 1,
                     "Failsafe", Screen::Menu);
    };
    items[2] = { "FSafe s", vfst };
    fns[2]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetFailsafeTimeout, ValueKind::Int, g.failsafe_timeout_s, 0, 3600, 1,
                     "FSafe s", Screen::Menu);
    };
    items[3] = back_item();
    fns[3]   = [](uint8_t) { go_back(); };
    return 4;
}

// ── PLAYBACK NODE ────────────────────────────────────────────────────────────
// Output sources that aren't live DMX-over-IP: stored scenes, SD-card FSEQ
// sequences, and the built-in test patterns.

uint8_t build_playback(ListItem* items, OnClick* fns) {
    items[0] = { "Scenes", "" };
    fns[0]   = [](uint8_t) { go(NodeId::Scenes); };
    items[1] = { "FSEQ", "" };
    fns[1]   = [](uint8_t) {
        // Refresh the SD listing as we open the file browser.
        g_fseq_file_count = static_cast<uint8_t>(fseq::list_files(g_fseq_names, kFseqMenuMaxFiles));
        go(NodeId::Fseq);
    };
    items[2] = { "Test pattern", "" };
    fns[2]   = [](uint8_t) { go(NodeId::TestPattern); };
    items[3] = back_item();
    fns[3]   = [](uint8_t) { go_back(); };
    return 4;
}

// ── NETWORK NODE ─────────────────────────────────────────────────────────────

uint8_t build_network(ListItem* items, OnClick* fns) {
    static char vdhcp[8], vip[kOledCols + 1], vmsk[kOledCols + 1], vgw[kOledCols + 1], vweb[8];
    static char vshort[14], vlong[14];
    const auto& g = config::get_global();
    std::snprintf(vdhcp, sizeof(vdhcp), "%s", g.use_dhcp ? "ON" : "OFF");
    std::snprintf(vweb, sizeof(vweb), "%s", g.web_enabled ? "ON" : "OFF");
    auto fmt_ip = [](char* buf, size_t cap, uint32_t v) {
        std::snprintf(buf, cap, "%u.%u.%u.%u", static_cast<unsigned>((v >> 24) & 0xFFu),
                      static_cast<unsigned>((v >> 16) & 0xFFu),
                      static_cast<unsigned>((v >> 8) & 0xFFu), static_cast<unsigned>(v & 0xFFu));
    };
    fmt_ip(vip, sizeof(vip), g.static_ip);
    fmt_ip(vmsk, sizeof(vmsk), g.static_mask);
    fmt_ip(vgw, sizeof(vgw), g.static_gateway);
    truncate(vshort, sizeof(vshort), g.short_name);
    truncate(vlong, sizeof(vlong), g.long_name);

    items[0] = { "DHCP", vdhcp };
    fns[0]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::NetworkDhcp, ValueKind::Bool, g.use_dhcp ? 1 : 0, 0, 1, 1, "DHCP",
                     Screen::Menu);
    };
    items[1] = { "IP", vip };
    fns[1]   = [](uint8_t) {
        enter_edit_ip(IpField::StaticIp, config::get_global().static_ip, "IP", Screen::Menu);
    };
    items[2] = { "Msk", vmsk };
    fns[2]   = [](uint8_t) {
        enter_edit_ip(IpField::StaticMask, config::get_global().static_mask, "Mask", Screen::Menu);
    };
    items[3] = { "GW", vgw };
    fns[3]   = [](uint8_t) {
        enter_edit_ip(IpField::StaticGw, config::get_global().static_gateway, "GW", Screen::Menu);
    };
    items[4] = { "Web UI", vweb };
    fns[4]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::NetworkWebEnabled, ValueKind::Bool, g.web_enabled ? 1 : 0, 0, 1, 1,
                     "Web UI", Screen::Menu);
    };
    items[5] = { "Name", vshort };
    fns[5]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit_string(StringField::ArtnetShort, g.short_name, sizeof(g.short_name) - 1, "Name",
                            Screen::Menu);
    };
    items[6] = { "Long", vlong };
    fns[6]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit_string(StringField::ArtnetLong, g.long_name, sizeof(g.long_name) - 1, "Long",
                            Screen::Menu);
    };
    items[7] = back_item();
    fns[7]   = [](uint8_t) { go_back(); };
    return 8;
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kProtocolCount = static_cast<size_t>(led::Protocol::COUNT);

// The channel menu is layout-driven: the visible items depend on the protocol
// family. DMX512 output ignores the LED-only fields (color order, brightness,
// grouping, invert, clock), so they are hidden; clocked SPI strips add Clock.
enum class ChItem : uint8_t {
    Proto,
    Uni,
    Dmx,
    Pixels,  // labelled "Slots" in DMX512 mode
    Order,
    Bright,
    Gamma,
    Group,
    Invert,
    Clock,
    Identify,
    Back,
};

// Fills `out` (capacity ≥ 12) with the ordered items for `cc` and returns count.
uint8_t channel_items(const config::ChannelConfig& cc, ChItem* out) {
    uint8_t n = 0;
    out[n++]  = ChItem::Proto;
    // A disabled channel exposes only its protocol (so it can be re-enabled)
    // plus Back — no universe/pixel/LED settings.
    if (led::is_off(cc.protocol)) {
        out[n++] = ChItem::Back;
        return n;
    }
    out[n++] = ChItem::Uni;
    out[n++] = ChItem::Dmx;
    out[n++] = ChItem::Pixels;
    if (!led::is_dmx(cc.protocol)) {
        out[n++] = ChItem::Order;
        out[n++] = ChItem::Bright;
        out[n++] = ChItem::Gamma;
        out[n++] = ChItem::Group;
        out[n++] = ChItem::Invert;
        if (led::is_clocked(cc.protocol)) out[n++] = ChItem::Clock;
        out[n++] = ChItem::Identify;
    }
    out[n++] = ChItem::Back;
    return n;
}

// Channel node title is dynamic ("CHANNEL N"); build_channel fills this buffer
// each frame and the kNodes entry points its title at it.
char g_channel_title[16];

// Per-ChItem click action. Each reads the channel fresh at click time (the
// channel is s.channel_index, set when the node was opened from the main menu).
OnClick channel_action(ChItem it) {
    switch (it) {
    case ChItem::Proto:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChProtocol, ValueKind::Protocol, static_cast<int32_t>(cc.protocol), 0,
                       static_cast<int32_t>(kProtocolCount) - 1, 1, "Proto", Screen::Menu,
                       s.channel_index);
        };
    case ChItem::Uni:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit_uni(s.channel_index, cc.universe_start, Screen::Menu);
        };
    case ChItem::Dmx:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChDmx, ValueKind::Int, cc.dmx_start, 1, 512, 1, "DMX", Screen::Menu,
                       s.channel_index);
        };
    case ChItem::Pixels:
        return [](uint8_t) {
            // Upper bound is whatever the bus can clock out within the refresh
            // budget (and DMA buffer) for this protocol — e.g. 512 px WS2815
            // @60 Hz, 1024 @30 Hz, 512 slots for a DMX universe.
            const auto& cc     = config::get_channel(s.channel_index);
            const bool dmx     = led::is_dmx(cc.protocol);
            const int32_t pmax = dmx::channel_max_pixels(s.channel_index);
            int32_t pcur       = cc.pixel_count;
            if (pcur > pmax) pcur = pmax;
            enter_edit(Field::ChPixels, ValueKind::Int, pcur, 1, pmax, 1, dmx ? "Slots" : "Pixels",
                       Screen::Menu, s.channel_index);
            // Live strip ruler while editing (LED protocols only).
            if (!dmx) dmx::set_pixel_preview(s.channel_index, static_cast<uint16_t>(pcur));
        };
    case ChItem::Order:
        return [](uint8_t) {
            // 3-colour strips pick an RGB permutation; RGBW strips a W-suffixed
            // order. Restrict the cycle to the matching family.
            const auto& cc     = config::get_channel(s.channel_index);
            const bool rgbw    = led::is_rgbw(cc.protocol);
            const int32_t omin = rgbw ? static_cast<int32_t>(led::ColorOrder::RGBW) : 0;
            const int32_t omax = rgbw ? static_cast<int32_t>(led::ColorOrder::GRBW)
                                      : static_cast<int32_t>(led::ColorOrder::BGR);
            int32_t ocur       = static_cast<int32_t>(cc.color_order);
            if (ocur < omin) ocur = omin;
            if (ocur > omax) ocur = omax;
            enter_edit(Field::ChColorOrder, ValueKind::ColorOrder, ocur, omin, omax, 1, "Order",
                       Screen::Menu, s.channel_index);
        };
    case ChItem::Bright:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChBrightness, ValueKind::Int, cc.brightness, 0, 255, 1, "Bright",
                       Screen::Menu, s.channel_index);
        };
    case ChItem::Gamma:
        return [](uint8_t) {
            // Stored ×10: 10 = linear, 22 = the classic 2.2.
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChGamma, ValueKind::Int, cc.gamma_x10, 10, 40, 1, "Gamma",
                       Screen::Menu, s.channel_index);
        };
    case ChItem::Group:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChGrouping, ValueKind::Int, cc.grouping, 1, 8, 1, "Group",
                       Screen::Menu, s.channel_index);
        };
    case ChItem::Invert:
        return [](uint8_t) {
            const auto& cc = config::get_channel(s.channel_index);
            enter_edit(Field::ChInvert, ValueKind::Bool, cc.invert_direction ? 1 : 0, 0, 1, 1,
                       "Invert", Screen::Menu, s.channel_index);
        };
    case ChItem::Clock:
        return [](uint8_t) {
            // Snap to a real divisor rate; the picker steps through kClockChoices.
            const auto& cc = config::get_channel(s.channel_index);
            const int32_t snapped =
                kClockChoices[clock_choice_index(static_cast<int32_t>(cc.clock_hz))];
            enter_edit(Field::ChClock, ValueKind::ClockHz, snapped, kClockChoices[0],
                       kClockChoices[kClockChoiceCount - 1], 1, "Clock", Screen::Menu,
                       s.channel_index);
        };
    case ChItem::Identify:
        return [](uint8_t) { dmx::identify_start(s.channel_index); };  // 10 s blink; stay
    case ChItem::Back: return [](uint8_t) { go_back(); };
    }
    return nullptr;
}

uint8_t build_channel(ListItem* items, OnClick* fns) {
    const auto& cc = config::get_channel(s.channel_index);
    const bool dmx = led::is_dmx(cc.protocol);
    std::snprintf(g_channel_title, sizeof(g_channel_title), "CHANNEL %u", s.channel_index + 1);

    static char vproto[8], vuni[12], vdmx[8], vpix[8], vorder[8], vbri[8], vgrp[8], vinv[8],
        vclk[12], vgam[8];
    std::snprintf(vproto, sizeof(vproto), "%s", protocol_name(cc.protocol));
    format_uni(vuni, sizeof(vuni), cc.universe_start);
    std::snprintf(vdmx, sizeof(vdmx), "%u", cc.dmx_start);
    std::snprintf(vpix, sizeof(vpix), "%u", cc.pixel_count);
    std::snprintf(vorder, sizeof(vorder), "%s", color_order_name(cc.color_order));
    std::snprintf(vbri, sizeof(vbri), "%u", cc.brightness);
    std::snprintf(vgrp, sizeof(vgrp), "%u", cc.grouping);
    std::snprintf(vinv, sizeof(vinv), "%s", cc.invert_direction ? "ON" : "OFF");
    format_clock_mhz(static_cast<int32_t>(cc.clock_hz), vclk, sizeof(vclk));
    std::snprintf(vgam, sizeof(vgam), "%u.%u", cc.gamma_x10 / 10, cc.gamma_x10 % 10);

    ChItem order[12];
    const uint8_t count = channel_items(cc, order);
    for (uint8_t i = 0; i < count; ++i) {
        switch (order[i]) {
        case ChItem::Proto: items[i] = { "Proto", vproto }; break;
        case ChItem::Uni: items[i] = { "Uni", vuni }; break;
        case ChItem::Dmx: items[i] = { "DMX", vdmx }; break;
        case ChItem::Pixels: items[i] = { dmx ? "Slots" : "Pixels", vpix }; break;
        case ChItem::Order: items[i] = { "Order", vorder }; break;
        case ChItem::Bright: items[i] = { "Bright", vbri }; break;
        case ChItem::Gamma: items[i] = { "Gamma", vgam }; break;
        case ChItem::Identify: items[i] = { "Identify", "" }; break;
        case ChItem::Group: items[i] = { "Group", vgrp }; break;
        case ChItem::Invert: items[i] = { "Invert", vinv }; break;
        case ChItem::Clock: items[i] = { "Clock", vclk }; break;
        case ChItem::Back: items[i] = back_item(); break;
        }
        fns[i] = channel_action(order[i]);
    }
    return count;
}

// ── Node table + engine ──────────────────────────────────────────────────────

struct Node {
    const char* title;
    NodeId parent;  // ignored for Main (its back = Home)
    uint8_t (*build)(ListItem* items, OnClick* fns);
};

// Indexed by NodeId. Main's parent is a placeholder (back goes to Home).
const Node kNodes[static_cast<uint8_t>(NodeId::Count)] = {
    { "MENU", NodeId::Main, build_main },
    { "INPUTS", NodeId::Main, build_inputs },
    { "NETWORK", NodeId::Main, build_network },
    { "OUTPUT", NodeId::Main, build_output },
    { "PLAYBACK", NodeId::Main, build_playback },
    { g_channel_title, NodeId::Main, build_channel },
    { "SCENES", NodeId::Playback, build_scenes },
    { "FSEQ", NodeId::Playback, build_fseq },
    { "TEST PATTERN", NodeId::Playback, build_testpattern },
};

const Node& cur_node() {
    return kNodes[static_cast<uint8_t>(s.node)];
}

void go(NodeId n) {
    // Save the active cursor/scroll for the node we are leaving, restore the
    // destination's so back/forward returns to where you were.
    s.cur[static_cast<uint8_t>(s.node)] = s.cursor;
    s.scr[static_cast<uint8_t>(s.node)] = s.scroll;
    s.node                              = n;
    s.cursor                            = s.cur[static_cast<uint8_t>(n)];
    s.scroll                            = s.scr[static_cast<uint8_t>(n)];
    s.screen                            = Screen::Menu;
}

void go_back() {
    if (s.node == NodeId::Main) {
        s.screen = Screen::Home;
        return;
    }
    go(cur_node().parent);
}

void open_channel(uint8_t idx) {
    s.channel_index                              = idx;
    s.cur[static_cast<uint8_t>(NodeId::Channel)] = 0;  // a fresh channel starts at the top
    s.scr[static_cast<uint8_t>(NodeId::Channel)] = 0;
    go(NodeId::Channel);
}

void engine_render() {
    ListItem items[kMaxRows];
    OnClick fns[kMaxRows];
    const Node& n       = cur_node();
    const uint8_t count = n.build(items, fns);
    if (count > 0 && s.cursor >= count) s.cursor = count - 1;
    render_list(n.title, items, count, s.cursor);
}

void engine_dispatch(Event e) {
    ListItem items[kMaxRows];
    OnClick fns[kMaxRows];
    const uint8_t count = cur_node().build(items, fns);
    if (count == 0) return;
    if (s.cursor >= count) s.cursor = count - 1;
    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < count - 1) s.cursor++;
    if (e == Event::Click && fns[s.cursor]) fns[s.cursor](s.cursor);
}

// ── Long press: cancel an edit / go Back ────────────────────────────────────
// In any edit screen the pending change is DISCARDED (nothing committed) and we
// return to where we came from; in the menu engine it climbs one level (Main →
// Home).

void dispatch_long_press() {
    switch (s.screen) {
    case Screen::Home: break;
    case Screen::Menu: go_back(); break;
    case Screen::About: s.screen = Screen::Menu; break;
    // Edit screens: cancel — discard the pending value, commit nothing.
    case Screen::EditValue:
        dmx::clear_pixel_preview();
        s.screen = s.edit.return_screen;
        break;
    case Screen::EditString: s.screen = s.str_edit.return_screen; break;
    case Screen::EditIp: s.screen = s.ip_edit.return_screen; break;
    case Screen::EditUni: s.screen = s.uni_edit.return_screen; break;
    }
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

void menu_init() {
    s = State{};
}

void menu_render() {
    switch (s.screen) {
    case Screen::Home: render_home(); break;
    case Screen::Menu: engine_render(); break;
    case Screen::About: render_about(); break;
    case Screen::EditValue: render_edit_value(); break;
    case Screen::EditString: render_edit_string(); break;
    case Screen::EditIp: render_edit_ip(); break;
    case Screen::EditUni: render_edit_uni(); break;
    }
}

void menu_dispatch(Event e) {
    if (e == Event::LongPress) {
        dispatch_long_press();
        return;
    }
    switch (s.screen) {
    case Screen::Home:
        if (e == Event::Click) go(NodeId::Main);  // enter the menu engine at the top
        break;
    case Screen::Menu: engine_dispatch(e); break;
    case Screen::About: dispatch_about(e); break;
    case Screen::EditValue: dispatch_edit_value(e); break;
    case Screen::EditString: dispatch_edit_string(e); break;
    case Screen::EditIp: dispatch_edit_ip(e); break;
    case Screen::EditUni: dispatch_edit_uni(e); break;
    }
}

void menu_on_idle_timeout() {
    dmx::clear_pixel_preview();
    s.screen = Screen::Home;
    // Reopening from Home shows the main menu at the top.
    s.node                                    = NodeId::Main;
    s.cursor                                  = 0;
    s.scroll                                  = 0;
    s.cur[static_cast<uint8_t>(NodeId::Main)] = 0;
    s.scr[static_cast<uint8_t>(NodeId::Main)] = 0;
}

bool menu_is_home() {
    return s.screen == Screen::Home;
}

#ifdef PIXFROG_EMULATOR
void menu_debug_state(const char** screen_name, int* cursor, int* channel) {
    // Node names mirror the old per-screen names so the emulator agent API and
    // existing navigation scripts keep matching.
    static const char* const kNodeNames[static_cast<uint8_t>(NodeId::Count)] = {
        "MainMenu",    "InputsMenu", "NetworkMenu", "OutputMenu",      "PlaybackMenu",
        "ChannelMenu", "ScenesMenu", "FSeqMenu",    "TestPatternMenu",
    };
    if (screen_name) {
        switch (s.screen) {
        case Screen::Home: *screen_name = "Home"; break;
        case Screen::Menu: *screen_name = kNodeNames[static_cast<uint8_t>(s.node)]; break;
        case Screen::About: *screen_name = "About"; break;
        case Screen::EditValue: *screen_name = "EditValue"; break;
        case Screen::EditString: *screen_name = "EditString"; break;
        case Screen::EditIp: *screen_name = "EditIp"; break;
        case Screen::EditUni: *screen_name = "EditUni"; break;
        }
    }
    if (cursor) *cursor = s.cursor;
    if (channel) *channel = s.channel_index;
}
#endif

}  // namespace pixfrog::ui::detail
