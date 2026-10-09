// Shared internals of the menu state machine, split across menu*.cpp: layout
// constants, the menu state, node/field types and the helpers every screen
// uses. Only the menu sources include it.
#pragma once

#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "icons_net.h"
#include "icons_status.h"
#include "led_output.h"
#include "led_protocols.h"
#include "menu_accel.h"
#include "net.h"
#include "sacn.h"
#include "ui.h"
#include "ui_internal.h"
#include "web_config.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace pixfrog::ui::detail::menu_impl {

// ── Local constants ──────────────────────────────────────────────────────────

// kCols is used for buffer sizing (worst-case OLED width 21 chars).
// For TFT, kRows/kCols from ui_internal.h govern the grid, but local
// buffers use kOledCols which covers both backends safely.
constexpr uint8_t kOledCols = 21;

// ── Canvas text helpers ───────────────────────────────────────────────────────
// Thin wrapper mapping OLED-style (row, col) coordinates to pixel coords. Every
// call site is in an OLED branch, so the TFT build has no use for it.

void draw_row(uint8_t row, uint8_t col, const char* str);

// ── TFT layout ────────────────────────────────────────────────────────────────
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// NV3007 rotated 90° → 428×142 landscape. Pixel-exact to pixfrog-screen.jsx:
// 22px header, 18px hint bar, two-column lists/home, native fonts (no scaling).
constexpr int kTW       = 428;                        // TFT width  (landscape, rotated)
constexpr int kTH       = 142;                        // TFT height (landscape, rotated)
constexpr int kHdrH     = 22;                         // header bar height (design HDR)
constexpr int kFootH    = 22;                         // edit-screen hint bar (design FOOT)
constexpr int kPadX     = 12;                         // header / status side padding
constexpr int kIndent   = 10;                         // list-row side padding
constexpr int kCols2    = 2;                          // two side-by-side columns
constexpr int kColW     = kTW / kCols2;               // 214px per column
constexpr int kListRows = 4;                          // design ROWS = 4 per column
constexpr int kMaxVis   = kCols2 * kListRows;         // 8 visible list items
constexpr int kRowH     = (kTH - kHdrH) / kListRows;  // 30px list row
constexpr int kChip     = 20;                         // 20×20 channel chip
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

// ── Status icons (16×16, two 1bpp layers from icons_net.h) ───────────────────
// The 'dim' layer is the muted base shape (e.g. the wired cable), the 'main'
// layer is the active overlay (e.g. the red X, the green check). Both are
// composited over the framebuffer so the icon sits on any backdrop.
constexpr int kIconSize = 16;

void draw_icon_layer(int x, int y, const uint8_t* alpha, Color fg);

// ── Status strip ─────────────────────────────────────────────────────────────
// A dynamically-composed row of header indicators, packed left-to-right from a
// start x inside the header band. Only active services push an item, so the
// strip grows and shrinks with the live config. An item is either a 16×16 icon
// mask (composited in a theme colour) or a short Body-face label — one small
// graphical component that keeps the header self-arranging.
struct StatusStrip {
    int x;       // running pen position (left edge of the next item)
    int band_h;  // header band height (items are vertically centred in it)

    void icon(const uint8_t* alpha, Color fg) {
        draw_icon_layer(x, (band_h - kIconSize) / 2, alpha, fg);
        x += kIconSize + kGap;
    }
    void label(const char* s, Color fg) {
        text_body(x, 0, band_h, s, fg);
        x += body_w(s) + kGap;
    }

    static constexpr int kGap = 7;  // spacing between consecutive items
};

// Network-state icon: one per NetState, themed to the design palette.
//   Disconnected → red X over a dim grey cable
//   Acquiring     → dim grey cable + orange dots
//   Connected     → green cable
//   Error         → orange "?" over a dim grey cable
void draw_net_icon(int x, int y, ui::NetState st);

// Data-flow icon: idle / receiving / transmitting / both. The 'main' layer is
// the active arrow (bright), the 'dim' layer is the inactive one (muted).
enum class DataFlow : uint8_t { None, Receive, Transmit, Both };

void draw_data_icon(int x, int y, DataFlow f);

// Channel-number badge: a rounded square filled with the wiring-family colour
// when the channel is in use, or a hollow 2px ring of that colour when it is
// Off. The digit is the homogeneous Body face — big enough to fill the square.
// `behind` is the row tint the AA edges and the hollow centre blend into.
//
// Body figures don't fill their 9×16 advance cell: the ink box spans roughly
// cols 0–6 / rows 2–11 (caps hug the baseline at row 12, the descender rows stay
// blank), so centring on the advance drifts the digit up and to the left. Offset
// the draw origin so the digit's *ink* box lands on the badge centre instead.
void draw_chan_badge(int x, int y, int side, int number, Color family, bool filled, Color behind);

