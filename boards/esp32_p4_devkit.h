// Board definition: Espressif ESP32-P4 Function EV Board
// (datasheet:
// https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)
//
// Pinout source: official Function EV Board header connector pinout
// (only GPIOs exposed on the user-facing header are candidates here).
//
// GPIO matrix capability: on ESP32-P4 every peripheral signal — including
// the 16 data lines of the LCD_CAM bus and PCLK — is routed through the
// GPIO matrix. Any free GPIO is therefore a valid LCD_CAM data line, so
// the only constraints on the assignment below are:
//   - strapping pins to avoid at boot:
//       GPIO 0  — bootloader entry
//       GPIO 6  — download/normal mode select
//       GPIO 35 — SDIO mode (not on this connector anyway)
//   - pins reserved for I2C / UART / Ethernet RMII (internal) elsewhere.
//
// All assignments below sit on the exposed header and avoid the strapping
// pins above. Pin pairs are grouped logically for routing convenience.

#pragma once

#include <stdint.h>

namespace pixfrog::board {

// ────────────────────────────────────────────────────────────────────────────
// Identification
// ────────────────────────────────────────────────────────────────────────────

constexpr const char* kBoardName = "esp32_p4_devkit";
constexpr const char* kBoardRev  = "v0";

// ────────────────────────────────────────────────────────────────────────────
// LCD_CAM 16-bit parallel — DATA[k] and CLOCK[k] for 8 LED channels
// ────────────────────────────────────────────────────────────────────────────
//
// Bit position on the parallel bus → physical GPIO.
// Bit 0 = CH1 DATA, Bit 1 = CH1 CLOCK, Bit 2 = CH2 DATA, etc.

constexpr int kLedDataGpio[8]  = { 2, 4, 22, 24, 26, 32, 47, 53 };
constexpr int kLedClockGpio[8] = { 3, 5, 23, 25, 16, 33, 48, 54 };

// Full 16-pin bus in bit order (DATA0, CLK0, DATA1, CLK1, ...).
// clang-format off
constexpr int kLedBusGpio[16] = {
     2,  3,   4,  5,  22, 23,  24, 25,
    26, 16,  32, 33,  47, 48,  53, 54,
};
// clang-format on

// ────────────────────────────────────────────────────────────────────────────
// Ethernet — RMII to on-board IP101GRI PHY
// ────────────────────────────────────────────────────────────────────────────
//
// The PHY's RMII pins are wired directly to the SoC inside the Function
// EV Board and are NOT brought out to the header connector, so they do
// not constrain our LED pinout above.
//
// MDC, MDIO and PHY_RST below are the values used by the official
// Espressif Function EV Board reference build; revalidate against the
// board schematic before flashing on a different revision.

constexpr int kEthMdcGpio      = 29;
constexpr int kEthMdioGpio     = 30;
constexpr int kEthPhyResetGpio = -1;    // PHY has its own RC reset on the EV Board
constexpr int kEthPhyAddress   = 0x01;  // strap-determined on IP101GRI

// ────────────────────────────────────────────────────────────────────────────
// I2C — shared bus for OLED + seesaw encoder
// ────────────────────────────────────────────────────────────────────────────

constexpr int kI2cPort        = 0;
constexpr int kI2cSdaGpio     = 7;  // header pin SDA(GPIO7)
constexpr int kI2cSclGpio     = 8;  // header pin SCL(GPIO8)
constexpr uint32_t kI2cFreqHz = 400'000;

constexpr uint8_t kOledI2cAddr    = 0x3C;
constexpr uint8_t kEncoderI2cAddr = 0x36;  // Adafruit seesaw 4991 default
constexpr int kEncoderIntGpio     = 21;    // active-LOW IRQ from seesaw

// ────────────────────────────────────────────────────────────────────────────
// Miscellaneous
// ────────────────────────────────────────────────────────────────────────────

constexpr int kStatusLedGpio = 1;

// UART0 console (TXD/RXD) is on the dedicated pins GPIO 37 / GPIO 38
// printed on the header silkscreen. Configured by the IDF default
// console driver — not used directly from this board file.
constexpr int kDebugTxGpio = 37;
constexpr int kDebugRxGpio = 38;

}  // namespace pixfrog::board
