// Who answers pixfrog.local when several boxes share the network.
//
// Every box is pixfrog-<last 4 hex of its MAC>.local; the shared alias
// pixfrog.local is published by exactly one of them. The web task browses
// _http._tcp (product=pixfrog) periodically, records every sibling it sees
// here, and holds the alias while no live sibling outranks it: a box marked
// "preferred hub" beats the others, then the lowest MAC wins. A sibling not
// seen for kPeerTtlMs is forgotten, so one lost browse answer does not make
// the alias flap, and a dead holder is replaced within a TTL.
// Pure logic, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pixfrog::web::hub {

constexpr const char* kAlias  = "pixfrog";
constexpr uint32_t kBrowseMs  = 15'000;  // browse period
constexpr uint32_t kPeerTtlMs = 45'000;  // three missed browses
constexpr size_t kMaxPeers    = 16;
constexpr size_t kHostnameMax = 16;  // "pixfrog-7a6f" + NUL

struct Candidate {
    uint8_t mac[6];
    bool preferred;
};

// a ranks before b for the alias.
inline bool outranks(const Candidate& a, const Candidate& b) {
    if (a.preferred != b.preferred) return a.preferred;
    return std::memcmp(a.mac, b.mac, 6) < 0;
}

// "pixfrog-7a6f" from the last two MAC bytes.
inline void hostname_for(const uint8_t mac[6], char* out, size_t cap) {
    std::snprintf(out, cap, "%s-%02x%02x", kAlias, mac[4], mac[5]);
}

// 12 hex digits (the "mac" TXT item), case-insensitive.
inline bool parse_mac(const char* s, uint8_t out[6]) {
    if (!s || std::strlen(s) != 12) return false;
    for (int i = 0; i < 6; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            unsigned d;
            if (c >= '0' && c <= '9')
                d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
            v = v * 16 + d;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

inline void format_mac(const uint8_t mac[6], char out[13]) {
    std::snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
}

// The siblings seen lately (self excluded by the caller or by `self` below).
class Roster {
  public:
    void saw(const Candidate& c, uint32_t now_ms) {
        Entry* slot = nullptr;
        for (auto& e : entries_)
            if (e.used && std::memcmp(e.c.mac, c.mac, 6) == 0) slot = &e;
        if (!slot)
            for (auto& e : entries_)
                if (!e.used) {
                    slot = &e;
                    break;
                }
        if (!slot) {  // full: replace the stalest
            slot = &entries_[0];
            for (auto& e : entries_)
                if (now_ms - e.seen_ms > now_ms - slot->seen_ms) slot = &e;
        }
        slot->used    = true;
        slot->c       = c;
        slot->seen_ms = now_ms;
    }

    // True while no sibling seen within the TTL outranks `self`.
    bool holds_alias(const Candidate& self, uint32_t now_ms) {
        expire(now_ms);
        for (const auto& e : entries_)
            if (e.used && std::memcmp(e.c.mac, self.mac, 6) != 0 && outranks(e.c, self))
                return false;
        return true;
    }

    // The candidate that holds the alias among self + live siblings.
    Candidate holder(const Candidate& self, uint32_t now_ms) {
        expire(now_ms);
        Candidate best = self;
        for (const auto& e : entries_)
            if (e.used && outranks(e.c, best)) best = e.c;
        return best;
    }

    size_t live(uint32_t now_ms) {
        expire(now_ms);
        size_t n = 0;
        for (const auto& e : entries_)
            n += e.used ? 1 : 0;
        return n;
    }

    void clear() {
        for (auto& e : entries_)
            e.used = false;
    }

  private:
    struct Entry {
        Candidate c;
        uint32_t seen_ms;
        bool used;
    };
    void expire(uint32_t now_ms) {
        for (auto& e : entries_)
            if (e.used && now_ms - e.seen_ms > kPeerTtlMs) e.used = false;
    }
    Entry entries_[kMaxPeers]{};
};

}  // namespace pixfrog::web::hub
