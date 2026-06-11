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
#include "lcd_cam_output.h"
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
constexpr int kTW     = 320;                      // TFT width  (landscape)
constexpr int kTH     = 240;                      // TFT height (landscape)
constexpr int kHdrH   = 28;                       // header bar height
constexpr int kItemH  = 24;                       // list item height
constexpr int kTxtSc  = 2;                        // standard text scale (12x16 per char)
constexpr int kTxtH   = 8 * kTxtSc;               // 16px
constexpr int kPad    = (kItemH - kTxtH) / 2;     // vertical padding in item
constexpr int kIndent = 6;                        // left margin
constexpr int kMaxVis = (kTH - kHdrH) / kItemH;   // 8 visible items
constexpr int kBadge  = kItemH - 6;               // square channel-badge side (18px)
constexpr int kGutter = 5 + kBadge + 6;           // label x: clears the badge column
constexpr int kChevW  = kFontCellWidth * kTxtSc;  // chevron glyph width (12px)
#endif

// ── Screens ─────────────────────────────────────────────────────────────────
enum class Screen : uint8_t {
    Home,
    MainMenu,
    ArtnetMenu,
    NetworkMenu,
    ChannelMenu,
    TestPatternMenu,
    About,
    EditValue,
    EditString,
    EditIp,
};

// ── Editable fields ─────────────────────────────────────────────────────────
enum class Field : uint8_t {
    None,
    ArtnetNet,
    ArtnetSubnet,
    ArtnetReplyUnicast,
    ArtnetSacn,
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
    ChGrouping,
    ChInvert,
    ChClock,
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
    Screen return_screen = Screen::MainMenu;
    const char* label    = "";
};

struct EditStringCtx {
    char buf[65]         = {};  // max(short=18, long=64) + null
    uint8_t max_len      = 0;
    uint8_t cursor       = 0;  // 0..max_len; max_len = DONE position
    StringField field    = StringField::ArtnetShort;
    Screen return_screen = Screen::ArtnetMenu;
    const char* label    = "";
};

struct EditIpCtx {
    uint32_t value       = 0;  // host-order
    uint8_t cursor       = 0;  // 0..4; 4 = DONE position
    IpField field        = IpField::StaticIp;
    Screen return_screen = Screen::NetworkMenu;
    const char* label    = "";
};

struct State {
    Screen screen         = Screen::Home;
    uint8_t cursor        = 0;
    uint8_t channel_index = 0;
    EditCtx edit;
    EditStringCtx str_edit;
    EditIpCtx ip_edit;
};

State s;

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

int char_to_idx(char c) {
    for (int i = 0; i < kAlphabetLen; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return 0;  // unknown → space
}

char idx_to_char(int i) {
    if (i < 0) i = ((i % kAlphabetLen) + kAlphabetLen) % kAlphabetLen;
    if (i >= kAlphabetLen) i %= kAlphabetLen;
    return kAlphabet[i];
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
    case led::ColorOrder::RGBWW: return "RGBWW";
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
    default: return "Hold";
    }
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
    }
}

// ── List rendering with scrolling viewport ─────────────────────────────────

struct ListItem {
    const char* label;
    const char* value;
    int8_t badge    = -1;  // ≥0 → draw a numbered badge (badge+1)
    Color badge_col = color::BadgeGreen;
    Color value_col = color::Gold;
};

