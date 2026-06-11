// Adafruit seesaw 4991 (I2C QT Rotary Encoder) — minimal client.
//
// Seesaw I2C protocol: each register is addressed by a 2-byte (module, function)
// tuple. Reads are split transactions with a short delay between write and read
// so the seesaw firmware has time to compute the response.
//
// Pinout for the 4991 board:
//   - Encoder A/B internal to the seesaw (no GPIO mapping needed)
//   - Push-button on seesaw GPIO 24, active LOW with pull-up
//   - INT_N: open-drain output, asserted (LOW) on any enabled int source.
//     Cleared by reading GPIO_INTFLAG (for GPIO ints) and ENCODER_DELTA
//     (for encoder ints).

#include "ui_internal.h"

#include <cstring>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "ENC";

// ── Seesaw register map ─────────────────────────────────────────────────────
constexpr uint8_t kSeeBaseGpio    = 0x01;
constexpr uint8_t kSeeBaseEncoder = 0x11;

constexpr uint8_t kSeeGpioDirClrBulk = 0x03;
constexpr uint8_t kSeeGpioBulk       = 0x04;
constexpr uint8_t kSeeGpioBulkSet    = 0x05;
constexpr uint8_t kSeeGpioIntEnSet   = 0x08;
constexpr uint8_t kSeeGpioIntFlag    = 0x0A;
constexpr uint8_t kSeeGpioPullEnSet  = 0x0B;

constexpr uint8_t kSeeEncoderIntEnSet = 0x10;
constexpr uint8_t kSeeEncoderPosition = 0x30;
constexpr uint8_t kSeeEncoderDelta    = 0x40;

// On-board NeoPixel (seesaw NEOPIXEL module). The 4991's single WS2812 is wired
// to seesaw GPIO 6 and is GRB-ordered.
constexpr uint8_t kSeeBaseNeopixel = 0x0E;
constexpr uint8_t kSeeNeoPin       = 0x01;
constexpr uint8_t kSeeNeoSpeed     = 0x02;
constexpr uint8_t kSeeNeoBufLen    = 0x03;
constexpr uint8_t kSeeNeoBuf       = 0x04;
constexpr uint8_t kSeeNeoShow      = 0x05;
constexpr uint8_t kSeeNeoPixelPin  = 6;

constexpr uint8_t kSeeSwitchPin   = 24;
constexpr uint32_t kSeeSwitchMask = 1u << kSeeSwitchPin;

// Microseconds between the write-address and read-data phases of a seesaw
// read. The firmware needs this long to populate the response buffer.
constexpr int kSeeReadDelayUs = 250;
constexpr int kI2cTimeoutMs   = 50;

i2c_master_dev_handle_t g_dev = nullptr;
int g_int_gpio                = -1;
int32_t g_last_pos            = 0;
bool g_last_btn               = false;

// Item A3: debounce for the push button. Any transition closer than this
// to the previous one is silently ignored, suppressing the multi-click
// echoes the seesaw can emit on a noisy mechanical switch contact.
constexpr int64_t kBtnDebounceUs = 20'000;  // 20 ms
int64_t g_last_btn_change_us     = 0;

// Long-press: holding past this threshold emits LongPress immediately (no
// need to release), exactly once per hold — the button must be released
// before any further event can fire. The release after a LongPress is
// swallowed so a single hold never also produces a Click; short presses
// emit Click on release.
constexpr int64_t kLongPressUs = 1'000'000;  // 1 s
int64_t g_press_start_us       = 0;
bool g_long_fired              = false;

// ── Pending event ring ──────────────────────────────────────────────────────
constexpr size_t kEvtQ = 8;
Event g_q[kEvtQ];
size_t g_qh = 0, g_qt = 0;

void push_evt(Event e) {
    g_q[g_qh] = e;
    g_qh      = (g_qh + 1) % kEvtQ;
    if (g_qh == g_qt) g_qt = (g_qt + 1) % kEvtQ;  // drop oldest on overflow
}
Event pop_evt() {
    if (g_qh == g_qt) return Event::None;
    Event e = g_q[g_qt];
    g_qt    = (g_qt + 1) % kEvtQ;
    return e;
}

