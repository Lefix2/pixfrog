// The UI's hardware layer and task loop: the display drivers (SSD1306 over
// I2C, NV3007 / ST7789 over SPI), the seesaw encoder and its NeoPixel, the
// LEDC backlight, and ui.cpp's task (splash → menu loop, idle timeout, dim
// wake). Built once per display Kconfig — harness_ui_oled, _nv3007,
// _nv3007_rot180, _st7789 — against device models on the I2C/SPI shims.

#include <cstring>
#include <vector>

#include "config_store.h"
#include "harness.h"
#include "menu_accel.h"
#include "menu_fake.h"
#include "shim_control.h"
#include "ui.h"
#include "ui_internal.h"

using namespace pixfrog;
using ui::detail::Event;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint8_t kEncAddr  = 0x36;
constexpr uint8_t kOledAddr = 0x3C;

// Adafruit 4991: seesaw with the encoder module, a switch on GPIO 24 and one
// NeoPixel. Register reads are addressed by a 2-byte write first.
struct Seesaw : shim::I2cDevice {
    int32_t pos  = 0;
    bool pressed = false;
    bool nack    = false;
    uint8_t reg[2]{};
    std::vector<Bytes> writes;  // register writes (with a payload)
    uint8_t r = 0, g = 0, b = 0;
    int shows = 0;

    bool write(const uint8_t* d, size_t n) override {
        if (nack) return false;
        if (n == 2) {
            reg[0] = d[0];
            reg[1] = d[1];
            if (d[0] == 0x0E && d[1] == 0x05) ++shows;  // NEOPIXEL_SHOW
            return true;
        }
        writes.emplace_back(d, d + n);
        if (n == 7 && d[0] == 0x0E && d[1] == 0x04) {  // NEOPIXEL_BUF: offset, G, R, B
            g = d[4];
            r = d[5];
            b = d[6];
        }
        return true;
    }
    bool read(uint8_t* d, size_t n) override {
        if (nack || n != 4) return false;
        if (reg[0] == 0x11 && reg[1] == 0x30) {  // ENCODER_POSITION, signed BE
            const auto u = static_cast<uint32_t>(pos);
            d[0]         = static_cast<uint8_t>(u >> 24);
            d[1]         = static_cast<uint8_t>(u >> 16);
            d[2]         = static_cast<uint8_t>(u >> 8);
            d[3]         = static_cast<uint8_t>(u);
            return true;
        }
        if (reg[0] == 0x01 && reg[1] == 0x04) {  // GPIO_BULK: pin 24 = bit 0 of byte 0
            std::memset(d, 0xFF, 4);
            if (pressed) d[0] &= 0xFE;  // pull-up, switch to GND
            return true;
        }
        return false;
    }
    bool wrote(uint8_t module, uint8_t fn) const {
        for (const auto& w : writes)
            if (w[0] == module && w[1] == fn) return true;
        return false;
    }
};

// SSD1306 in horizontal addressing: 0x00-prefixed commands, 0x40-prefixed
// page data after a {B0|page, 00, 10} cursor command.
struct Ssd1306 : shim::I2cDevice {
    uint8_t ram[8][128];
    int page       = 0;
    int page_data  = 0;  // data transfers received
    int init_cmds  = 0;
    bool nack      = false;
    bool nack_data = false;
    Ssd1306() { std::memset(ram, 0xA5, sizeof(ram)); }
    bool write(const uint8_t* d, size_t n) override {
        if (nack) return false;
        if (d[0] == 0x00) {
            if (n == 4 && (d[1] & 0xF8) == 0xB0)
                page = d[1] & 7;
            else
                init_cmds += static_cast<int>(n - 1);
            return true;
        }
        if (d[0] == 0x40 && n == 129) {
            if (nack_data) return false;
            std::memcpy(ram[page], d + 1, 128);
            ++page_data;
            return true;
        }
        return false;
    }
    bool read(uint8_t*, size_t) override { return false; }
    bool page_blank(int p) const {
        for (int x = 0; x < 128; ++x)
            if (ram[p][x]) return false;
        return true;
    }
};

Seesaw g_enc;
Ssd1306 g_oled;

