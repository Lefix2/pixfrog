// sacn_parser — pure (header-only) helpers for sACN / E1.31 packet parsing.
// No IDF / lwIP / config_store deps; safe to include from host-side tests.
//
// E1.31 wire layout (data packet, offsets in bytes):
//   0-1    RLP preamble size      0x0010 (BE)
//   2-3    RLP postamble size     0x0000
//   4-15   ACN packet identifier  "ASC-E1.17\0\0\0"
//   16-17  root flags + length
//   18-21  root vector            0x00000004 data / 0x00000008 extended
//   22-37  CID (sender UUID)
//   38-39  framing flags + length
//   40-43  framing vector         0x00000002 data / 0x00000001 sync (extended)
//   44-107 source name (64, UTF-8, null-padded)
//   108    priority (0..200, default 100)
//   109-110 sync address (BE; 0 = unsynchronized)
//   111    sequence number
//   112    options: bit7 preview, bit6 stream_terminated, bit5 force_sync
//   113-114 universe (BE, 1..63999)
//   115-116 DMP flags + length
//   117    DMP vector             0x02
//   118    address & data type    0xA1
//   119-120 first property address 0x0000
//   121-122 address increment     0x0001
//   123-124 property value count  (BE, 1 + slots)
//   125    start code (property value 0)
//   126-   DMX slots (count - 1 bytes)

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pixfrog::sacn::parser {

constexpr uint16_t kSacnPort = 5568;

constexpr uint32_t kRootVectorData     = 0x00000004;
constexpr uint32_t kRootVectorExtended = 0x00000008;
constexpr uint32_t kFrameVectorData    = 0x00000002;
constexpr uint32_t kFrameVectorSync    = 0x00000001;

constexpr uint8_t kOptPreview    = 0x80;
constexpr uint8_t kOptTerminated = 0x40;
constexpr uint8_t kOptForceSync  = 0x20;

constexpr uint8_t kAcnId[12] = { 0x41, 0x53, 0x43, 0x2D, 0x45, 0x31,
                                 0x2E, 0x31, 0x37, 0x00, 0x00, 0x00 };

constexpr size_t kMinDataLen = 126;
constexpr size_t kMinSyncLen = 49;

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Root-layer check shared by data and sync parsing. Returns the root vector
// (0 on malformed packet).
inline uint32_t parse_root(const uint8_t* buf, size_t len) {
    if (!buf || len < 48) return 0;
    if (be16(buf) != 0x0010 || be16(buf + 2) != 0x0000) return 0;
    if (std::memcmp(buf + 4, kAcnId, sizeof(kAcnId)) != 0) return 0;
    return be32(buf + 18);
}

struct DataFields {
    uint16_t universe;  // 1..63999
    uint8_t priority;   // 0..200
    uint8_t sequence;
    uint8_t options;        // kOpt* bits
    uint16_t sync_address;  // 0 = unsynchronized
    uint8_t start_code;
    uint16_t data_len;    // DMX slots (excluding start code)
    const uint8_t* data;  // points into the caller's buf
    const uint8_t* cid;   // 16 bytes, points into the caller's buf
};

inline bool parse_data(const uint8_t* buf, size_t len, DataFields* out) {
    if (len < kMinDataLen) return false;
    if (parse_root(buf, len) != kRootVectorData) return false;
    if (be32(buf + 40) != kFrameVectorData) return false;
    if (buf[117] != 0x02 || buf[118] != 0xA1) return false;

    const uint16_t prop_count = be16(buf + 123);
    if (prop_count < 1 || prop_count > 513) return false;
    if (125u + prop_count > len) return false;

    const uint16_t universe = be16(buf + 113);
    if (universe < 1 || universe > 63999) return false;

    if (out) {
        out->universe     = universe;
        out->priority     = buf[108];
        out->sequence     = buf[111];
        out->options      = buf[112];
        out->sync_address = be16(buf + 109);
        out->start_code   = buf[125];
        out->data_len     = static_cast<uint16_t>(prop_count - 1);
        out->data         = buf + 126;
        out->cid          = buf + 22;
    }
    return true;
}

// E1.31 universe synchronization packet (root extended / framing sync).
// Returns true and the sync address when the packet is a valid sync frame.
inline bool parse_sync(const uint8_t* buf, size_t len, uint16_t* sync_address_out) {
    if (len < kMinSyncLen) return false;
    if (parse_root(buf, len) != kRootVectorExtended) return false;
    if (be32(buf + 40) != kFrameVectorSync) return false;
    if (sync_address_out) *sync_address_out = be16(buf + 45);
    return true;
}

// Multicast group for a universe: 239.255.hi.lo (E1.31 §9.3.1).
inline uint32_t multicast_group_host(uint16_t universe) {
    return (239u << 24) | (255u << 16) | (static_cast<uint32_t>(universe >> 8) << 8) |
           (universe & 0xFF);
}

// 32-bit FNV-1a of the sender CID — the nonzero merge-source key used by
// dmx::write_universe_from_source (0 is its free-slot sentinel).
inline uint32_t source_id_from_cid(const uint8_t cid[16]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) {
        h ^= cid[i];
        h *= 16777619u;
    }
    return h != 0 ? h : 1;
}