void render_list(const char* title, const ListItem* items, uint8_t count, uint8_t cursor) {
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    // ── TFT: dark list with rounded cursor highlight ──────────────────────────
    canvas_clear(color::Black);

    // Header bar + thin accent line.
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    canvas_fill_rect(0, kHdrH - 1, kTW, 1, color::HeaderLine);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, title, color::Cream, color::HeaderBg, kTxtSc);

    const int visible = kMaxVis;
    int first         = 0;
    if (count > static_cast<uint8_t>(visible)) {
        if (cursor >= static_cast<uint8_t>(visible)) first = cursor - (visible - 1);
        if (first + visible > count) first = count - visible;
    }
    for (int i = 0; i < visible && first + i < count; ++i) {
        const int idx       = first + i;
        const int ry        = kHdrH + i * kItemH;
        const ListItem& it  = items[idx];
        const bool sel      = (idx == cursor);
        const bool is_back  = (it.label[0] == '[');
        const Color text_bg = sel ? color::CursorBg : color::Black;

        if (sel) {
            // Rounded warm highlight, inset, with a brighter top accent edge.
            canvas_fill_round_rect(3, ry + 1, kTW - 6, kItemH - 2, 6, color::CursorBg);
            canvas_fill_rect(9, ry + 1, kTW - 18, 2, color::Gold);
        }

        // Left gutter: numbered channel badge when present.
        if (it.badge >= 0) {
            canvas_fill_round_rect(5, ry + 3, kBadge, kBadge, 4, it.badge_col);
            char num[8];
            std::snprintf(num, sizeof(num), "%d", it.badge + 1);
            const int nx = 5 + (kBadge - kFontCellWidth * kTxtSc) / 2;
            canvas_draw_text(nx, ry + kPad, num, color::Black, it.badge_col, kTxtSc);
        }

        canvas_draw_text(kGutter, ry + kPad, it.label, color::Cream, text_bg, kTxtSc);

        // Chevron at the right edge for rows that open a sub-screen.
        const int chev_x = kTW - kChevW - kIndent;
        if (!is_back) {
            canvas_draw_text(chev_x, ry + kPad, ">", color::DarkGray, text_bg, kTxtSc);
        }

        // Right-aligned gold value, left of the chevron.
        if (it.value && it.value[0]) {
            const int vw   = static_cast<int>(std::strlen(it.value)) * kFontCellWidth * kTxtSc;
            const int vend = is_back ? (kTW - kIndent) : (chev_x - 6);
            canvas_draw_text(vend - vw, ry + kPad, it.value, it.value_col, text_bg, kTxtSc);
        }
    }
    // Scroll indicator
    if (count > static_cast<uint8_t>(visible)) {
        if (first > 0) canvas_fill_rect(kTW - 4, kHdrH, 4, 6, color::LightGray);
        if (first + visible < count) canvas_fill_rect(kTW - 4, kTH - 6, 4, 6, color::LightGray);
    }