ui::InitConfig ui_cfg() {
    ui::InitConfig c{};
    c.i2c_port      = 0;
    c.i2c_sda_gpio  = 7;
    c.i2c_scl_gpio  = 8;
    c.i2c_freq_hz   = 400000;
    c.encoder_addr  = kEncAddr;
    c.oled_addr     = kOledAddr;
    c.spi_host      = 1;
    c.spi_clk_gpio  = 0;
    c.spi_mosi_gpio = 6;
    c.spi_cs_gpio   = 20;
    c.tft_dc_gpio   = 21;
    c.tft_rst_gpio  = 27;
    c.spi_freq_hz   = 20000000;
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
    c.tft_width  = 428;
    c.tft_height = 142;
#else
    c.tft_width  = 320;
    c.tft_height = 240;
#endif
    c.tft_backlight_gpio = 45;
    return c;
}

i2c_master_bus_handle_t new_bus() {
    i2c_master_bus_config_t c{};
    i2c_master_bus_handle_t bus = nullptr;
    i2c_new_master_bus(&c, &bus);
    return bus;
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        once = true;
    }
    shim::faults_clear();
    shim::lcd_reset();
    shim::ledc_reset();
    shim::tasks_forget();
    g_enc  = Seesaw{};
    g_oled = Ssd1306{};
    shim::i2c_attach(kEncAddr, &g_enc);
    shim::i2c_attach(kOledAddr, &g_oled);
    fake::menu() = fake::Menu{};
}

// Drains every queued encoder event.
std::vector<Event> poll_all() {
    std::vector<Event> out;
    for (Event e; (e = ui::detail::encoder_poll()) != Event::None;)
        out.push_back(e);
    return out;
}

size_t count(const std::vector<Event>& v, Event e) {
    size_t n = 0;
    for (Event x : v)
        n += x == e;
    return n;
}

}  // namespace

// ── Seesaw encoder ──────────────────────────────────────────────────────────

TEST(encoder_init_sets_the_switch_pin_up_as_a_pulled_up_input) {
    EXPECT_FALSE(ui::detail::encoder_init(nullptr, kEncAddr));
    auto* bus = new_bus();
    shim::fail_next(shim::Fault::I2cAddDevice);
    EXPECT_FALSE(ui::detail::encoder_init(bus, kEncAddr));
    g_enc.nack = true;
    EXPECT_FALSE(ui::detail::encoder_init(bus, kEncAddr));
    g_enc.nack = false;
    g_enc.pos  = 41;
    EXPECT_TRUE(ui::detail::encoder_init(bus, kEncAddr));
    EXPECT_TRUE(g_enc.wrote(0x01, 0x03));                       // DIRCLR: input
    EXPECT_TRUE(g_enc.wrote(0x01, 0x0B));                       // PULLENSET
    EXPECT_TRUE(g_enc.wrote(0x01, 0x05));                       // BULK_SET: bias the pull-up
    const Bytes mask = { 0x01, 0x03, 0x01, 0x00, 0x00, 0x00 };  // 1 << 24, big-endian
    EXPECT_TRUE(g_enc.writes[0] == mask);
    EXPECT_TRUE(poll_all().empty());  // the start position is not a rotation
}

TEST(rotation_becomes_one_event_per_detent) {
    EXPECT_TRUE(ui::detail::encoder_init(new_bus(), kEncAddr));
    g_enc.pos += 3;
    auto ev    = poll_all();
    EXPECT_EQ(ev.size(), 3u);
    EXPECT_EQ(count(ev, Event::RotateRight), 3u);
    g_enc.pos -= 2;
    ev         = poll_all();
    EXPECT_EQ(count(ev, Event::RotateLeft), 2u);
    EXPECT_EQ(ev.size(), 2u);
    // Negative positions are sign-extended: 1 → -5 is six steps left.
    g_enc.pos = -5;
    ev        = poll_all();
    EXPECT_EQ(count(ev, Event::RotateLeft), 6u);
    // A burst is clamped, and the 8-slot ring keeps the newest 7.
    g_enc.pos += 40;
    ev         = poll_all();
    EXPECT_EQ(ev.size(), 7u);
    EXPECT_EQ(count(ev, Event::RotateRight), 7u);
}

