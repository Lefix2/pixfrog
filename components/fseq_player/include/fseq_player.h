#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::fseq {

constexpr size_t kMaxNameLen = 64;
constexpr size_t kMaxFiles   = 32;

// VFS mount point of the SD card (root directory of the FSEQ files).
constexpr const char* kMountPath = "/sdcard";

// GPIO configuration for the SDMMC 4-bit interface.
struct InitConfig {
    int clk_gpio;
    int cmd_gpio;
    int d0_gpio;
    int d1_gpio;
    int d2_gpio;
    int d3_gpio;
};

// SD-card presence state, updated by the background monitor task.
enum class SdState : uint8_t { Absent, Mounted };

// Initialise the SDMMC host, attempt an immediate mount, and start the 1 s
// monitor task that handles hot-plug insertion and removal.
// Returns false only if the monitor task cannot be created (fatal).
bool init(const InitConfig& cfg);

// List .fseq files in the root directory of the SD card.
// Fills names[i] with null-terminated filenames (up to kMaxNameLen-1 chars).
// Returns the number of files found, capped at max.
size_t list_files(char names[][kMaxNameLen], size_t max);

// Start playing filename (null-terminated, 8.3 FAT name or short path).
// Stops any currently playing file first.
// Returns false if no SD card is present, or the file cannot be opened/parsed.
bool start(const char* filename);

// Stop playback.
void stop();

// Jump the running playback to the given time position (clamped to the
// sequence length). No-op when nothing is playing. The seek is consumed by
// the playback task at the next frame boundary (≤ one step_time late).
// Returns false if no playback is active.
bool seek_ms(uint32_t ms);

// Current playback position / total length, in milliseconds.
// Both return 0 when nothing is playing.
uint32_t position_ms();
uint32_t duration_ms();

// Returns the filename currently playing, or nullptr if stopped.
const char* active_file();

enum class Status : uint8_t { Idle, Playing, Error };

SdState sd_state();
Status status();
const char* error_string();  // last error, or "" if none

}  // namespace pixfrog::fseq
