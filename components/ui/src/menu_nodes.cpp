// The menu tree's nodes: each build_* fills one scrolling list (rows + click
// actions) for the engine in menu.cpp.

#include "menu_internal.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

// ── MAIN MENU ───────────────────────────────────────────────────────────────
// The five jobs of the box: run the show (first: the live controls), then in
// the order a rig is built — describe the rig, build the looks that play on
// it, patch the DMX — and look after the box itself. The web UI follows the
// same order, with the dashboard in place of Show.

uint8_t build_main(ListItem* items, OnClick* fns) {
    uint8_t n = 0;
    items[n]  = { "Show", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Show); };
    items[n]  = { "Rig", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Rig); };
    items[n]  = { "Looks", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Looks); };
    items[n]  = { "DMX", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Dmx); };
    items[n]  = { "Box", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Box); };
    items[n]  = back_item("HOME");
    fns[n++]  = [](uint8_t) { go_back(); };
    return n;
}

// ── SHOW NODE ───────────────────────────────────────────────────────────────
// Running the show: the grand master, blackout and strobe (runtime, all
// outputs), the scene crossfade time, what plays — scenes, FSEQ files — and
// what the strips do when the live input drops.

uint8_t build_show(ListItem* items, OnClick* fns) {
    static char vmaster[8], vbo[8], vstrobe[8], vfade[8], vfsm[8], vfst[8];
    const auto& g = config::get_global();
    std::snprintf(vmaster, sizeof(vmaster), "%u%%",
                  (static_cast<unsigned>(dmx::master_local(0)) * 100u + 32767u) / 65535u);
    std::snprintf(vbo, sizeof(vbo), "%s", dmx::blackout_local() ? "ON" : "OFF");
    const uint8_t hz10 = dmx::strobe_local(0);
    if (hz10)
        std::snprintf(vstrobe, sizeof(vstrobe), "%uHz", hz10 / 10u);
    else
        std::snprintf(vstrobe, sizeof(vstrobe), "Off");
    std::snprintf(vfade, sizeof(vfade), "%u.%us", g.scene_fade_ms / 1000u,
                  (g.scene_fade_ms / 100u) % 10u);
    std::snprintf(vfsm, sizeof(vfsm), "%s", failsafe_name(g.failsafe_mode));
    std::snprintf(vfst, sizeof(vfst), "%us", g.failsafe_timeout_s);
    uint8_t n = 0;
    items[n]  = { "Master", vmaster };
    fns[n++]  = [](uint8_t) {
        enter_edit(Field::ShowMaster, ValueKind::Int,
                    (static_cast<int32_t>(dmx::master_local(0)) * 100 + 32767) / 65535, 0, 100, 5,
                    "Master", Screen::Menu);
    };
    items[n] = { "Blackout", vbo };
    fns[n++] = [](uint8_t) { dmx::blackout_toggle(); };  // instant, like the desk button
    items[n] = { "Strobe", vstrobe };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::ShowStrobe, ValueKind::Int, dmx::strobe_local(0) / 10, 0, 25, 1,
                   "Strobe Hz", Screen::Menu);
    };
    items[n] = { "Fade", vfade };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::ShowFade, ValueKind::Tenths, config::get_global().scene_fade_ms / 100, 0,
                   config::kMaxSceneFadeMs / 100, 1, "Fade", Screen::Menu);
    };
    items[n] = { "Scenes", "" };
    fns[n++] = [](uint8_t) { go(NodeId::Scenes); };
    items[n] = { "FSEQ", "" };
    fns[n++] = [](uint8_t) {
        // Refresh the SD listing as we open the file browser.
        g_fseq_file_count = static_cast<uint8_t>(fseq::list_files(g_fseq_names, kFseqMenuMaxFiles));
        go(NodeId::Fseq);
    };
    items[n] = { "Failsafe", vfsm };
    fns[n++] = [](uint8_t) {
        const auto& gl = config::get_global();
        enter_edit(Field::ArtnetFailsafeMode, ValueKind::Failsafe, gl.failsafe_mode, 0, 3, 1,
                   "Failsafe", Screen::Menu);
    };
    items[n] = { "FSafe s", vfst };
    fns[n++] = [](uint8_t) {
        const auto& gl = config::get_global();
        enter_edit(Field::ArtnetFailsafeTimeout, ValueKind::Int, gl.failsafe_timeout_s, 0, 3600, 1,
                   "FSafe s", Screen::Menu);
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── LOOKS NODE ──────────────────────────────────────────────────────────────
// What the box plays on its own: the scenes (the effect bank is edited from
// the web UI or the console).

uint8_t build_looks(ListItem* items, OnClick* fns) {
    static char vcount[8];
    std::snprintf(vcount, sizeof(vcount), "%u", static_cast<unsigned>(config::num_scenes()));
    items[0] = { "Scenes", vcount };
    fns[0]   = [](uint8_t) { go(NodeId::SceneList); };
    items[1] = back_item();
    fns[1]   = [](uint8_t) { go_back(); };
    return 2;
}

// ── RIG NODE ────────────────────────────────────────────────────────────────
// The installation: the eight outputs (protocol badge; an Off one greyed) —
// each with its LED hardware, dead pixels and fixtures — then the frame rate
// they share and the built-in test patterns.

constexpr uint8_t kChannelRows = config::kNumChannels;  // 8

namespace {
// The eight output rows of a list: "Output n" with its protocol and badge.
uint8_t output_rows(ListItem* items, OnClick* fns, OnClick open) {
    static char names[kChannelRows][12];
    for (uint8_t i = 0; i < kChannelRows; ++i) {
        std::snprintf(names[i], sizeof(names[i]), "Output %u", i + 1);
        const auto& cc     = config::get_channel(i);
        items[i]           = {};
        items[i].label     = names[i];
        items[i].value     = protocol_name(cc.protocol);  // points to static string
        items[i].badge     = static_cast<int8_t>(i);
        items[i].badge_col = badge_color(cc.protocol);
        items[i].value_col = led::is_off(cc.protocol) ? color::DarkGray : color::Gold;
        fns[i]             = open;
    }
    return kChannelRows;
}
}  // namespace

uint8_t build_rig(ListItem* items, OnClick* fns) {
    static char vrefresh[8];
    std::snprintf(vrefresh, sizeof(vrefresh), "%uHz", config::get_global().refresh_rate_hz);
    uint8_t n = output_rows(items, fns, open_channel);
    items[n]  = { "Refresh", vrefresh };
    fns[n++]  = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::GlobalRefresh, ValueKind::Int, g.refresh_rate_hz, config::kMinRefreshHz,
                    config::kMaxRefreshHz, 1, "Refresh", Screen::Menu);
    };
    items[n] = { "Test pattern", "" };
    fns[n++] = [](uint8_t) { go(NodeId::TestPattern); };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── DMX NODE ────────────────────────────────────────────────────────────────