TEST(press_release_is_a_click_holding_is_one_long_press) {
    EXPECT_TRUE(ui::detail::encoder_init(new_bus(), kEncAddr));
    shim::advance_ms(100);
    g_enc.pressed = true;
    EXPECT_TRUE(poll_all().empty());  // down: nothing yet
    shim::advance_ms(100);
    g_enc.pressed = false;
    auto ev       = poll_all();
    EXPECT_EQ(ev.size(), 1u);
    EXPECT_TRUE(!ev.empty() && ev[0] == Event::Click);

    // Bounce: a transition within 20 ms of the last one is ignored.
    g_enc.pressed = true;
    shim::advance_ms(5);
    EXPECT_TRUE(poll_all().empty());
    g_enc.pressed = false;
    EXPECT_TRUE(poll_all().empty());

    // Hold: LongPress once past 600 ms, and the release makes no Click.
    shim::advance_ms(50);
    g_enc.pressed = true;
    EXPECT_TRUE(poll_all().empty());
    shim::advance_ms(300);
    EXPECT_TRUE(poll_all().empty());
    shim::advance_ms(400);
    ev = poll_all();
    EXPECT_EQ(ev.size(), 1u);
    EXPECT_TRUE(!ev.empty() && ev[0] == Event::LongPress);
    shim::advance_ms(1000);
    EXPECT_TRUE(poll_all().empty());  // still held: not again
    g_enc.pressed = false;
    shim::advance_ms(50);
    EXPECT_TRUE(poll_all().empty());  // the release is swallowed
}

TEST(a_failed_read_yields_no_event) {
    EXPECT_TRUE(ui::detail::encoder_init(new_bus(), kEncAddr));
    g_enc.pos += 2;
    shim::fail_next(shim::Fault::I2cTransmit);  // position address phase
    shim::fail_next(shim::Fault::I2cReceive);   // button data phase
    EXPECT_TRUE(poll_all().empty());
    EXPECT_EQ(poll_all().size(), 2u);  // the next poll catches up
}

TEST(the_neopixel_breathes_on_home_and_holds_green_in_config) {
    EXPECT_TRUE(ui::detail::encoder_init(new_bus(), kEncAddr));
    ui::detail::encoder_led_tick();  // before init: nothing
    EXPECT_EQ(g_enc.shows, 0);
    g_enc.nack = true;
    ui::detail::encoder_led_init();  // NeoPixel pin refused: LED stays off
    g_enc.nack = false;
    ui::detail::encoder_led_tick();
    EXPECT_EQ(g_enc.shows, 0);

    ui::detail::encoder_led_init();
    EXPECT_TRUE(g_enc.wrote(0x0E, 0x01));  // pin
    EXPECT_TRUE(g_enc.wrote(0x0E, 0x03));  // buffer length
    EXPECT_EQ(g_enc.shows, 1);
    // Breathing: dim and green-dominant, rising then falling (~6 s period).
    uint8_t peak = 0;
    for (int i = 0; i < 180; ++i) {
        ui::detail::encoder_led_tick();
        if (g_enc.g > peak) peak = g_enc.g;
        EXPECT_TRUE(g_enc.g >= g_enc.r && g_enc.g >= g_enc.b);
    }
    EXPECT_TRUE(peak > 40 && peak < 80);  // ceiling ~31 %
    ui::detail::encoder_led_flash();      // on HOME: no flash…
    ui::detail::encoder_led_tick();
    EXPECT_TRUE(g_enc.r < 30);
    ui::detail::encoder_led_flash();  // …not even one carried into config
    ui::detail::encoder_led_set_active(true);
    ui::detail::encoder_led_tick();
    EXPECT_TRUE(g_enc.r < 30);
    ui::detail::encoder_led_set_active(false);
    ui::detail::encoder_led_tick();

    // Config: ramps to full green within ~0.5 s, a flash blips yellow.
    ui::detail::encoder_led_set_active(true);
    for (int i = 0; i < 20; ++i)
        ui::detail::encoder_led_tick();
    EXPECT_EQ(g_enc.g, 0xC8);
    EXPECT_EQ(g_enc.r, 0x18);
    ui::detail::encoder_led_flash();
    ui::detail::encoder_led_tick();
    EXPECT_TRUE(g_enc.r > 0xC0);  // mostly yellow
    for (int i = 0; i < 20; ++i)
        ui::detail::encoder_led_tick();
    EXPECT_EQ(g_enc.r, 0x18);  // faded back
    const int shows = g_enc.shows;
    ui::detail::encoder_led_tick();  // unchanged colour: no I2C traffic
    EXPECT_EQ(g_enc.shows, shows);
    ui::detail::encoder_led_set_active(false);
    ui::detail::encoder_led_tick();
    EXPECT_TRUE(g_enc.g < 0xC8);  // back to breathing
}