#else
    // ── OLED: classic scrolling text list ────────────────────────────────────
    canvas_clear();
    draw_row(0, 0, title);
    const uint8_t visible = kRows - 1;
    uint8_t first         = 0;
    if (count > visible) {
        if (cursor >= visible) first = cursor - (visible - 1);
        if (first + visible > count) first = count - visible;
    }
    for (uint8_t i = 0; i < visible && first + i < count; ++i) {
        const uint8_t idx = first + i;
        char line[kOledCols + 1];
        const char prefix = (idx == cursor) ? '>' : ' ';
        if (items[idx].value && items[idx].value[0]) {
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
    // ── TFT dashboard ────────────────────────────────────────────────────────
    canvas_clear(color::Black);

    // Header bar
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    if (!config::is_persistence_ok()) {
        canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, "pixfrog [!NVS]", color::Red,
                         color::HeaderBg, kTxtSc);
    } else {
        canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, "pixfrog", color::White, color::HeaderBg,
                         kTxtSc);
    }

    const auto stats = dmx::get_stats();

    // Right side of header: FPS + link state
    const Color link_col = ui::is_link_up() ? color::Green : color::Red;
    const char* link_str = ui::is_link_up() ? "UP" : "DOWN";
    canvas_draw_text(kTW - static_cast<int>(std::strlen(link_str)) * kFontCellWidth * kTxtSc -
                         kIndent,
                     (kHdrH - kTxtH) / 2, link_str, link_col, color::HeaderBg, kTxtSc);

    std::snprintf(line, sizeof(line), "%lufps", static_cast<unsigned long>(stats.current_fps));
    const int fps_x = kTW - static_cast<int>(std::strlen(link_str)) * kFontCellWidth * kTxtSc -
                      kIndent - static_cast<int>(std::strlen(line)) * kFontCellWidth * kTxtSc - 8;
    canvas_draw_text(fps_x, (kHdrH - kTxtH) / 2, line, color::Cyan, color::HeaderBg, kTxtSc);

    // IP row below header
    int ry = kHdrH;
    canvas_fill_rect(0, ry, kTW, kItemH, color::AltRowBg);
    const uint32_t ip = ui::get_ip();
    if (ip == 0) {
        canvas_draw_text(kIndent, ry + kPad, "IP: ---", color::LightGray, color::AltRowBg, kTxtSc);
    } else {
        std::snprintf(line, sizeof(line), "IP: %u.%u.%u.%u",
                      static_cast<unsigned>((ip >> 24) & 0xFFu),
                      static_cast<unsigned>((ip >> 16) & 0xFFu),
                      static_cast<unsigned>((ip >> 8) & 0xFFu), static_cast<unsigned>(ip & 0xFFu));
        canvas_draw_text(kIndent, ry + kPad, line, color::White, color::AltRowBg, kTxtSc);
    }
    std::snprintf(line, sizeof(line), "Pkts:%llu",
                  static_cast<unsigned long long>(stats.artnet_packets_rx));
    canvas_draw_text(kTW - static_cast<int>(std::strlen(line)) * kFontCellWidth * kTxtSc - kIndent,
                     ry + kPad, line, color::LightGray, color::AltRowBg, kTxtSc);

    // 8 channel rows; fill remaining height evenly
    ry                 += kItemH;
    constexpr int kChH  = (kTH - kHdrH - kItemH) / 8;  // ~23px each
    for (int i = 0; i < 8; ++i) {
        const int cy       = ry + i * kChH;
        const Color row_bg = (i & 1) ? color::AltRowBg : color::Black;
        canvas_fill_rect(0, cy, kTW, kChH, row_bg);

        const auto& cc = config::get_channel(i);
        const int ty   = cy + (kChH - kTxtH) / 2;
        char ch_label[4];
        std::snprintf(ch_label, sizeof(ch_label), "CH%d", i + 1);

        // Disabled channel: greyed label + "Off", no universe/pixels/activity.
        if (led::is_off(cc.protocol)) {
            canvas_draw_text(kIndent, ty, ch_label, color::DarkGray, row_bg, kTxtSc);
            canvas_draw_text(kIndent + 4 * kFontCellWidth * kTxtSc, ty, "Off", color::DarkGray,
                             row_bg, kTxtSc);
            continue;
        }

        canvas_draw_text(kIndent, ty, ch_label, color::White, row_bg, kTxtSc);

        // Protocol
        const char* proto = protocol_name(cc.protocol);
        canvas_draw_text(kIndent + 4 * kFontCellWidth * kTxtSc, ty, proto, color::Gold, row_bg,
                         kTxtSc);

        // Universe
        std::snprintf(line, sizeof(line), "%u", cc.universe_start);
        canvas_draw_text(kIndent + 14 * kFontCellWidth * kTxtSc, ty, line, color::LightGray, row_bg,
                         kTxtSc);

        // Pixels
        std::snprintf(line, sizeof(line), "%upx", cc.pixel_count);
        canvas_draw_text(kIndent + 19 * kFontCellWidth * kTxtSc, ty, line, color::DarkGray, row_bg,
                         kTxtSc);

        // Activity indicator dot on the right edge
        const bool active   = dmx::is_channel_active(i);
        const bool ok       = dmx::is_channel_capacity_ok(i);
        const bool fsafe    = dmx::is_channel_failsafe(i);
        const Color act_col = !ok    ? color::Red
                            : fsafe  ? color::Orange
                            : active ? color::Green
                                     : color::DarkGray;
        canvas_fill_rect(kTW - 10, cy + 2, 8, kChH - 4, act_col);
    }
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

constexpr uint8_t kMainItemCount                  = 13;
constexpr const char* kMainLabels[kMainItemCount] = {
    "ArtNet",       "Network",   "Channel 1",      "Channel 2", "Channel 3",
    "Channel 4",    "Channel 5", "Channel 6",      "Channel 7", "Channel 8",
    "Test pattern", "About",     "[Back to HOME]",
};

void render_main_menu() {
    ListItem items[kMainItemCount];
    for (uint8_t i = 0; i < kMainItemCount; ++i) {
        items[i].label = kMainLabels[i];
        items[i].value = "";
        // Channel rows (indices 2..9): show the configured protocol + a badge.
        // A disabled channel ("Off") is greyed out instead of gold.
        if (i >= 2 && i <= 9) {
            const auto& cc     = config::get_channel(i - 2);
            items[i].value     = protocol_name(cc.protocol);  // points to static string
            items[i].badge     = static_cast<int8_t>(i - 2);
            items[i].badge_col = badge_color(cc.protocol);
            items[i].value_col = led::is_off(cc.protocol) ? color::DarkGray : color::Gold;
        }
    }
    render_list("MENU", items, kMainItemCount, s.cursor);
}

