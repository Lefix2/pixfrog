// control_console — line-oriented command server on the UART0 console port
// (reachable through the board's USB-UART bridge, /dev/ttyACM0 @ 115200).
//
// Lets an attached host (human or AI agent, e.g. pyserial on /dev/ttyACM0)
// fully drive the device for bench tests: read/write every GlobalConfig and
// ChannelConfig field, read live telemetry, inject DMX data without ArtNet,
// read back universe/pixel buffers, switch calibration patterns, reboot.
//
// Machine protocol: every command answers with `key=value` lines followed by
// a terminating `OK` line, or a single `ERR <reason>` line. ESP_LOG output is
// interleaved on the same port; send `loglevel none` first for strict parsing.
//
// Commands run in the console REPL task. Config writes share ui_task's
// NVS path and are not locked against it — don't turn the encoder while a
// script is configuring the device.

#pragma once

namespace pixfrog::console {

// Spawn the REPL task. Call once, after config/dmx/lcd/ui/artnet are up.
void start();

}  // namespace pixfrog::console
