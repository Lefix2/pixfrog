// Host-side unit tests for artnet::parser.

#include <cstdio>
#include <cstring>
#include <vector>

#include "artnet_parser.h"

using namespace pixfrog::artnet::parser;

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
        if (a)                                                                                     \
            g_pass++;                                                                              \
        else {                                                                                     \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #a);                      \
        }                                                                                          \
    } while (0)

// ── header ──────────────────────────────────────────────────────────────────

static void test_header_too_short() {
    const uint8_t buf[6] = { 'A', 'r', 't', '-', 'N', 'e' };
    EXPECT_TRUE(!parse_header(buf, sizeof(buf), nullptr));
}

static void test_header_wrong_magic() {
    const uint8_t buf[12] = { 'X', 'Y', 'Z', '-', 'N', 'e', 't', 0, 0x00, 0x50, 0, 0 };
    EXPECT_TRUE(!parse_header(buf, sizeof(buf), nullptr));
}

static void test_header_valid_dmx_opcode() {
    const uint8_t buf[12] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x50, 0, 14 };
    uint16_t op           = 0;
    EXPECT_TRUE(parse_header(buf, sizeof(buf), &op));
    EXPECT_EQ(op, 0x5000);
}

static void test_header_valid_poll_opcode() {
    const uint8_t buf[14] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x20, 0, 14, 0, 0 };
    uint16_t op           = 0;
    EXPECT_TRUE(parse_header(buf, sizeof(buf), &op));
    EXPECT_EQ(op, 0x2000);
}

// ── ArtDmx body ─────────────────────────────────────────────────────────────

