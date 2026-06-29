// Behavioral pin for librecomp/src/math_routines.cpp — the 64-bit integer and
// float compiler-runtime shims the recompiler emits calls to. Each shim takes
// its operands in the o32 argument registers (a 64-bit value occupies a
// high/low register pair) and returns through r2:r3 (or f0). These cases lock
// the exact register packing and result, so a refactor that swaps a half or
// changes a cast is caught immediately.
#include "test_framework.h"
#include "recomp.h"

extern "C" {
    void __udivdi3_recomp (uint8_t*, recomp_context*);
    void __divdi3_recomp  (uint8_t*, recomp_context*);
    void __umoddi3_recomp (uint8_t*, recomp_context*);
    void __ull_div_recomp (uint8_t*, recomp_context*);
    void __ll_div_recomp  (uint8_t*, recomp_context*);
    void __ll_mul_recomp  (uint8_t*, recomp_context*);
    void __ull_rem_recomp (uint8_t*, recomp_context*);
    void __ull_to_d_recomp(uint8_t*, recomp_context*);
    void __ull_to_f_recomp(uint8_t*, recomp_context*);
    void __ull_rshift_recomp(uint8_t*, recomp_context*);
    void __ll_to_f_recomp (uint8_t*, recomp_context*);
    void __f_to_ll_recomp (uint8_t*, recomp_context*);
    void __ll_lshift_recomp(uint8_t*, recomp_context*);
}

namespace {
// A 64-bit argument occupies a high/low register pair; a lands in r4:r5 and
// b in r6:r7, matching how each shim reconstructs its operands.
void set_a(recomp_context& c, uint64_t v) {
    c.r4 = static_cast<uint32_t>(v >> 32);
    c.r5 = static_cast<uint32_t>(v & 0xFFFFFFFFu);
}
void set_b(recomp_context& c, uint64_t v) {
    c.r6 = static_cast<uint32_t>(v >> 32);
    c.r7 = static_cast<uint32_t>(v & 0xFFFFFFFFu);
}
// Result is returned in r2 (high 32) : r3 (low 32).
uint64_t ret64(const recomp_context& c) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(c.r2)) << 32)
         |  static_cast<uint32_t>(c.r3);
}
} // namespace

TEST(math_ll_mul) {
    recomp_context c{};
    set_a(c, 0x0000000100000000ull); set_b(c, 3);        // 2^32 * 3
    __ll_mul_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), 0x0000000300000000ull);
    set_a(c, 7); set_b(c, 6);                            // small
    __ll_mul_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)42);
}

TEST(math_udivdi3_and_ull_div) {
    recomp_context c{};
    set_a(c, 0xFFFFFFFFFFFFFFFFull); set_b(c, 2);
    __udivdi3_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), 0x7FFFFFFFFFFFFFFFull);
    set_a(c, 100); set_b(c, 7);
    __ull_div_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)14);
}

TEST(math_signed_div) {
    recomp_context c{};
    set_a(c, (uint64_t)(int64_t)-8); set_b(c, 2);
    __divdi3_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)(int64_t)-4);
    set_a(c, (uint64_t)(int64_t)-100); set_b(c, 3);      // trunc toward zero
    __ll_div_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)(int64_t)-33);
}

TEST(math_mod) {
    recomp_context c{};
    set_a(c, 17); set_b(c, 5);
    __umoddi3_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)2);
    set_a(c, 100); set_b(c, 7);
    __ull_rem_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)2);
}

TEST(math_shifts) {
    recomp_context c{};
    set_a(c, 0x8000000000000000ull); set_b(c, 1);        // logical >>
    __ull_rshift_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), 0x4000000000000000ull);
    set_a(c, 1); set_b(c, 40);                           // <<
    __ll_lshift_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), 0x0000010000000000ull);
}

TEST(math_float_conversions) {
    recomp_context c{};
    set_a(c, 0x0000000100000000ull);                     // 2^32
    __ull_to_d_recomp(nullptr, &c);
    CHECK_TRUE(c.f0.d == 4294967296.0);
    set_a(c, 16777216);                                  // 2^24, exact in float
    __ull_to_f_recomp(nullptr, &c);
    CHECK_TRUE(c.f0.fl == 16777216.0f);
    set_a(c, (uint64_t)(int64_t)-2048);
    __ll_to_f_recomp(nullptr, &c);
    CHECK_TRUE(c.f0.fl == -2048.0f);
    c.f12.fl = 1234.5f;                                  // truncates toward zero
    __f_to_ll_recomp(nullptr, &c);
    CHECK_EQ(ret64(c), (uint64_t)1234);
}