void dispatch_main_menu(Event e) {
    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < kMainItemCount - 1) s.cursor++;
    if (e == Event::Click) {
        if (s.cursor == 0) {
            s.screen = Screen::ArtnetMenu;
            s.cursor = 0;
        } else if (s.cursor == 1) {
            s.screen = Screen::NetworkMenu;
            s.cursor = 0;
        } else if (s.cursor >= 2 && s.cursor <= 9) {
            s.channel_index = s.cursor - 2;
            s.screen        = Screen::ChannelMenu;
            s.cursor        = 0;
        } else if (s.cursor == 10) {
            s.screen = Screen::TestPatternMenu;
            s.cursor = 0;
        } else if (s.cursor == 11) {
            s.screen = Screen::About;
            s.cursor = 0;
        } else {
            s.screen = Screen::Home;
            s.cursor = 0;
        }
    }
}

// ── ABOUT ───────────────────────────────────────────────────────────────────

void render_about() {
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    canvas_clear(color::Black);
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, "ABOUT", color::White, color::HeaderBg, kTxtSc);

    int y = kHdrH + 20;
    canvas_draw_text(kIndent, y, "pixfrog", color::Cream, color::Black, kTxtSc);
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
    if (e == Event::Click) {
        s.screen = Screen::MainMenu;
        s.cursor = 11;
    }
}

// ── TEST PATTERN MENU (TODO B5) ─────────────────────────────────────────────

constexpr uint8_t kTestPatternItemCount                         = 4;
constexpr const char* kTestPatternLabels[kTestPatternItemCount] = {
    "Pat 0 sq 1kHz",
    "Pat 1 walk-1",
    "Pat 2 altern.",
    "[Stop & Back]",
};

void render_test_pattern_menu() {
    const int8_t active                               = lcd::get_calibration_mode();
    char marked[kTestPatternItemCount][kOledCols + 1] = {};
    ListItem items[kTestPatternItemCount];
    for (uint8_t i = 0; i < kTestPatternItemCount; ++i) {
        if (i < 3 && active == static_cast<int8_t>(i)) {
            std::snprintf(marked[i], kOledCols + 1, "%s *", kTestPatternLabels[i]);
            items[i].label = marked[i];
        } else {
            items[i].label = kTestPatternLabels[i];
        }
        items[i].value = "";
    }
    render_list("TEST PATTERN", items, kTestPatternItemCount, s.cursor);
}

void dispatch_test_pattern_menu(Event e) {
    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < kTestPatternItemCount - 1) s.cursor++;
    if (e != Event::Click) return;
    if (s.cursor < 3) {
        // Activate the chosen pattern; stay in the menu so the user can
        // switch between patterns or stop without leaving.
        lcd::set_calibration_mode(static_cast<int8_t>(s.cursor));
    } else {
        // Stop calibration and return.
        lcd::set_calibration_mode(-1);
        s.screen = Screen::MainMenu;
        s.cursor = 10;
    }
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
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    canvas_clear(color::Black);
    // Header
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    char hdr[32];
    std::snprintf(hdr, sizeof(hdr), "EDIT %s", s.edit.label);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, hdr, color::White, color::HeaderBg, kTxtSc);

    // Large current value (scale 3)
    constexpr int kBigSc = 3;
    constexpr int kBigH  = 8 * kBigSc;
    char val[24];
    format_value(s.edit, s.edit.current, val, sizeof(val));
    const int val_w = static_cast<int>(std::strlen(val)) * kFontCellWidth * kBigSc;
    canvas_draw_text((kTW - val_w) / 2, kHdrH + 30, val, color::Cyan, color::Black, kBigSc);

    // "was: X" when changed
    if (s.edit.current != s.edit.original) {
        char orig[24];
        format_value(s.edit, s.edit.original, orig, sizeof(orig));
        char was[32];
        std::snprintf(was, sizeof(was), "was: %s", orig);
        canvas_draw_text(kIndent, kHdrH + 30 + kBigH + 12, was, color::Orange, color::Black,
                         kTxtSc);
    }
#else
    canvas_clear();
    // Buffers are sized to hold the prefix plus a full kOledCols-wide value.
    char line[kOledCols + 4];
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
    case Field::GlobalRefresh: {
        auto g            = config::get_global();
        g.refresh_rate_hz = static_cast<uint8_t>(v);
        config::set_global(g);
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
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }

    default: break;
    }
}

