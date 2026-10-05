// The value editors: generic int/bool/enum, string, IP address, universe.

#include "menu_internal.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

// ── EDIT VALUE — generic int / bool / enum ──────────────────────────────────

void enter_edit(Field field, ValueKind kind, int32_t cur, int32_t mn, int32_t mx, int32_t step,
                const char* label, Screen return_screen, uint8_t channel) {
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

    const bool gauge = is_gauge_kind(s.edit.kind);
    const bool boolf = (s.edit.kind == ValueKind::Bool);
    const bool enumf = is_enum_kind(s.edit.kind);
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
        int nc     = 0;
        int center = -1;  // index of the off==0 (Mega, selected) cell within cells[]
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
            if (off == 0) center = nc;
            ++nc;
        }
        // Anchor the selected cell at screen-centre and flow its neighbours
        // outward from there — summing the whole row's width and centring
        // that shifted the big digit off-centre near either end of the range,
        // where one side has fewer (or no) neighbours than the other.
        int xs[5];
        xs[center] = kTW / 2 - cells[center].w / 2;
        for (int i = center + 1; i < nc; ++i)
            xs[i] = xs[i - 1] + cells[i - 1].w + gap;
        for (int i = center - 1; i >= 0; --i)
            xs[i] = xs[i + 1] - gap - cells[i].w;
        for (int i = 0; i < nc; ++i) {
            Cell& c        = cells[i];
            const int chh  = canvas_font_h(c.f);
            const int yy   = cy - chh / 2;
            const bool ctr = (c.f == FontId::Mega);
            const int x    = xs[i];
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
        }
        draw_hint_bar("adjust", "apply", "cancel");
    } else if (boolf) {
        // Big ON/OFF word + a track/knob switch below it, grouped and centred
        // like the numeric gauge's "value + bar" — a lone word left the rest
        // of the screen empty and gave no "this is a toggle" affordance.
        const char* v      = s.edit.current ? "ON" : "OFF";
        const int vw       = canvas_text_w(v, FontId::Mega);
        constexpr int kSwW = 72;
        constexpr int kSwH = 30;
        const int groupH   = mh + 10 + kSwH;
        const int top      = kHdrH + ((kTH - kFootH) - kHdrH - groupH) / 2;
        canvas_draw_text_f((kTW - vw) / 2, top, v, color::EditCyan, color::Black, FontId::Mega);

        const int sx      = (kTW - kSwW) / 2;
        const int sy      = top + mh + 10;
        const Color track = s.edit.current ? color::FrogLine : color::IdleGreen;
        canvas_fill_round_rect_aa(sx, sy, kSwW, kSwH, kSwH / 2, track, color::Black);
        const int knobD = kSwH - 6;
        const int knobX = s.edit.current ? sx + kSwW - knobD - 3 : sx + 3;
        canvas_fill_round_rect_aa(knobX, sy + 3, knobD, knobD, knobD / 2, color::Cream, track);
        draw_hint_bar("toggle", "apply", "cancel");
    } else {
        // Numeric gauge — value + bar centred as a group in the content area.
        char val[24];
        format_value(s.edit, s.edit.current, val, sizeof(val));
        const int vw     = canvas_text_w(val, FontId::Mega);
        const int sh     = canvas_font_h(FontId::Small);
        const int gh     = 10;
        const int groupH = mh + 6 + gh + 8 + sh;  // value, gap, bar, gap, labels
        const int top    = kHdrH + ((kTH - kFootH) - kHdrH - groupH) / 2;
        canvas_draw_text_f((kTW - vw) / 2, top, val, color::EditCyan, color::Black, FontId::Mega);
        if (gauge) {
            const int gx = 30, gw = kTW - 60, gy = top + mh + 6;
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

    const bool gauge = is_gauge_kind(s.edit.kind);
    const bool enumf = is_enum_kind(s.edit.kind);

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
        } else if (s.edit.kind == ValueKind::Bool) {
            // Track/knob switch in the gauge's slot — a lone big word otherwise
            // left the rest of the screen empty with no toggle affordance.
            constexpr int kSwW = 96, kSwH = 40;
            const int sx      = (kTW - kSwW) / 2;
            const int sy      = kHdrH + 74;
            const Color track = s.edit.current ? color::FrogLine : color::CursorBg;
            canvas_fill_round_rect(sx, sy, kSwW, kSwH, kSwH / 2, track);
            const int knobD = kSwH - 8;
            const int knobX = s.edit.current ? sx + kSwW - knobD - 4 : sx + 4;
            canvas_fill_round_rect(knobX, sy + 4, knobD, knobD, knobD / 2, color::Cream);
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

    // Show the original (pre-edit) value when the user has moved
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
    if (is_gauge_kind(s.edit.kind)) {
        char lo[16], hi[16];
        format_value(s.edit, s.edit.min, lo, sizeof(lo));
        format_value(s.edit, s.edit.max, hi, sizeof(hi));
        std::snprintf(ctx, sizeof(ctx), "  %s..%s", lo, hi);
        draw_row(5, 0, ctx);
    } else if (is_enum_kind(s.edit.kind)) {
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
    case Field::ShowMaster:
        dmx::master_set(dmx::kAllOutputs, static_cast<uint16_t>(v * 65535 / 100));
        break;
    case Field::ShowStrobe: dmx::strobe_set(dmx::kAllOutputs, static_cast<uint8_t>(v * 10)); break;
    case Field::ShowFade: {
        auto g          = config::get_global();
        g.scene_fade_ms = static_cast<uint16_t>(v * 100);
        config::set_global(g);
        break;
    }
    case Field::CtlEnabled:
    case Field::CtlUniverse:
    case Field::CtlAddress:
    case Field::CtlPreset:
    case Field::CtlSlotFn:
    case Field::CtlSlotMask:
    case Field::CtlSlotIndex:
    case Field::CtlSlotFine: {
        auto c   = config::get_control();
        auto& sl = c.slots[g_ctl_slot < config::kMaxControlSlots ? g_ctl_slot : 0];
        switch (s.edit.field) {
        case Field::CtlEnabled: c.enabled = static_cast<uint8_t>(v); break;
        case Field::CtlUniverse: c.universe = static_cast<uint16_t>(v); break;
        case Field::CtlAddress: c.address = static_cast<uint16_t>(v); break;
        case Field::CtlPreset:
            config::control_apply_preset(c, v ? config::ControlPreset::Full
                                              : config::ControlPreset::Simple);
            break;
        case Field::CtlSlotFn:
            sl.fn = static_cast<uint8_t>(v);
            if (sl.fn != static_cast<uint8_t>(config::CtlFn::Master)) sl.flags = 0;
            break;
        case Field::CtlSlotMask: sl.mask = static_cast<uint8_t>(v); break;
        case Field::CtlSlotIndex: sl.index = static_cast<uint8_t>(v - 1); break;
        case Field::CtlSlotFine: sl.flags = v ? config::kCtlFlagFine : 0; break;
        default: break;
        }
        config::ControlConfig check = c;
        config::sanitize_control(check);
        if (check.count == c.count) save_control(c);  // refused if it would pass channel 512
        break;
    }
    case Field::GlobalRefresh: {
        auto g            = config::get_global();
        g.refresh_rate_hz = static_cast<uint8_t>(v);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::SpeakerVolume: {
        auto g           = config::get_global();
        g.speaker_volume = static_cast<uint8_t>(v);
        config::set_global(g);
        break;
    }
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    case Field::DisplayBrightness: {
        auto g           = config::get_global();
        g.tft_brightness = static_cast<uint8_t>(v);
        config::set_global(g);
        break;
    }
    case Field::DisplayIdleDim: {
        auto g         = config::get_global();
        g.tft_idle_dim = static_cast<uint8_t>(v);
        config::set_global(g);
        break;
    }
    case Field::DisplayDimDelay: {
        auto g            = config::get_global();
        g.tft_dim_delay_s = static_cast<uint16_t>(v);
        config::set_global(g);
        break;
    }
    case Field::DisplayPixelRefresh:
        // Not persisted — the click that commits the duration starts the run,
        // and edit.return_screen (Screen::PixelRefresh) takes us there.
        g_pixel_refresh_s  = v;
        s.refresh_until_ms = now_ms() + static_cast<uint32_t>(v) * 1000u;
        break;
#endif
    case Field::NetworkDhcp: {
        auto g     = config::get_global();
        g.use_dhcp = (v != 0);
        config::set_global(g);
        dmx::mark_global_dirty();
        break;
    }
    case Field::NetworkIpFallback: {
        auto g        = config::get_global();
        g.ip_fallback = static_cast<uint8_t>(v);  // read when the fallback is taken
        config::set_global(g);
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
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChGapPos: {
        auto c         = config::get_channel(s.edit.channel);
        const auto p0  = static_cast<uint16_t>(v - 1);
        const size_t n = led::gap_count(c.gaps, led::kMaxPixelGaps);
        if (g_gap_index < n) {
            c.gaps[g_gap_index].pos = p0;
        } else if (n < led::kMaxPixelGaps) {
            c.gaps[n] = { p0, 1 };
        }
        config::set_channel(s.edit.channel, c);  // normalizes: the gap may move slot
        dmx::mark_channel_dirty(s.edit.channel);
        // Follow the gap to its sorted slot for the length edit that comes next.
        const auto& stored = config::get_channel(s.edit.channel);
        for (size_t k = 0; k < led::kMaxPixelGaps && stored.gaps[k].len; ++k)
            if (p0 >= stored.gaps[k].pos && p0 < stored.gaps[k].pos + stored.gaps[k].len)
                g_gap_index = static_cast<uint8_t>(k);
        break;
    }
    case Field::ChGapLen: {
        auto c = config::get_channel(s.edit.channel);
        if (g_gap_index < led::kMaxPixelGaps) c.gaps[g_gap_index].len = static_cast<uint16_t>(v);
        config::set_channel(s.edit.channel, c);  // len 0 → dropped by normalize
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChPacking: {
        auto c    = config::get_channel(s.edit.channel);
        c.packing = static_cast<uint8_t>(v);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::ChPixelMap: {
        // Off: DMX control, the fixtures on their profiles. On again: the
        // pixel layout the output had.
        auto c = config::get_channel(s.edit.channel);
        if (v) {
            if (c.packing == config::kPackControl)
                c.packing = pixel_layout_before_control(s.edit.channel);
        } else {
            note_pixel_layout(s.edit.channel, c.packing);
            c.packing = config::kPackControl;
        }
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    // One fixture of the open channel. The editors' ranges keep it clear of
    // its neighbours, so the list keeps its order and g_fix_index its fixture.
    case Field::FixFirst:
    case Field::FixLen:
    case Field::FixReversed:
    case Field::FixProfile: {
        auto c = config::get_channel(s.edit.channel);
        if (g_fix_index >= config::fixture_count(c.fixtures, config::kMaxFixtures)) break;
        const config::Fixture f = c.fixtures[g_fix_index];
        uint16_t pos = f.pos, len = config::fixture_len(f);
        bool rev        = config::fixture_reversed(f);
        uint8_t profile = config::fixture_profile(f);
        switch (s.edit.field) {
        case Field::FixFirst: pos = static_cast<uint16_t>(v - 1); break;
        case Field::FixLen: len = static_cast<uint16_t>(v); break;
        case Field::FixReversed: rev = v != 0; break;
        default: profile = static_cast<uint8_t>(v); break;
        }
        c.fixtures[g_fix_index] = config::make_fixture(pos, len, rev, profile);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::FixSplit: {
        // N equal fixtures over the strip; the LEDs left over stay without one.
        auto c             = config::get_channel(s.edit.channel);
        const uint16_t end = strip_end(c);
        const auto each    = static_cast<uint16_t>(end / v);
        if (each == 0) break;
        std::memset(c.fixtures, 0, sizeof(c.fixtures));
        for (int32_t i = 0; i < v; ++i)
            c.fixtures[i] = config::make_fixture(static_cast<uint16_t>(i * each), each);
        config::set_channel(s.edit.channel, c);
        dmx::mark_channel_dirty(s.edit.channel);
        break;
    }
    case Field::SceneGroup:
    case Field::PartEffect:
    case Field::PartMode:
    case Field::PartReverse: {
        if (g_scene_index >= config::num_scenes()) break;
        auto sc = config::get_scene(g_scene_index);
        if (s.edit.field == Field::SceneGroup) {
            sc.group = static_cast<uint8_t>(v);
        } else if (g_part_index < sc.num_parts) {
            auto& p            = sc.parts[g_part_index];
            const uint8_t mode = config::scene_mode_of(p.fixture_mode);
            const bool rev     = config::scene_reverse_of(p.fixture_mode);
            if (s.edit.field == Field::PartEffect)
                p.effect = static_cast<uint8_t>(v);
            else if (s.edit.field == Field::PartMode)
                p.fixture_mode = config::pack_scene_mode(static_cast<uint8_t>(v), rev, -1);
            else
                p.fixture_mode = config::pack_scene_mode(mode, v != 0, -1);
        }
        config::set_scene(g_scene_index, sc);
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
        const int32_t mult  = (s.edit.kind == ValueKind::Int) ? accel_note_rotation(e) : 1;
        const int32_t step  = s.edit.step * mult;
        s.edit.current     += (e == Event::RotateRight) ? step : -step;
        if (s.edit.current < s.edit.min) s.edit.current = s.edit.min;
        if (s.edit.current > s.edit.max) s.edit.current = s.edit.max;
        // Live strip preview while the pixel count moves.
        if (s.edit.field == Field::ChPixels && dmx::pixel_preview_channel() == s.edit.channel) {
            dmx::set_pixel_preview(s.edit.channel, static_cast<uint16_t>(s.edit.current));
        }
        if (s.edit.field == Field::ChGapPos || s.edit.field == Field::ChGapLen) {
            const auto& cc      = config::get_channel(s.edit.channel);
            const bool existing = g_gap_index < led::gap_count(cc.gaps, led::kMaxPixelGaps);
            if (s.edit.field == Field::ChGapPos)
                preview_gap_edit(static_cast<uint16_t>(s.edit.current - 1),
                                 existing ? cc.gaps[g_gap_index].len : 1);
            else if (existing)
                preview_gap_edit(cc.gaps[g_gap_index].pos, static_cast<uint16_t>(s.edit.current));
        }
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
        // Live backlight preview: the panel follows the encoder, nothing stored.
        if (s.edit.field == Field::DisplayBrightness)
            backlight_preview(static_cast<uint8_t>(s.edit.current));
#endif
    }
    if (e == Event::Click) {
        const Field done = s.edit.field;
        dmx::clear_pixel_preview();
        commit_edit();
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
        backlight_preview_end();  // after commit_edit: hands over to the stored level
#endif
        s.screen = s.edit.return_screen;
        if (done == Field::ChGapPos) enter_gap_len_edit();  // position, then length
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
        // 2× font (Mega): a tighter window of cells centred on the cursor.
        constexpr int kVis = 13;
        int st             = static_cast<int>(cursor) - kVis / 2;
        if (st < 0) st = 0;
        if (st + kVis > len) st = len - kVis;
        if (st < 0) st = 0;
        const int shown = (len - st < kVis) ? (len - st) : kVis;
        const int adv   = canvas_font_adv(FontId::Mega) + 2;
        const int mfh   = canvas_font_h(FontId::Mega);
        int x           = (kTW - shown * adv) / 2;
        const int ty    = cy - mfh / 2;
        for (int i = 0; i < shown; ++i) {
            const char raw   = s.str_edit.buf[st + i];
            const bool act   = (st + i == cursor);
            const bool blank = (raw == ' ');
            char ch[2]       = { blank ? '_' : raw, 0 };
            const Color col  = act ? color::EditCyan : blank ? color::IdleGreen : color::Cream;
            canvas_draw_text_f(x + (adv - canvas_font_adv(FontId::Mega)) / 2, ty, ch, col,
                               color::Black, FontId::Mega);
            if (act) canvas_fill_rect(x + 2, ty + mfh + 1, adv - 4, 3, color::EditCyan);
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

    if (s.str_edit.field == StringField::SceneName) {
        if (g_scene_index >= config::num_scenes()) return;
        auto sc        = config::get_scene(g_scene_index);
        const size_t n = std::min(std::strlen(s.str_edit.buf), sizeof(sc.name) - 1);
        std::memset(sc.name, 0, sizeof(sc.name));
        std::memcpy(sc.name, s.str_edit.buf, n);
        config::set_scene(g_scene_index, sc);
        return;
    }
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
            const int32_t step  = accel_note_rotation(e);
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
            const int32_t step   = accel_note_rotation(e);
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

}  // namespace pixfrog::ui::detail::menu_impl
