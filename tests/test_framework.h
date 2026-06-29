// Minimal self-contained unit-test scaffold for N64ModernRuntime.
//
// No external dependencies: a test is a function registered with TEST(),
// assertions use CHECK_EQ / CHECK_TRUE, and main() (in test_main.cpp) runs
// every registered test and reports a pass/fail tally with a process exit
// code suitable for CI.
#ifndef N64MR_TEST_FRAMEWORK_H
#define N64MR_TEST_FRAMEWORK_H

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

namespace n64mr_test {

struct Case {
    const char* name;
    void (*fn)(struct Result&);
};

struct Result {
    int checks = 0;
    int failures = 0;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)(Result&)) { registry().push_back({name, fn}); }
};

// Stringify a value for failure messages without pulling <sstream>.
inline std::string show(uint64_t v) { char b[32]; std::snprintf(b, sizeof(b), "0x%016llX", (unsigned long long)v); return b; }
inline std::string show(double v)   { char b[40]; std::snprintf(b, sizeof(b), "%.17g", v); return b; }

#define TEST(name)                                                      \
    static void name(n64mr_test::Result& _r);                          \
    static n64mr_test::Registrar _reg_##name(#name, name);             \
    static void name(n64mr_test::Result& _r)

#define CHECK_EQ(actual, expected)                                      \
    do {                                                                \
        _r.checks++;                                                    \
        auto _a = (actual); auto _e = (expected);                       \
        if (_a != _e) {                                                 \
            _r.failures++;                                              \
            std::printf("    FAIL %s:%d  %s == %s\n      got %s want %s\n", \
                __FILE__, __LINE__, #actual, #expected,                 \
                n64mr_test::show(_a).c_str(), n64mr_test::show(_e).c_str()); \
        }                                                               \
    } while (0)

#define CHECK_TRUE(cond)                                                \
    do {                                                                \
        _r.checks++;                                                    \
        if (!(cond)) { _r.failures++;                                   \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

} // namespace n64mr_test

#endif
