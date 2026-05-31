// Board definition: Waveshare ESP32-P4 Module DEV-KIT
// (datasheet:
// https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)
//
// Pinout source: Waveshare ESP32-P4 Module DEV-KIT schematic (P6 40-pin header).
// Only GPIOs exposed on P6 are candidates here.
//
// GPIO matrix capability: on ESP32-P4 every peripheral signal — including
// the 16 data lines of the LCD_CAM bus and PCLK — is routed through the
// GPIO matrix. Any free GPIO is therefore a valid LCD_CAM data line, so
// the only constraints on the assignment below are:
//   - strapping pins, latched at reset (ESP32-P4 datasheet §3.3):
//       GPIO 35 — boot/download select; LOW at reset = serial bootloader.
//                 Driven by the on-board BOOT button + auto-program circuit,
//                 and doubles as RMII TXD1 (see Ethernet). Not on P6.
//       GPIO 36 — must read HIGH for a reliable serial-download boot.
//       GPIO 34 — selects the JTAG signal source at boot.
//                 GPIO 37/38 are strappable too but serve as the UART0
//                 console here; latches free them once reset is released.
//                 None of the LED pins below land on a strapping pin.
//   - pins reserved for I2C / UART / Ethernet RMII elsewhere.
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
constexpr int kLedClockGpio[8] = { 3, 5, 23, 25, 46, 33, 48, 54 };

// Full 16-pin bus in bit order (DATA0, CLK0, DATA1, CLK1, ...).
// clang-format off
constexpr int kLedBusGpio[16] = {
     2,  3,   4,  5,  22, 23,  24, 25,
    26, 46,  32, 33,  47, 48,  53, 54,
};
// clang-format on

// ────────────────────────────────────────────────────────────────────────────
// Ethernet — RMII to on-board IP101GRI PHY
// ────────────────────────────────────────────────────────────────────────────
//
// Verified against the Waveshare DEV-KIT schematic (PHY U7 = IP101GRI) and
// the official board pinout. The PHY's RMII + management pins use fixed SoC
// GPIOs (28-35, 49-52) and PHY RESET is GPIO51; none are on the P6 header,
// so they do not constrain the LED pinout above.
//
// RMII data/clock equal the ESP32-P4 ETH_ESP32_EMAC_DEFAULT_CONFIG() defaults
// (IDF v5.5), so main/init_network() only has to override MDC/MDIO:
//   TXD0=34  TXD1=35  TX_EN=49  RXD0=29  RXD1=30  CRS_DV=28  REF_CLK(50M)=50
//
// Management bus + reset are read straight off U7:
//   MDC = pin 22, MDIO = pin 23, RESET = pin 32.

constexpr int kEthMdcGpio      = 31;
constexpr int kEthMdioGpio     = 52;
constexpr int kEthPhyResetGpio = 51;    // drive HIGH to release U7 from reset
constexpr int kEthPhyAddress   = 0x01;  // IP101GRI strap (AD0/AD3); verify if link fails

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
