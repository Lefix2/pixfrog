// The menu tree's nodes: each build_* fills one scrolling list (rows + click
// actions) for the engine in menu.cpp.

#include "menu_internal.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

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
    items[n]  = { "Settings", "" };
    fns[n++]  = [](uint8_t) { go(NodeId::Settings); };
    items[n]  = { "About", "" };
    fns[n++]  = [](uint8_t) { s.screen = Screen::About; };
    items[n]  = back_item("HOME");
    fns[n++]  = [](uint8_t) { go_back(); };
    return n;
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
// Play/stop only — editing colours and masks on a rotary encoder is web/UART
// territory. Every playing scene is starred: several play at once when their
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
    static char vctl[8];
    std::snprintf(vctl, sizeof(vctl), "%s", config::get_control().enabled ? "ON" : "OFF");
    items[6] = { "DMX ctrl", vctl };
    fns[6]   = [](uint8_t) { go(NodeId::Control); };
    items[7] = back_item();
    fns[7]   = [](uint8_t) { go_back(); };
    return 8;
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
        const auto& g = config::get_global();
        enter_edit(Field::GlobalRefresh, ValueKind::Int, g.refresh_rate_hz, config::kMinRefreshHz,
                     config::kMaxRefreshHz, 1, "Refresh", Screen::Menu);
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
    // Show control (runtime, all outputs): grand master, blackout, strobe; and
    // the scene crossfade time (persisted).
    static char vmaster[8], vbo[8], vstrobe[8], vfade[8];
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
    items[3] = { "Master", vmaster };
    fns[3]   = [](uint8_t) {
        enter_edit(Field::ShowMaster, ValueKind::Int,
                     (static_cast<int32_t>(dmx::master_local(0)) * 100 + 32767) / 65535, 0, 100, 5,
                     "Master", Screen::Menu);
    };
    items[4] = { "Blackout", vbo };
    fns[4]   = [](uint8_t) { dmx::blackout_toggle(); };  // instant, like the desk button
    items[5] = { "Strobe", vstrobe };
    fns[5]   = [](uint8_t) {
        enter_edit(Field::ShowStrobe, ValueKind::Int, dmx::strobe_local(0) / 10, 0, 25, 1,
                     "Strobe Hz", Screen::Menu);
    };
    items[6] = { "Fade", vfade };
    fns[6]   = [](uint8_t) {
        enter_edit(Field::ShowFade, ValueKind::Tenths, config::get_global().scene_fade_ms / 100, 0,
                     config::kMaxSceneFadeMs / 100, 1, "Fade", Screen::Menu);
    };
    items[7] = back_item();
    fns[7]   = [](uint8_t) { go_back(); };
    return 8;
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
    static char vshort[14], vlong[14], vfb[12];
    const auto& g = config::get_global();
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
    // Only meaningful in DHCP mode; kept last so the rows above keep their place.
    items[7] = { "NoDHCP", vfb };
    fns[7]   = [](uint8_t) {
        enter_edit(Field::NetworkIpFallback, ValueKind::IpFallback,
                     config::get_global().ip_fallback, 0, 1, 1, "No DHCP", Screen::Menu);
    };
    items[8] = back_item();
    fns[8]   = [](uint8_t) { go_back(); };
    return 9;
}

// ── CHANNEL MENU ────────────────────────────────────────────────────────────

constexpr size_t kProtocolCount = static_cast<size_t>(led::Protocol::COUNT);

// The channel menu is layout-driven: the visible items depend on the protocol
// family — an Off channel shows only Proto; clocked SPI strips add Clock.
enum class ChItem : uint8_t {
    Proto,
    Uni,
    Dmx,
    Pixels,
    Gaps,  // dead pixels submenu
    Order,
    Bright,
    Gamma,
    Group,
    Invert,
    Clock,
    Identify,
    Back,
};

// Fills `out` (capacity ≥ 13) with the ordered items for `cc` and returns count.
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
    out[n++] = ChItem::Gaps;
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
    case ChItem::Identify:
        return [](uint8_t) { dmx::identify_start(s.channel_index); };  // 3 blinks; stay
    case ChItem::Back: return [](uint8_t) { go_back(); };
    }
    return nullptr;
}

uint8_t build_channel(ListItem* items, OnClick* fns) {
    const auto& cc = config::get_channel(s.channel_index);
    std::snprintf(g_channel_title, sizeof(g_channel_title), "CHANNEL %u", s.channel_index + 1);

    static char vproto[8], vuni[12], vdmx[8], vpix[8], vorder[8], vbri[8], vgrp[8], vinv[8],
        vclk[12], vgam[8], vgaps[8];
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
    const size_t ngaps = led::gap_count(cc.gaps, led::kMaxPixelGaps);
    if (ngaps)
        std::snprintf(vgaps, sizeof(vgaps), "%u", static_cast<unsigned>(ngaps));
    else
        std::snprintf(vgaps, sizeof(vgaps), "-");

    ChItem order[13];
    const uint8_t count = channel_items(cc, order);
    for (uint8_t i = 0; i < count; ++i) {
        switch (order[i]) {
        case ChItem::Proto: items[i] = { "Proto", vproto }; break;
        case ChItem::Uni: items[i] = { "Uni", vuni }; break;
        case ChItem::Dmx: items[i] = { "DMX", vdmx }; break;
        case ChItem::Pixels: items[i] = { "Pixels", vpix }; break;
        case ChItem::Gaps: items[i] = { "Dead px", vgaps }; break;
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

}  // namespace pixfrog::ui::detail::menu_impl
