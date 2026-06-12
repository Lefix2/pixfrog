// Host-side unit tests for fpp::parser (FPP MultiSync wire format).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fpp_sync_parser.h"

using namespace pixfrog::fpp::parser;

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

// Build a sync packet the way fppd does: header + fixed fields + filename.
static std::vector<uint8_t> make_sync(uint8_t action, uint8_t file_type, uint32_t frame,
                                      float seconds, const char* filename) {
    const size_t name_len = std::strlen(filename);
    std::vector<uint8_t> p(17 + name_len + 1, 0);
    std::memcpy(p.data(), "FPPD", 4);
    p[4] = kPktSync;
    const uint16_t extra = static_cast<uint16_t>(p.size() - kHeaderSize);
    p[5] = extra & 0xFF;
    p[6] = extra >> 8;
    p[7] = action;
    p[8] = file_type;
    p[9]  = frame & 0xFF;
    p[10] = (frame >> 8) & 0xFF;
    p[11] = (frame >> 16) & 0xFF;
    p[12] = (frame >> 24) & 0xFF;
    std::memcpy(p.data() + 13, &seconds, 4);
    std::memcpy(p.data() + 17, filename, name_len + 1);
    return p;
}

static void test_sync_extract() {
    auto p = make_sync(kSyncSync, kFileSeq, 1234, 30.85f, "show.fseq");
    SyncFields f{};
    EXPECT_TRUE(parse_sync(p.data(), p.size(), &f));
    EXPECT_EQ(f.action, kSyncSync);
    EXPECT_EQ(f.file_type, kFileSeq);
    EXPECT_EQ(f.frame_number, 1234u);
    EXPECT_TRUE(f.seconds_elapsed > 30.84f && f.seconds_elapsed < 30.86f);
    EXPECT_EQ(std::strcmp(f.filename, "show.fseq"), 0);
}

static void test_sync_actions() {
    for (uint8_t a : { kSyncStart, kSyncStop, kSyncSync, kSyncOpen }) {
        auto p = make_sync(a, kFileSeq, 0, 0.0f, "x.fseq");
        SyncFields f{};
        EXPECT_TRUE(parse_sync(p.data(), p.size(), &f));
        EXPECT_EQ(f.action, a);
    }
    auto bad = make_sync(4, kFileSeq, 0, 0.0f, "x.fseq");  // action out of range
    EXPECT_TRUE(!parse_sync(bad.data(), bad.size(), nullptr));
}

static void test_rejects_non_sync() {
    auto p = make_sync(kSyncSync, kFileSeq, 0, 0.0f, "x.fseq");
    p[4]   = 0;  // pktType ping
    EXPECT_TRUE(!parse_sync(p.data(), p.size(), nullptr));
    auto q = make_sync(kSyncSync, kFileSeq, 0, 0.0f, "x.fseq");
    q[0]   = 'X';  // bad magic
    EXPECT_TRUE(!parse_sync(q.data(), q.size(), nullptr));
    uint8_t tiny[10] = { 'F', 'P', 'P', 'D', 1 };
    EXPECT_TRUE(!parse_sync(tiny, sizeof(tiny), nullptr));
}

static void test_filename_truncated_not_overflowed() {
    std::string longname(200, 'a');
    auto p = make_sync(kSyncStart, kFileSeq, 0, 0.0f, longname.c_str());
    SyncFields f{};
    EXPECT_TRUE(parse_sync(p.data(), p.size(), &f));
    EXPECT_EQ(std::strlen(f.filename), kMaxFileName - 1);
}

static void test_filename_unterminated_in_packet() {
    auto p = make_sync(kSyncStart, kFileSeq, 0, 0.0f, "abcdef");
    p.pop_back();  // drop the NUL: filename runs to the end of the datagram
    SyncFields f{};
    EXPECT_TRUE(parse_sync(p.data(), p.size(), &f));
    EXPECT_EQ(std::strcmp(f.filename, "abcdef"), 0);
}

int main() {
    test_sync_extract();
    test_sync_actions();
    test_rejects_non_sync();
    test_filename_truncated_not_overflowed();
    test_filename_unterminated_in_packet();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