// ── Per-source gate (priority + sequence) ───────────────────────────────────
// E1.31 arbitration, tracked per (universe, source CID):
// - priority (§6.6): a packet passes when its priority is at least the
//   highest of the OTHER live sources on that universe, so a source may lower
//   its own priority without being shut out by its own past. Equal priority
//   passes through: the dmx_manager 2-source merge arbitrates there.
// - sequence (§6.7.2): per source, a packet 0..19 behind the last one is a
//   duplicate or out of order and is discarded; a larger step back is a
//   restarted sender and is taken.
// - a source silent for kSourceTimeoutMs is forgotten (§6.7.1).

constexpr uint32_t kSourceTimeoutMs = 2500;

struct SourceGate {
    uint32_t source_id = 0;  // CID hash (source_id_from_cid)
    uint32_t last_ms   = 0;
    uint16_t universe  = 0;
    uint8_t priority   = 0;
    uint8_t sequence   = 0;
    bool active        = false;
};

// Decide whether a data packet may be applied, updating the table (linear
// scan). `terminated` forgets that source on that universe. `*takeover` is set
// when the packet outranks live sources of that universe: the caller drops
// them from the downstream merge.
template <size_t N>
inline bool gate_accept(SourceGate (&gates)[N], uint16_t universe, uint32_t source_id,
                        uint8_t priority, uint8_t sequence, bool terminated, uint32_t now_ms,
                        bool* takeover = nullptr) {
    if (takeover) *takeover = false;
    SourceGate* self     = nullptr;
    SourceGate* free_one = nullptr;
    int others_max       = -1;  // highest priority of the other live sources
    for (auto& g : gates) {
        if (g.active && now_ms - g.last_ms > kSourceTimeoutMs) g.active = false;  // §6.7.1
        if (!g.active) {
            if (!free_one) free_one = &g;
            continue;
        }
        if (g.universe != universe) continue;
        if (g.source_id == source_id)
            self = &g;
        else if (g.priority > others_max)
            others_max = g.priority;
    }

    if (terminated) {
        if (self) self->active = false;
        return false;
    }
    if (self) {
        const int8_t step = static_cast<int8_t>(sequence - self->sequence);
        if (step <= 0 && step > -20) return false;  // duplicate / out of order
    } else {
        if (!free_one) {  // table full: reclaim the stalest entry
            free_one = &gates[0];
            for (auto& g : gates)
                if (g.last_ms < free_one->last_ms) free_one = &g;
        }
        self            = free_one;
        self->universe  = universe;
        self->source_id = source_id;
        self->active    = true;
    }
    self->priority = priority;
    self->sequence = sequence;
    self->last_ms  = now_ms;
    if (priority < others_max) return false;  // outranked (still tracked)
    if (takeover && priority > others_max && others_max >= 0) *takeover = true;
    return true;
}

}  // namespace pixfrog::sacn::parser
