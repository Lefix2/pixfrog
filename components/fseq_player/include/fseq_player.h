#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::fseq {

constexpr size_t kMaxNameLen = 64;
constexpr size_t kMaxFiles   = 32;

// GPIO configuration for the SDMMC 4-bit interface.
struct InitConfig {
    int clk_gpio;
    int cmd_gpio;
    int d0_gpio;
    int d1_gpio;
    int d2_gpio;
    int d3_gpio;
};

// Attempt to mount the microSD card.  Called once at boot.
// Returns true on success.  The FSEQ menu shows "No SD card" on failure.
bool init(const InitConfig& cfg);

// List .fseq files in the root directory of the SD card.
// Fills names[i] with null-terminated filenames (up to kMaxNameLen-1 chars).
// Returns the number of files found, capped at max.
size_t list_files(char names[][kMaxNameLen], size_t max);

// Start playing filename (null-terminated, 8.3 FAT name or short path).
// Stops any currently playing file first.
// Returns false if the file cannot be opened or parsed.
bool start(const char* filename);

// Stop playback.
void stop();

// Returns the filename currently playing, or nullptr if stopped.
const char* active_file();

enum class Status : uint8_t { Idle, Playing, Error };

Status      status();
const char* error_string(); // last error, or "" if none

} // namespace pixfrog::fseq