// Edit-screen hint bar: green keycaps + dim actions, three segments (design
// Hints "TURN · PRESS · HOLD"). Drawn flush to the bottom over a top hairline.
void draw_hint_bar(const char* v_turn, const char* v_press, const char* v_hold);
#endif

// Shared screen header: dark bar + signature-green accent line + title.
void draw_tft_header(const char* title, Color title_col = color::Cream);

// Small-font status capsule over a solid backdrop; returns the x past it.
constexpr int kPillH = 14;

int pill_width(const char* txt);

int draw_pill(int x, int y, const char* txt, Color bg, Color fg, Color behind);

// Channel-number badge: AA rounded square with the digit drawn on a transparent
// background so its ink composites over the badge and never squares off the
// rounded corners.
[[maybe_unused]] void draw_badge(int x, int y, int side, int number, Color badge_col, Color num_col,
                                 Color behind);

// Right-aligned standard-scale text; returns the x where the text starts.
[[maybe_unused]] int draw_text_r(int x_end, int y, const char* txt, Color fg, Color bg);

// Packet counters outgrow their column fast; keep them to ≤6 glyphs.
[[maybe_unused]] void fmt_count(char* out, size_t cap, uint64_t v);
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
    Stats,  // "nerd stats" telemetry page
    EditValue,
    EditString,
    EditIp,
    EditUni,
    PixelRefresh,  // full-screen RGBW flash, see render_pixel_refresh()
};

// ── Menu tree nodes ─────────────────────────────────────────────────────────
// Every scrolling list is a node. The engine handles cursor, scroll, enter and
// back (via each node's parent) generically, so navigation carries no hardcoded
// cursor indices. Indexed by value into kNodes[].
enum class NodeId : uint8_t {
    Main,
    Show,      // master / blackout / strobe, what plays, signal loss
    Looks,     // the scenes to edit
    Rig,       // the outputs, their frame rate, the test patterns
    Dmx,       // patch, control universe, auto-patch
    Box,       // network, settings, about
    Network,   // IP addressing, identity, DMX reception
    Settings,  // the box itself: screen, speaker, nerd stats
    Channel,   // one output: LED hardware, dead pixels, fixtures
    Scenes,
    Fseq,
    TestPattern,
    Gaps,
    Control,      // DMX control universe
    ControlSlot,  // one slot of it
    Fixtures,     // the open channel's fixtures
    Fixture,      // one of them
    SceneList,    // scenes to edit (Scenes above plays them)
    SceneEdit,    // one scene: name, group, parts
    ScenePart,    // one part: outputs, effect, fixture mode, direction
    PartOutputs,  // its outputs, one toggle each
    PatchList,    // the outputs, for their DMX patch
    OutputPatch,  // one output's: pixel map, layout, universe, address, profiles
    Count,
};

// Forward declarations: build_* lambdas wire these as click actions.
void go(NodeId n);
void go_back();
void open_channel(uint8_t idx);

extern uint8_t g_ctl_slot;
extern char g_ctl_slot_title[12];
void save_control(const config::ControlConfig& c);

// What the fixture and scene editors have open, and their dynamic titles.
extern uint8_t g_fix_index;
extern uint8_t g_scene_index;
extern uint8_t g_part_index;
extern char g_fix_title[12];
extern char g_scene_title[12];
extern char g_part_title[12];
extern char g_patch_title[12];
// First LED past the strip of `cc`, dead LEDs included (physical, 0-based).
uint16_t strip_end(const config::ChannelConfig& cc);
// Short names of the DMX layouts and the fixture modes (value column).
const char* packing_label(uint8_t packing);
const char* fix_mode_label(uint8_t mode);
// The effect's name ("Effect 3" when it has none, "?" past the bank), and the
// group picker's ("Outputs" for 0, else group v-1's name).
void effect_label(size_t index, char* out, size_t cap);
void group_label(int32_t v, char* out, size_t cap);

extern uint8_t g_gap_index;  // gap being edited (== gap_count for a new one)
void preview_gap_edit(uint16_t pos0, uint16_t len);
void enter_gap_len_edit();

extern std::atomic<bool> g_speaker;  // ui::set_speaker_present()