// Addressing and remote control, in the order they are set up: where each
// output sits, the control universe, and — last — the auto-patch that
// re-addresses them all. What the box listens to is under BOX › NETWORK.

uint8_t build_dmx(ListItem* items, OnClick* fns) {
    static char vctl[8];
    std::snprintf(vctl, sizeof(vctl), "%s", config::get_control().enabled ? "ON" : "OFF");
    uint8_t n = 0;
    items[n]  = { "Patch", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::PatchList); };
    items[n]  = { "Control uni", vctl };
    fns[n++]  = [](uint8_t) { go(NodeId::Control); };
    items[n]  = { "Auto-patch", "" };
    fns[n++]  = [](uint8_t) {
        // Cascade all outputs from a base universe; seed with output 1's so
        // re-patching in place is one click.
        enter_edit(Field::AutoPatch, ValueKind::Int, config::get_channel(0).universe_start, 0,
                    32767, 1, "Patch", Screen::Menu);
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── PATCH NODES ─────────────────────────────────────────────────────────────
// The eight outputs again, this time for where they sit in the universes.
// One output has two switches, each with its own address: "Pixel map" (every
// LED has its DMX channels: layout, universe, address) and "Fixtures" (each
// fixture on the channels of its DMX profile: universe, address, then one row
// a fixture to pick its profile). Both, either or none.

namespace {
void open_patch(uint8_t idx) {
    s.channel_index                                  = idx;
    s.cur[static_cast<uint8_t>(NodeId::OutputPatch)] = 0;
    s.scr[static_cast<uint8_t>(NodeId::OutputPatch)] = 0;
    go(NodeId::OutputPatch);
}
}  // namespace

uint8_t build_patch_list(ListItem* items, OnClick* fns) {
    uint8_t n = output_rows(items, fns, open_patch);
    static char values[kChannelRows][16];
    for (uint8_t i = 0; i < kChannelRows; ++i) {  // the address, not the protocol
        const auto& cc = config::get_channel(i);
        if (led::is_off(cc.protocol)) continue;
        if (config::pixel_mapped(cc) && config::fixture_controlled(cc))  // both: the universes
            std::snprintf(values[i], sizeof(values[i]), "%u+F%u", cc.universe_start,
                          config::fix_universe(cc));
        else if (config::pixel_mapped(cc))
            std::snprintf(values[i], sizeof(values[i]), "%u.%u", cc.universe_start, cc.dmx_start);
        else if (config::fixture_controlled(cc))
            std::snprintf(values[i], sizeof(values[i]), "F%u.%u", config::fix_universe(cc),
                          config::fix_dmx_start(cc));
        else
            std::snprintf(values[i], sizeof(values[i]), "-");
        items[i].value = values[i];
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

char g_patch_title[12];

uint8_t build_output_patch(ListItem* items, OnClick* fns) {
    const auto& cc = config::get_channel(s.channel_index);
    std::snprintf(g_patch_title, sizeof(g_patch_title), "PATCH %u", s.channel_index + 1);
    uint8_t n = 0;
    if (led::is_off(cc.protocol)) {  // nothing to patch on an output that is off
        items[n] = back_item();
        fns[n++] = [](uint8_t) { go_back(); };
        return n;
    }
    const bool pixels = config::pixel_mapped(cc), fixtures = config::fixture_controlled(cc);
    static char vuni[12], vdmx[8], vfuni[12], vfdmx[8];
    static char flabel[config::kMaxFixtures][16], fvalue[config::kMaxFixtures][10];
    items[n] = { "Pixel map", pixels ? "ON" : "OFF" };
    fns[n++] = [](uint8_t) {
        const auto& c = config::get_channel(s.channel_index);
        enter_edit(Field::ChPixelMap, ValueKind::Bool, config::pixel_mapped(c) ? 1 : 0, 0, 1, 1,
                   "Pixel map", Screen::Menu, s.channel_index);
    };
    if (pixels) {
        format_uni(vuni, sizeof(vuni), cc.universe_start);
        std::snprintf(vdmx, sizeof(vdmx), "%u", cc.dmx_start);
        items[n] = { "Layout", packing_label(config::pixel_layout(cc)) };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_edit(Field::ChPacking, ValueKind::Packing, config::pixel_layout(c), 0,
                       config::kPackControl - 1, 1, "DMX layout", Screen::Menu, s.channel_index);
        };
        items[n] = { "Uni", vuni };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_edit_uni(s.channel_index, c.universe_start, Screen::Menu);
        };
        items[n] = { "DMX", vdmx };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_edit(Field::ChDmx, ValueKind::Int, c.dmx_start, 1, 512, 1, "DMX", Screen::Menu,
                       s.channel_index);
        };
    }
    items[n] = { "Fixtures", fixtures ? "ON" : "OFF" };
    fns[n++] = [](uint8_t) {
        const auto& c = config::get_channel(s.channel_index);
        enter_edit(Field::ChFixtureCtl, ValueKind::Bool, config::fixture_controlled(c) ? 1 : 0, 0,
                   1, 1, "Fixtures", Screen::Menu, s.channel_index);
    };
    if (fixtures) {
        format_uni(vfuni, sizeof(vfuni), config::fix_universe(cc));
        std::snprintf(vfdmx, sizeof(vfdmx), "%u", config::fix_dmx_start(cc));
        items[n] = { "Fix uni", vfuni };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_edit_uni(s.channel_index, config::fix_universe(c), Screen::Menu, true);
        };
        items[n] = { "Fix DMX", vfdmx };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_edit(Field::ChFixDmx, ValueKind::Int, config::fix_dmx_start(c), 1, 512, 1,
                       "Fix DMX", Screen::Menu, s.channel_index);
        };
        // One row a fixture, its profile to pick. They take their channels one
        // after the other from the address above.
        static uint8_t first_row;
        first_row       = n;
        const size_t nf = config::fixture_count(cc.fixtures, config::kMaxFixtures);
        EditCtx as{};
        as.kind = ValueKind::Profile;
        for (size_t k = 0; k < nf; ++k) {
            std::snprintf(flabel[k], sizeof(flabel[k]), "F%u %u-%u", static_cast<unsigned>(k + 1),
                          cc.fixtures[k].pos + 1u,
                          cc.fixtures[k].pos + config::fixture_len(cc.fixtures[k]));
            format_value(as, config::fixture_profile(cc.fixtures[k]), fvalue[k], sizeof(fvalue[k]));
            items[n] = { flabel[k], fvalue[k] };
            fns[n++] = [](uint8_t row) {
                g_fix_index      = static_cast<uint8_t>(row - first_row);
                const auto& c    = config::get_channel(s.channel_index);
                const int32_t hi = config::get_profiles().count ? config::get_profiles().count - 1
                                                                : 0;
                int32_t cur      = config::fixture_profile(c.fixtures[g_fix_index]);
                if (cur > hi) cur = 0;  // its profile left the bank: the first, as the box plays it
                enter_edit(Field::FixProfile, ValueKind::Profile, cur, 0, hi, 1, "DMX profile",
                           Screen::Menu, s.channel_index);
            };
        }
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── BOX NODE ────────────────────────────────────────────────────────────────

