// Emulator-facing telemetry injection for the dmx_manager host stub.
// Lets the agent API drive the HOME dashboard (FPS, packet count, per-channel
// activity dots) without a real DMX engine.

#pragma once

#include <cstdint>

void emu_dmx_set_stats(uint32_t fps, uint64_t pkts);
void emu_dmx_set_active(int ch, bool on);