// ── Editable fields ─────────────────────────────────────────────────────────
enum class Field : uint8_t {
    None,
    ArtnetReplyUnicast,
    ArtnetSacn,
    ArtnetFpp,
    ArtnetFailsafeMode,
    ArtnetFailsafeTimeout,
    GlobalRefresh,
    SpeakerVolume,
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    DisplayBrightness,
    DisplayIdleDim,
    DisplayDimDelay,
    DisplayPixelRefresh,
#endif
    NetworkDhcp,
    NetworkIpFallback,
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
    ChGapPos,  // first dead LED of gap s.gap_index (1-based); chains to ChGapLen
    ChGapLen,  // its length; 0 removes the gap
    ChPacking,
    ChPixelMap,    // pixel mapping on / off
    ChFixtureCtl,  // fixtures on their DMX profiles on / off
    ChFixDmx,
    FixFirst,  // fixture g_fix_index of the open channel: first LED (1-based)
    FixLen,
    FixReversed,
    FixProfile,
    FixSplit,    // replace the list with N equal fixtures over the strip
    SceneGroup,  // scene g_scene_index: 0 = its outputs, g + 1 = a fixture group
    PartEffect,  // part g_part_index of it
    PartMode,
    PartReverse,
    AutoPatch,
    ShowMaster,
    ShowStrobe,
    ShowFade,
    CtlEnabled,
    CtlUniverse,
    CtlAddress,
    CtlPreset,
    CtlSlotFn,
    CtlSlotMask,
    CtlSlotIndex,
    CtlSlotFine,
};

enum class StringField : uint8_t {
    ArtnetShort,
    ArtnetLong,
    SceneName,  // scene g_scene_index
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
    ClockHz,     // clocked-SPI rate, picked from kClockChoices (achievable divisors)
    CtlFn,       // control-universe function
    Preset,      // control-universe preset (Simple / Full)
    Mask,        // outputs bitmask, shown as "1234----"
    Tenths,      // tenths shown as "2.5s"
    Percent,     // a level shown as "64%"
    IpFallback,  // address without a DHCP server (link-local / Art-Net)
    Packing,     // DMX layout of an output
    FixMode,     // each / strip / chain / mirror
    Effect,      // an effect of the bank, by name
    Group,       // "Outputs" or a fixture group, by name
    Profile,     // a fixture DMX profile, by name
};

// An output's brightness (0..255) as the % the menus show, and back; the
// round trip keeps every percent.
inline uint8_t brightness_pct(uint8_t b) {
    return static_cast<uint8_t>((b * 100u + 127u) / 255u);
}
inline uint8_t brightness_from_pct(int32_t pct) {
    return static_cast<uint8_t>((pct * 255 + 50) / 100);
}

// Pick-from-a-list kinds (wheel) vs numeric ones (gauge).
bool is_enum_kind(ValueKind k);
bool is_gauge_kind(ValueKind k);

// TFT labels of the control functions, indexed by config::CtlFn.
const char* ctl_fn_label(uint8_t fn);

// "12------" style: one digit per output in the mask.
void format_mask(uint8_t mask, char* out, size_t cap);

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
    bool fixtures        = false;  // the output's fixture address, not its pixels'
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
    uint32_t refresh_until_ms = 0;  // Screen::PixelRefresh deadline, 0 = not running
};

extern State s;

// Cached FSEQ file list — populated when entering FSeqMenu.
constexpr uint8_t kFseqMenuMaxFiles = 8;
extern char g_fseq_names[kFseqMenuMaxFiles][fseq::kMaxNameLen];
extern uint8_t g_fseq_file_count;

extern RotationAccel g_accel;

void accel_reset();

// Call on every rotation detent; returns the step multiplier to apply.
int32_t accel_note_rotation(Event e);

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

int char_to_idx(char c);

char idx_to_char(int i);

// ── Helpers: enum → name ────────────────────────────────────────────────────

const char* protocol_name(led::Protocol p);

// Channel badge colour mirrors the wiring family: clocked SPI strips stand out
// from the common NRZ pixels; a disabled channel is greyed out.
Color badge_color(led::Protocol p);

const char* color_order_name(led::ColorOrder o);

void truncate(char* dst, size_t cap, const char* src);

const char* failsafe_name(uint8_t m);

// Clocked-SPI rates the 16 MHz bus can actually realise (f = 16 MHz / spc, spc
// even): 8/4/2/1 MHz are the clean integer-divisor steps (spc 2/4/8/16). The
// picker offers only these so every shown value maps exactly to itself instead
// of being silently rounded by led::timing_for. APA102/SK9822 ≤30 MHz, LPD8806
// ≤20 MHz per the datasheets, so all four are in spec.
constexpr int32_t kClockChoices[] = { 1'000'000, 2'000'000, 4'000'000, 8'000'000 };
constexpr int kClockChoiceCount   = static_cast<int>(sizeof(kClockChoices) /
                                                     sizeof(kClockChoices[0]));