static std::vector<uint8_t> make_dmx_packet(uint16_t universe, const uint8_t* data,
                                            uint16_t data_len) {
    std::vector<uint8_t> pkt(18 + data_len, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0x50;
    pkt[10] = 0;
    pkt[11] = 14;
    pkt[12] = 0;
    pkt[13] = 0;
    pkt[14] = static_cast<uint8_t>(universe & 0xFF);
    pkt[15] = static_cast<uint8_t>((universe >> 8) & 0x7F);
    pkt[16] = static_cast<uint8_t>((data_len >> 8) & 0xFF);
    pkt[17] = static_cast<uint8_t>(data_len & 0xFF);
    if (data && data_len) std::memcpy(pkt.data() + 18, data, data_len);
    return pkt;
}

static void test_dmx_too_short() {
    const uint8_t buf[10] = {};
    EXPECT_TRUE(!parse_dmx(buf, sizeof(buf), nullptr));
}

static void test_dmx_length_mismatch() {
    // data_len field claims 100 bytes but the packet is only 18+50.
    auto pkt = make_dmx_packet(0x0123, nullptr, 100);
    pkt.resize(18 + 50);
    EXPECT_TRUE(!parse_dmx(pkt.data(), pkt.size(), nullptr));
}

static void test_dmx_valid_extract() {
    uint8_t payload[64];
    for (int i = 0; i < 64; ++i)
        payload[i] = static_cast<uint8_t>(i * 2);
    auto pkt = make_dmx_packet(0x0123, payload, 64);

    DmxFields f{};
    EXPECT_TRUE(parse_dmx(pkt.data(), pkt.size(), &f));
    EXPECT_EQ(f.universe, 0x0123);
    EXPECT_EQ(f.data_len, 64);
    EXPECT_EQ(f.data[0], 0);
    EXPECT_EQ(f.data[63], 126);
}

static void test_dmx_universe_decomposition() {
    // universe 0x0235 = net=2, sub=3, uni=5
    auto pkt = make_dmx_packet(0x0235, nullptr, 0);
    DmxFields f{};
    EXPECT_TRUE(parse_dmx(pkt.data(), pkt.size(), &f));
    EXPECT_EQ(f.universe, 0x0235);
}

// ── universe_matches ────────────────────────────────────────────────────────

static void test_universe_matches_exact() {
    EXPECT_TRUE(universe_matches(0x0235, 2, 3));   // net=2, sub=3
    EXPECT_TRUE(!universe_matches(0x0235, 2, 4));  // wrong subnet
    EXPECT_TRUE(!universe_matches(0x0235, 3, 3));  // wrong net
    EXPECT_TRUE(!universe_matches(0x0235, 0, 0));  // neither matches
    EXPECT_TRUE(universe_matches(0x0000, 0, 0));   // default
}

static void test_universe_matches_high_bits_ignored() {
    // High bit of net byte (0x80) should be masked off.
    EXPECT_TRUE(universe_matches(0x0235, 0x82, 3));  // 0x82 & 0x7F == 2
}

// ── build_poll_reply ────────────────────────────────────────────────────────

static void test_poll_reply_header_and_size() {
    uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
    PollReplyInputs in{};
    in.local_ip_host = 0xC0A80164;  // 192.168.1.100
    in.artnet_net    = 0;
    in.artnet_subnet = 0;
    in.short_name    = "pixfrog";
    in.long_name     = "pixfrog LED controller";
    in.node_report   = "#0001 [1] pixfrog OK";
    in.mac           = mac;
    in.bind_index    = 1;
    in.sw_out[0]     = 0;
    in.sw_out[1]     = 1;
    in.sw_out[2]     = 2;
    in.sw_out[3]     = 3;

    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);

    // Art-Net magic
    EXPECT_EQ(std::memcmp(pkt, "Art-Net\0", 8), 0);

    // OpCode 0x2100 LE
    EXPECT_EQ(pkt[8], 0x00);
    EXPECT_EQ(pkt[9], 0x21);

    // IP a.b.c.d order
    EXPECT_EQ(pkt[10], 192);
    EXPECT_EQ(pkt[11], 168);
    EXPECT_EQ(pkt[12], 1);
    EXPECT_EQ(pkt[13], 100);

    // Port 0x1936 LE
    EXPECT_EQ(pkt[14], 0x36);
    EXPECT_EQ(pkt[15], 0x19);

    // Names
    EXPECT_EQ(std::memcmp(pkt + 26, "pixfrog", 7), 0);
    EXPECT_EQ(std::memcmp(pkt + 44, "pixfrog LED controller", 22), 0);
    EXPECT_EQ(std::memcmp(pkt + 108, "#0001 [1] pixfrog OK", 20), 0);

    // NumPorts = 4
    EXPECT_EQ(pkt[172], 0x00);
    EXPECT_EQ(pkt[173], 0x04);

    // PortType / GoodOutputA
    for (int p = 0; p < 4; ++p) {
        EXPECT_EQ(pkt[174 + p], 0x80);
        EXPECT_EQ(pkt[182 + p], 0x80);
    }
    // SwOut
    EXPECT_EQ(pkt[190], 0);
    EXPECT_EQ(pkt[191], 1);
    EXPECT_EQ(pkt[192], 2);
    EXPECT_EQ(pkt[193], 3);

    // Style = StNode
    EXPECT_EQ(pkt[200], 0x00);

    // MAC
    EXPECT_EQ(std::memcmp(pkt + 201, mac, 6), 0);

    // BindIp = same as IP
    EXPECT_EQ(pkt[207], 192);
    EXPECT_EQ(pkt[210], 100);

    // BindIndex
    EXPECT_EQ(pkt[211], 1);
}

static void test_poll_reply_disabled_port() {
    uint8_t mac[6] = {};
    PollReplyInputs in{};
    in.short_name      = "";
    in.long_name       = "";
    in.node_report     = "";
    in.mac             = mac;
    in.bind_index      = 1;
    in.port_enabled[1] = false;  // port 1 is a disabled (Off) channel
    in.sw_out[1]       = 5;

    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);

    // Disabled port advertises no port type and no output activity…
    EXPECT_EQ(pkt[174 + 1], 0x00);
    EXPECT_EQ(pkt[182 + 1], 0x00);
    // …but its SwOut nibble is still written, and other ports stay enabled.
    EXPECT_EQ(pkt[190 + 1], 5);
    EXPECT_EQ(pkt[174 + 0], 0x80);
    EXPECT_EQ(pkt[182 + 0], 0x80);
}

