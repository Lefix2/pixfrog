// Bespoke menu screens: HOME dashboard, PIXEL REFRESH, ABOUT, NERD STATS.

#include "menu_internal.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

// ── HOME ────────────────────────────────────────────────────────────────────

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// Per-channel metric shown in HOME's rotating right-hand slot. The unit shares
// the value's Body face (px / % / g), so the suffix alone identifies the metric
// as the display cycles through them.
enum class ChMetric : uint8_t { Pixels, Bright, Gamma, Order, Count };

// Advance one metric every kHomeRotateMs — slow enough to read, brisk enough to
// feel live. Global (all LED channels show the same metric at once).
constexpr uint32_t kHomeRotateMs = 2600;

void format_ch_metric(const config::ChannelConfig& cc, ChMetric m, char* out, size_t cap) {
    switch (m) {
    case ChMetric::Bright:
        std::snprintf(out, cap, "%u%%",
                      (static_cast<unsigned>(cc.brightness) * 100u + 127u) / 255u);
        return;
    case ChMetric::Gamma:
        std::snprintf(out, cap, "%u.%ug", cc.gamma_x10 / 10, cc.gamma_x10 % 10);
        return;
    case ChMetric::Order: std::snprintf(out, cap, "%s", color_order_name(cc.color_order)); return;
    case ChMetric::Pixels:
    default: std::snprintf(out, cap, "%upx", cc.pixel_count); return;
    }
}
#endif