// ── Low-level I2C helpers ───────────────────────────────────────────────────

bool seesaw_write(uint8_t module, uint8_t function, const uint8_t* data = nullptr, size_t len = 0) {
    if (!g_dev) return false;
    uint8_t buf[16];
    if (len > sizeof(buf) - 2) return false;
    buf[0] = module;
    buf[1] = function;
    if (data && len) std::memcpy(buf + 2, data, len);
    esp_err_t err = i2c_master_transmit(g_dev, buf, 2 + len, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write(%02X,%02X) failed: %s", module, function, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool seesaw_read(uint8_t module, uint8_t function, uint8_t* dst, size_t len) {
    if (!g_dev) return false;
    const uint8_t reg[2] = { module, function };
    esp_err_t err        = i2c_master_transmit(g_dev, reg, 2, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read addr(%02X,%02X) failed: %s", module, function, esp_err_to_name(err));
        return false;
    }
    // Seesaw needs time to populate its TX FIFO. esp_rom_delay_us is safe
    // to call from a task and doesn't yield the CPU.
    esp_rom_delay_us(kSeeReadDelayUs);
    err = i2c_master_receive(g_dev, dst, len, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read data(%02X,%02X) failed: %s", module, function, esp_err_to_name(err));
        return false;
    }
    return true;
}

void be32(uint32_t v, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

bool seesaw_read_position(int32_t& pos) {
    uint8_t buf[4];
    if (!seesaw_read(kSeeBaseEncoder, kSeeEncoderPosition, buf, 4)) return false;
    // Signed big-endian. Sign-extend the high byte through the int32.
    pos = (static_cast<int32_t>(static_cast<int8_t>(buf[0])) << 24) |
          (static_cast<int32_t>(buf[1]) << 16) | (static_cast<int32_t>(buf[2]) << 8) |
          static_cast<int32_t>(buf[3]);
    return true;
}

bool seesaw_read_button(bool& pressed) {
    uint8_t buf[4];
    if (!seesaw_read(kSeeBaseGpio, kSeeGpioBulk, buf, 4)) return false;
    // GPIO_BULK is a 4-byte BE bitmap of all 32 pins; pin 24 = bit 0 of buf[0].
    // Pull-up + switch to GND → released = 1, pressed = 0.
    pressed = (buf[0] & 0x01) == 0;
    return true;
}

// Reading INTFLAG / DELTA also clears the seesaw's internal interrupt
// latches, releasing the INT_N line so the next change can fire our GPIO ISR.
void seesaw_clear_ints() {
    uint8_t scratch[4];
    seesaw_read(kSeeBaseGpio, kSeeGpioIntFlag, scratch, 4);
    seesaw_read(kSeeBaseEncoder, kSeeEncoderDelta, scratch, 4);
}

// ── On-board NeoPixel feedback ──────────────────────────────────────────────
// Two modes, set from the UI loop:
//   • status screen (set_active(false)): the LED slowly breathes theme green.
//   • config (set_active(true)): it ramps to full green and holds; flash() blips
//     it to yellow on each action, crossfading back to green.
// encoder_led_tick() animates both at the UI loop's ~30 Hz so nothing steps.

struct Rgb {
    uint8_t r, g, b;
};

// Pure-ish green so it doesn't read teal; warm yellow for the config action flash.
constexpr Rgb kBreathGreen = { 0x18, 0xC8, 0x28 };  // (24, 200, 40)
constexpr Rgb kFlashYellow = { 0xFA, 0xC0, 0x14 };  // (250, 192, 20)

constexpr uint16_t kBreathInc = 364;  // phase / tick → ~6 s period (3 s up / 3 s down) at 30 Hz
constexpr uint8_t kBreathMin  = 6;    // breath floor
constexpr uint8_t kBreathMax  = 80;   // breath ceiling (~31%)
constexpr uint8_t kActiveLvl  = 255;  // config base intensity
constexpr uint8_t kRampStep   = 17;   // base / tick → ~0.5 s ramp to max at 30 Hz
constexpr uint8_t kFlashFade  = 16;   // flash / tick → ~0.5 s fade at 30 Hz

bool g_led_ok       = false;
bool g_led_active   = false;  // false = breathe (status screen), true = config
uint16_t g_led_ph   = 0;      // breathing phase, wraps at 65536
uint8_t g_led_base  = 0;      // config base level, ramps to kActiveLvl
uint8_t g_led_flash = 0;      // yellow flash level, decays to 0
uint8_t g_led_wr_r = 1, g_led_wr_g = 1, g_led_wr_b = 1;  // last bytes sent (forces 1st write)

void neopixel_show(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_led_ok) return;
    if (r == g_led_wr_r && g == g_led_wr_g && b == g_led_wr_b) return;
    g_led_wr_r = r;
    g_led_wr_g = g;
    g_led_wr_b = b;
    // BUF: 2-byte BE pixel offset (0) then GRB bytes for the one pixel.
    const uint8_t buf[5] = { 0x00, 0x00, g, r, b };
    seesaw_write(kSeeBaseNeopixel, kSeeNeoBuf, buf, sizeof(buf));
    seesaw_write(kSeeBaseNeopixel, kSeeNeoShow);
}

uint8_t scale8(uint8_t c, uint8_t lv) {
    return static_cast<uint8_t>(static_cast<uint16_t>(c) * lv / 255);
}
uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return static_cast<uint8_t>(a + (static_cast<int>(b) - a) * t / 255);
}