static void test_poll_reply_net_subnet_masked() {
    uint8_t mac[6] = {};
    PollReplyInputs in{};
    in.local_ip_host = 0;
    in.artnet_net    = 0xFF;  // upper bit must be masked off
    in.artnet_subnet = 0xFF;  // upper nibble must be masked off
    in.short_name    = "";
    in.long_name     = "";
    in.node_report   = "";
    in.mac           = mac;
    in.bind_index    = 1;

    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);
    EXPECT_EQ(pkt[18], 0x7F);
    EXPECT_EQ(pkt[19], 0x0F);
}

static void test_poll_reply_sw_out_masked() {
    uint8_t mac[6] = {};
    PollReplyInputs in{};
    in.short_name  = "";
    in.long_name   = "";
    in.node_report = "";
    in.mac         = mac;
    in.bind_index  = 2;
    in.sw_out[0]   = 0xF7;
    in.sw_out[1]   = 0x10;
    in.sw_out[2]   = 0x0F;
    in.sw_out[3]   = 0xAA;

    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);
    EXPECT_EQ(pkt[190], 0x07);
    EXPECT_EQ(pkt[191], 0x00);
    EXPECT_EQ(pkt[192], 0x0F);
    EXPECT_EQ(pkt[193], 0x0A);
}

static void test_poll_reply_name_truncation() {
    uint8_t mac[6] = {};
    PollReplyInputs in{};
    // 30-char name should truncate to 18 in ShortName field.
    in.short_name  = "abcdefghijklmnopqrstuvwxyz1234";
    in.long_name   = "";
    in.node_report = "";
    in.mac         = mac;
    in.bind_index  = 1;
    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);
    EXPECT_EQ(std::memcmp(pkt + 26, "abcdefghijklmnopqr", 18), 0);
    // The 19th byte (offset 44) should be the start of LongName, not 's'.
    EXPECT_EQ(pkt[44], 0);
}

// ── main ────────────────────────────────────────────────────────────────────

// ── ArtNzs ──────────────────────────────────────────────────────────────────

