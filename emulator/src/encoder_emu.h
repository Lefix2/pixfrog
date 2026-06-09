// Emulator-facing input API for the host encoder backend (encoder_host.cpp).
// main_emulator.cpp pushes events here from the SDL keyboard handler and the
// stdin agent protocol; encoder_poll() (called by the UI loop) drains them.
//
// A separate EmuEvent enum keeps the emulator main free of the detail:: types.

#pragma once

enum class EmuEvent { RotateLeft, RotateRight, Click };

void emu_push_event(EmuEvent e);
