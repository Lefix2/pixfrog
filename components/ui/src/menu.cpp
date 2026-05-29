#include "ui_internal.h"

#include <cstdio>
#include <cstring>

#include "config_store.h"
#include "dmx_manager.h"

namespace pixfrog::ui::detail {

namespace {

enum class Screen : uint8_t {
    Home,
    MainMenu,
    ArtnetMenu,
    NetworkMenu,
    ChannelMenu,
    ChannelEdit,
};

struct State {
    Screen   screen        = Screen::Home;
    uint8_t  cursor        = 0;
    uint8_t  channel_index = 0;        // current channel when in ChannelMenu / ChannelEdit
    bool     dirty         = true;
};

State s;

constexpr const char* kMainItems[] = {
    "ArtNet",
    "Network",
    "Channel 1", "Channel 2", "Channel 3", "Channel 4",
    "Channel 5", "Channel 6", "Channel 7", "Channel 8",
    "Back to HOME",
};

void render_home() {
    char line[24];
    oled_clear();

    const auto stats = dmx::get_stats();
    oled_draw_text(0, 0, "pixfrog");
    std::snprintf(line, sizeof(line), "IP    : %s", "—");      // TODO: wire ip
    oled_draw_text(1, 0, line);
    std::snprintf(line, sizeof(line), "FPS   : %lu", static_cast<unsigned long>(stats.current_fps));
    oled_draw_text(2, 0, line);
    std::snprintf(line, sizeof(line), "Pkts  : %llu", static_cast<unsigned long long>(stats.artnet_packets_rx));
    oled_draw_text(3, 0, line);
    std::snprintf(line, sizeof(line), "CH 12345678");
    oled_draw_text(5, 0, line);
    std::snprintf(line, sizeof(line), "   --------");          // TODO: real per-channel status
    oled_draw_text(6, 0, line);
}

void render_main_menu() {
    oled_clear();
    oled_draw_text(0, 0, "MENU");
    for (size_t i = 0; i < sizeof(kMainItems)/sizeof(kMainItems[0]) && i < 7; ++i) {
        char line[24];
        std::snprintf(line, sizeof(line), "%c %s", i == s.cursor ? '>' : ' ', kMainItems[i]);
        oled_draw_text(static_cast<uint8_t>(1 + i), 0, line);
    }
}

void render_channel_menu() {
    char line[24];
    const auto& cc = config::get_channel(s.channel_index);
    oled_clear();
    std::snprintf(line, sizeof(line), "Channel %u", s.channel_index + 1);
    oled_draw_text(0, 0, line);
    std::snprintf(line, sizeof(line), "Proto : %u", static_cast<unsigned>(cc.protocol));
    oled_draw_text(1, 0, line);
    std::snprintf(line, sizeof(line), "Uni   : %u", cc.universe_start);
    oled_draw_text(2, 0, line);
    std::snprintf(line, sizeof(line), "DMX   : %u", cc.dmx_start);
    oled_draw_text(3, 0, line);
    std::snprintf(line, sizeof(line), "Pix   : %u", cc.pixel_count);
    oled_draw_text(4, 0, line);
    std::snprintf(line, sizeof(line), "Bri   : %u", cc.brightness);
    oled_draw_text(5, 0, line);
    oled_draw_text(7, 0, "(click=back)");
}

}  // namespace

void menu_init()        { s = State{}; }
bool menu_is_dirty()    { return s.dirty; }
void menu_clear_dirty() { s.dirty = false; }

void menu_render() {
    switch (s.screen) {
        case Screen::Home:        render_home();         break;
        case Screen::MainMenu:    render_main_menu();    break;
        case Screen::ChannelMenu: render_channel_menu(); break;
        default:                  render_main_menu();    break;
    }
}

void menu_dispatch(Event e) {
    s.dirty = true;
    switch (s.screen) {
        case Screen::Home:
            if (e == Event::Click) { s.screen = Screen::MainMenu; s.cursor = 0; }
            break;
        case Screen::MainMenu: {
            const uint8_t max_item = (sizeof(kMainItems)/sizeof(kMainItems[0])) - 1;
            if (e == Event::RotateLeft  && s.cursor > 0)        s.cursor--;
            if (e == Event::RotateRight && s.cursor < max_item) s.cursor++;
            if (e == Event::Click) {
                if (s.cursor == 0)                                       s.screen = Screen::ArtnetMenu;
                else if (s.cursor == 1)                                  s.screen = Screen::NetworkMenu;
                else if (s.cursor >= 2 && s.cursor <= 9)                 { s.channel_index = s.cursor - 2; s.screen = Screen::ChannelMenu; }
                else if (s.cursor == max_item)                           s.screen = Screen::Home;
            }
            break;
        }
        case Screen::ChannelMenu:
            if (e == Event::Click) s.screen = Screen::MainMenu;
            break;
        default:
            // TODO: implement remaining screens (ArtnetMenu, NetworkMenu, ChannelEdit)
            if (e == Event::Click) s.screen = Screen::MainMenu;
            break;
    }
}

void menu_on_idle_timeout() {
    s.screen = Screen::Home;
    s.cursor = 0;
    s.dirty  = true;
}

}  // namespace pixfrog::ui::detail