static void test_nzs_valid_extract() {
    std::vector<uint8_t> pkt(18 + 4, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0x51;
    pkt[12] = 7;     // sequence
    pkt[13] = 0xCC;  // start code
    pkt[14] = 0x23;  // sub_uni
    pkt[15] = 0x01;  // net
    pkt[16] = 0x00;
    pkt[17] = 0x04;  // length 4
    pkt[18] = 0xAA;
    NzsFields f{};
    EXPECT_TRUE(parse_nzs(pkt.data(), pkt.size(), &f));
    EXPECT_EQ(f.universe, (1 << 8) | 0x23);
    EXPECT_EQ(f.start_code, 0xCC);
    EXPECT_EQ(f.data_len, 4);
    EXPECT_EQ(f.data[0], 0xAA);
}

static void test_nzs_rejects_truncated() {
    std::vector<uint8_t> pkt(18 + 2, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[16] = 0x00;
    pkt[17] = 0x04;  // claims 4 bytes, only 2 present
    EXPECT_TRUE(!parse_nzs(pkt.data(), pkt.size(), nullptr));
}

// ── ArtTimeCode ─────────────────────────────────────────────────────────────

static std::vector<uint8_t> make_time_code_packet(uint8_t fr, uint8_t s, uint8_t m, uint8_t h,
                                                  uint8_t type) {
    std::vector<uint8_t> pkt(kTimeCodeSize, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0x97;
    pkt[11] = 14;  // ProtVer
    pkt[14] = fr;
    pkt[15] = s;
    pkt[16] = m;
    pkt[17] = h;
    pkt[18] = type;
    return pkt;
}

static void test_time_code_extract_and_ms() {
    auto pkt = make_time_code_packet(12, 34, 5, 1, 1);  // 01:05:34:12 @ EBU 25fps
    TimeCodeFields tc{};
    EXPECT_TRUE(parse_time_code(pkt.data(), pkt.size(), &tc));
    EXPECT_EQ(tc.frames, 12);
    EXPECT_EQ(tc.seconds, 34);
    EXPECT_EQ(tc.minutes, 5);
    EXPECT_EQ(tc.hours, 1);
    EXPECT_EQ(tc.type, 1);
    // 1h + 5min + 34s = 3934s; 12 frames @ 40 ms = 480 ms.
    EXPECT_EQ(time_code_to_ms(tc), 3934000u + 480u);
}

static void test_time_code_frame_rates() {
    TimeCodeFields tc{};
    tc.frames = 23;
    tc.type   = 0;  // film 24 fps → 23 * 41.667 ms
    EXPECT_EQ(time_code_to_ms(tc), 23u * 41667u / 1000u);
    tc.frames = 29;
    tc.type   = 3;  // SMPTE 30 fps
    EXPECT_EQ(time_code_to_ms(tc), 29u * 33333u / 1000u);
}

static void test_time_code_rejects_bad() {
    auto pkt = make_time_code_packet(0, 0, 0, 0, 4);  // type 4 = invalid
    EXPECT_TRUE(!parse_time_code(pkt.data(), pkt.size(), nullptr));
    auto short_pkt = make_time_code_packet(0, 0, 0, 0, 0);
    short_pkt.resize(kTimeCodeSize - 1);
    EXPECT_TRUE(!parse_time_code(short_pkt.data(), short_pkt.size(), nullptr));
}

// ── ArtAddress ──────────────────────────────────────────────────────────────

static std::vector<uint8_t> make_address_packet() {
    std::vector<uint8_t> pkt(kAddressSize, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0x60;
    pkt[11] = 14;  // ProtVer
    return pkt;
}

static void test_address_program_bits() {
    auto pkt = make_address_packet();
    pkt[12]  = 0x85;  // program net = 5
    pkt[13]  = 2;     // bind index 2
    std::memcpy(pkt.data() + 14, "newshort", 8);
    std::memcpy(pkt.data() + 32, "newlong", 7);
    pkt[100] = 0x8A;  // port 0: program universe nibble A
    pkt[101] = 0x00;  // port 1: no change
    pkt[104] = 0x83;  // program subnet = 3
    pkt[106] = 0x04;  // AcLedLocate

    AddressFields f{};
    EXPECT_TRUE(parse_address(pkt.data(), pkt.size(), &f));
    EXPECT_EQ(f.net_switch, 0x85);
    EXPECT_EQ(f.sub_switch, 0x83);
    EXPECT_EQ(f.bind_index, 2);
    EXPECT_EQ(f.sw_out[0], 0x8A);
    EXPECT_EQ(f.sw_out[1], 0x00);
    EXPECT_EQ(f.command, 0x04);
    EXPECT_TRUE(std::memcmp(f.short_name, "newshort", 8) == 0);
    EXPECT_TRUE(std::memcmp(f.long_name, "newlong", 7) == 0);
}

static void test_address_rejects_short_packet() {
    auto pkt = make_address_packet();
    EXPECT_TRUE(!parse_address(pkt.data(), kAddressSize - 1, nullptr));
}

static void test_address_merge_commands() {
    for (uint8_t p = 0; p < 4; ++p) {
        EXPECT_TRUE(is_merge_ltp_command(kAcMergeLtp0 + p));
        EXPECT_TRUE(is_merge_htp_command(kAcMergeHtp0 + p));
    }
    EXPECT_TRUE(!is_merge_ltp_command(kAcMergeLtp0 + 4));  // 0x14 = AcLedNormal
    EXPECT_TRUE(!is_merge_ltp_command(kAcCancelMerge));
    EXPECT_TRUE(!is_merge_htp_command(kAcMergeLtp0));
    EXPECT_TRUE(!is_merge_ltp_command(kAcMergeHtp0));
    EXPECT_TRUE(!is_merge_htp_command(kAcNone));
}

static void test_poll_reply_merge_bits() {
    uint8_t mac[6] = {};
    PollReplyInputs in{};
    in.short_name      = "";
    in.long_name       = "";
    in.node_report     = "";
    in.mac             = mac;
    in.bind_index      = 1;
    in.merge_ltp       = true;
    in.port_merging[2] = true;
    in.port_enabled[3] = false;
    in.port_merging[3] = true;  // disabled port must stay silent regardless

    uint8_t pkt[kPollReplySize];
    build_poll_reply(pkt, in);
    EXPECT_EQ(pkt[182 + 0], 0x82);  // transmitting + LTP
    EXPECT_EQ(pkt[182 + 2], 0x8A);  // transmitting + merging + LTP
    EXPECT_EQ(pkt[182 + 3], 0x00);  // disabled

    in.merge_ltp       = false;
    in.port_merging[2] = false;
    build_poll_reply(pkt, in);
    EXPECT_EQ(pkt[182 + 0], 0x80);  // HTP, not merging: bits 1/3 clear
    EXPECT_EQ(pkt[182 + 2], 0x80);
}

// ── ArtIpProg + reply ───────────────────────────────────────────────────────

static void test_ip_prog_extract() {
    std::vector<uint8_t> pkt(30, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8]  = 0x00;
    pkt[9]  = 0xF8;
    pkt[14] = 0x86;  // enable + program IP + program subnet
    pkt[16] = 192;
    pkt[17] = 168;
    pkt[18] = 1;
    pkt[19] = 99;
    pkt[20] = 255;
    pkt[21] = 255;
    pkt[22] = 255;
    pkt[23] = 0;
    pkt[26] = 192;
    pkt[27] = 168;
    pkt[28] = 1;
    pkt[29] = 1;
    IpProgFields f{};
    EXPECT_TRUE(parse_ip_prog(pkt.data(), pkt.size(), &f));
    EXPECT_EQ(f.command, 0x86);
    EXPECT_EQ(f.prog_ip, 0xC0A80163u);
    EXPECT_EQ(f.prog_mask, 0xFFFFFF00u);
    EXPECT_EQ(f.prog_gw, 0xC0A80101u);
}

static void test_ip_prog_reply_layout() {
    uint8_t pkt[kIpProgReplySize];
    IpProgReplyInputs in{};
    in.ip           = 0x0A000001u;  // 10.0.0.1
    in.mask         = 0xFF000000u;
    in.gw           = 0x0A0000FEu;
    in.dhcp_enabled = true;
    build_ip_prog_reply(pkt, in);
    EXPECT_EQ(pkt[8], 0x00);
    EXPECT_EQ(pkt[9], 0xF9);  // OpIpProgReply LE
    EXPECT_EQ(pkt[16], 10);
    EXPECT_EQ(pkt[19], 1);
    EXPECT_EQ(pkt[20], 255);
    EXPECT_EQ(pkt[26], 0x40);  // Status: DHCP enabled
    EXPECT_EQ(pkt[28], 10);
    EXPECT_EQ(pkt[31], 0xFE);
}

int main() {
    test_header_too_short();
    test_header_wrong_magic();
    test_header_valid_dmx_opcode();
    test_header_valid_poll_opcode();
    test_dmx_too_short();
    test_dmx_length_mismatch();
    test_dmx_valid_extract();
    test_dmx_universe_decomposition();
    test_universe_matches_exact();
    test_universe_matches_high_bits_ignored();
    test_poll_reply_header_and_size();
    test_poll_reply_disabled_port();
    test_poll_reply_net_subnet_masked();
    test_poll_reply_sw_out_masked();
    test_poll_reply_name_truncation();
    test_nzs_valid_extract();
    test_nzs_rejects_truncated();
    test_time_code_extract_and_ms();
    test_time_code_frame_rates();
    test_time_code_rejects_bad();
    test_address_program_bits();
    test_address_rejects_short_packet();
    test_address_merge_commands();
    test_poll_reply_merge_bits();
    test_ip_prog_extract();
    test_ip_prog_reply_layout();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