void dispatch_edit_value(Event e) {
    if (e == Event::RotateLeft || e == Event::RotateRight) {
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
        char c    = s.str_edit.buf[win_start + i];
        window[i] = (c == ' ') ? '_' : c;
    }

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    canvas_clear(color::Black);
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, title, color::White, color::HeaderBg, kTxtSc);

    canvas_draw_text(kIndent, kHdrH + 20, window, color::Cyan, color::Black, kTxtSc);

    if (cursor < len) {
        // Highlight active char
        const int col    = cursor - win_start;
        const int high_x = kIndent + col * kFontCellWidth * kTxtSc;
        canvas_fill_rect(high_x, kHdrH + 20 + kTxtH + 4, kFontCellWidth * kTxtSc, 4, color::Cyan);
    } else {
        canvas_draw_text(kIndent, kHdrH + 20 + kTxtH + 10, "[end of string]", color::Orange,
                         color::Black, kTxtSc);
    }
#else
    canvas_clear();
    draw_row(0, 0, title);
    draw_row(2, 0, window);

    if (cursor < len) {
        char caret[kWin + 1];
        std::memset(caret, 0, sizeof(caret));
        const int col = cursor - win_start;
        for (int i = 0; i < col && i < kWin; ++i)
            caret[i] = ' ';
        if (col >= 0 && col < kWin) caret[col] = '^';
        draw_row(3, 0, caret);
    } else {
        draw_row(3, 0, "[end of string]");
    }
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
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            int idx                            = char_to_idx(s.str_edit.buf[s.str_edit.cursor]);
            idx                               += (e == Event::RotateRight) ? 1 : -1;
            s.str_edit.buf[s.str_edit.cursor]  = idx_to_char(idx);
        } else if (e == Event::Click) {
            s.str_edit.cursor++;
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

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    canvas_clear(color::Black);
    canvas_fill_rect(0, 0, kTW, kHdrH, color::HeaderBg);
    canvas_draw_text(kIndent, (kHdrH - kTxtH) / 2, title, color::White, color::HeaderBg, kTxtSc);

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

// ── ARTNET MENU ─────────────────────────────────────────────────────────────

void render_artnet_menu() {
    char vnet[8], vsub[8], vrefresh[8], vunicast[8], vsacn[8], vfsm[8], vfst[8];
    char vshort[14], vlong[14];
    const auto& g = config::get_global();
    std::snprintf(vnet, sizeof(vnet), "%u", g.artnet_net);
    std::snprintf(vsub, sizeof(vsub), "%u", g.artnet_subnet);
    std::snprintf(vrefresh, sizeof(vrefresh), "%uHz", g.refresh_rate_hz);
    std::snprintf(vunicast, sizeof(vunicast), "%s", g.artnet_poll_reply_unicast ? "ON" : "OFF");
    std::snprintf(vsacn, sizeof(vsacn), "%s", g.sacn_enabled ? "ON" : "OFF");
    std::snprintf(vfsm, sizeof(vfsm), "%s", failsafe_name(g.failsafe_mode));
    std::snprintf(vfst, sizeof(vfst), "%us", g.failsafe_timeout_s);
    truncate(vshort, sizeof(vshort), g.short_name);
    truncate(vlong, sizeof(vlong), g.long_name);

    ListItem items[10] = {
        { "Net", vnet },         { "Sub", vsub },         { "Short", vshort }, { "Long", vlong },
        { "Refresh", vrefresh }, { "Unicast", vunicast }, { "sACN", vsacn },   { "FSafe", vfsm },
        { "FSafeS", vfst },      { "[Back]", "" },
    };
    render_list("ARTNET", items, 10, s.cursor);
}

void dispatch_artnet_menu(Event e) {
    constexpr uint8_t kCount = 10;
    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < kCount - 1) s.cursor++;
    if (e != Event::Click) return;
    const auto& g = config::get_global();
    switch (s.cursor) {
    case 0:
        enter_edit(Field::ArtnetNet, ValueKind::Int, g.artnet_net, 0, 127, 1, "Net",
                   Screen::ArtnetMenu);
        break;
    case 1:
        enter_edit(Field::ArtnetSubnet, ValueKind::Int, g.artnet_subnet, 0, 15, 1, "Sub",
                   Screen::ArtnetMenu);
        break;
    case 2:
        enter_edit_string(StringField::ArtnetShort, g.short_name, sizeof(g.short_name) - 1, "Short",
                          Screen::ArtnetMenu);
        break;
    case 3:
        enter_edit_string(StringField::ArtnetLong, g.long_name, sizeof(g.long_name) - 1, "Long",
                          Screen::ArtnetMenu);
        break;
    // Refresh: step 30 → only [30, 60] reachable; satisfies spec §4.
    case 4:
        enter_edit(Field::GlobalRefresh, ValueKind::Int, g.refresh_rate_hz, 30, 60, 30, "Refresh",
                   Screen::ArtnetMenu);
        break;
    case 5:
        enter_edit(Field::ArtnetReplyUnicast, ValueKind::Bool, g.artnet_poll_reply_unicast ? 1 : 0,
                   0, 1, 1, "Unicast", Screen::ArtnetMenu);
        break;
    case 6:
        enter_edit(Field::ArtnetSacn, ValueKind::Bool, g.sacn_enabled ? 1 : 0, 0, 1, 1, "sACN",
                   Screen::ArtnetMenu);
        break;
    case 7:
        enter_edit(Field::ArtnetFailsafeMode, ValueKind::Failsafe, g.failsafe_mode, 0, 2, 1,
                   "FSafe", Screen::ArtnetMenu);
        break;
    case 8:
        enter_edit(Field::ArtnetFailsafeTimeout, ValueKind::Int, g.failsafe_timeout_s, 0, 3600, 1,
                   "FSafeS", Screen::ArtnetMenu);
        break;
    case 9:
        s.screen = Screen::MainMenu;
        s.cursor = 0;
        break;
    }
}

