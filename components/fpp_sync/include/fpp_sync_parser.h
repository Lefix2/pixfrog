// Pure parser for the FPP MultiSync control protocol (UDP 32320).
// Header-only and ESP-free so it can be unit-tested on the host.
//
// Wire format (FPP src/MultiSync.h, packed little-endian):
//   ControlPkt: char fppd[4]="FPPD", uint8 pktType, uint16 extraDataLen
//   SyncPkt   : uint8 action, uint8 fileType, uint32 frameNumber,
//               float secondsElapsed, char filename[] (NUL-terminated)

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cstring>

namespace pixfrog::fpp::parser {

constexpr uint16_t kPort              = 32320;
constexpr const char* kMulticastGroup = "239.70.80.80";

constexpr size_t kHeaderSize = 7;  // "FPPD" + pktType + extraDataLen
constexpr uint8_t kPktSync   = 1;

// SyncPkt.action
constexpr uint8_t kSyncStart = 0;
constexpr uint8_t kSyncStop  = 1;
constexpr uint8_t kSyncSync  = 2;
constexpr uint8_t kSyncOpen  = 3;

// SyncPkt.fileType
constexpr uint8_t kFileSeq   = 0;  // .fseq sequence
constexpr uint8_t kFileMedia = 1;  // audio/video — not handled here

constexpr size_t kMaxFileName = 64;
constexpr size_t kSyncMinSize = kHeaderSize + 10 + 1;  // fixed fields + NUL

struct SyncFields {
    uint8_t action;     // kSync*
    uint8_t file_type;  // kFile*
    uint32_t frame_number;
    float seconds_elapsed;
    char filename[kMaxFileName];  // NUL-terminated, truncated if longer
};

// Accepts only "FPPD" packets of type sync with a valid action; other FPP
// packet types (ping, blanking, …) return false and should just be ignored.
inline bool parse_sync(const uint8_t* buf, size_t len, SyncFields* out) {
    if (!buf || len < kSyncMinSize) return false;
    if (std::memcmp(buf, "FPPD", 4) != 0) return false;
    if (buf[4] != kPktSync) return false;
    if (buf[7] > kSyncOpen) return false;
    if (out) {
        out->action    = buf[7];
        out->file_type = buf[8];
        out->frame_number = static_cast<uint32_t>(buf[9]) | (static_cast<uint32_t>(buf[10]) << 8) |
                            (static_cast<uint32_t>(buf[11]) << 16) |
                            (static_cast<uint32_t>(buf[12]) << 24);
        std::memcpy(&out->seconds_elapsed, buf + 13, 4);  // float LE, host is LE too
        const size_t avail = len - 17;
        size_t n           = 0;
        while (n < avail && buf[17 + n] != 0)
            ++n;
        if (n >= sizeof(out->filename)) n = sizeof(out->filename) - 1;
        std::memcpy(out->filename, buf + 17, n);
        out->filename[n] = '\0';
    }
    return true;
}

}  // namespace pixfrog::fpp::parser