#ifndef CONFIG_PIXFROG_DISPLAY_TFT
// ── SSD1306 OLED ────────────────────────────────────────────────────────────

TEST(oled_init_waits_for_the_panel_then_blanks_it) {
    EXPECT_FALSE(ui::detail::oled_init(nullptr, kOledAddr));
    auto* bus = new_bus();
    shim::fail_next(shim::Fault::I2cAddDevice);
    EXPECT_FALSE(ui::detail::oled_init(bus, kOledAddr));
    // Absent: 20 probes 20 ms apart, then give up.
    shim::i2c_attach(kOledAddr, nullptr);
    const int64_t t0 = shim::now_us();
    EXPECT_FALSE(ui::detail::oled_init(bus, kOledAddr));
    EXPECT_TRUE(shim::now_us() - t0 >= 380000);
    shim::i2c_attach(kOledAddr, &g_oled);
    // The cold-boot NACK: a few failed probes, then the panel answers.
    shim::i2c_nack_probes(3);
    g_oled.nack = true;  // …but rejects the init sequence
    EXPECT_FALSE(ui::detail::oled_init(bus, kOledAddr));
    g_oled.nack = false;
    shim::i2c_nack_probes(3);
    EXPECT_TRUE(ui::detail::oled_init(bus, kOledAddr));
    EXPECT_TRUE(g_oled.init_cmds > 20);
    EXPECT_EQ(g_oled.page_data, 8);  // undefined GDDRAM: every page pushed
    for (int p = 0; p < 8; ++p)
        EXPECT_TRUE(g_oled.page_blank(p));
}

TEST(oled_flush_pushes_only_the_pages_that_changed) {
    EXPECT_TRUE(ui::detail::oled_init(new_bus(), kOledAddr));
    g_oled.page_data = 0;
    ui::detail::canvas_draw_text(6, 16, "AB", ui::detail::color::White);  // row 2, col 1
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.page_data, 1);
    EXPECT_FALSE(g_oled.page_blank(2));
    EXPECT_EQ(g_oled.ram[2][0], 0);  // column 0 is col 0's cell, left blank
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.page_data, 1);                                      // unchanged: zero bytes
    ui::detail::canvas_draw_text(0, 24, "X", ui::detail::color::Black);  // black ink: no-op
    ui::detail::oled_draw_text(8, 0, "out");                             // past the last row
    ui::detail::canvas_draw_text(0, 0, nullptr, ui::detail::color::White);
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.page_data, 1);
    // Pixels: bounds-checked, bit y&7 of page y/8.
    ui::detail::oled_set_pixel(-1, 0, true);
    ui::detail::oled_set_pixel(128, 0, true);
    ui::detail::oled_set_pixel(0, 64, true);
    ui::detail::oled_set_pixel(5, 63, true);
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.ram[7][5], 0x80);
    ui::detail::oled_set_pixel(5, 63, false);
    // A failed page transfer is retried on the next flush.
    g_oled.nack_data = true;
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.ram[7][5], 0x80);
    g_oled.nack_data = false;
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.ram[7][5], 0);
    g_oled.nack = true;  // the cursor command itself fails
    ui::detail::oled_set_pixel(1, 1, true);
    ui::detail::canvas_flush();
    g_oled.nack = false;
    ui::detail::canvas_flush();
    EXPECT_EQ(g_oled.ram[0][1], 0x02);
    // A long line is clipped to 21 columns.
    ui::detail::oled_draw_text(3, 18, "abcdefgh");
    ui::detail::canvas_flush();
    EXPECT_FALSE(g_oled.page_blank(3));
    // The 1 bpp canvas has no shapes; it still answers its size.
    EXPECT_EQ(ui::detail::canvas_width(), 128);
    EXPECT_EQ(ui::detail::canvas_height(), 64);
    ui::detail::canvas_fill_rect(0, 0, 10, 10, ui::detail::color::White);
    ui::detail::canvas_hline(0, 0, 10, ui::detail::color::White);
    ui::detail::canvas_vline(0, 0, 10, ui::detail::color::White);
    ui::detail::canvas_fill_round_rect(0, 0, 10, 10, 2, ui::detail::color::White);
    ui::detail::canvas_draw_mask(0, 0, 8, 8, nullptr, ui::detail::color::White);
    ui::detail::canvas_invalidate();
    ui::detail::canvas_clear();
    ui::detail::canvas_flush();
    for (int p = 0; p < 8; ++p)
        EXPECT_TRUE(g_oled.page_blank(p));
}