uint8_t breath_level() {
    const uint16_t tri = g_led_ph < 32768 ? g_led_ph : static_cast<uint16_t>(65535 - g_led_ph);
    const uint8_t t    = static_cast<uint8_t>(tri >> 7);  // 0..255
    return static_cast<uint8_t>(kBreathMin + (kBreathMax - kBreathMin) * t / 255);
}

void led_render() {
    const uint8_t lv = g_led_active ? g_led_base : breath_level();
    uint8_t r = scale8(kBreathGreen.r, lv), g = scale8(kBreathGreen.g, lv),
            b = scale8(kBreathGreen.b, lv);
    if (g_led_flash > 0) {  // crossfade green base → full yellow by flash level
        r = lerp8(r, kFlashYellow.r, g_led_flash);
        g = lerp8(g, kFlashYellow.g, g_led_flash);
        b = lerp8(b, kFlashYellow.b, g_led_flash);
    }
    neopixel_show(r, g, b);
}

}  // namespace

bool encoder_init(i2c_master_bus_handle_t bus, uint8_t addr, int int_gpio) {
    if (!bus) return false;
    g_int_gpio = int_gpio;

    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = 400'000;
    esp_err_t err           = i2c_master_bus_add_device(bus, &dev_cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_add_device: %s", esp_err_to_name(err));
        return false;
    }

    // Item 3: configure the switch pin and enable interrupts.
    uint8_t mask_be[4];
    be32(kSeeSwitchMask, mask_be);

    // 1. Switch pin = input (clear direction bit)
    if (!seesaw_write(kSeeBaseGpio, kSeeGpioDirClrBulk, mask_be, 4)) return false;
    // 2. Enable internal pull-up on the switch
    if (!seesaw_write(kSeeBaseGpio, kSeeGpioPullEnSet, mask_be, 4)) return false;
    // 3. Set the pin high to bias the pull-up (some seesaws need this explicit step)
    if (!seesaw_write(kSeeBaseGpio, kSeeGpioBulkSet, mask_be, 4)) return false;
    // 4. Enable GPIO interrupt on the switch pin
    if (!seesaw_write(kSeeBaseGpio, kSeeGpioIntEnSet, mask_be, 4)) return false;
    // 5. Enable encoder 0 interrupt
    uint8_t one = 0x01;
    if (!seesaw_write(kSeeBaseEncoder, kSeeEncoderIntEnSet, &one, 1)) return false;

    // Read initial state (also clears any latched interrupts).
    seesaw_read_position(g_last_pos);
    seesaw_read_button(g_last_btn);
    seesaw_clear_ints();

    ESP_LOGI(TAG, "seesaw init OK at 0x%02X, INT_N on GPIO %d, start pos=%ld", addr, int_gpio,
             static_cast<long>(g_last_pos));
    return true;
}