// ── NETWORK MENU ────────────────────────────────────────────────────────────

void render_network_menu() {
    char vdhcp[8], vip[kOledCols + 1], vmsk[kOledCols + 1], vgw[kOledCols + 1], vweb[8];
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

    ListItem items[6] = {
        { "DHCP", vdhcp }, { "IP", vip },      { "Msk", vmsk },
        { "GW", vgw },     { "Web UI", vweb }, { "[Back]", "" },
    };
    render_list("NETWORK", items, 6, s.cursor);
}

void dispatch_network_menu(Event e) {
    constexpr uint8_t kCount = 6;
    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < kCount - 1) s.cursor++;
    if (e != Event::Click) return;
    const auto& g = config::get_global();
    switch (s.cursor) {
    case 0:
        enter_edit(Field::NetworkDhcp, ValueKind::Bool, g.use_dhcp ? 1 : 0, 0, 1, 1, "DHCP",
                   Screen::NetworkMenu);
        break;
    case 1: enter_edit_ip(IpField::StaticIp, g.static_ip, "IP", Screen::NetworkMenu); break;
    case 2: enter_edit_ip(IpField::StaticMask, g.static_mask, "Mask", Screen::NetworkMenu); break;
    case 3: enter_edit_ip(IpField::StaticGw, g.static_gateway, "GW", Screen::NetworkMenu); break;
    case 4:
        enter_edit(Field::NetworkWebEnabled, ValueKind::Bool, g.web_enabled ? 1 : 0, 0, 1, 1,
                   "Web UI", Screen::NetworkMenu);
        break;
    case 5:
        s.screen = Screen::MainMenu;
        s.cursor = 1;
        break;
    }
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kColorOrderCount = static_cast<size_t>(led::ColorOrder::COUNT);
constexpr size_t kProtocolCount   = static_cast<size_t>(led::Protocol::COUNT);

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
    Group,
    Invert,
    Clock,
    Back,
};

// Fills `out` (capacity ≥ 10) with the ordered items for `cc` and returns count.
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
        out[n++] = ChItem::Group;
        out[n++] = ChItem::Invert;
        if (led::is_clocked(cc.protocol)) out[n++] = ChItem::Clock;
    }
    out[n++] = ChItem::Back;
    return n;
}

uint8_t channel_item_count() {
    ChItem items[10];
    return channel_items(config::get_channel(s.channel_index), items);
}

