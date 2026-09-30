// Minimal test framework for the IDF-shim harness: self-registering TEST()
// cases, EXPECT_* checks that keep going, a PASS=/FAIL= summary like the
// component suites, and a non-zero exit on any failure.
#pragma once

#include <cstdio>
#include <cstring>
#include <vector>

namespace harness {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& passes() {
    static int n = 0;
    return n;
}
inline int& failures() {
    static int n = 0;
    return n;
}
inline const char*& current() {
    static const char* c = "";
    return c;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({ name, fn }); }
};

inline void check(bool ok, const char* expr, const char* file, int line, long long a = 0,
                  long long b = 0, bool show = false) {
    if (ok) {
        ++passes();
        return;
    }
    ++failures();
    if (show)
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s (%lld vs %lld)\n", current(), file, line, expr, a,
                     b);
    else
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current(), file, line, expr);
}

// Runs every case (or those whose name contains argv[1]); returns the exit code.
inline int run_all(int argc, char** argv, void (*setup)() = nullptr) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    for (const auto& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        current() = c.name;
        if (setup) setup();
        c.fn();
    }
    std::printf("PASS=%d FAIL=%d (%zu cases)\n", passes(), failures(), registry().size());
    return failures() ? 1 : 0;
}

}  // namespace harness

#define TEST(name)                                                                                 \
    static void name();                                                                            \
    static harness::Registrar name##_reg(#name, name);                                             \
    static void name()

#define EXPECT_TRUE(x) harness::check((x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) harness::check(!(x), "!(" #x ")", __FILE__, __LINE__)
#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        const long long va_ = static_cast<long long>(a), vb_ = static_cast<long long>(b);          \
        harness::check(va_ == vb_, #a " == " #b, __FILE__, __LINE__, va_, vb_, true);              \
    } while (0)
#define EXPECT_STREQ(a, b)                                                                         \
    harness::check(std::strcmp((a), (b)) == 0, #a " == " #b, __FILE__, __LINE__)