Event encoder_poll() {
    Event e = pop_evt();
    if (e != Event::None) return e;

    int32_t pos   = g_last_pos;
    bool btn      = g_last_btn;
    bool any_read = false;

    if (seesaw_read_position(pos)) {
        any_read            = true;
        const int32_t delta = pos - g_last_pos;
        g_last_pos          = pos;
        // Adafruit 4991 issues one tick per detent, but reads can occasionally
        // double-step on fast rotation. Clamp to a reasonable burst per poll.
        constexpr int32_t kMaxBurst = 16;
        int32_t d                   = delta;
        if (d > kMaxBurst) d = kMaxBurst;
        if (d < -kMaxBurst) d = -kMaxBurst;
        for (int32_t i = 0; i < d; ++i)
            push_evt(Event::RotateRight);
        for (int32_t i = 0; i < -d; ++i)
            push_evt(Event::RotateLeft);
    }

    if (seesaw_read_button(btn)) {
        any_read          = true;
        const int64_t now = esp_timer_get_time();
        if (btn != g_last_btn) {
            if (now - g_last_btn_change_us >= kBtnDebounceUs) {
                if (btn && !g_last_btn) {
                    // Press down: start the long-press timer.
                    g_press_start_us = now;
                    g_long_fired     = false;
                } else if (!btn && g_last_btn) {
                    // Release: short press = Click, unless LongPress already fired.
                    if (!g_long_fired) push_evt(Event::Click);
                }
                g_last_btn           = btn;
                g_last_btn_change_us = now;
            }
            // Else: bounce, ignore the transition; the next poll will see
            // either the stable new state or a return to old state.
        } else if (btn && !g_long_fired && now - g_press_start_us >= kLongPressUs) {
            push_evt(Event::LongPress);
            g_long_fired = true;
        }
    }

    if (any_read) seesaw_clear_ints();
    return pop_evt();
}

void encoder_led_init() {
    const uint8_t pin = kSeeNeoPixelPin;
    if (!seesaw_write(kSeeBaseNeopixel, kSeeNeoPin, &pin, 1)) {
        ESP_LOGW(TAG, "neopixel pin set failed");
        return;
    }
    const uint8_t speed = 1;  // 800 kHz
    seesaw_write(kSeeBaseNeopixel, kSeeNeoSpeed, &speed, 1);
    const uint8_t len_be[2] = { 0x00, 0x03 };  // 1 RGB pixel = 3 bytes
    if (!seesaw_write(kSeeBaseNeopixel, kSeeNeoBufLen, len_be, 2)) {
        ESP_LOGW(TAG, "neopixel buf-length set failed");
        return;
    }
    g_led_ok = true;
    led_render();
}

void encoder_led_set_active(bool active) {
    // Entering config: seed the ramp at the current breath level so there's no
    // dip before it climbs to full.
    if (active && !g_led_active) g_led_base = breath_level();
    g_led_active = active;
}

void encoder_led_flash() {
    if (g_led_active) g_led_flash = 255;  // yellow blip, config only
}

void encoder_led_tick() {
    if (!g_led_ok) return;
    g_led_ph = static_cast<uint16_t>(g_led_ph + kBreathInc);  // wraps at 65536
    if (g_led_active) {
        if (g_led_base < kActiveLvl)
            g_led_base = (kActiveLvl - g_led_base > kRampStep)
                           ? static_cast<uint8_t>(g_led_base + kRampStep)
                           : kActiveLvl;
    } else {
        g_led_base  = 0;
        g_led_flash = 0;  // no action flashes on the status screen
    }
    if (g_led_flash > 0)
        g_led_flash = (g_led_flash > kFlashFade) ? static_cast<uint8_t>(g_led_flash - kFlashFade)
                                                 : 0;
    led_render();
}

}  // namespace pixfrog::ui::detail