TEST(oled_splash_shows_the_frog_until_a_click_or_timeout) {
    EXPECT_TRUE(ui::detail::oled_init(new_bus(), kOledAddr));
    EXPECT_FALSE(ui::detail::splash_render(0, false));
    EXPECT_FALSE(g_oled.page_blank(0));  // the frog mark
    EXPECT_TRUE(ui::detail::splash_render(10, true));
    EXPECT_TRUE(ui::detail::splash_render(60000, false));
}
#endif

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
// ── LEDC backlight backend ──────────────────────────────────────────────────

TEST(backlight_pwm_is_gamma_squared_with_a_floor) {
    using ui::detail::backlight_backend_init;
    using ui::detail::backlight_backend_set;
    backlight_backend_init(-1);  // hard-wired: nothing to drive
    backlight_backend_set(50, false);
    EXPECT_EQ(shim::ledc_log().steps, 0);
    shim::fail_next(shim::Fault::LedcTimer);
    backlight_backend_init(45);
    backlight_backend_set(50, false);
    EXPECT_EQ(shim::ledc_log().steps, 0);
    shim::fail_next(shim::Fault::LedcChannel);
    backlight_backend_init(45);
    backlight_backend_set(50, false);
    EXPECT_EQ(shim::ledc_log().steps, 0);

    backlight_backend_init(45);
    const auto& l = shim::ledc_log();
    EXPECT_EQ(l.gpio, 45);
    EXPECT_EQ(l.freq_hz, 20000u);  // out of the audio band
    EXPECT_EQ(l.duty, 0u);         // dark until the first frame
    backlight_backend_set(100, false);
    EXPECT_EQ(l.duty, 1023u);  // statically high
    EXPECT_EQ(l.fade_stops, 1);
    backlight_backend_set(50, false);
    EXPECT_EQ(l.duty, 255u);  // 50² / 100² of full
    backlight_backend_set(1, false);
    EXPECT_EQ(l.duty, 10u);  // the 1 % floor
    backlight_backend_set(0, false);
    EXPECT_EQ(l.duty, 0u);
    backlight_backend_set(200, true);
    EXPECT_EQ(l.duty, 1023u);
    EXPECT_EQ(l.fades, 1);
    EXPECT_EQ(l.last_fade_ms, 400u);

    shim::ledc_reset();
    shim::fail_next(shim::Fault::LedcFade);  // no fade service: steps instead
    backlight_backend_init(45);
    backlight_backend_set(80, true);
    EXPECT_EQ(shim::ledc_log().fades, 0);
    EXPECT_EQ(shim::ledc_log().fade_stops, 0);
    EXPECT_EQ(shim::ledc_log().duty, 654u);
}

#endif

#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
// ── NV3007 (landscape canvas, transposed into a portrait GRAM) ──────────────

