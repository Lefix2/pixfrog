// Menu state machine for pixfrog's OLED UI.
//
// Conventions:
//   Rotate-right = cursor-down / value-up.
//   Rotate-left  = cursor-up   / value-down.
//   Click on a list entry = enter edit or sub-screen.
//   Click in edit mode = commit (writes NVS, marks dmx dirty), return.
//   Click on a "[Back]" entry = return one level.
//   Idle timeout → HOME.

#include "menu_internal.h"

namespace pixfrog::ui::detail::menu_impl {

std::atomic<bool> g_speaker{ false };

// ── Node table + engine ──────────────────────────────────────────────────────

struct Node {
    const char* title;
    NodeId parent;  // ignored for Main (its back = Home)
    uint8_t (*build)(ListItem* items, OnClick* fns);
};

// Indexed by NodeId. Main's parent is a placeholder (back goes to Home).
const Node kNodes[static_cast<uint8_t>(NodeId::Count)] = {
    { "MENU", NodeId::Main, build_main },
    { "SHOW", NodeId::Main, build_show },
    { "LOOKS", NodeId::Main, build_looks },
    { "RIG", NodeId::Main, build_rig },
    { "DMX", NodeId::Main, build_dmx },
    { "BOX", NodeId::Main, build_box },
    { "NETWORK", NodeId::Box, build_network },
    { "SETTINGS", NodeId::Box, build_settings },
    { g_channel_title, NodeId::Rig, build_channel },
    { "SCENES", NodeId::Show, build_scenes },
    { "FSEQ", NodeId::Show, build_fseq },
    { "TEST PATTERN", NodeId::Rig, build_testpattern },
    { "DEAD PIXELS", NodeId::Channel, build_gaps },
    { "CONTROL UNI", NodeId::Dmx, build_control },
    { g_ctl_slot_title, NodeId::Control, build_control_slot },
    { "FIXTURES", NodeId::Channel, build_fixtures },
    { g_fix_title, NodeId::Fixtures, build_fixture },
    { "EDIT SCENES", NodeId::Looks, build_scene_list },
    { g_scene_title, NodeId::SceneList, build_scene_edit },
    { g_part_title, NodeId::SceneEdit, build_scene_part },
    { "OUTPUTS", NodeId::ScenePart, build_part_outputs },
    { "PATCH", NodeId::Dmx, build_patch_list },
    { g_patch_title, NodeId::PatchList, build_output_patch },
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
        // Re-entering from Home starts at the top (matches menu_on_idle_timeout).
        s.cur[static_cast<uint8_t>(NodeId::Main)] = 0;
        s.scr[static_cast<uint8_t>(NodeId::Main)] = 0;
        s.cursor                                  = 0;
        s.scroll                                  = 0;
        return;
    }
    const NodeId leaving = s.node;
    go(cur_node().parent);
    // A menu we back out of restarts at the top next time it's opened.
    s.cur[static_cast<uint8_t>(leaving)] = 0;
    s.scr[static_cast<uint8_t>(leaving)] = 0;
}

void open_channel(uint8_t idx) {
    s.channel_index                              = idx;
    s.cur[static_cast<uint8_t>(NodeId::Channel)] = 0;  // a fresh channel starts at the top
    s.scr[static_cast<uint8_t>(NodeId::Channel)] = 0;
    go(NodeId::Channel);
}

void engine_render() {
    static ListItem items[kMaxRows];
    static OnClick fns[kMaxRows];
    const Node& n       = cur_node();
    const uint8_t count = n.build(items, fns);
    if (count > 0 && s.cursor >= count) s.cursor = count - 1;
    render_list(n.title, items, count, s.cursor);
}

void engine_dispatch(Event e) {
    static ListItem items[kMaxRows];
    static OnClick fns[kMaxRows];
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
    case Screen::Stats: s.screen = Screen::Menu; break;
    // Edit screens: cancel — discard the pending value, commit nothing.
    case Screen::EditValue: {
        dmx::clear_pixel_preview();
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
        backlight_preview_end();  // cancel → back to the stored level
#endif
        s.screen = s.edit.return_screen;
        break;
    }
    case Screen::EditString: s.screen = s.str_edit.return_screen; break;
    case Screen::EditIp: s.screen = s.ip_edit.return_screen; break;
    case Screen::EditUni: s.screen = s.uni_edit.return_screen; break;
    case Screen::PixelRefresh: dispatch_pixel_refresh(Event::Click); break;
    }
}

}  // namespace pixfrog::ui::detail::menu_impl

