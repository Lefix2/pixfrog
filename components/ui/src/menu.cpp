// Menu state machine for pixfrog's OLED UI.
//
// Convention:
//   - Rotate-right = cursor-down in lists, value-up in edit.
//   - Rotate-left  = cursor-up in lists, value-down in edit.
//   - Click on list entry = enter edit or sub-screen.
//   - Click in edit mode = commit (writes NVS, marks dmx dirty), return.
//   - Click on a "[Back]" entry = return one level.
//   - Idle timeout → HOME.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"
#include "ui.h"

namespace pixfrog::ui::detail {

namespace {

constexpr uint8_t kRows  = 8;   // OLED text rows
constexpr uint8_t kCols  = 21;  // chars per row

// ── Screens ─────────────────────────────────────────────────────────────────
enum class Screen : uint8_t {
    Home,
    MainMenu,
    ArtnetMenu,
    NetworkMenu,
    ChannelMenu,
    EditValue,
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

enum class ValueKind : uint8_t {
    Int,            // raw integer
    Bool,           // 0/1 → OFF/ON
    Protocol,       // 0..7 → led::Protocol names
    ColorOrder,     // 0..N → led::ColorOrder names
};

struct EditCtx {
    Field       field        = Field::None;
    ValueKind   kind         = ValueKind::Int;
    int32_t     current      = 0;
    int32_t     step         = 1;
    int32_t     min          = 0;
    int32_t     max          = 0;
    uint8_t     channel      = 0;
    Screen      return_screen = Screen::MainMenu;
    const char* label        = "";
};

struct State {
    Screen  screen        = Screen::Home;
    uint8_t cursor        = 0;
    uint8_t channel_index = 0;
    EditCtx edit;
};

State s;

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
        case ValueKind::Int:
            std::snprintf(out, cap, "%ld", static_cast<long>(v));
            return;
        case ValueKind::Bool:
            std::snprintf(out, cap, "%s", v ? "ON" : "OFF");
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
    const char* value;   // optional; may be ""
};

void render_list(const char* title, const ListItem* items, uint8_t count, uint8_t cursor) {
    oled_clear();
    oled_draw_text(0, 0, title);
    // Up to 7 visible rows (rows 1..7). Scroll so cursor is always visible.
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
        if (s.cursor == 0)                                          { s.screen = Screen::ArtnetMenu;  s.cursor = 0; }
        else if (s.cursor == 1)                                     { s.screen = Screen::NetworkMenu; s.cursor = 0; }
        else if (s.cursor >= 2 && s.cursor <= 9)                    {
            s.channel_index = s.cursor - 2;
            s.screen = Screen::ChannelMenu;
            s.cursor = 0;
        }
        else                                                        { s.screen = Screen::Home; s.cursor = 0; }
    }
}

// ── EDIT VALUE — generic ────────────────────────────────────────────────────

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

// ── ARTNET MENU ─────────────────────────────────────────────────────────────

void render_artnet_menu() {
    char vnet[8], vsub[8];
    char vshort[kCols + 1], vlong[kCols + 1];
    const auto& g = config::get_global();
    std::snprintf(vnet, sizeof(vnet), "%u", g.artnet_net);
    std::snprintf(vsub, sizeof(vsub), "%u", g.artnet_subnet);
    truncate(vshort, 13, g.short_name);
    truncate(vlong,  13, g.long_name);

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
    if (e == Event::Click) {
        const auto& g = config::get_global();
        switch (s.cursor) {
            case 0: enter_edit(Field::ArtnetNet,    ValueKind::Int, g.artnet_net,    0, 127, 1, "Net",    Screen::ArtnetMenu); break;
            case 1: enter_edit(Field::ArtnetSubnet, ValueKind::Int, g.artnet_subnet, 0,  15, 1, "Sub",    Screen::ArtnetMenu); break;
            case 2: /* short_name edit: char-by-char editor TODO */                                                            break;
            case 3: /* long_name  edit: char-by-char editor TODO */                                                            break;
            case 4: s.screen = Screen::MainMenu; s.cursor = 0;                                                                 break;
        }
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
    if (e == Event::Click) {
        const auto& g = config::get_global();
        switch (s.cursor) {
            case 0: enter_edit(Field::NetworkDhcp, ValueKind::Bool, g.use_dhcp ? 1 : 0, 0, 1, 1, "DHCP", Screen::NetworkMenu); break;
            case 1: /* static IP edit: 4-octet editor TODO */   break;
            case 2: /* static mask edit: 4-octet editor TODO */ break;
            case 3: /* static GW edit: 4-octet editor TODO */   break;
            case 4: s.screen = Screen::MainMenu; s.cursor = 1;  break;
        }
    }
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kColorOrderCount = static_cast<size_t>(led::ColorOrder::COUNT);
constexpr size_t kProtocolCount   = static_cast<size_t>(led::Protocol::COUNT);

uint8_t channel_item_count() {
    // Base 9 fields (Proto, Uni, DMX, Pix, Order, Bri, Group, Invert) + Back = 9.
    // Clock only shown for clocked protocols → +1.
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
        // Skip Clock row: shift "[Back]" up.
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

    // When non-clocked, the Clock row is hidden and [Back] is at index 8.
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
    }
}

void menu_on_idle_timeout() {
    s.screen = Screen::Home;
    s.cursor = 0;
}

}  // namespace pixfrog::ui::detail
