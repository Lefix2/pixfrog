// artnet_parser — pure (header-only) helpers for Art-Net packet parsing
// and ArtPollReply building. No IDF / lwIP / config_store deps; safe to
// include from host-side unit tests.
//
// All multi-byte fields follow the Art-Net 4 spec wire layout:
//   Opcode (offset 8..9)         — little endian
//   Port (offset 14..15)         — little endian (0x36, 0x19 = 6454)
//   IP (offset 10..13, 207..210) — byte-sequenced a.b.c.d
//   Net/Subnet (18, 19)          — high 7 bits / low 4 bits of universe
//   NumPorts (172..173)          — big endian (always 0x00, 0x04 here)
//   Per-port arrays (174..193)   — one byte per port, 4 max

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pixfrog::artnet::parser {

constexpr char kArtnetId[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

constexpr uint16_t kOpDmx         = 0x5000;
constexpr uint16_t kOpNzs         = 0x5100;
constexpr uint16_t kOpPoll        = 0x2000;
constexpr uint16_t kOpPollReply   = 0x2100;
constexpr uint16_t kOpSync        = 0x5200;
constexpr uint16_t kOpAddress     = 0x6000;
constexpr uint16_t kOpCommand     = 0x2400;
constexpr uint16_t kOpTrigger     = 0x9900;
constexpr uint16_t kOpTimeCode    = 0x9700;
constexpr uint16_t kOpIpProg      = 0xF800;
constexpr uint16_t kOpIpProgReply = 0xF900;

constexpr size_t kPollReplySize   = 239;
constexpr size_t kIpProgReplySize = 34;

// ── Header ──────────────────────────────────────────────────────────────────

inline bool parse_header(const uint8_t* buf, size_t len, uint16_t* opcode_out) {
    if (!buf || len < 12) return false;
    if (std::memcmp(buf, kArtnetId, 8) != 0) return false;
    if (opcode_out) {
        *opcode_out = static_cast<uint16_t>(buf[8]) | (static_cast<uint16_t>(buf[9]) << 8);
    }
    return true;
}

// ── ArtDmx body ─────────────────────────────────────────────────────────────

struct DmxFields {
    uint16_t universe;  // 15-bit (net << 8) | sub_uni
    uint16_t data_len;
    const uint8_t* data;  // points into the caller's buf
};

inline bool parse_dmx(const uint8_t* buf, size_t len, DmxFields* out) {
    if (!buf || len < 18) return false;
    const uint16_t sub_uni  = buf[14];
    const uint16_t net      = buf[15] & 0x7F;
    const uint16_t universe = (net << 8) | sub_uni;
    const uint16_t data_len = (static_cast<uint16_t>(buf[16]) << 8) | buf[17];
    if (18u + data_len > len) return false;
    if (out) {
        out->universe = universe;
        out->data_len = data_len;
        out->data     = buf + 18;
    }
    return true;
}

namespace detail {

inline void write_ip(uint8_t* dst, uint32_t host_ip) {
    dst[0] = static_cast<uint8_t>((host_ip >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((host_ip >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((host_ip >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(host_ip & 0xFF);
}

inline void copy_bounded(uint8_t* dst, size_t cap, const char* src) {
    if (!src) return;
    size_t n = 0;
    while (n < cap && src[n] != '\0') {
        dst[n] = static_cast<uint8_t>(src[n]);
        n++;
    }
}

}  // namespace detail

// ── ArtNzs body ─────────────────────────────────────────────────────────────
// Same layout as ArtDmx except byte 12 = Sequence, byte 13 = StartCode
// (ArtDmx has Physical there and an implicit start code 0).

struct NzsFields {
    uint16_t universe;
    uint8_t start_code;
    uint16_t data_len;
    const uint8_t* data;
};

inline bool parse_nzs(const uint8_t* buf, size_t len, NzsFields* out) {
    if (!buf || len < 18) return false;
    const uint16_t sub_uni  = buf[14];
    const uint16_t net      = buf[15] & 0x7F;
    const uint16_t data_len = (static_cast<uint16_t>(buf[16]) << 8) | buf[17];
    if (18u + data_len > len) return false;
    if (out) {
        out->universe   = (net << 8) | sub_uni;
        out->start_code = buf[13];
        out->data_len   = data_len;
        out->data       = buf + 18;
    }
    return true;
}

// ── ArtAddress body ─────────────────────────────────────────────────────────
// Remote programming from the controller. Spec semantics: a value is applied
// only when its bit 7 is set (program low bits); 0x00 means "no change".
// Names are applied when their first byte is non-zero.

constexpr size_t kAddressSize = 107;

// ArtAddress Command values (Art-Net 4 §6.3). The merge commands are
// per-port (low 2 bits select the port within the bind group).
constexpr uint8_t kAcNone        = 0x00;
constexpr uint8_t kAcCancelMerge = 0x01;
constexpr uint8_t kAcMergeLtp0   = 0x10;  // ..0x13
constexpr uint8_t kAcMergeHtp0   = 0x50;  // ..0x53

inline bool is_merge_ltp_command(uint8_t command) {
    return (command & 0xFC) == kAcMergeLtp0;
}
inline bool is_merge_htp_command(uint8_t command) {
    return (command & 0xFC) == kAcMergeHtp0;
}

struct AddressFields {
    uint8_t net_switch;      // raw byte: 0x80|n programs n, 0x00 = no change
    uint8_t sub_switch;      // raw byte: 0x80|s programs s, 0x00 = no change
    uint8_t bind_index;      // 0/1 = root, 2.. = subsequent bind groups of 4 ports
    uint8_t sw_out[4];       // raw bytes: 0x80|u programs the port's low nibble
    uint8_t command;         // AcNone / AcCancelMerge / Ac* — caller decides
    const char* short_name;  // points into buf; NOT null-terminated past 18
    const char* long_name;   // points into buf; NOT null-terminated past 64
};

inline bool parse_address(const uint8_t* buf, size_t len, AddressFields* out) {
    if (!buf || len < kAddressSize) return false;
    if (out) {
        out->net_switch = buf[12];
        out->bind_index = buf[13];
        out->short_name = reinterpret_cast<const char*>(buf + 14);
        out->long_name  = reinterpret_cast<const char*>(buf + 32);
        for (int p = 0; p < 4; ++p)
            out->sw_out[p] = buf[100 + p];
        out->sub_switch = buf[104];
        out->command    = buf[106];
    }
    return true;
}

// ── ArtIpProg body + ArtIpProgReply builder ─────────────────────────────────
// Command bits: 7=enable programming, 6=enable DHCP, 4=reset to defaults,
// 3=program gateway, 2=program IP, 1=program subnet, 0=program port (dep.).

struct IpProgFields {
    uint8_t command;
    uint32_t prog_ip;    // host-order
    uint32_t prog_mask;  // host-order
    uint32_t prog_gw;    // host-order
};

inline uint32_t read_ip(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline bool parse_ip_prog(const uint8_t* buf, size_t len, IpProgFields* out) {
    if (!buf || len < 30) return false;
    if (out) {
        out->command   = buf[14];
        out->prog_ip   = read_ip(buf + 16);
        out->prog_mask = read_ip(buf + 20);
        out->prog_gw   = read_ip(buf + 26);
    }
    return true;
}

struct IpProgReplyInputs {
    uint32_t ip;    // host-order
    uint32_t mask;  // host-order
    uint32_t gw;    // host-order
    bool dhcp_enabled;
};

inline void build_ip_prog_reply(uint8_t pkt[kIpProgReplySize], const IpProgReplyInputs& in) {
    std::memset(pkt, 0, kIpProgReplySize);
    std::memcpy(pkt, kArtnetId, 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0xF9;
    pkt[10] = 0x00;
    pkt[11] = 14;  // ProtVer
    detail::write_ip(pkt + 16, in.ip);
    detail::write_ip(pkt + 20, in.mask);
    pkt[24] = 0x36;
    pkt[25] = 0x19;                           // port 6454 (deprecated field)
    pkt[26] = in.dhcp_enabled ? 0x40 : 0x00;  // Status: bit 6 = DHCP enabled
    detail::write_ip(pkt + 28, in.gw);
}

// ── ArtTimeCode body ────────────────────────────────────────────────────────
// Wall-clock timecode broadcast by the controller (Art-Net 4 §9.1). Used to
// slave FSEQ playback to a master desk/player.

constexpr size_t kTimeCodeSize = 19;

struct TimeCodeFields {
    uint8_t frames;   // 0..fps-1
    uint8_t seconds;  // 0..59
    uint8_t minutes;  // 0..59
    uint8_t hours;    // 0..23
    uint8_t type;     // 0=Film 24fps, 1=EBU 25fps, 2=DF 29.97fps, 3=SMPTE 30fps
};

inline bool parse_time_code(const uint8_t* buf, size_t len, TimeCodeFields* out) {
    if (!buf || len < kTimeCodeSize) return false;
    if (buf[18] > 3) return false;
    if (out) {
        out->frames  = buf[14];
        out->seconds = buf[15];
        out->minutes = buf[16];
        out->hours   = buf[17];
        out->type    = buf[18];
    }
    return true;
}

// Milliseconds since 00:00:00:00 represented by a timecode value.
inline uint32_t time_code_to_ms(const TimeCodeFields& tc) {
    // Frame duration in µs per type (29.97 DF = 30000/1001 fps ≈ 33.367 ms).
    static constexpr uint32_t kFrameUs[4] = { 41667u, 40000u, 33367u, 33333u };
    const uint32_t base_ms = (tc.hours * 3600u + tc.minutes * 60u + tc.seconds) * 1000u;
    return base_ms + tc.frames * kFrameUs[tc.type & 3] / 1000u;
}

// ── Net/subnet filter ───────────────────────────────────────────────────────

inline bool universe_matches(uint16_t universe, uint8_t our_net, uint8_t our_subnet) {
    return ((universe >> 8) & 0x7F) == (our_net & 0x7F) &&
           ((universe >> 4) & 0x0F) == (our_subnet & 0x0F);
}

// ── ArtPollReply builder ────────────────────────────────────────────────────

struct PollReplyInputs {
    uint32_t local_ip_host;   // host-order IPv4
    uint8_t artnet_net;       // 0..127
    uint8_t artnet_subnet;    // 0..15
    const char* short_name;   // <=18 chars
    const char* long_name;    // <=64 chars
    const char* node_report;  // <=64 chars, may be nullptr
    const uint8_t* mac;       // 6 bytes (must not be null)
    uint8_t bind_index;       // 1 for primary, 2.. for bound replies
    uint8_t sw_out[4];        // low 4 bits of each port's universe
    // A disabled (Off) channel advertises no port type and no output activity,
    // so controllers don't see it as a live DMX universe.
    bool port_enabled[4] = { true, true, true, true };
    // GoodOutputA merge reporting: bit 1 (merge mode is LTP, node-wide) and
    // bit 3 (this port is currently merging two sources).
    bool merge_ltp       = false;
    bool port_merging[4] = { false, false, false, false };
};

inline void build_poll_reply(uint8_t pkt[kPollReplySize], const PollReplyInputs& in) {
    std::memset(pkt, 0, kPollReplySize);

    std::memcpy(pkt, kArtnetId, 8);
    pkt[8] = 0x00;
    pkt[9] = 0x21;
    detail::write_ip(pkt + 10, in.local_ip_host);
    pkt[14] = 0x36;
    pkt[15] = 0x19;
    pkt[16] = 0x00;
    pkt[17] = 0x01;
    pkt[18] = in.artnet_net & 0x7F;
    pkt[19] = in.artnet_subnet & 0x0F;
    pkt[20] = 0x00;  // OemHi
    pkt[21] = 0x00;  // OemLo (unknown OEM)
    pkt[22] = 0x00;  // UbeaVersion
    pkt[23] = 0xD0;  // Status1
    pkt[24] = 0xFF;  // EstaMan lo
    pkt[25] = 0xFF;  // EstaMan hi (0xFFFF = unknown)

    detail::copy_bounded(pkt + 26, 18, in.short_name);
    detail::copy_bounded(pkt + 44, 64, in.long_name);
    detail::copy_bounded(pkt + 108, 64, in.node_report);

    pkt[172] = 0x00;
    pkt[173] = 0x04;
    for (uint8_t p = 0; p < 4; ++p) {
        if (in.port_enabled[p]) {
            pkt[174 + p] = 0x80;                   // PortType: output, DMX512
            uint8_t good = 0x80;                   // GoodOutputA: transmitting
            if (in.port_merging[p]) good |= 0x08;  // merging ArtNet data
            if (in.merge_ltp) good |= 0x02;        // merge mode is LTP
            pkt[182 + p] = good;
        }  // disabled: PortType/GoodOutput stay 0 (memset)
        pkt[190 + p] = in.sw_out[p] & 0x0F;  // SwOut low nibble
    }
    pkt[200] = 0x00;  // Style: StNode

    if (in.mac) std::memcpy(pkt + 201, in.mac, 6);
    detail::write_ip(pkt + 207, in.local_ip_host);
    pkt[211] = in.bind_index;
    pkt[212] = 0x08;  // Status2: DHCP capable
}

}  // namespace pixfrog::artnet::parser