TEST(nv3007_init_validates_then_wakes_and_blanks_the_panel) {
    auto c   = ui_cfg();
    auto tft = ui::detail::TftConfig{ c.spi_host,
                                      c.spi_clk_gpio,
                                      c.spi_mosi_gpio,
                                      c.spi_cs_gpio,
                                      c.tft_dc_gpio,
                                      c.tft_rst_gpio,
                                      c.spi_freq_hz,
                                      320,
                                      240,
                                      -1 };
    EXPECT_FALSE(ui::detail::tft_init(tft));  // not 428×142
    tft.width  = 428;
    tft.height = 142;
    shim::fail_next(shim::Fault::Semaphore);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::fail_next(shim::Fault::SpiBus);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::fail_next(shim::Fault::LcdNewIo);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::lcd_reset();
    EXPECT_TRUE(ui::detail::tft_init(tft));
    const auto& l = shim::lcd_log();
    EXPECT_EQ(shim::gpio_level(c.tft_rst_gpio), 1u);            // out of reset
    EXPECT_TRUE(!l.commands.empty() && l.commands[0] == 0xFF);  // page unlock first
    bool slpout = false, dispon_last = false;
    for (int cmd : l.commands)
        slpout |= cmd == 0x11;
    dispon_last = !l.commands.empty() && l.commands.back() == 0x29;
    EXPECT_TRUE(slpout);
    EXPECT_TRUE(dispon_last);  // display on only after GRAM is black
    for (int y = 0; y < 428; y += 7)
        for (int x = 12; x < 12 + 142; x += 5)
            EXPECT_EQ(l.at(x, y), 0);
    EXPECT_EQ(l.at(11, 0), 0xA5A5);  // the glass starts at source 12
    EXPECT_EQ(ui::detail::tft_width(), 428);
    EXPECT_EQ(ui::detail::tft_height(), 142);
    EXPECT_TRUE(ui::detail::tft_fb_alloc(64) != nullptr);
    shim::lcd_reset();
    tft.rst_gpio = -1;  // no reset line: straight to the init sequence
    EXPECT_TRUE(ui::detail::tft_init(tft));
}

