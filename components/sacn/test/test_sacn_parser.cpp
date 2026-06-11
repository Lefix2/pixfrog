// Host-side unit tests for sacn::parser (header-only pure code).

#include <cstdio>
#include <cstring>
#include <vector>

#include "sacn_parser.h"

using namespace pixfrog::sacn::parser;

static int g_pass = 0, g_fail = 0;

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long va = static_cast<long long>(a);                                                  \
        long long vb = static_cast<long long>(b);                                                  \
        if (va == vb) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__, __LINE__, #a,  \
                         #b, va, vb);                                                              \
        }                                                                                          \
    } while (0)

#define EXPECT_TRUE(a)                                                                             \
    do {                                                                                           \
        if (a) {                                                                                   \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #a);                      \
        }                                                                                          \
    } while (0)

// ── Packet builders ─────────────────────────────────────────────────────────

static std::vector<uint8_t> make_data_packet(uint16_t universe, uint8_t priority,
                                             const uint8_t* slots, uint16_t slot_count,
                                             uint8_t options = 0, uint8_t start_code = 0) {
    std::vector<uint8_t> p(126 + slot_count, 0);
    p[0] = 0x00;
    p[1] = 0x10;  // preamble
    std::memcpy(p.data() + 4, kAcnId, sizeof(kAcnId));
    p[18] = 0x00;
    p[19] = 0x00;
    p[20] = 0x00;
    p[21] = 0x04;  // root vector: data
    for (int i = 0; i < 16; ++i)
        p[22 + i] = static_cast<uint8_t>(i);  // CID
    p[40] = 0x00;
    p[41] = 0x00;
    p[42] = 0x00;
    p[43] = 0x02;  // framing vector: data
    std::memcpy(p.data() + 44, "host-test", 9);
    p[108]                    = priority;
    p[111]                    = 1;  // sequence
    p[112]                    = options;
    p[113]                    = static_cast<uint8_t>(universe >> 8);
    p[114]                    = static_cast<uint8_t>(universe & 0xFF);
    p[117]                    = 0x02;  // DMP vector
    p[118]                    = 0xA1;  // addr & data type
    p[121]                    = 0x00;
    p[122]                    = 0x01;  // addr increment
    const uint16_t prop_count = static_cast<uint16_t>(1 + slot_count);
    p[123]                    = static_cast<uint8_t>(prop_count >> 8);
    p[124]                    = static_cast<uint8_t>(prop_count & 0xFF);
    p[125]                    = start_code;
    if (slots && slot_count) std::memcpy(p.data() + 126, slots, slot_count);
    return p;
}

static std::vector<uint8_t> make_sync_packet(uint16_t sync_address) {
    std::vector<uint8_t> p(49, 0);
    p[0] = 0x00;
    p[1] = 0x10;
    std::memcpy(p.data() + 4, kAcnId, sizeof(kAcnId));
    p[21] = 0x08;  // root vector: extended
    p[43] = 0x01;  // framing vector: sync
    p[45] = static_cast<uint8_t>(sync_address >> 8);
    p[46] = static_cast<uint8_t>(sync_address & 0xFF);
    return p;
}

// ── Data packet parsing ─────────────────────────────────────────────────────

static void test_parse_valid_data() {
    uint8_t slots[3] = { 0xFF, 0x01, 0x02 };
    auto p           = make_data_packet(7, 100, slots, 3);
    DataFields f{};
    EXPECT_TRUE(parse_data(p.data(), p.size(), &f));
    EXPECT_EQ(f.universe, 7);
    EXPECT_EQ(f.priority, 100);
    EXPECT_EQ(f.start_code, 0);
    EXPECT_EQ(f.data_len, 3);
    EXPECT_EQ(f.data[0], 0xFF);
    EXPECT_EQ(f.data[2], 0x02);
    EXPECT_EQ(f.options, 0);
}

static void test_parse_full_universe() {
    uint8_t slots[512];
    std::memset(slots, 0xAB, sizeof(slots));
    auto p = make_data_packet(63999, 200, slots, 512);
    DataFields f{};
    EXPECT_TRUE(parse_data(p.data(), p.size(), &f));
    EXPECT_EQ(f.universe, 63999);
    EXPECT_EQ(f.data_len, 512);
    EXPECT_EQ(f.data[511], 0xAB);
}

static void test_parse_rejects_bad_preamble() {
    uint8_t slots[1] = { 1 };
    auto p           = make_data_packet(1, 100, slots, 1);
    p[1]             = 0x11;
    EXPECT_TRUE(!parse_data(p.data(), p.size(), nullptr));
}

static void test_parse_rejects_bad_acn_id() {
    uint8_t slots[1] = { 1 };
    auto p           = make_data_packet(1, 100, slots, 1);
    p[4]             = 'X';
    EXPECT_TRUE(!parse_data(p.data(), p.size(), nullptr));
}

static void test_parse_rejects_universe_zero() {
    uint8_t slots[1] = { 1 };
    auto p           = make_data_packet(0, 100, slots, 1);
    EXPECT_TRUE(!parse_data(p.data(), p.size(), nullptr));
}

static void test_parse_rejects_truncated() {
    uint8_t slots[10] = {};
    auto p            = make_data_packet(1, 100, slots, 10);
    EXPECT_TRUE(!parse_data(p.data(), p.size() - 5, nullptr));  // count overruns buffer
    EXPECT_TRUE(!parse_data(p.data(), 100, nullptr));           // below minimum
}

static void test_parse_options_and_start_code() {
    uint8_t slots[1] = { 1 };
    auto p           = make_data_packet(1, 100, slots, 1, kOptPreview | kOptTerminated, 0xDD);
    DataFields f{};
    EXPECT_TRUE(parse_data(p.data(), p.size(), &f));
    EXPECT_TRUE((f.options & kOptPreview) != 0);
    EXPECT_TRUE((f.options & kOptTerminated) != 0);
    EXPECT_EQ(f.start_code, 0xDD);
}