uint8_t build_box(ListItem* items, OnClick* fns) {
    items[0] = { "Network", "" };
    fns[0]   = [](uint8_t) { go(NodeId::Network); };
    items[1] = { "Settings", "" };
    fns[1]   = [](uint8_t) { go(NodeId::Settings); };
    items[2] = { "About", "" };
    fns[2]   = [](uint8_t) { s.screen = Screen::About; };
    items[3] = back_item();
    fns[3]   = [](uint8_t) { go_back(); };
    return 4;
}

// ── TEST PATTERN NODE ───────────────────────────────────────────────────────

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
// Play/stop only: one click starts a scene (they are edited under "Edit
// scenes"). Every playing scene is starred: several play at once when their
// output masks differ (a scene claims only its own outputs).

uint8_t build_scenes(ListItem* items, OnClick* fns) {
    static char marked[config::kMaxScenes][kOledCols + 1];
    const auto count = static_cast<uint8_t>(config::num_scenes());
    for (uint8_t i = 0; i < count; ++i) {
        const auto& sc = config::get_scene(i);
        if (dmx::scene_outputs(i)) {
            std::snprintf(marked[i], sizeof(marked[i]), "%s *", sc.name);
            items[i] = { marked[i], "" };
        } else {
            items[i] = { sc.name, "" };
        }
        fns[i] = [](uint8_t idx) { dmx::scene_start(idx); };  // stay to switch scenes
    }
    uint8_t n = count;
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
        // Bracketed like "[Stop]" so it reads as inert status text — no chevron
        // implying a sub-menu that a null fn would then silently swallow.
        items[0] = { no_card ? "[No SD card]" : "[No .fseq files]", "" };
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

// ── DMX CONTROL NODE ─────────────────────────────────────────────────────────
// The control universe: on/off, where it lives, a preset to start from, then
// one row per slot ("1-2  Master") — click to edit it — and [Add].

uint8_t build_control(ListItem* items, OnClick* fns) {
    const auto& c = config::get_control();
    static char ven[8], vuni[8], vaddr[8], vfoot[8];
    static char slabel[config::kMaxControlSlots][12], svalue[config::kMaxControlSlots][12];
    std::snprintf(ven, sizeof(ven), "%s",
                  c.enabled ? (dmx::control_live() ? "LIVE" : "ON") : "OFF");
    std::snprintf(vuni, sizeof(vuni), "%u", c.universe);
    std::snprintf(vaddr, sizeof(vaddr), "%u", c.address);
    std::snprintf(vfoot, sizeof(vfoot), "%uch",
                  static_cast<unsigned>(config::control_footprint(c)));
    uint8_t n = 0;
    items[n]  = { "Enabled", ven };
    fns[n++]  = [](uint8_t) {
        enter_edit(Field::CtlEnabled, ValueKind::Bool, config::get_control().enabled, 0, 1, 1,
                    "DMX ctrl", Screen::Menu);
    };
    items[n] = { "Universe", vuni };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::CtlUniverse, ValueKind::Int, config::get_control().universe, 0, 32767, 1,
                   "Ctrl uni", Screen::Menu);
    };
    items[n] = { "Address", vaddr };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::CtlAddress, ValueKind::Int, config::get_control().address, 1, 512, 1,
                   "Ctrl addr", Screen::Menu);
    };
    items[n] = { "Preset", vfoot };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::CtlPreset, ValueKind::Preset, 0, 0, 1, 1, "Preset", Screen::Menu);
    };
    constexpr uint8_t kFirstSlotRow = 4;
    unsigned at                     = c.address;
    for (uint8_t i = 0; i < c.count; ++i) {
        const auto& sl   = c.slots[i];
        const unsigned w = config::control_slot_width(sl);
        if (w == 2)
            std::snprintf(slabel[i], sizeof(slabel[i]), "%u-%u", at, at + 1);
        else
            std::snprintf(slabel[i], sizeof(slabel[i]), "%u", at);
        std::snprintf(svalue[i], sizeof(svalue[i]), "%s", ctl_fn_label(sl.fn));
        at       += w;
        items[n]  = { slabel[i], svalue[i] };
        fns[n++]  = [](uint8_t row) {
            g_ctl_slot = static_cast<uint8_t>(row - kFirstSlotRow);
            s.cur[static_cast<uint8_t>(NodeId::ControlSlot)] = 0;
            s.scr[static_cast<uint8_t>(NodeId::ControlSlot)] = 0;
            go(NodeId::ControlSlot);
        };
    }
    if (c.count < config::kMaxControlSlots) {
        items[n] = { "[Add]", "" };
        fns[n++] = [](uint8_t) {
            auto cc = config::get_control();
            if (cc.count >= config::kMaxControlSlots) return;
            cc.slots[cc.count++]        = config::control_slot(config::CtlFn::Master);
            config::ControlConfig check = cc;
            config::sanitize_control(check);
            if (check.count < cc.count) return;  // would end past channel 512
            save_control(cc);
            g_ctl_slot = static_cast<uint8_t>(cc.count - 1);
            go(NodeId::ControlSlot);
        };
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── SLOT NODE ────────────────────────────────────────────────────────────────
// One control slot: its function, the outputs it acts on, and the fields that
// only some functions have (colour number, 16-bit master).

uint8_t build_control_slot(ListItem* items, OnClick* fns) {
    const auto& c = config::get_control();
    if (g_ctl_slot >= c.count) g_ctl_slot = c.count ? static_cast<uint8_t>(c.count - 1) : 0;
    std::snprintf(g_ctl_slot_title, sizeof(g_ctl_slot_title), "SLOT %u", g_ctl_slot + 1u);
    uint8_t n = 0;
    if (c.count == 0) {
        items[n] = back_item();
        fns[n++] = [](uint8_t) { go_back(); };
        return n;
    }
    const auto& sl = c.slots[g_ctl_slot];
    static char vfn[10], vmask[10], vidx[4], vfine[4];
    std::snprintf(vfn, sizeof(vfn), "%s", ctl_fn_label(sl.fn));
    format_mask(sl.mask, vmask, sizeof(vmask));
    std::snprintf(vidx, sizeof(vidx), "%u", sl.index + 1u);
    std::snprintf(vfine, sizeof(vfine), "%s", (sl.flags & config::kCtlFlagFine) ? "ON" : "OFF");
    items[n] = { "Function", vfn };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::CtlSlotFn, ValueKind::CtlFn, config::get_control().slots[g_ctl_slot].fn,
                   0, static_cast<int32_t>(config::CtlFn::Count) - 1, 1, "Function", Screen::Menu);
    };
    items[n] = { "Outputs", vmask };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::CtlSlotMask, ValueKind::Mask,
                   config::get_control().slots[g_ctl_slot].mask, 1, 255, 1, "Outputs",
                   Screen::Menu);
    };
    const auto fn = static_cast<config::CtlFn>(sl.fn);
    if (fn == config::CtlFn::Red || fn == config::CtlFn::Green || fn == config::CtlFn::Blue) {
        items[n] = { "Colour #", vidx };
        fns[n++] = [](uint8_t) {
            enter_edit(Field::CtlSlotIndex, ValueKind::Int,
                       config::get_control().slots[g_ctl_slot].index + 1, 1,
                       static_cast<int32_t>(config::kSceneColorsMax), 1, "Colour #", Screen::Menu);
        };
    }
    if (fn == config::CtlFn::Master) {
        items[n] = { "16-bit", vfine };
        fns[n++] = [](uint8_t) {
            const auto& sl2 = config::get_control().slots[g_ctl_slot];
            enter_edit(Field::CtlSlotFine, ValueKind::Bool,
                       (sl2.flags & config::kCtlFlagFine) ? 1 : 0, 0, 1, 1, "16-bit", Screen::Menu);
        };
    }
    items[n] = { "[Delete]", "" };
    fns[n++] = [](uint8_t) {
        auto cc = config::get_control();
        if (g_ctl_slot >= cc.count) return;
        for (size_t i = g_ctl_slot; i + 1 < cc.count; ++i)
            cc.slots[i] = cc.slots[i + 1];
        --cc.count;
        save_control(cc);
        go_back();
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── SETTINGS NODE ────────────────────────────────────────────────────────────
// The box itself rather than the show: the backlight (TFT builds), the
// speaker volume (boards with the speaker mod), the nerd stats. The dim delay
// is the backlight's own — unrelated to `home_timeout_s`, which only decides
// when the menu walks back to HOME.

uint8_t build_settings(ListItem* items, OnClick* fns) {
    uint8_t n     = 0;
    const auto& g = config::get_global();
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    static char vbright[8], vdim[8], vdelay[8];
    std::snprintf(vbright, sizeof(vbright), "%u%%", config::tft_brightness_pct(g));
    const uint8_t dim = config::tft_idle_dim_pct(g);
    if (dim == 0)
        std::snprintf(vdim, sizeof(vdim), "Off");
    else
        std::snprintf(vdim, sizeof(vdim), "-%u%%", dim);
    const uint16_t delay = config::tft_dim_delay_s(g);
    if (delay == 0)
        std::snprintf(vdelay, sizeof(vdelay), "Off");
    else
        std::snprintf(vdelay, sizeof(vdelay), "%us", delay);

    items[n] = { "Bright", vbright };
    fns[n++] = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::DisplayBrightness, ValueKind::Int, config::tft_brightness_pct(g),
                   config::kTftBrightnessMin, 100, 5, "Bright", Screen::Menu);
    };
    items[n] = { "Idle dim", vdim };
    fns[n++] = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::DisplayIdleDim, ValueKind::Int, config::tft_idle_dim_pct(g), 0, 100, 5,
                   "Idle dim", Screen::Menu);
    };
    items[n] = { "Dim after", vdelay };
    fns[n++] = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::DisplayDimDelay, ValueKind::Int, config::tft_dim_delay_s(g), 0,
                   config::kTftDimDelayMaxS, 5, "Dim after", Screen::Menu);
    };
    static char vrefresh[8];
    std::snprintf(vrefresh, sizeof(vrefresh), "%ds", static_cast<int>(g_pixel_refresh_s));
    items[n] = { "Refresh px", vrefresh };
    fns[n++] = [](uint8_t) {
        enter_edit(Field::DisplayPixelRefresh, ValueKind::Int, g_pixel_refresh_s, kPixelRefreshMinS,
                   kPixelRefreshMaxS, 5, "Refresh px", Screen::PixelRefresh);
    };