namespace pixfrog::ui::detail {

using namespace menu_impl;

// ── Public API ──────────────────────────────────────────────────────────────

void menu_init() {
    s = State{};
}

void menu_render() {
    switch (s.screen) {
    case Screen::Home: render_home(); break;
    case Screen::Menu: engine_render(); break;
    case Screen::About: render_about(); break;
    case Screen::Stats: render_stats(); break;
    case Screen::EditValue: render_edit_value(); break;
    case Screen::EditString: render_edit_string(); break;
    case Screen::EditIp: render_edit_ip(); break;
    case Screen::EditUni: render_edit_uni(); break;
    case Screen::PixelRefresh: render_pixel_refresh(); break;
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
    case Screen::Stats: dispatch_stats(e); break;
    case Screen::EditValue: dispatch_edit_value(e); break;
    case Screen::EditString: dispatch_edit_string(e); break;
    case Screen::EditIp: dispatch_edit_ip(e); break;
    case Screen::EditUni: dispatch_edit_uni(e); break;
    case Screen::PixelRefresh: dispatch_pixel_refresh(e); break;
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

float menu_gauge_level() {
    if (s.screen != Screen::EditValue || !is_gauge_kind(s.edit.kind) || s.edit.max <= s.edit.min)
        return -1.0f;
    return static_cast<float>(s.edit.current - s.edit.min) /
           static_cast<float>(s.edit.max - s.edit.min);
}

int menu_press_kind(Event e) {
    if (e == Event::LongPress) return s.screen == Screen::Home ? 0 : -1;
    if (e != Event::Click) return 0;
    switch (s.screen) {
    case Screen::About:
    case Screen::Stats: return -1;  // a click there goes back
    case Screen::Menu: {
        static ListItem items[kMaxRows];
        static OnClick fns[kMaxRows];
        const uint8_t count = cur_node().build(items, fns);
        if (count == 0) return 0;
        return items[s.cursor < count ? s.cursor : count - 1].back ? -1 : 1;
    }
    default: return 1;  // HOME → menu, an edit committed or stepped
    }
}

uint32_t menu_fingerprint() {
    uint32_t h = 2166136261u;  // FNV-1a over what a rotation moves
    auto mix   = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i, v >>= 8)
            h = (h ^ (v & 0xFF)) * 16777619u;
    };
    mix(static_cast<uint32_t>(s.screen));
    mix(static_cast<uint32_t>(s.node));
    mix(s.cursor);
    mix(static_cast<uint32_t>(s.edit.current));
    mix(s.str_edit.cursor);
    for (const char* c = s.str_edit.buf; *c; ++c)
        mix(static_cast<uint8_t>(*c));
    mix(s.ip_edit.value);
    mix(s.ip_edit.cursor);
    mix(s.uni_edit.value);
    mix(s.uni_edit.cursor);
    return h;
}

#ifdef PIXFROG_EMULATOR
void menu_debug_state(const char** screen_name, int* cursor, int* channel) {
    // Node names mirror the old per-screen names so the emulator agent API and
    // existing navigation scripts keep matching.
    static const char* const kNodeNames[] = {
        "MainMenu",        "ShowMenu",        "LooksMenu",       "RigMenu",       "DmxMenu",
        "BoxMenu",         "NetworkMenu",     "SettingsMenu",    "ChannelMenu",   "ScenesMenu",
        "FSeqMenu",        "TestPatternMenu", "GapsMenu",        "ControlMenu",   "ControlSlotMenu",
        "FixturesMenu",    "FixtureMenu",     "SceneListMenu",   "SceneEditMenu", "ScenePartMenu",
        "PartOutputsMenu", "PatchListMenu",   "OutputPatchMenu",
    };

    static_assert(sizeof(kNodeNames) / sizeof(kNodeNames[0]) == static_cast<size_t>(NodeId::Count),
                  "one emulator name per menu node");
    if (screen_name) {
        switch (s.screen) {
        case Screen::Home: *screen_name = "Home"; break;
        case Screen::Menu: *screen_name = kNodeNames[static_cast<uint8_t>(s.node)]; break;
        case Screen::About: *screen_name = "About"; break;
        case Screen::Stats: *screen_name = "Stats"; break;
        case Screen::EditValue: *screen_name = "EditValue"; break;
        case Screen::EditString: *screen_name = "EditString"; break;
        case Screen::EditIp: *screen_name = "EditIp"; break;
        case Screen::EditUni: *screen_name = "EditUni"; break;
        case Screen::PixelRefresh: *screen_name = "PixelRefresh"; break;
        }
    }
    if (cursor) *cursor = s.cursor;
    if (channel) *channel = s.channel_index;
}
#endif

}  // namespace pixfrog::ui::detail

namespace pixfrog::ui {
void set_speaker_present(bool present) {
    detail::menu_impl::g_speaker.store(present, std::memory_order_relaxed);
}
}  // namespace pixfrog::ui