void render_home() {
    char line[48];
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 landscape 428×142 dashboard ────────────────────────────────────
    // Single status line (22px) | green rule | 8 channels, row-major 4×2 grid.
    constexpr int kChTop = kHdrH;               // channel grid top (22)
    constexpr int kChH   = (kTH - kChTop) / 4;  // 30px per channel row
    constexpr int kCellW = kTW / 2;             // 214px per channel column

    canvas_clear(color::Black);
    const auto stats = dmx::get_stats();
    bool rx_active   = false;
    for (int i = 0; i < 8; ++i)
        rx_active |= dmx::is_channel_active(i);

    // ── Status line: IP + live-service strip (left) · fps + icons (right) ─────
    // Left: the address, then a self-arranging strip of active services (web
    // globe, sACN) via StatusStrip. Right: current/cap fps + data/net icons.
    {
        const int h       = kHdrH;
        const auto& g     = config::get_global();
        const uint32_t ip = ui::get_ip();
        int ip_end        = kPadX;
        if (ip == 0) {
            text_body(kPadX, 0, h, "0.0.0.0", color::DimGreen);
            ip_end += body_w("0.0.0.0");
        } else {
            std::snprintf(
                line, sizeof(line), "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
                static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
                static_cast<unsigned>(ip & 0xFFu));
            text_body(kPadX, 0, h, line, color::Cream);
            ip_end += body_w(line);
        }
        // Active-service strip to the right of the IP: web globe, then sACN.
        StatusStrip strip{ ip_end + 11, h };
        if (g.web_enabled) strip.icon(kIcon_globe, color::FrogLine);
        if (g.sacn_enabled) strip.label("sACN", color::GoodBright);
        // Show control: blackout outranks a dimmed master.
        if (dmx::blackout_effective()) {
            strip.label("BO", color::Red);
        } else if (dmx::master_effective(0) < dmx::kMasterFull) {
            static char mst[8];
            std::snprintf(mst, sizeof(mst), "M%u%%",
                          (static_cast<unsigned>(dmx::master_effective(0)) * 100u + 32767u) /
                              65535u);
            strip.label(mst, color::Gold);
        }
        if (dmx::control_live()) strip.label("CTL", color::FrogLine);

        // Right group (flush right): net icon · data icon · "fps" · cur/cap.
        const int iconY = (h - kIconSize) / 2;
        int rx          = kTW - kPadX - kIconSize;
        draw_net_icon(rx, iconY, ui::get_net_state());
        rx                  -= 2 + kIconSize;
        const DataFlow flow  = rx_active ? DataFlow::Receive : DataFlow::None;
        draw_data_icon(rx, iconY, flow);
        rx -= 6 + body_w("fps");
        text_body(rx, 0, h, "fps", color::DimGreen);
        // "/<cap>" in dim, then the live fps in gold — reads "56/60 fps".
        std::snprintf(line, sizeof(line), "/%u", g.refresh_rate_hz);
        rx -= 3 + body_w(line);
        text_body(rx, 0, h, line, color::DimGreen);
        std::snprintf(line, sizeof(line), "%lu", static_cast<unsigned long>(stats.current_fps));
        rx -= body_w(line);
        text_body(rx, 0, h, line, color::Gold);
    }
    canvas_hline(0, kHdrH - 1, kTW, color::FrogLine);

    // ── Channel grid: 8 single-line cells, row-major (1 2 / 3 4 / …) ──────────
    // Soft zebra rows (no hairlines), one homogeneous Body cell for every value.
    // Per cell: [badge] PROTOCOL   <universe range>   <rotating metric>  •
    // Universe values share a common centre-x (centred among themselves, not
    // right-aligned); the metric column rotates through pixels/brightness/gamma/
    // order, the unit suffix (px/%/g) telling which is on screen.
    const int dotR   = kCellW - 9;     // activity dot / "!" right edge
    const int rotR   = dotR - 11;      // rotating-metric right edge (clears the dot)
    const int uniCtr = kCellW - 98;    // shared centre-x of the universe column
    const int protoX = 7 + kChip + 6;  // protocol name left edge within a cell
    const int metric = static_cast<int>((now_ms() / kHomeRotateMs) %
                                        static_cast<uint32_t>(ChMetric::Count));
    for (int i = 0; i < 8; ++i) {
        const int col      = i % 2;
        const int row      = i / 2;
        const int x0       = col * kCellW;
        const int cy       = kChTop + row * kChH;
        const Color row_bg = (row & 1) ? color::RowAlt : color::Black;
        if (col == 0) canvas_fill_rect(0, cy, kTW, kChH, row_bg);

        const auto& cc = config::get_channel(i);
        const bool off = led::is_off(cc.protocol);
        const bool ok  = dmx::is_channel_capacity_ok(i);

        // Badge: filled in the wiring-family colour when in use, hollow when Off.
        const int chcy = cy + (kChH - kChip) / 2;
        draw_chan_badge(x0 + 7, chcy, kChip, i + 1, badge_color(cc.protocol), !off, row_bg);

        // Protocol name.
        const char* pname = protocol_name(cc.protocol);
        text_body(x0 + protoX, cy, kChH, pname, off ? color::DimGreen : color::Cream, row_bg);

        // Activity: live dot (or "!" over capacity).
        if (!ok) {
            text_body(x0 + dotR - body_w("!"), cy, kChH, "!", color::BadCoral, row_bg);
        } else if (!off) {
            const bool act     = dmx::is_channel_active(i);
            const bool fsafe   = dmx::is_channel_failsafe(i);
            const Color dotcol = fsafe ? color::Orange : act ? color::GoodBright : color::IdleGreen;
            canvas_fill_round_rect_aa(x0 + dotR - 8, cy + (kChH - 8) / 2, 8, 8, 4, dotcol, row_bg);
        }
        if (!off) {
            // Rotating metric (right-aligned).
            char mv[16];
            format_ch_metric(cc, static_cast<ChMetric>(metric), mv, sizeof(mv));
            const int mvw        = body_w(mv);
            const int metricLeft = x0 + rotR - mvw;

            // Universe range, centred on the shared column axis. When a wide
            // range would meet a wide metric it is nudged left to keep a small
            // gap (perfect centring holds for the common short values).
            const int span = channel_universe_span(cc);
            char ur[24];  // two 10-digit numbers and a dash
            if (span > 1)
                std::snprintf(ur, sizeof(ur), "%u-%u", cc.universe_start,
                              static_cast<unsigned>(cc.universe_start + span - 1));
            else
                std::snprintf(ur, sizeof(ur), "%u", cc.universe_start);
            const int urw      = body_w(ur);
            const int protoEnd = x0 + protoX + body_w(pname);
            int uni_x          = x0 + uniCtr - urw / 2;
            if (uni_x + urw > metricLeft - 4) uni_x = metricLeft - 4 - urw;
            if (uni_x < protoEnd + 3) uni_x = protoEnd + 3;
            text_body(uni_x, cy, kChH, ur, ok ? color::Gold : color::BadCoral, row_bg);

            text_body(metricLeft, cy, kChH, mv, color::LightGray, row_bg);
        }
    }
    // Thin divider between the two channel columns.
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
    px = draw_pill(px, py, line, color::HeaderBg, color::DarkGray, color::Black);
    if (dmx::blackout_effective()) {
        draw_pill(px, py, "BO", color::Red, color::Black, color::Black);
    } else if (dmx::master_effective(0) < dmx::kMasterFull) {
        std::snprintf(line, sizeof(line), "M%u%%",
                      (static_cast<unsigned>(dmx::master_effective(0)) * 100u + 32767u) / 65535u);
        draw_pill(px, py, line, color::Gold, color::Black, color::Black);
    }
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
            // Red: the stored count is more than this refresh rate can drive.
            draw_text_r(kColPixEnd, ty, line, ok ? color::LightGray : color::Red, row_bg);
            std::snprintf(line, sizeof(line), "%u%%",
                          (static_cast<unsigned>(cc.brightness) * 100u + 127u) / 255u);
            draw_text_r(kColBriEnd, ty, line, color::LightGray, row_bg);
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

    // Warn loudly when NVS is broken (config does NOT persist).
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

    // Link state, distinct from IP since DHCP can be pending.
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

// ── PIXEL REFRESH ───────────────────────────────────────────────────────────
// Drives every pixel through the full RGBW set for a chosen number of seconds.
// Two jobs: exercise sub-pixels that have been holding one value for a long
// time, and repaint rows the incremental flush may have left stale (the panel
// occasionally keeps a row that every layer above believes it already sent —
// see 9cf7ced). This is the manual recovery for that, which is why menu_render
// no longer forces a full repaint on every screen change.

constexpr uint32_t kPixelRefreshStepMs = 250;  // per-colour dwell

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
int32_t g_pixel_refresh_s = kPixelRefreshDefS;
#endif

void render_pixel_refresh() {
    const uint32_t now = now_ms();
    if (now >= s.refresh_until_ms) {
        s.refresh_until_ms = 0;
        s.screen           = Screen::Menu;
        canvas_invalidate();  // hand the panel back fully repainted
        return;
    }
    static const Color kSeq[] = { color::Red, color::Green, color::Blue, color::White };
    canvas_clear(kSeq[(now / kPixelRefreshStepMs) % 4]);
    // The whole point is that every row physically reaches the glass, so never
    // let the row diff decide this one.
    canvas_invalidate();
}

void dispatch_pixel_refresh(Event e) {
    // Any input aborts early — no one should have to wait out 300 s.
    if (e == Event::Click || e == Event::RotateLeft || e == Event::RotateRight) {
        s.refresh_until_ms = 0;
        s.screen           = Screen::Menu;
        canvas_invalidate();
    }
}

// ── ABOUT ───────────────────────────────────────────────────────────────────

void render_about() {
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    // ── NV3007 design: rasterised frog logo + wordmark + one-liners ───────────
    canvas_clear(color::Black);
    draw_tft_header("ABOUT");
    const int cy = (kHdrH + kTH) / 2;
    // Real SVG-rasterised frog (final splash frame), not a procedural glyph.
    const int fw = splash_anim_w(), fh = splash_anim_h();
    const int fy = (kHdrH + kTH) / 2 - fh / 2;
    canvas_draw_mask(kPadX, fy, fw, fh, splash_anim_frame(splash_anim_count() - 1), color::FrogLine,
                     color::Transparent);
    const int tx = kPadX + fw + 14;
    canvas_draw_text_f(tx, cy - canvas_font_h(FontId::Mega) / 2 - 8, "pixfrog", color::FrogLine,
                       color::Black, FontId::Mega);
    text_body(tx, cy + 6, 16, "8-channel ArtNet . sACN node", color::Cream);
    text_body(tx, cy + 24, 16, fw_build_info(), color::DimGreen);
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

// ── NERD STATS ──────────────────────────────────────────────────────────────
// Read-only telemetry page: frame rate, packet counters, errors, and the
// service/refresh/addressing state that used to crowd the HOME header. Any
// click or long-press returns to the menu.

void render_stats() {
    const auto st = dmx::get_stats();
    const auto& g = config::get_global();
    char fps[12], hz[8];
    std::snprintf(fps, sizeof(fps), "%lu", static_cast<unsigned long>(st.current_fps));
    std::snprintf(hz, sizeof(hz), "%uHz", g.refresh_rate_hz);

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    // fmt_count + the colour-TFT helpers only exist in the TFT build.
    char frames[16], artnet[16], sacnrx[16], errs[12], under[12];
    fmt_count(frames, sizeof(frames), st.frames_emitted);
    fmt_count(artnet, sizeof(artnet), st.artnet_packets_rx);
    fmt_count(sacnrx, sizeof(sacnrx), st.sacn_packets_rx);
    std::snprintf(errs, sizeof(errs), "%llu",
                  static_cast<unsigned long long>(st.artnet_bad_packets));
    std::snprintf(under, sizeof(under), "%lu", static_cast<unsigned long>(st.dma_underruns));
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    canvas_clear(color::Black);
    draw_tft_header("NERD STATS");
    const int colW    = kTW / 2;
    const int rowH    = (kTH - kHdrH) / 5;
    const Color errc  = st.artnet_bad_packets ? color::BadCoral : color::DimGreen;
    const Color undc  = st.dma_underruns ? color::Orange : color::DimGreen;
    const Color sacnc = g.sacn_enabled ? color::GoodBright : color::DimGreen;
    const Color webc  = g.web_enabled ? color::GoodBright : color::DimGreen;
    const char* lL[5] = { "FPS", "Frames", "ArtNet", "sACN rx", "Errors" };
    const char* vL[5] = { fps, frames, artnet, sacnrx, errs };
    const Color cL[5] = { color::Gold, color::Cream, color::Cream, color::Cream, errc };
    const char* lR[5] = { "Refresh", "Net", "sACN", "Web", "Underrun" };
    const char* vR[5] = { hz, g.use_dhcp ? "DHCP" : "STATIC", g.sacn_enabled ? "ON" : "OFF",
                          g.web_enabled ? "ON" : "OFF", under };
    const Color cR[5] = { color::Gold, color::Cream, sacnc, webc, undc };
    for (int r = 0; r < 5; ++r) {
        const int ry   = kHdrH + r * rowH;
        const Color bg = (r & 1) ? color::RowAlt : color::Black;
        if (r & 1) canvas_fill_rect(0, ry, kTW, rowH, bg);
        text_body(kPadX, ry, rowH, lL[r], color::DimGreen, bg);
        text_body(colW - kPadX - body_w(vL[r]), ry, rowH, vL[r], cL[r], bg);
        text_body(colW + kPadX, ry, rowH, lR[r], color::DimGreen, bg);
        text_body(kTW - kPadX - body_w(vR[r]), ry, rowH, vR[r], cR[r], bg);
    }
#else
    canvas_clear(color::Black);
    draw_tft_header("NERD STATS");
    const char* l[7] = { "FPS", "Frames", "ArtNet", "sACN", "Errors", "Underrun", "Refresh" };
    const char* v[7] = { fps, frames, artnet, sacnrx, errs, under, hz };
    int y            = kHdrH + 6;
    for (int i = 0; i < 7; ++i) {
        canvas_draw_text(kIndent, y, l[i], color::DimGreen, color::Black, kTxtSc);
        draw_text_r(kTW - kIndent, y, v[i], color::Cream, color::Black);
        y += 28;
    }
#endif
#else
    // OLED: raw counters (no fmt_count) in the classic text rows.
    canvas_clear();
    draw_row(0, 0, "NERD STATS");
    char ln[kOledCols + 1];
    std::snprintf(ln, sizeof(ln), "FPS  :%s", fps);
    draw_row(1, 0, ln);
    std::snprintf(ln, sizeof(ln), "Art  :%llu",
                  static_cast<unsigned long long>(st.artnet_packets_rx));
    draw_row(2, 0, ln);
    std::snprintf(ln, sizeof(ln), "sACN :%llu",
                  static_cast<unsigned long long>(st.sacn_packets_rx));
    draw_row(3, 0, ln);
    std::snprintf(ln, sizeof(ln), "Err  :%llu",
                  static_cast<unsigned long long>(st.artnet_bad_packets));
    draw_row(4, 0, ln);
    std::snprintf(ln, sizeof(ln), "Undr :%lu", static_cast<unsigned long>(st.dma_underruns));
    draw_row(5, 0, ln);
    std::snprintf(ln, sizeof(ln), "Hz   :%s", hz);
    draw_row(6, 0, ln);
#endif
}

void dispatch_stats(Event e) {
    if (e == Event::Click) s.screen = Screen::Menu;
}

}  // namespace pixfrog::ui::detail::menu_impl
