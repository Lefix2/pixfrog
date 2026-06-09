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

constexpr uint16_t kOpDmx       = 0x5000;
constexpr uint16_t kOpPoll      = 0x2000;
constexpr uint16_t kOpPollReply = 0x2100;
constexpr uint16_t kOpSync      = 0x5200;

constexpr size_t kPollReplySize = 239;

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
};

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
            pkt[174 + p] = 0x80;  // PortType: output, DMX512
            pkt[182 + p] = 0x80;  // GoodOutputA: transmitting
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