#endif
    if (g_speaker.load(std::memory_order_relaxed)) {
        static char vvol[8];
        const uint8_t v = config::speaker_volume_pct(g);
        if (v == 0)
            std::snprintf(vvol, sizeof(vvol), "Off");
        else
            std::snprintf(vvol, sizeof(vvol), "%u%%", v);
        items[n] = { "Volume", vvol };
        fns[n++] = [](uint8_t) {
            enter_edit(Field::SpeakerVolume, ValueKind::Int,
                       config::speaker_volume_pct(config::get_global()), 0, 100, 5, "Volume",
                       Screen::Menu);
        };
    }
    items[n] = { "Nerd stats", "" };
    fns[n++] = [](uint8_t) { s.screen = Screen::Stats; };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── NETWORK NODE ─────────────────────────────────────────────────────────────

uint8_t build_network(ListItem* items, OnClick* fns) {
    static char vdhcp[8], vip[kOledCols + 1], vmsk[kOledCols + 1], vgw[kOledCols + 1], vweb[8];
    static char vshort[14], vlong[14], vfb[12], vunicast[8], vsacn[8], vfpp[8];
    const auto& g = config::get_global();
    std::snprintf(vunicast, sizeof(vunicast), "%s", g.artnet_poll_reply_unicast ? "ON" : "OFF");
    std::snprintf(vsacn, sizeof(vsacn), "%s", g.sacn_enabled ? "ON" : "OFF");
    std::snprintf(vfpp, sizeof(vfpp), "%s", g.fpp_remote ? "ON" : "OFF");
    std::snprintf(vfb, sizeof(vfb), "%s",
                  g.ip_fallback == config::kIpFallbackArtnet ? "ARTNET" : "LINK");
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
    // Only meaningful in DHCP mode.
    items[7] = { "NoDHCP", vfb };
    fns[7]   = [](uint8_t) {
        enter_edit(Field::NetworkIpFallback, ValueKind::IpFallback,
                     config::get_global().ip_fallback, 0, 1, 1, "No DHCP", Screen::Menu);
    };
    // What the box listens to (Art-Net is always on).
    items[8] = { "sACN", vsacn };
    fns[8]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetSacn, ValueKind::Bool, g.sacn_enabled ? 1 : 0, 0, 1, 1, "sACN",
                     Screen::Menu);
    };
    items[9] = { "FPP", vfpp };
    fns[9]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetFpp, ValueKind::Bool, g.fpp_remote ? 1 : 0, 0, 1, 1, "FPP",
                     Screen::Menu);
    };
    items[10] = { "Unicast", vunicast };
    fns[10]   = [](uint8_t) {
        const auto& g = config::get_global();
        enter_edit(Field::ArtnetReplyUnicast, ValueKind::Bool, g.artnet_poll_reply_unicast ? 1 : 0,
                     0, 1, 1, "Unicast", Screen::Menu);
    };
    items[11] = back_item();
    fns[11]   = [](uint8_t) { go_back(); };
    return 12;
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kProtocolCount = static_cast<size_t>(led::Protocol::COUNT);