// ── Sync packet ─────────────────────────────────────────────────────────────

static void test_parse_sync() {
    auto p             = make_sync_packet(42);
    uint16_t sync_addr = 0;
    EXPECT_TRUE(parse_sync(p.data(), p.size(), &sync_addr));
    EXPECT_EQ(sync_addr, 42);
    // A data packet is not a sync packet.
    uint8_t slots[1] = { 1 };
    auto d           = make_data_packet(1, 100, slots, 1);
    EXPECT_TRUE(!parse_sync(d.data(), d.size(), &sync_addr));
}

// ── Multicast group ─────────────────────────────────────────────────────────

static void test_multicast_group() {
    EXPECT_EQ(multicast_group_host(1), 0xEFFF0001u);      // 239.255.0.1
    EXPECT_EQ(multicast_group_host(256), 0xEFFF0100u);    // 239.255.1.0
    EXPECT_EQ(multicast_group_host(63999), 0xEFFFF9FFu);  // 239.255.249.255
}

// ── Source gate ─────────────────────────────────────────────────────────────

static void test_gate_higher_priority_wins() {
    SourceGate gates[4] = {};
    EXPECT_TRUE(gate_accept(gates, 1, 100, false, 1000));   // first source
    EXPECT_TRUE(!gate_accept(gates, 1, 50, false, 1100));   // lower → rejected
    EXPECT_TRUE(gate_accept(gates, 1, 150, false, 1200));   // higher → wins
    EXPECT_TRUE(!gate_accept(gates, 1, 100, false, 1300));  // now below 150
    EXPECT_TRUE(gate_accept(gates, 1, 150, false, 1400));   // equal → accepted
}

static void test_gate_timeout_reclaims() {
    SourceGate gates[4] = {};
    EXPECT_TRUE(gate_accept(gates, 1, 200, false, 1000));
    EXPECT_TRUE(!gate_accept(gates, 1, 10, false, 2000));  // within 2.5 s
    EXPECT_TRUE(gate_accept(gates, 1, 10, false, 4000));   // expired → low prio ok
}

static void test_gate_terminated_releases() {
    SourceGate gates[4] = {};
    EXPECT_TRUE(gate_accept(gates, 1, 200, false, 1000));
    EXPECT_TRUE(!gate_accept(gates, 1, 200, true, 1100));  // terminate (no data applied)
    EXPECT_TRUE(gate_accept(gates, 1, 10, false, 1200));   // slot free → low prio ok
}

static void test_gate_universes_independent() {
    SourceGate gates[4] = {};
    EXPECT_TRUE(gate_accept(gates, 1, 200, false, 1000));
    EXPECT_TRUE(gate_accept(gates, 2, 10, false, 1000));  // other universe unaffected
    EXPECT_TRUE(!gate_accept(gates, 1, 10, false, 1100));
}

static void test_gate_eviction_when_full() {
    SourceGate gates[2] = {};
    EXPECT_TRUE(gate_accept(gates, 1, 100, false, 1000));
    EXPECT_TRUE(gate_accept(gates, 2, 100, false, 2000));
    EXPECT_TRUE(gate_accept(gates, 3, 100, false, 3000));  // evicts stalest (uni 1)
    EXPECT_TRUE(gate_accept(gates, 1, 10, false, 3100));   // uni 1 forgotten → accepted
}

static void test_gate_takeover_flag() {
    SourceGate gates[4] = {};
    bool takeover       = true;
    EXPECT_TRUE(gate_accept(gates, 1, 100, false, 1000, &takeover));
    EXPECT_TRUE(!takeover);  // first source: no takeover
    EXPECT_TRUE(gate_accept(gates, 1, 100, false, 1100, &takeover));
    EXPECT_TRUE(!takeover);  // equal priority: no takeover
    EXPECT_TRUE(gate_accept(gates, 1, 150, false, 1200, &takeover));
    EXPECT_TRUE(takeover);  // priority rose on a live universe
    takeover = true;
    EXPECT_TRUE(gate_accept(gates, 1, 200, false, 5000, &takeover));
    EXPECT_TRUE(!takeover);  // expired slot: reclaim, not a takeover
}

// ── Merge-source key from CID ───────────────────────────────────────────────

static void test_source_id_from_cid() {
    uint8_t cid_a[16] = {}, cid_b[16] = {};
    for (int i = 0; i < 16; ++i)
        cid_a[i] = static_cast<uint8_t>(i);
    cid_b[15] = 1;  // differs in one byte

    EXPECT_TRUE(source_id_from_cid(cid_a) != 0);  // 0 is the merge free-slot sentinel
    EXPECT_TRUE(source_id_from_cid(cid_b) != 0);
    EXPECT_EQ(source_id_from_cid(cid_a), source_id_from_cid(cid_a));  // deterministic
    EXPECT_TRUE(source_id_from_cid(cid_a) != source_id_from_cid(cid_b));
}

int main() {
    test_parse_valid_data();
    test_parse_full_universe();
    test_parse_rejects_bad_preamble();
    test_parse_rejects_bad_acn_id();
    test_parse_rejects_universe_zero();
    test_parse_rejects_truncated();
    test_parse_options_and_start_code();
    test_parse_sync();
    test_multicast_group();
    test_gate_higher_priority_wins();
    test_gate_timeout_reclaims();
    test_gate_terminated_releases();
    test_gate_universes_independent();
    test_gate_eviction_when_full();
    test_gate_takeover_flag();
    test_source_id_from_cid();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