// Index of the choice nearest to `hz` (legacy/odd values snap to a real rate).
int clock_choice_index(int32_t hz);

void format_clock_mhz(int32_t hz, char* out, size_t cap);

void format_value(const EditCtx& e, int32_t v, char* out, size_t cap);

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

// Widest node: the scene list (every scene + Stop + Back). Build buffers size
// to it; they are static (ui_task only) so the bigger list costs no stack.
constexpr uint8_t kMaxRows = config::kMaxControlSlots + 8;  // the longest list: DMX CONTROL

// A "return to the parent menu" row. Rendered with a left back-arrow glyph
// instead of bracketed text; `label` lets Main say "HOME" and the test-pattern
// node say "Stop & back".
ListItem back_item(const char* label = "Back");

// PIXEL REFRESH duration bounds (TFT): the item edits g_pixel_refresh_s, the
// last duration picked, so reopening it offers it again. Deliberately not in
// GlobalConfig: this is a bench tool, not a setting worth an NVS write.
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
constexpr int32_t kPixelRefreshMinS = 5;
constexpr int32_t kPixelRefreshMaxS = 300;
constexpr int32_t kPixelRefreshDefS = 30;
extern int32_t g_pixel_refresh_s;
#endif

// Channel node title ("CHANNEL N"), filled by build_channel each frame.
extern char g_channel_title[16];

// ── Defined across menu*.cpp ─────────────────────────────────────────────────
Color fade_gray(int d);
void render_list(const char* title, const ListItem* items, uint8_t count, uint8_t cursor);
void render_home();
void render_pixel_refresh();
void dispatch_pixel_refresh(Event e);
void render_about();
void dispatch_about(Event e);
void render_stats();
void dispatch_stats(Event e);
uint8_t build_main(ListItem* items, OnClick* fns);
uint8_t build_testpattern(ListItem* items, OnClick* fns);
uint8_t build_scenes(ListItem* items, OnClick* fns);
uint8_t build_fseq(ListItem* items, OnClick* fns);
uint8_t build_control(ListItem* items, OnClick* fns);
uint8_t build_control_slot(ListItem* items, OnClick* fns);
uint8_t build_show(ListItem* items, OnClick* fns);
uint8_t build_looks(ListItem* items, OnClick* fns);
uint8_t build_rig(ListItem* items, OnClick* fns);
uint8_t build_dmx(ListItem* items, OnClick* fns);
uint8_t build_box(ListItem* items, OnClick* fns);
uint8_t build_patch_list(ListItem* items, OnClick* fns);
uint8_t build_output_patch(ListItem* items, OnClick* fns);
uint8_t build_settings(ListItem* items, OnClick* fns);
uint8_t build_network(ListItem* items, OnClick* fns);
uint8_t build_channel(ListItem* items, OnClick* fns);
void preview_gap_edit(uint16_t pos0, uint16_t len);
void enter_gap_len_edit();
uint8_t build_gaps(ListItem* items, OnClick* fns);
uint8_t build_fixtures(ListItem* items, OnClick* fns);
uint8_t build_fixture(ListItem* items, OnClick* fns);
uint8_t build_scene_list(ListItem* items, OnClick* fns);
uint8_t build_scene_edit(ListItem* items, OnClick* fns);
uint8_t build_scene_part(ListItem* items, OnClick* fns);
uint8_t build_part_outputs(ListItem* items, OnClick* fns);
void enter_edit(Field field, ValueKind kind, int32_t cur, int32_t mn, int32_t mx, int32_t step,
                const char* label, Screen return_screen, uint8_t channel = 0xFF);
void render_edit_value();
void dispatch_edit_value(Event e);
void enter_edit_string(StringField field, const char* source, uint8_t max_len, const char* label,
                       Screen return_screen);
void render_edit_string();
void dispatch_edit_string(Event e);
void format_uni(char* buf, size_t cap, uint16_t v);
void enter_edit_ip(IpField field, uint32_t current, const char* label, Screen return_screen);
void render_edit_ip();
void dispatch_edit_ip(Event e);
void enter_edit_uni(uint8_t channel, uint16_t current, Screen return_screen, bool fixtures = false);
void render_edit_uni();
void dispatch_edit_uni(Event e);
void go(NodeId n);
void go_back();
void open_channel(uint8_t idx);
void engine_render();
void engine_dispatch(Event e);
void dispatch_long_press();

}  // namespace pixfrog::ui::detail::menu_impl