void render_channel_menu() {
    const auto& cc = config::get_channel(s.channel_index);
    const bool dmx = led::is_dmx(cc.protocol);
    char title[kOledCols + 1];
    std::snprintf(title, sizeof(title), "CHANNEL %u", s.channel_index + 1);

    char vproto[8], vuni[8], vdmx[8], vpix[8], vorder[8], vbri[8], vgrp[8], vinv[8], vclk[12];
    std::snprintf(vproto, sizeof(vproto), "%s", protocol_name(cc.protocol));
    std::snprintf(vuni, sizeof(vuni), "%u", cc.universe_start);
    std::snprintf(vdmx, sizeof(vdmx), "%u", cc.dmx_start);
    std::snprintf(vpix, sizeof(vpix), "%u", cc.pixel_count);
    std::snprintf(vorder, sizeof(vorder), "%s", color_order_name(cc.color_order));
    std::snprintf(vbri, sizeof(vbri), "%u", cc.brightness);
    std::snprintf(vgrp, sizeof(vgrp), "%u", cc.grouping);
    std::snprintf(vinv, sizeof(vinv), "%s", cc.invert_direction ? "ON" : "OFF");
    std::snprintf(vclk, sizeof(vclk), "%lukHz", static_cast<unsigned long>(cc.clock_hz / 1000u));

    ChItem order[10];
    const uint8_t count = channel_items(cc, order);
    ListItem items[10];
    for (uint8_t i = 0; i < count; ++i) {
        switch (order[i]) {
        case ChItem::Proto: items[i] = { "Proto", vproto }; break;
        case ChItem::Uni: items[i] = { "Uni", vuni }; break;
        case ChItem::Dmx: items[i] = { "DMX", vdmx }; break;
        case ChItem::Pixels: items[i] = { dmx ? "Slots" : "Pixels", vpix }; break;
        case ChItem::Order: items[i] = { "Order", vorder }; break;
        case ChItem::Bright: items[i] = { "Bright", vbri }; break;
        case ChItem::Group: items[i] = { "Group", vgrp }; break;
        case ChItem::Invert: items[i] = { "Invert", vinv }; break;
        case ChItem::Clock: items[i] = { "Clock", vclk }; break;
        case ChItem::Back: items[i] = { "[Back]", "" }; break;
        }
    }
    render_list(title, items, count, s.cursor);
}