// The output menu is layout-driven: the visible items depend on the protocol
// family — an Off output shows only Proto; clocked SPI strips add Clock. Its
// DMX side (universe, address, layout) is the output's patch, under DMX.
enum class ChItem : uint8_t {
    Proto,
    Pixels,
    Gaps,      // dead pixels submenu
    Fixtures,  // fixtures submenu
    Order,
    Bright,
    Gamma,
    Group,
    Invert,
    Clock,
    Identify,
    Back,
};

// Fills `out` (capacity ≥ 15) with the ordered items for `cc` and returns count.
uint8_t channel_items(const config::ChannelConfig& cc, ChItem* out) {
    uint8_t n = 0;
    out[n++]  = ChItem::Proto;
    // A disabled channel exposes only its protocol (so it can be re-enabled)
    // plus Back — no universe/pixel/LED settings.
    if (led::is_off(cc.protocol)) {
        out[n++] = ChItem::Back;
        return n;
    }
    out[n++] = ChItem::Pixels;
    out[n++] = ChItem::Gaps;
    out[n++] = ChItem::Fixtures;
    out[n++] = ChItem::Order;
    out[n++] = ChItem::Bright;
    out[n++] = ChItem::Gamma;
    out[n++] = ChItem::Group;
    out[n++] = ChItem::Invert;
    if (led::is_clocked(cc.protocol)) out[n++] = ChItem::Clock;
    out[n++] = ChItem::Identify;
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
    case ChItem::Pixels:
        return [](uint8_t) {
            // Upper bound is whatever the bus can clock out within the refresh
            // budget (and DMA buffer) for this protocol — e.g. 512 px WS2815
            // @60 Hz, 1024 @30 Hz.
            const auto& cc     = config::get_channel(s.channel_index);
            const int32_t pmax = dmx::channel_max_pixels(s.channel_index);
            int32_t pcur       = cc.pixel_count;
            if (pcur > pmax) pcur = pmax;
            enter_edit(Field::ChPixels, ValueKind::Int, pcur, 1, pmax, 1, "Pixels", Screen::Menu,
                       s.channel_index);
            // Live strip ruler while editing.
            dmx::set_pixel_preview(s.channel_index, static_cast<uint16_t>(pcur));
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
    case ChItem::Gaps: return [](uint8_t) { go(NodeId::Gaps); };
    case ChItem::Fixtures: return [](uint8_t) { go(NodeId::Fixtures); };
    case ChItem::Identify:
        return [](uint8_t) { dmx::identify_start(s.channel_index); };  // 3 blinks; stay
    case ChItem::Back: return [](uint8_t) { go_back(); };
    }
    return nullptr;
}

