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
#include "led_protocols.h"
#include "ui.h"

namespace pixfrog::ui::detail {

namespace {

constexpr uint8_t kRows  = 8;
constexpr uint8_t kCols  = 21;

// ── Screens ─────────────────────────────────────────────────────────────────
enum class Screen : uint8_t {
    Home,
    MainMenu,
    ArtnetMenu,
    NetworkMenu,
    ChannelMenu,
    EditValue,
    EditString,
    EditIp,
};

// ── Editable fields ─────────────────────────────────────────────────────────
enum class Field : uint8_t {
    None,
    ArtnetNet,
    ArtnetSubnet,
    NetworkDhcp,
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
};

struct EditCtx {
    Field       field         = Field::None;
    ValueKind   kind          = ValueKind::Int;
    int32_t     current       = 0;
    int32_t     step          = 1;
    int32_t     min           = 0;
    int32_t     max           = 0;
    uint8_t     channel       = 0;
    Screen      return_screen = Screen::MainMenu;
    const char* label         = "";
};

struct EditStringCtx {
    char        buf[65]       = {};   // max(short=18, long=64) + null
    uint8_t     max_len       = 0;
    uint8_t     cursor        = 0;    // 0..max_len; max_len = DONE position
    StringField field         = StringField::ArtnetShort;
    Screen      return_screen = Screen::ArtnetMenu;
    const char* label         = "";
};

struct EditIpCtx {
    uint32_t    value         = 0;    // host-order
    uint8_t     cursor        = 0;    // 0..4; 4 = DONE position
    IpField     field         = IpField::StaticIp;
    Screen      return_screen = Screen::NetworkMenu;
    const char* label         = "";
};

struct State {
    Screen        screen        = Screen::Home;
    uint8_t       cursor        = 0;
    uint8_t       channel_index = 0;
    EditCtx       edit;
    EditStringCtx str_edit;
    EditIpCtx     ip_edit;
};

State s;

// ── Alphabet for string editing ─────────────────────────────────────────────
constexpr const char kAlphabet[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "-_.";
constexpr int kAlphabetLen = sizeof(kAlphabet) - 1;  // 66

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
        case led::Protocol::WS2815:  return "WS2815";
        case led::Protocol::WS2812B: return "WS2812B";
        case led::Protocol::WS2811:  return "WS2811";
        case led::Protocol::SK6812:  return "SK6812";
        case led::Protocol::WS2814:  return "WS2814";
        case led::Protocol::APA102:  return "APA102";
        case led::Protocol::SK9822:  return "SK9822";
        case led::Protocol::LPD8806: return "LPD8806";
        default:                     return "?";
    }
}

const char* color_order_name(led::ColorOrder o) {
    switch (o) {
        case led::ColorOrder::RGB:   return "RGB";
        case led::ColorOrder::RBG:   return "RBG";
        case led::ColorOrder::GRB:   return "GRB";
        case led::ColorOrder::GBR:   return "GBR";
        case led::ColorOrder::BRG:   return "BRG";
        case led::ColorOrder::BGR:   return "BGR";
        case led::ColorOrder::RGBW:  return "RGBW";
        case led::ColorOrder::GRBW:  return "GRBW";
        case led::ColorOrder::RGBWW: return "RGBWW";
        default:                     return "?";
    }
}

void truncate(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

void format_value(const EditCtx& e, int32_t v, char* out, size_t cap) {
    switch (e.kind) {
        case ValueKind::Int:        std::snprintf(out, cap, "%ld", static_cast<long>(v));                                 return;
        case ValueKind::Bool:       std::snprintf(out, cap, "%s",  v ? "ON" : "OFF");                                     return;
        case ValueKind::Protocol:   std::snprintf(out, cap, "%s",  protocol_name(static_cast<led::Protocol>(v)));         return;
        case ValueKind::ColorOrder: std::snprintf(out, cap, "%s",  color_order_name(static_cast<led::ColorOrder>(v)));    return;
    }
}

// ── List rendering with scrolling viewport ─────────────────────────────────

struct ListItem {
    const char* label;
    const char* value;
};

void render_list(const char* title, const ListItem* items, uint8_t count, uint8_t cursor) {
    oled_clear();
    oled_draw_text(0, 0, title);
    const uint8_t visible = kRows - 1;
    uint8_t first = 0;
    if (count > visible) {
        if (cursor >= visible) first = cursor - (visible - 1);
        if (first + visible > count) first = count - visible;
    }
    for (uint8_t i = 0; i < visible && first + i < count; ++i) {
        const uint8_t idx = first + i;
        char line[kCols + 1];
        const char prefix = (idx == cursor) ? '>' : ' ';
        if (items[idx].value && items[idx].value[0]) {
            std::snprintf(line, sizeof(line), "%c%s:%s",
                          prefix, items[idx].label, items[idx].value);
        } else {
            std::snprintf(line, sizeof(line), "%c%s",
                          prefix, items[idx].label);
        }
        oled_draw_text(static_cast<uint8_t>(1 + i), 0, line);
    }
}

// ── HOME ────────────────────────────────────────────────────────────────────

void render_home() {
    char line[kCols + 1];
    oled_clear();
    oled_draw_text(0, 0, "pixfrog");

    const uint32_t ip = ui::get_ip();
    if (ip == 0) {
        oled_draw_text(1, 0, "IP   : ---.---.---.---");
    } else {
        std::snprintf(line, sizeof(line), "IP   : %u.%u.%u.%u",
                      (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu,
                      (ip >>  8) & 0xFFu,  ip        & 0xFFu);
        oled_draw_text(1, 0, line);
    }

    const auto stats = dmx::get_stats();
    std::snprintf(line, sizeof(line), "FPS  : %lu",
                  static_cast<unsigned long>(stats.current_fps));
    oled_draw_text(2, 0, line);
    std::snprintf(line, sizeof(line), "Pkts : %llu",
                  static_cast<unsigned long long>(stats.artnet_packets_rx));
    oled_draw_text(3, 0, line);

    oled_draw_text(5, 0, "CH 1 2 3 4 5 6 7 8");
    char ch_line[kCols + 1];
    ch_line[0] = ' '; ch_line[1] = ' '; ch_line[2] = ' ';
    for (int i = 0; i < 8; ++i) {
        ch_line[3 + i * 2]     = dmx::is_channel_active(i) ? '*' : '-';
        ch_line[3 + i * 2 + 1] = ' ';
    }
    ch_line[3 + 8 * 2 - 1] = '\0';
    oled_draw_text(6, 0, ch_line);
}

// ── MAIN MENU ───────────────────────────────────────────────────────────────

constexpr uint8_t kMainItemCount = 11;
constexpr const char* kMainLabels[kMainItemCount] = {
    "ArtNet",  "Network",
    "Channel 1", "Channel 2", "Channel 3", "Channel 4",
    "Channel 5", "Channel 6", "Channel 7", "Channel 8",
    "[Back to HOME]",
};

void render_main_menu() {
    ListItem items[kMainItemCount];
    for (uint8_t i = 0; i < kMainItemCount; ++i) {
        items[i].label = kMainLabels[i];
        items[i].value = "";
    }
    render_list("MENU", items, kMainItemCount, s.cursor);
}

void dispatch_main_menu(Event e) {
    if (e == Event::RotateLeft  && s.cursor > 0)                   s.cursor--;
    if (e == Event::RotateRight && s.cursor < kMainItemCount - 1)  s.cursor++;
    if (e == Event::Click) {
        if (s.cursor == 0)                          { s.screen = Screen::ArtnetMenu;  s.cursor = 0; }
        else if (s.cursor == 1)                     { s.screen = Screen::NetworkMenu; s.cursor = 0; }
        else if (s.cursor >= 2 && s.cursor <= 9)    {
            s.channel_index = s.cursor - 2;
            s.screen = Screen::ChannelMenu;
            s.cursor = 0;
        }
        else                                        { s.screen = Screen::Home; s.cursor = 0; }
    }
}

// ── EDIT VALUE — generic int / bool / enum ──────────────────────────────────

void enter_edit(Field field, ValueKind kind, int32_t cur, int32_t mn, int32_t mx,
                int32_t step, const char* label, Screen return_screen,
                uint8_t channel = 0xFF) {
    s.edit.field         = field;
    s.edit.kind          = kind;
    s.edit.current       = cur;
    s.edit.min           = mn;
    s.edit.max           = mx;
    s.edit.step          = step;
    s.edit.label         = label;
    s.edit.return_screen = return_screen;
    s.edit.channel       = channel;
    s.screen             = Screen::EditValue;
}

void render_edit_value() {
    oled_clear();
    char line[kCols + 1];
    std::snprintf(line, sizeof(line), "EDIT %s", s.edit.label);
    oled_draw_text(0, 0, line);

    char val[kCols + 1];
    format_value(s.edit, s.edit.current, val, sizeof(val));
    std::snprintf(line, sizeof(line), "  %s", val);
    oled_draw_text(3, 0, line);

    oled_draw_text(5, 0, "rotate to change");
    oled_draw_text(6, 0, "click to commit");
}

void commit_edit() {
    const int32_t v = s.edit.current;
    switch (s.edit.field) {
        case Field::ArtnetNet:    { auto g = config::get_global(); g.artnet_net    = static_cast<uint8_t>(v); config::set_global(g); dmx::mark_global_dirty(); break; }
        case Field::ArtnetSubnet: { auto g = config::get_global(); g.artnet_subnet = static_cast<uint8_t>(v); config::set_global(g); dmx::mark_global_dirty(); break; }
        case Field::NetworkDhcp:  { auto g = config::get_global(); g.use_dhcp      = (v != 0);               config::set_global(g); dmx::mark_global_dirty(); break; }

        case Field::ChProtocol:   { auto c = config::get_channel(s.edit.channel); c.protocol         = static_cast<led::Protocol>(v);   config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChColorOrder: { auto c = config::get_channel(s.edit.channel); c.color_order      = static_cast<led::ColorOrder>(v); config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChUniverse:   { auto c = config::get_channel(s.edit.channel); c.universe_start   = static_cast<uint16_t>(v);        config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChDmx:        { auto c = config::get_channel(s.edit.channel); c.dmx_start        = static_cast<uint16_t>(v);        config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChPixels:     { auto c = config::get_channel(s.edit.channel); c.pixel_count      = static_cast<uint16_t>(v);        config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChBrightness: { auto c = config::get_channel(s.edit.channel); c.brightness       = static_cast<uint8_t>(v);         config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChGrouping:   { auto c = config::get_channel(s.edit.channel); c.grouping         = static_cast<uint8_t>(v);         config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChInvert:     { auto c = config::get_channel(s.edit.channel); c.invert_direction = (v != 0);                        config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }
        case Field::ChClock:      { auto c = config::get_channel(s.edit.channel); c.clock_hz         = static_cast<uint32_t>(v);        config::set_channel(s.edit.channel, c); dmx::mark_channel_dirty(s.edit.channel); break; }

        default: break;
    }
}

void dispatch_edit_value(Event e) {
    if (e == Event::RotateLeft)  s.edit.current -= s.edit.step;
    if (e == Event::RotateRight) s.edit.current += s.edit.step;
    if (s.edit.current < s.edit.min) s.edit.current = s.edit.min;
    if (s.edit.current > s.edit.max) s.edit.current = s.edit.max;
    if (e == Event::Click) {
        commit_edit();
        s.screen = s.edit.return_screen;
    }
}

// ── EDIT STRING — char-by-char ──────────────────────────────────────────────

void enter_edit_string(StringField field, const char* source, uint8_t max_len,
                       const char* label, Screen return_screen) {
    s.str_edit.field         = field;
    s.str_edit.max_len       = max_len;
    s.str_edit.cursor        = 0;
    s.str_edit.return_screen = return_screen;
    s.str_edit.label         = label;
    std::memset(s.str_edit.buf, ' ', sizeof(s.str_edit.buf));
    s.str_edit.buf[max_len]  = '\0';
    for (uint8_t i = 0; i < max_len && source[i] != '\0'; ++i) {
        s.str_edit.buf[i] = source[i];
    }
    s.screen = Screen::EditString;
}

void render_edit_string() {
    oled_clear();
    char title[kCols + 1];
    if (s.str_edit.cursor < s.str_edit.max_len) {
        std::snprintf(title, sizeof(title), "EDIT %s [%u/%u]",
                      s.str_edit.label,
                      static_cast<unsigned>(s.str_edit.cursor + 1),
                      static_cast<unsigned>(s.str_edit.max_len));
    } else {
        std::snprintf(title, sizeof(title), "EDIT %s [end]",
                      s.str_edit.label);
    }
    oled_draw_text(0, 0, title);

    // Window of up to 20 chars centred on cursor.
    constexpr int kWin = 20;
    const uint8_t cursor = s.str_edit.cursor;
    const uint8_t len    = s.str_edit.max_len;
    int win_start = static_cast<int>(cursor) - 10;
    if (win_start < 0)            win_start = 0;
    if (win_start + kWin > len)   win_start = len - kWin;
    if (win_start < 0)            win_start = 0;

    char window[kWin + 1] = {};
    for (int i = 0; i < kWin && win_start + i < len; ++i) {
        char c = s.str_edit.buf[win_start + i];
        window[i] = (c == ' ') ? '_' : c;   // visible underscore for spaces
    }
    oled_draw_text(2, 0, window);

    // Cursor caret '^' below the char.
    if (cursor < len) {
        char caret[kWin + 1] = {};
        const int col = cursor - win_start;
        for (int i = 0; i < col && i < kWin; ++i) caret[i] = ' ';
        if (col >= 0 && col < kWin) caret[col] = '^';
        oled_draw_text(3, 0, caret);
        oled_draw_text(5, 0, "rotate=change char");
        oled_draw_text(6, 0, "click=next char");
    } else {
        oled_draw_text(3, 0, "[end of string]");
        oled_draw_text(5, 0, "rotate=back to chr");
        oled_draw_text(6, 0, "click=commit & exit");
    }
}

void commit_edit_string() {
    // Trim trailing spaces.
    int end = s.str_edit.max_len;
    while (end > 0 && s.str_edit.buf[end - 1] == ' ') --end;
    s.str_edit.buf[end] = '\0';

    auto g = config::get_global();
    if (s.str_edit.field == StringField::ArtnetShort) {
        std::memset(g.short_name, 0, sizeof(g.short_name));
        std::strncpy(g.short_name, s.str_edit.buf, sizeof(g.short_name) - 1);
    } else {
        std::memset(g.long_name, 0, sizeof(g.long_name));
        std::strncpy(g.long_name, s.str_edit.buf, sizeof(g.long_name) - 1);
    }
    config::set_global(g);
    dmx::mark_global_dirty();
}

void dispatch_edit_string(Event e) {
    const uint8_t len = s.str_edit.max_len;
    if (s.str_edit.cursor < len) {
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            int idx = char_to_idx(s.str_edit.buf[s.str_edit.cursor]);
            idx += (e == Event::RotateRight) ? 1 : -1;
            s.str_edit.buf[s.str_edit.cursor] = idx_to_char(idx);
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
}

void render_edit_ip() {
    oled_clear();
    char title[kCols + 1];
    std::snprintf(title, sizeof(title), "EDIT %s", s.ip_edit.label);
    oled_draw_text(0, 0, title);

    // Render as "192.[168].001.123" with brackets around current octet.
    const uint32_t v = s.ip_edit.value;
    const uint8_t  oct[4] = {
        static_cast<uint8_t>((v >> 24) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >>  8) & 0xFF),
        static_cast<uint8_t> (v        & 0xFF),
    };
    char line[kCols + 1] = {};
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == s.ip_edit.cursor) pos += std::snprintf(line + pos, sizeof(line) - pos, "[%u]", oct[i]);
        else                       pos += std::snprintf(line + pos, sizeof(line) - pos, "%u",   oct[i]);
        if (i < 3) pos += std::snprintf(line + pos, sizeof(line) - pos, ".");
        if (pos >= static_cast<int>(sizeof(line)) - 1) break;
    }
    oled_draw_text(3, 0, line);

    if (s.ip_edit.cursor < 4) {
        oled_draw_text(5, 0, "rotate=change oct");
        oled_draw_text(6, 0, "click=next octet");
    } else {
        oled_draw_text(5, 0, "[ready to commit]");
        oled_draw_text(6, 0, "click=commit & exit");
    }
}

void commit_edit_ip() {
    auto g = config::get_global();
    switch (s.ip_edit.field) {
        case IpField::StaticIp:   g.static_ip      = s.ip_edit.value; break;
        case IpField::StaticMask: g.static_mask    = s.ip_edit.value; break;
        case IpField::StaticGw:   g.static_gateway = s.ip_edit.value; break;
    }
    config::set_global(g);
    dmx::mark_global_dirty();
}

void dispatch_edit_ip(Event e) {
    if (s.ip_edit.cursor < 4) {
        if (e == Event::RotateLeft || e == Event::RotateRight) {
            const int shift = (3 - s.ip_edit.cursor) * 8;
            int32_t oct = (s.ip_edit.value >> shift) & 0xFF;
            oct += (e == Event::RotateRight) ? 1 : -1;
            if (oct < 0)   oct = 0;
            if (oct > 255) oct = 255;
            const uint32_t mask = ~(0xFFu << shift);
            s.ip_edit.value = (s.ip_edit.value & mask) |
                              (static_cast<uint32_t>(oct) << shift);
        } else if (e == Event::Click) {
            s.ip_edit.cursor++;
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
    char vnet[8], vsub[8];
    char vshort[14], vlong[14];
    const auto& g = config::get_global();
    std::snprintf(vnet, sizeof(vnet), "%u", g.artnet_net);
    std::snprintf(vsub, sizeof(vsub), "%u", g.artnet_subnet);
    truncate(vshort, sizeof(vshort), g.short_name);
    truncate(vlong,  sizeof(vlong),  g.long_name);

    ListItem items[5] = {
        {"Net",    vnet},
        {"Sub",    vsub},
        {"Short",  vshort},
        {"Long",   vlong},
        {"[Back]", ""},
    };
    render_list("ARTNET", items, 5, s.cursor);
}

void dispatch_artnet_menu(Event e) {
    constexpr uint8_t kCount = 5;
    if (e == Event::RotateLeft  && s.cursor > 0)           s.cursor--;
    if (e == Event::RotateRight && s.cursor < kCount - 1)  s.cursor++;
    if (e != Event::Click) return;
    const auto& g = config::get_global();
    switch (s.cursor) {
        case 0: enter_edit(Field::ArtnetNet,    ValueKind::Int, g.artnet_net,    0, 127, 1, "Net", Screen::ArtnetMenu); break;
        case 1: enter_edit(Field::ArtnetSubnet, ValueKind::Int, g.artnet_subnet, 0,  15, 1, "Sub", Screen::ArtnetMenu); break;
        case 2: enter_edit_string(StringField::ArtnetShort, g.short_name, sizeof(g.short_name) - 1, "Short", Screen::ArtnetMenu); break;
        case 3: enter_edit_string(StringField::ArtnetLong,  g.long_name,  sizeof(g.long_name)  - 1, "Long",  Screen::ArtnetMenu); break;
        case 4: s.screen = Screen::MainMenu; s.cursor = 0; break;
    }
}

// ── NETWORK MENU ────────────────────────────────────────────────────────────

void render_network_menu() {
    char vdhcp[8], vip[kCols + 1], vmsk[kCols + 1], vgw[kCols + 1];
    const auto& g = config::get_global();
    std::snprintf(vdhcp, sizeof(vdhcp), "%s", g.use_dhcp ? "ON" : "OFF");
    auto fmt_ip = [](char* buf, size_t cap, uint32_t v) {
        std::snprintf(buf, cap, "%u.%u.%u.%u",
                      (v >> 24) & 0xFFu, (v >> 16) & 0xFFu,
                      (v >>  8) & 0xFFu,  v        & 0xFFu);
    };
    fmt_ip(vip,  sizeof(vip),  g.static_ip);
    fmt_ip(vmsk, sizeof(vmsk), g.static_mask);
    fmt_ip(vgw,  sizeof(vgw),  g.static_gateway);

    ListItem items[5] = {
        {"DHCP", vdhcp},
        {"IP",   vip},
        {"Msk",  vmsk},
        {"GW",   vgw},
        {"[Back]", ""},
    };
    render_list("NETWORK", items, 5, s.cursor);
}

void dispatch_network_menu(Event e) {
    constexpr uint8_t kCount = 5;
    if (e == Event::RotateLeft  && s.cursor > 0)           s.cursor--;
    if (e == Event::RotateRight && s.cursor < kCount - 1)  s.cursor++;
    if (e != Event::Click) return;
    const auto& g = config::get_global();
    switch (s.cursor) {
        case 0: enter_edit(Field::NetworkDhcp, ValueKind::Bool, g.use_dhcp ? 1 : 0, 0, 1, 1, "DHCP", Screen::NetworkMenu); break;
        case 1: enter_edit_ip(IpField::StaticIp,   g.static_ip,      "IP",   Screen::NetworkMenu); break;
        case 2: enter_edit_ip(IpField::StaticMask, g.static_mask,    "Mask", Screen::NetworkMenu); break;
        case 3: enter_edit_ip(IpField::StaticGw,   g.static_gateway, "GW",   Screen::NetworkMenu); break;
        case 4: s.screen = Screen::MainMenu; s.cursor = 1; break;
    }
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kColorOrderCount = static_cast<size_t>(led::ColorOrder::COUNT);
constexpr size_t kProtocolCount   = static_cast<size_t>(led::Protocol::COUNT);

uint8_t channel_item_count() {
    const auto& cc = config::get_channel(s.channel_index);
    return led::is_clocked(cc.protocol) ? 10 : 9;
}

void render_channel_menu() {
    const auto& cc = config::get_channel(s.channel_index);
    char title[kCols + 1];
    std::snprintf(title, sizeof(title), "CHANNEL %u", s.channel_index + 1);

    char vproto[8], vuni[8], vdmx[8], vpix[8], vorder[8],
         vbri[8],   vgrp[8], vinv[8], vclk[12];
    std::snprintf(vproto, sizeof(vproto), "%s", protocol_name(cc.protocol));
    std::snprintf(vuni,   sizeof(vuni),   "%u", cc.universe_start);
    std::snprintf(vdmx,   sizeof(vdmx),   "%u", cc.dmx_start);
    std::snprintf(vpix,   sizeof(vpix),   "%u", cc.pixel_count);
    std::snprintf(vorder, sizeof(vorder), "%s", color_order_name(cc.color_order));
    std::snprintf(vbri,   sizeof(vbri),   "%u", cc.brightness);
    std::snprintf(vgrp,   sizeof(vgrp),   "%u", cc.grouping);
    std::snprintf(vinv,   sizeof(vinv),   "%s", cc.invert_direction ? "ON" : "OFF");
    std::snprintf(vclk,   sizeof(vclk),   "%lukHz", static_cast<unsigned long>(cc.clock_hz / 1000u));

    ListItem items[10] = {
        {"Proto",  vproto},
        {"Uni",    vuni},
        {"DMX",    vdmx},
        {"Pixels", vpix},
        {"Order",  vorder},
        {"Bright", vbri},
        {"Group",  vgrp},
        {"Invert", vinv},
        {"Clock",  vclk},
        {"[Back]", ""},
    };

    if (led::is_clocked(cc.protocol)) {
        render_list(title, items, 10, s.cursor);
    } else {
        items[8] = items[9];
        render_list(title, items, 9, s.cursor);
    }
}

void dispatch_channel_menu(Event e) {
    const uint8_t count = channel_item_count();
    if (e == Event::RotateLeft  && s.cursor > 0)         s.cursor--;
    if (e == Event::RotateRight && s.cursor < count - 1) s.cursor++;
    if (e != Event::Click) return;

    const auto& cc = config::get_channel(s.channel_index);
    const Screen ret = Screen::ChannelMenu;
    const uint8_t ch = s.channel_index;
    const bool clocked = led::is_clocked(cc.protocol);
    const uint8_t back_idx = clocked ? 9 : 8;

    if (s.cursor == back_idx) { s.screen = Screen::MainMenu; s.cursor = 2 + ch; return; }

    switch (s.cursor) {
        case 0: enter_edit(Field::ChProtocol,   ValueKind::Protocol,   static_cast<int32_t>(cc.protocol),    0, static_cast<int32_t>(kProtocolCount) - 1,   1, "Proto",  ret, ch); break;
        case 1: enter_edit(Field::ChUniverse,   ValueKind::Int,        cc.universe_start,                    1, 32767,                                       1, "Uni",    ret, ch); break;
        case 2: enter_edit(Field::ChDmx,        ValueKind::Int,        cc.dmx_start,                         1,   512,                                       1, "DMX",    ret, ch); break;
        case 3: enter_edit(Field::ChPixels,     ValueKind::Int,        cc.pixel_count,                       1,  1024,                                       1, "Pixels", ret, ch); break;
        case 4: enter_edit(Field::ChColorOrder, ValueKind::ColorOrder, static_cast<int32_t>(cc.color_order), 0, static_cast<int32_t>(kColorOrderCount) - 1, 1, "Order",  ret, ch); break;
        case 5: enter_edit(Field::ChBrightness, ValueKind::Int,        cc.brightness,                        0,   255,                                       1, "Bright", ret, ch); break;
        case 6: enter_edit(Field::ChGrouping,   ValueKind::Int,        cc.grouping,                          1,     8,                                       1, "Group",  ret, ch); break;
        case 7: enter_edit(Field::ChInvert,     ValueKind::Bool,       cc.invert_direction ? 1 : 0,          0,     1,                                       1, "Invert", ret, ch); break;
        case 8: if (clocked) enter_edit(Field::ChClock, ValueKind::Int, static_cast<int32_t>(cc.clock_hz),    250'000, 24'000'000,                     250'000, "Clock",  ret, ch); break;
    }
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

void menu_init() { s = State{}; }

void menu_render() {
    switch (s.screen) {
        case Screen::Home:        render_home();         break;
        case Screen::MainMenu:    render_main_menu();    break;
        case Screen::ArtnetMenu:  render_artnet_menu();  break;
        case Screen::NetworkMenu: render_network_menu(); break;
        case Screen::ChannelMenu: render_channel_menu(); break;
        case Screen::EditValue:   render_edit_value();   break;
        case Screen::EditString:  render_edit_string();  break;
        case Screen::EditIp:      render_edit_ip();      break;
    }
}

void menu_dispatch(Event e) {
    switch (s.screen) {
        case Screen::Home:
            if (e == Event::Click) { s.screen = Screen::MainMenu; s.cursor = 0; }
            break;
        case Screen::MainMenu:    dispatch_main_menu(e);    break;
        case Screen::ArtnetMenu:  dispatch_artnet_menu(e);  break;
        case Screen::NetworkMenu: dispatch_network_menu(e); break;
        case Screen::ChannelMenu: dispatch_channel_menu(e); break;
        case Screen::EditValue:   dispatch_edit_value(e);   break;
        case Screen::EditString:  dispatch_edit_string(e);  break;
        case Screen::EditIp:      dispatch_edit_ip(e);      break;
    }
}

void menu_on_idle_timeout() {
    s.screen = Screen::Home;
    s.cursor = 0;
}

}  // namespace pixfrog::ui::detail