TEST(nv3007_landscape_pixels_land_at_their_portrait_address) {
    auto c   = ui_cfg();
    auto tft = ui::detail::TftConfig{ c.spi_host,
                                      c.spi_clk_gpio,
                                      c.spi_mosi_gpio,
                                      c.spi_cs_gpio,
                                      c.tft_dc_gpio,
                                      -1,
                                      c.spi_freq_hz,
                                      428,
                                      142,
                                      -1 };
    EXPECT_TRUE(ui::detail::tft_init(tft));
    // A 7×40 landscape block (taller than one 32-row transpose chunk).
    const int x1 = 100, y1 = 50, w = 7, h = 40;
    std::vector<uint16_t> px(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px[y * w + x] = static_cast<uint16_t>(((y1 + y) << 9) | (x1 + x));
    ui::detail::tft_draw_bitmap(x1, y1, x1 + w, y1 + h, px.data());
    const auto& l = shim::lcd_log();
    int wrong     = 0;
    for (int y = y1; y < y1 + h; ++y)
        for (int x = x1; x < x1 + w; ++x) {
#ifdef CONFIG_PIXFROG_NV3007_ROT180
            const int nx = y, ny = 427 - x;  // landscape (x,y) → native (y, 427-x)
#else
            const int nx = 141 - y, ny = x;  // landscape (x,y) → native (141-y, x)
#endif
            wrong += l.at(12 + nx, ny) != static_cast<uint16_t>((y << 9) | x);
        }
    EXPECT_EQ(wrong, 0);
    const int tx = l.color_tx;
    ui::detail::tft_draw_bitmap(5, 5, 5, 9, px.data());  // empty: nothing sent
    EXPECT_EQ(l.color_tx, tx);
}
#endif

#if defined(CONFIG_PIXFROG_DISPLAY_TFT) && !defined(CONFIG_PIXFROG_DISPLAY_NV3007)
// ── ST7789 (esp_lcd vendor panel) ───────────────────────────────────────────

TEST(st7789_init_orients_and_blanks_before_display_on) {
    auto c   = ui_cfg();
    auto tft = ui::detail::TftConfig{ c.spi_host,
                                      c.spi_clk_gpio,
                                      c.spi_mosi_gpio,
                                      c.spi_cs_gpio,
                                      c.tft_dc_gpio,
                                      c.tft_rst_gpio,
                                      c.spi_freq_hz,
                                      320,
                                      240,
                                      -1 };
    shim::fail_next(shim::Fault::SpiBus);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::fail_next(shim::Fault::LcdNewIo);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::lcd_reset();
    shim::fail_next(shim::Fault::LcdNewPanel);
    EXPECT_FALSE(ui::detail::tft_init(tft));
    shim::lcd_reset();
    EXPECT_TRUE(ui::detail::tft_init(tft));
    const auto& l = shim::lcd_log();
    EXPECT_TRUE(l.swap_xy);  // portrait glass, landscape canvas
    EXPECT_TRUE(l.mirror_x);
    EXPECT_FALSE(l.mirror_y);
    EXPECT_FALSE(l.inverted);
    EXPECT_TRUE(l.display_on);
    EXPECT_EQ(l.draws, 15);  // 240 rows black in 16-row bands
    EXPECT_EQ(l.last_y2, 240);
    std::vector<uint16_t> px(4 * 2, 0x1234);
    ui::detail::tft_draw_bitmap(10, 20, 14, 22, px.data());
    EXPECT_EQ(l.draws, 16);
    EXPECT_EQ(l.last_x1, 10);
    EXPECT_EQ(ui::detail::tft_width(), 320);
    EXPECT_EQ(ui::detail::tft_height(), 240);
    EXPECT_TRUE(ui::detail::tft_fb_alloc(64) != nullptr);
}
#endif

// ── ui.cpp: start, task loop, platform hooks ────────────────────────────────
// Last: the driver cases above rely on first-init state (the NV3007 blacks its
// GRAM from a still-zero .bss staging buffer, as at boot).

TEST(start_fails_cleanly_on_each_missing_piece) {
    shim::fail_next(shim::Fault::I2cBus);
    EXPECT_FALSE(ui::start(ui_cfg()));
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    shim::fail_next(shim::Fault::SpiBus);
#else
    shim::i2c_attach(kOledAddr, nullptr);  // no OLED on the bus
#endif
    EXPECT_FALSE(ui::start(ui_cfg()));
    shim::i2c_attach(kOledAddr, &g_oled);
    shim::lcd_reset();
    g_enc.nack = true;
    EXPECT_FALSE(ui::start(ui_cfg()));
    EXPECT_FALSE(shim::task_created("ui"));
}

namespace {
int g_step = 0;
void ui_script() {
    ++g_step;
    if (g_step == 5) g_enc.pressed = true;  // click: skips the splash
    if (g_step == 7) g_enc.pressed = false;
    if (g_step == 12) g_enc.pos += 2;  // in the menu loop
    if (g_step == 14) fake::menu().home = false;
    if (g_step == 20) {
        g_enc.pressed = true;
    }
    if (g_step == 22) g_enc.pressed = false;
}
}  // namespace

TEST(the_task_plays_the_splash_then_runs_the_menu) {
    auto g           = config::get_global();
    g.home_timeout_s = 2;
    config::set_global(g);
    EXPECT_TRUE(ui::start(ui_cfg()));
    EXPECT_TRUE(shim::task_created("ui"));
    g_step = 0;
    EXPECT_TRUE(shim::run_task_for("ui", 120, ui_script));
    const auto& m = fake::menu();
    EXPECT_TRUE(m.inited);
    EXPECT_TRUE(m.renders > 50);
    // The splash click is not dispatched: only the rotation and the later click.
    EXPECT_EQ(count(m.events, Event::RotateRight), 2u);
    EXPECT_EQ(count(m.events, Event::Click), 1u);
    EXPECT_EQ(m.timeouts, 1);  // 2 s without input, once
    EXPECT_EQ(g_enc.g, 0xC8);  // out of HOME: the LED holds full green
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    EXPECT_TRUE(shim::ledc_log().duty > 0);  // backlight on after the first frame
#else
    EXPECT_FALSE(g_oled.page_blank(0));  // the menu text reached the panel
#endif
    g.home_timeout_s = 30;
    config::set_global(g);
}

TEST(platform_hooks_and_network_state) {
    shim::advance_ms(1234);
    EXPECT_TRUE(ui::detail::now_ms() >= 1234u);
    EXPECT_STREQ(ui::detail::fw_version(), "v0.0.0-host");
    EXPECT_TRUE(std::strstr(ui::detail::fw_build_info(), "IDF") != nullptr);
    EXPECT_TRUE(std::strstr(ui::detail::fw_build_info(), "2026") != nullptr);
    ui::set_ip(0xC0A80232);
    EXPECT_EQ(ui::get_ip(), 0xC0A80232u);
    ui::set_link_up(true);
    EXPECT_TRUE(ui::is_link_up());
    ui::set_link_up(false);
    EXPECT_FALSE(ui::is_link_up());
    ui::set_net_state(ui::NetState::Acquiring);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Acquiring);
}

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
namespace {
int g_dim_step      = 0;
uint32_t g_min_duty = 0;
void dim_script() {
    ++g_dim_step;
    const uint32_t d = shim::ledc_log().duty;
    if (g_dim_step > 10 && d < g_min_duty) g_min_duty = d;
    if (g_dim_step == 3) g_enc.pressed = true;  // skip the splash
    if (g_dim_step == 5) g_enc.pressed = false;
    if (g_dim_step == 100) g_enc.pos += 1;  // dimmed by now: only wakes
    if (g_dim_step == 110) g_enc.pos += 1;  // awake: reaches the menu
}
}  // namespace