void dispatch_channel_menu(Event e) {
    const auto& cc = config::get_channel(s.channel_index);
    ChItem order[10];
    const uint8_t count = channel_items(cc, order);

    if (e == Event::RotateLeft && s.cursor > 0) s.cursor--;
    if (e == Event::RotateRight && s.cursor < count - 1) s.cursor++;
    if (e != Event::Click) return;

    const Screen ret = Screen::ChannelMenu;
    const uint8_t ch = s.channel_index;
    const bool dmx   = led::is_dmx(cc.protocol);

    switch (order[s.cursor]) {
    case ChItem::Proto:
        enter_edit(Field::ChProtocol, ValueKind::Protocol, static_cast<int32_t>(cc.protocol), 0,
                   static_cast<int32_t>(kProtocolCount) - 1, 1, "Proto", ret, ch);
        break;
    case ChItem::Uni:
        enter_edit(Field::ChUniverse, ValueKind::Int, cc.universe_start, 1, 32767, 1, "Uni", ret,
                   ch);
        break;
    case ChItem::Dmx:
        enter_edit(Field::ChDmx, ValueKind::Int, cc.dmx_start, 1, 512, 1, "DMX", ret, ch);
        break;
    case ChItem::Pixels:
        // DMX512 maps one universe → up to 512 slots; LED strips go up to 1024 px.
        enter_edit(Field::ChPixels, ValueKind::Int, cc.pixel_count, 1, dmx ? 512 : 1024, 1,
                   dmx ? "Slots" : "Pixels", ret, ch);
        // Light the strip as a live ruler while the count is being edited
        // (LED protocols only — a DMX512 universe has nothing to show).
        if (!dmx) dmx::set_pixel_preview(ch, cc.pixel_count);
        break;
    case ChItem::Order:
        enter_edit(Field::ChColorOrder, ValueKind::ColorOrder, static_cast<int32_t>(cc.color_order),
                   0, static_cast<int32_t>(kColorOrderCount) - 1, 1, "Order", ret, ch);
        break;
    case ChItem::Bright:
        enter_edit(Field::ChBrightness, ValueKind::Int, cc.brightness, 0, 255, 1, "Bright", ret,
                   ch);
        break;
    case ChItem::Group:
        enter_edit(Field::ChGrouping, ValueKind::Int, cc.grouping, 1, 8, 1, "Group", ret, ch);
        break;
    case ChItem::Invert:
        enter_edit(Field::ChInvert, ValueKind::Bool, cc.invert_direction ? 1 : 0, 0, 1, 1, "Invert",
                   ret, ch);
        break;
    case ChItem::Clock:
        enter_edit(Field::ChClock, ValueKind::Int, static_cast<int32_t>(cc.clock_hz), 250'000,
                   24'000'000, 250'000, "Clock", ret, ch);
        break;
    case ChItem::Back:
        s.screen = Screen::MainMenu;
        s.cursor = 2 + ch;
        break;
    }
}

// ── Long press: commit-as-is and climb one level ────────────────────────────
// In any edit screen the current state is committed instantly (a string edit
// keeps every remaining character as-is); in a list menu it acts as Back.

void dispatch_long_press() {
    switch (s.screen) {
    case Screen::Home: break;
    case Screen::MainMenu:
        s.screen = Screen::Home;
        s.cursor = 0;
        break;
    case Screen::ArtnetMenu:
        s.screen = Screen::MainMenu;
        s.cursor = 0;
        break;
    case Screen::NetworkMenu:
        s.screen = Screen::MainMenu;
        s.cursor = 1;
        break;
    case Screen::ChannelMenu:
        s.screen = Screen::MainMenu;
        s.cursor = 2 + s.channel_index;
        break;
    case Screen::TestPatternMenu:
        lcd::set_calibration_mode(-1);
        s.screen = Screen::MainMenu;
        s.cursor = 10;
        break;
    case Screen::About:
        s.screen = Screen::MainMenu;
        s.cursor = 11;
        break;
    case Screen::EditValue:
        dmx::clear_pixel_preview();
        commit_edit();
        s.screen = s.edit.return_screen;
        break;
    case Screen::EditString:
        commit_edit_string();
        s.screen = s.str_edit.return_screen;
        break;
    case Screen::EditIp:
        commit_edit_ip();
        s.screen = s.ip_edit.return_screen;
        break;
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
    case Screen::MainMenu: render_main_menu(); break;
    case Screen::ArtnetMenu: render_artnet_menu(); break;
    case Screen::NetworkMenu: render_network_menu(); break;
    case Screen::ChannelMenu: render_channel_menu(); break;
    case Screen::TestPatternMenu: render_test_pattern_menu(); break;
    case Screen::About: render_about(); break;
    case Screen::EditValue: render_edit_value(); break;
    case Screen::EditString: render_edit_string(); break;
    case Screen::EditIp: render_edit_ip(); break;
    }
}

void menu_dispatch(Event e) {
    if (e == Event::LongPress) {
        dispatch_long_press();
        return;
    }
    switch (s.screen) {
    case Screen::Home:
        if (e == Event::Click) {
            s.screen = Screen::MainMenu;
            s.cursor = 0;
        }
        break;
    case Screen::MainMenu: dispatch_main_menu(e); break;
    case Screen::ArtnetMenu: dispatch_artnet_menu(e); break;
    case Screen::NetworkMenu: dispatch_network_menu(e); break;
    case Screen::ChannelMenu: dispatch_channel_menu(e); break;
    case Screen::TestPatternMenu: dispatch_test_pattern_menu(e); break;
    case Screen::About: dispatch_about(e); break;
    case Screen::EditValue: dispatch_edit_value(e); break;
    case Screen::EditString: dispatch_edit_string(e); break;
    case Screen::EditIp: dispatch_edit_ip(e); break;
    }
}

void menu_on_idle_timeout() {
    dmx::clear_pixel_preview();
    s.screen = Screen::Home;
    s.cursor = 0;
}

bool menu_is_home() {
    return s.screen == Screen::Home;
}

#ifdef PIXFROG_EMULATOR
void menu_debug_state(const char** screen_name, int* cursor, int* channel) {
    static const char* const kNames[] = {
        "Home",  "MainMenu",  "ArtnetMenu", "NetworkMenu", "ChannelMenu", "TestPatternMenu",
        "About", "EditValue", "EditString", "EditIp",
    };
    if (screen_name) *screen_name = kNames[static_cast<uint8_t>(s.screen)];
    if (cursor) *cursor = s.cursor;
    if (channel) *channel = s.channel_index;
}
#endif

}  // namespace pixfrog::ui::detail