uint8_t build_channel(ListItem* items, OnClick* fns) {
    const auto& cc = config::get_channel(s.channel_index);
    std::snprintf(g_channel_title, sizeof(g_channel_title), "OUTPUT %u", s.channel_index + 1);

    static char vproto[8], vpix[8], vorder[8], vbri[8], vgrp[8], vinv[8], vclk[12], vgam[8],
        vgaps[8], vfix[8];
    std::snprintf(vproto, sizeof(vproto), "%s", protocol_name(cc.protocol));
    std::snprintf(vpix, sizeof(vpix), "%u", cc.pixel_count);
    std::snprintf(vorder, sizeof(vorder), "%s", color_order_name(cc.color_order));
    std::snprintf(vbri, sizeof(vbri), "%u", cc.brightness);
    std::snprintf(vgrp, sizeof(vgrp), "%u", cc.grouping);
    std::snprintf(vinv, sizeof(vinv), "%s", cc.invert_direction ? "ON" : "OFF");
    format_clock_mhz(static_cast<int32_t>(cc.clock_hz), vclk, sizeof(vclk));
    std::snprintf(vgam, sizeof(vgam), "%u.%u", cc.gamma_x10 / 10, cc.gamma_x10 % 10);
    const size_t ngaps = led::gap_count(cc.gaps, led::kMaxPixelGaps);
    if (ngaps)
        std::snprintf(vgaps, sizeof(vgaps), "%u", static_cast<unsigned>(ngaps));
    else
        std::snprintf(vgaps, sizeof(vgaps), "-");

    const size_t nfix = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    if (nfix)
        std::snprintf(vfix, sizeof(vfix), "%u", static_cast<unsigned>(nfix));
    else
        std::snprintf(vfix, sizeof(vfix), "-");

    ChItem order[15];
    const uint8_t count = channel_items(cc, order);
    for (uint8_t i = 0; i < count; ++i) {
        switch (order[i]) {
        case ChItem::Proto: items[i] = { "Proto", vproto }; break;
        case ChItem::Pixels: items[i] = { "Pixels", vpix }; break;
        case ChItem::Gaps: items[i] = { "Dead px", vgaps }; break;
        case ChItem::Fixtures: items[i] = { "Fixtures", vfix }; break;
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

// ── DEAD PIXELS NODE ─────────────────────────────────────────────────────────
// One row per gap of the open channel ("LED 12  x2"), "[Add]" while a slot is
// free, Back. Clicking a gap edits its first LED, then its length (0 removes
// it); both edits drive the strip ruler with the pending gap so the dead LED
// is seen moving before anything is stored.

// The open channel's gaps with gap g_gap_index set to (pos0, len) — what the
// ruler shows while that gap is being edited.
void preview_gap_edit(uint16_t pos0, uint16_t len) {
    const auto& cc = config::get_channel(s.channel_index);
    led::PixelGap tmp[led::kMaxPixelGaps];
    std::memcpy(tmp, cc.gaps, sizeof(tmp));
    if (g_gap_index < led::kMaxPixelGaps) tmp[g_gap_index] = { pos0, len };
    dmx::set_preview_gaps(tmp, led::kMaxPixelGaps);
    dmx::set_pixel_preview(s.channel_index, dmx::effective_channel(s.channel_index).pixel_count);
}

void enter_gap_pos_edit(uint8_t k) {
    const auto& cc      = config::get_channel(s.channel_index);
    g_gap_index         = k;
    const bool existing = k < led::gap_count(cc.gaps, led::kMaxPixelGaps);
    const uint16_t pos0 = existing ? cc.gaps[k].pos : 0;
    const uint16_t len  = existing ? cc.gaps[k].len : 1;
    enter_edit(Field::ChGapPos, ValueKind::Int, pos0 + 1, 1,
               static_cast<int32_t>(led::kMaxPixelsPerChannel), 1, "Dead LED", Screen::Menu,
               s.channel_index);
    preview_gap_edit(pos0, len);
}

void enter_gap_len_edit() {
    const auto& cc     = config::get_channel(s.channel_index);
    const uint16_t len = g_gap_index < led::gap_count(cc.gaps, led::kMaxPixelGaps)
                           ? cc.gaps[g_gap_index].len
                           : 1;
    enter_edit(Field::ChGapLen, ValueKind::Int, len, 0, 64, 1, "Dead count", Screen::Menu,
               s.channel_index);
    if (g_gap_index < led::kMaxPixelGaps) preview_gap_edit(cc.gaps[g_gap_index].pos, len);
}

uint8_t build_gaps(ListItem* items, OnClick* fns) {
    static char labels[led::kMaxPixelGaps][12], values[led::kMaxPixelGaps][8];
    const auto& cc  = config::get_channel(s.channel_index);
    const size_t ng = led::gap_count(cc.gaps, led::kMaxPixelGaps);
    uint8_t n       = 0;
    for (size_t k = 0; k < ng; ++k) {
        std::snprintf(labels[k], sizeof(labels[k]), "LED %u", cc.gaps[k].pos + 1u);
        std::snprintf(values[k], sizeof(values[k]), "x%u", static_cast<unsigned>(cc.gaps[k].len));
        items[n] = { labels[k], values[k] };
        fns[n++] = [](uint8_t idx) { enter_gap_pos_edit(idx); };
    }
    if (ng < led::kMaxPixelGaps) {
        items[n] = { "[Add]", "" };
        fns[n++] = [](uint8_t) {
            const auto& c = config::get_channel(s.channel_index);
            enter_gap_pos_edit(static_cast<uint8_t>(led::gap_count(c.gaps, led::kMaxPixelGaps)));
        };
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── FIXTURES NODE ────────────────────────────────────────────────────────────
// The open channel's fixtures ("F2 60-118  x59", R = mounted the other way
// round), click to edit one. [Add] puts one after the last, as long as it —
// the first one takes the strip as it is, and the strip grows when the new
// fixture passes its end. "Split in" replaces the list with N equal fixtures.

namespace {

void open_fixture(uint8_t k) {
    g_fix_index                                  = k;
    s.cur[static_cast<uint8_t>(NodeId::Fixture)] = 0;
    s.scr[static_cast<uint8_t>(NodeId::Fixture)] = 0;
    go(NodeId::Fixture);
}

void add_fixture() {
    auto c            = config::get_channel(s.channel_index);
    const size_t nf   = config::fixture_count(c.fixtures, config::kMaxFixtures);  // < max: the row
    const int32_t end = strip_end(c);
    int32_t pos = 0, len = end;
    if (nf) {
        const config::Fixture& last = c.fixtures[nf - 1];
        len                         = config::fixture_len(last);
        pos                         = last.pos + len;
        if (pos + len > end) {  // past the strip: it grows, as far as the output can be driven
            const int32_t room = static_cast<int32_t>(dmx::channel_max_pixels(s.channel_index)) -
                                 c.pixel_count;
            int32_t grow = pos + len - end;
            if (grow > room) grow = room > 0 ? room : 0;
            len           = end + grow - pos;
            c.pixel_count = static_cast<uint16_t>(c.pixel_count + grow);
        }
    }
    if (len <= 0) return;  // no LED left for it
    c.fixtures[nf] = config::make_fixture(static_cast<uint16_t>(pos), static_cast<uint16_t>(len));
    config::set_channel(s.channel_index, c);
    dmx::mark_channel_dirty(s.channel_index);
    open_fixture(static_cast<uint8_t>(nf));
}

// The room fixture `k` of `cc` may move in: from the end of the one before it
// to the start of the next (the strip's end for the last), physical, 0-based.
void fixture_room(const config::ChannelConfig& cc, size_t k, uint16_t& lo, uint16_t& hi) {
    const size_t nf   = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    const uint16_t at = static_cast<uint16_t>(cc.fixtures[k].pos +
                                              config::fixture_len(cc.fixtures[k]));
    lo = k ? static_cast<uint16_t>(cc.fixtures[k - 1].pos + config::fixture_len(cc.fixtures[k - 1]))
           : 0;
    hi = k + 1 < nf ? cc.fixtures[k + 1].pos : strip_end(cc);
    if (hi < at) hi = at;  // one that already passes the strip's end keeps its length
}

}  // namespace

uint8_t build_fixtures(ListItem* items, OnClick* fns) {
    static char labels[config::kMaxFixtures][16], values[config::kMaxFixtures][8], vsplit[4];
    const auto& cc  = config::get_channel(s.channel_index);
    const size_t nf = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    uint8_t n       = 0;
    for (size_t k = 0; k < nf; ++k) {
        const unsigned len = config::fixture_len(cc.fixtures[k]);
        std::snprintf(labels[k], sizeof(labels[k]), "F%u %u-%u", static_cast<unsigned>(k + 1),
                      cc.fixtures[k].pos + 1u, cc.fixtures[k].pos + len);
        std::snprintf(values[k], sizeof(values[k]), "x%u%s", len,
                      config::fixture_reversed(cc.fixtures[k]) ? "R" : "");
        items[n] = { labels[k], values[k] };
        fns[n++] = [](uint8_t idx) { open_fixture(idx); };
    }
    if (nf < config::kMaxFixtures) {
        items[n] = { "[Add]", "" };
        fns[n++] = [](uint8_t) { add_fixture(); };
    }
    std::snprintf(vsplit, sizeof(vsplit), "%u", static_cast<unsigned>(nf));
    items[n] = { "Split in", vsplit };
    fns[n++] = [](uint8_t) {
        const auto& c     = config::get_channel(s.channel_index);
        const int32_t cur = static_cast<int32_t>(
            config::fixture_count(c.fixtures, config::kMaxFixtures));
        const int32_t end = strip_end(c);
        const int32_t max = end < static_cast<int32_t>(config::kMaxFixtures)
                              ? end
                              : static_cast<int32_t>(config::kMaxFixtures);  // a strip has an LED
        enter_edit(Field::FixSplit, ValueKind::Int, cur ? cur : 1, 1, max, 1, "Fixtures",
                   Screen::Menu, s.channel_index);
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── FIXTURE NODE ─────────────────────────────────────────────────────────────
// One fixture: where it starts, how long it is and which way round it is
// mounted (its DMX profile, in DMX control mode, is picked in the output's
// patch). The ranges stop at its neighbours, so two fixtures never overlap.

uint8_t build_fixture(ListItem* items, OnClick* fns) {
    const auto& cc  = config::get_channel(s.channel_index);
    const size_t nf = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    uint8_t n       = 0;
    std::snprintf(g_fix_title, sizeof(g_fix_title), "FIXTURE %u", g_fix_index + 1u);
    if (g_fix_index >= nf) {  // it was removed (here or from the web)
        items[n] = back_item();
        fns[n++] = [](uint8_t) { go_back(); };
        return n;
    }
    const config::Fixture& f = cc.fixtures[g_fix_index];
    static char vfirst[8], vlen[8];
    std::snprintf(vfirst, sizeof(vfirst), "%u", f.pos + 1u);
    std::snprintf(vlen, sizeof(vlen), "%u", static_cast<unsigned>(config::fixture_len(f)));
    items[n] = { "First LED", vfirst };
    fns[n++] = [](uint8_t) {
        const auto& c = config::get_channel(s.channel_index);
        uint16_t lo, hi;
        fixture_room(c, g_fix_index, lo, hi);
        const config::Fixture& fx = c.fixtures[g_fix_index];
        enter_edit(Field::FixFirst, ValueKind::Int, fx.pos + 1, lo + 1,
                   hi - config::fixture_len(fx) + 1, 1, "First LED", Screen::Menu, s.channel_index);
    };
    items[n] = { "LEDs", vlen };
    fns[n++] = [](uint8_t) {
        const auto& c = config::get_channel(s.channel_index);
        uint16_t lo, hi;
        fixture_room(c, g_fix_index, lo, hi);
        const config::Fixture& fx = c.fixtures[g_fix_index];
        enter_edit(Field::FixLen, ValueKind::Int, config::fixture_len(fx), 1, hi - fx.pos, 1,
                   "LEDs", Screen::Menu, s.channel_index);
    };
    items[n] = { "Reversed", config::fixture_reversed(f) ? "ON" : "OFF" };
    fns[n++] = [](uint8_t) {
        const auto& c = config::get_channel(s.channel_index);
        enter_edit(Field::FixReversed, ValueKind::Bool,
                   config::fixture_reversed(c.fixtures[g_fix_index]) ? 1 : 0, 0, 1, 1, "Reversed",
                   Screen::Menu, s.channel_index);
    };
    items[n] = { "[Delete]", "" };
    fns[n++] = [](uint8_t) {
        auto c = config::get_channel(s.channel_index);
        if (g_fix_index < config::kMaxFixtures) c.fixtures[g_fix_index] = config::Fixture{};
        config::set_channel(s.channel_index, c);  // normalize closes the list up
        dmx::mark_channel_dirty(s.channel_index);
        go_back();
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── EDIT SCENES NODE ─────────────────────────────────────────────────────────
// The scenes as things to edit (the SCENES node plays them): one row a scene,
// with the group it plays on or its number of parts, and [New].

namespace {

void open_scene(uint8_t i) {
    g_scene_index                                  = i;
    s.cur[static_cast<uint8_t>(NodeId::SceneEdit)] = 0;
    s.scr[static_cast<uint8_t>(NodeId::SceneEdit)] = 0;
    go(NodeId::SceneEdit);
}

void open_part(uint8_t k) {
    g_part_index                                   = k;
    s.cur[static_cast<uint8_t>(NodeId::ScenePart)] = 0;
    s.scr[static_cast<uint8_t>(NodeId::ScenePart)] = 0;
    go(NodeId::ScenePart);
}

// Output `o` joins the open part (leaving the part it was in) or leaves it. A
// part keeps its last output: it has to play somewhere.
void toggle_part_output(uint8_t o) {
    if (g_scene_index >= config::num_scenes()) return;
    auto sc = config::get_scene(g_scene_index);
    if (g_part_index >= sc.num_parts) return;
    const auto bit = static_cast<uint8_t>(1u << o);
    uint8_t& mine  = sc.parts[g_part_index].mask;
    if (mine & bit) {
        if (mine == bit) return;
        mine = static_cast<uint8_t>(mine & ~bit);
    } else {
        for (size_t k = 0; k < sc.num_parts; ++k)
            sc.parts[k].mask = static_cast<uint8_t>(sc.parts[k].mask & ~bit);
        mine = static_cast<uint8_t>(mine | bit);
    }
    const uint8_t keep = mine;
    config::set_scene(g_scene_index, sc);
    // A part left without an output is gone: follow ours to where it sits now.
    const auto& stored = config::get_scene(g_scene_index);
    for (size_t k = 0; k < stored.num_parts; ++k)
        if (stored.parts[k].mask == keep) g_part_index = static_cast<uint8_t>(k);
}

}  // namespace

uint8_t build_scene_list(ListItem* items, OnClick* fns) {
    static char values[config::kMaxScenes][10];
    const auto count = static_cast<uint8_t>(config::num_scenes());
    for (uint8_t i = 0; i < count; ++i) {
        const auto& sc = config::get_scene(i);
        if (config::scene_group(sc) >= 0)
            group_label(sc.group, values[i], sizeof(values[i]));
        else
            std::snprintf(values[i], sizeof(values[i]), "%up", sc.num_parts);
        items[i] = { sc.name[0] ? sc.name : "(no name)", values[i] };
        fns[i]   = open_scene;
    }
    uint8_t n = count;
    if (count < config::kMaxScenes) {
        items[n] = { "[New]", "" };
        fns[n++] = [](uint8_t) {
            // The first effect on every output, as from the web and the console.
            const int idx = config::add_scene(config::make_scene("New scene", 0xFF, 0));
            if (idx >= 0) open_scene(static_cast<uint8_t>(idx));
        };
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── SCENE NODE ───────────────────────────────────────────────────────────────
// One scene: its name, where it plays by default (its outputs or a fixture
// group), one row per part ("1234----  Chase") and [Add part], then play /
// stop to see the result, and delete.

uint8_t build_scene_edit(ListItem* items, OnClick* fns) {
    uint8_t n = 0;
    std::snprintf(g_scene_title, sizeof(g_scene_title), "SCENE %u", g_scene_index + 1u);
    if (g_scene_index >= config::num_scenes()) {  // deleted meanwhile
        items[n] = back_item();
        fns[n++] = [](uint8_t) { go_back(); };
        return n;
    }
    const auto& sc = config::get_scene(g_scene_index);
    static char vname[10], vgroup[10];
    static char plabel[config::kMaxSceneParts][10], pvalue[config::kMaxSceneParts][10];
    truncate(vname, sizeof(vname), sc.name);
    group_label(sc.group, vgroup, sizeof(vgroup));
    items[n] = { "Name", vname };
    fns[n++] = [](uint8_t) {
        enter_edit_string(StringField::SceneName, config::get_scene(g_scene_index).name,
                          config::kSceneNameMax - 1, "Name", Screen::Menu);
    };
    items[n] = { "Plays on", vgroup };
    fns[n++] = [](uint8_t) {
        const int32_t hi = config::get_groups().count;
        int32_t cur      = config::get_scene(g_scene_index).group;
        if (cur > hi) cur = 0;
        enter_edit(Field::SceneGroup, ValueKind::Group, cur, 0, hi, 1, "Plays on", Screen::Menu);
    };
    constexpr uint8_t kFirstPartRow = 2;
    for (uint8_t k = 0; k < sc.num_parts; ++k) {
        format_mask(sc.parts[k].mask, plabel[k], sizeof(plabel[k]));
        effect_label(sc.parts[k].effect, pvalue[k], sizeof(pvalue[k]));
        items[n] = { plabel[k], pvalue[k] };
        fns[n++] = [](uint8_t row) { open_part(static_cast<uint8_t>(row - kFirstPartRow)); };
    }
    if (sc.num_parts < config::kMaxSceneParts && config::scene_mask(sc) != 0xFF &&
        config::num_effects() > 0) {
        items[n] = { "[Add part]", "" };
        fns[n++] = [](uint8_t) {
            // The outputs no part holds yet, on the first effect.
            auto e = config::get_scene(g_scene_index);
            if (e.num_parts >= config::kMaxSceneParts) return;
            const auto free = static_cast<uint8_t>(~config::scene_mask(e));
            if (!free) return;
            e.parts[e.num_parts++] = config::ScenePart{ free, 0, config::kFixtureModeEach, 0 };
            config::set_scene(g_scene_index, e);
            const uint8_t parts = config::get_scene(g_scene_index).num_parts;
            if (parts) open_part(static_cast<uint8_t>(parts - 1));
        };
    }
    items[n] = { "[Play]", "" };
    fns[n++] = [](uint8_t) { dmx::scene_start(g_scene_index); };
    items[n] = { "[Stop]", "" };
    fns[n++] = [](uint8_t) { dmx::scene_stop_scene(g_scene_index); };
    items[n] = { "[Delete]", "" };
    fns[n++] = [](uint8_t) {
        dmx::scene_stop_scene(g_scene_index);
        if (config::delete_scene(g_scene_index))
            dmx::scene_list_edited(config::SceneEdit::Delete, g_scene_index);
        go_back();
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── PART NODE ────────────────────────────────────────────────────────────────
// One part of the open scene: its outputs, the effect of the bank it plays
// there, how the effect spreads over the fixtures, and its direction.

uint8_t build_scene_part(ListItem* items, OnClick* fns) {
    uint8_t n = 0;
    std::snprintf(g_part_title, sizeof(g_part_title), "PART %u", g_part_index + 1u);
    const bool gone = g_scene_index >= config::num_scenes() ||
                      g_part_index >= config::get_scene(g_scene_index).num_parts;
    if (gone) {
        items[n] = back_item();
        fns[n++] = [](uint8_t) { go_back(); };
        return n;
    }
    const config::ScenePart& p = config::get_scene(g_scene_index).parts[g_part_index];
    static char vmask[10], veffect[10];
    format_mask(p.mask, vmask, sizeof(vmask));
    effect_label(p.effect, veffect, sizeof(veffect));
    items[n] = { "Outputs", vmask };
    fns[n++] = [](uint8_t) {
        s.cur[static_cast<uint8_t>(NodeId::PartOutputs)] = 0;
        s.scr[static_cast<uint8_t>(NodeId::PartOutputs)] = 0;
        go(NodeId::PartOutputs);
    };
    if (config::num_effects() > 0) {
        items[n] = { "Effect", veffect };
        fns[n++] = [](uint8_t) {
            const int32_t hi = static_cast<int32_t>(config::num_effects()) - 1;
            int32_t cur      = config::get_scene(g_scene_index).parts[g_part_index].effect;
            if (cur > hi) cur = 0;
            enter_edit(Field::PartEffect, ValueKind::Effect, cur, 0, hi, 1, "Effect", Screen::Menu);
        };
    }
    items[n] = { "Fixtures", fix_mode_label(config::scene_mode_of(p.fixture_mode)) };
    fns[n++] = [](uint8_t) {
        const auto& part = config::get_scene(g_scene_index).parts[g_part_index];
        enter_edit(Field::PartMode, ValueKind::FixMode, config::scene_mode_of(part.fixture_mode), 0,
                   config::kFixtureModeCount - 1, 1, "Fixtures", Screen::Menu);
    };
    items[n] = { "Reverse", config::scene_reverse_of(p.fixture_mode) ? "ON" : "OFF" };
    fns[n++] = [](uint8_t) {
        const auto& part = config::get_scene(g_scene_index).parts[g_part_index];
        enter_edit(Field::PartReverse, ValueKind::Bool,
                   config::scene_reverse_of(part.fixture_mode) ? 1 : 0, 0, 1, 1, "Reverse",
                   Screen::Menu);
    };
    items[n] = { "[Delete]", "" };
    fns[n++] = [](uint8_t) {
        auto e = config::get_scene(g_scene_index);
        if (g_part_index >= e.num_parts) return;
        for (size_t k = g_part_index; k + 1 < e.num_parts; ++k)
            e.parts[k] = e.parts[k + 1];
        e.parts[--e.num_parts] = config::ScenePart{};
        config::set_scene(g_scene_index, e);
        go_back();
    };
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

// ── PART OUTPUTS NODE ────────────────────────────────────────────────────────
// The eight outputs, one click each: ON in this part, "part N" when another
// part of the scene holds it (a click takes it over), OFF when none does.

uint8_t build_part_outputs(ListItem* items, OnClick* fns) {
    static char labels[config::kNumChannels][10], values[config::kNumChannels][12];
    uint8_t n       = 0;
    const bool gone = g_scene_index >= config::num_scenes() ||
                      g_part_index >= config::get_scene(g_scene_index).num_parts;
    if (!gone) {
        const auto& sc = config::get_scene(g_scene_index);
        for (uint8_t o = 0; o < config::kNumChannels; ++o) {
            std::snprintf(labels[o], sizeof(labels[o]), "Output %u", o + 1u);
            std::snprintf(values[o], sizeof(values[o]), "OFF");
            for (uint8_t k = 0; k < sc.num_parts; ++k) {
                if (!((sc.parts[k].mask >> o) & 1)) continue;
                if (k == g_part_index)
                    std::snprintf(values[o], sizeof(values[o]), "ON");
                else
                    std::snprintf(values[o], sizeof(values[o]), "part %u", k + 1u);
            }
            items[n] = { labels[o], values[o] };
            fns[n++] = toggle_part_output;
        }
    }
    items[n] = back_item();
    fns[n++] = [](uint8_t) { go_back(); };
    return n;
}

}  // namespace pixfrog::ui::detail::menu_impl