TEST(the_ui_dims_when_idle_and_the_first_input_only_wakes_it) {
    auto g            = config::get_global();
    g.tft_dim_delay_s = 2;
    g.tft_idle_dim    = 50;
    config::set_global(g);
    EXPECT_TRUE(ui::start(ui_cfg()));
    // Re-init zeroed the PWM channel behind the policy's back (once per boot on
    // the device): resync it to full before the run.
    ui::detail::backlight_preview(99);
    ui::detail::backlight_preview_end();
    EXPECT_EQ(shim::ledc_log().duty, 1023u);
    g_dim_step = 0;
    g_min_duty = 1023;
    EXPECT_TRUE(shim::run_task_for("ui", 120, dim_script));
    EXPECT_EQ(g_min_duty, 255u);  // 50 % attenuation of full, after 2 s idle
    EXPECT_EQ(shim::ledc_log().duty, 1023u);
    EXPECT_FALSE(ui::detail::backlight_is_dimmed());                // woken by step 100
    EXPECT_EQ(count(fake::menu().events, Event::RotateRight), 1u);  // not the waking one
    g.tft_dim_delay_s = 0;
    g.tft_idle_dim    = 0;
    config::set_global(g);
}
#endif

// ── Rotation acceleration (menu_accel.h) ────────────────────────────────────

TEST(rotation_accelerates_then_a_reversal_or_a_pause_steps_back) {
    ui::detail::RotationAccel a;
    uint32_t t = 1000;
    int32_t m  = 0;
    for (int i = 0; i < 9; ++i, t += 50)
        m = a.note(t, true);
    EXPECT_EQ(m, 1);
    EXPECT_EQ(a.note(t += 50, true), 10);  // 10th detent in a row
    for (int i = 0; i < 25; ++i)
        m = a.note(t += 50, true);
    EXPECT_EQ(m, 10);
    EXPECT_EQ(a.note(t += 50, true), 100);  // 36th
    // A reversal is a correction: fine steps back over the overshoot.
    EXPECT_EQ(a.note(t += 50, false), 1);
    // ×100 needs close detents: a 200 ms hesitation drops to ×10, not ×1…
    for (int i = 0; i < 40; ++i)
        m = a.note(t += 50, false);
    EXPECT_EQ(m, 100);
    EXPECT_EQ(a.note(t += 200, false), 10);
    // …and a real pause (> 350 ms) to ×1.
    EXPECT_EQ(a.note(t += 400, false), 1);
    a.reset();
    EXPECT_EQ(a.note(t += 10, false), 1);  // a reset forgets the streak
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
