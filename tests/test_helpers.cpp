// Behavioral pin for librecomp/include/librecomp/helpers.hpp — the o32 argument
// and return ABI accessors every libultra shim is built on. _arg<i,T> pulls
// argument i out of the a-registers (a0..a3 == r4..r7), narrowing integrals and
// reinterpreting floats; _return<T> writes the result back into r2 (integers) or
// f0 (floats). Locking this contract guards every shim against an ABI slip when
// the accessors are reworked. (Results are bound to locals first so the template
// commas don't trip the function-like CHECK_EQ macro.)
#include "test_framework.h"
#include "recomp.h"
#include "librecomp/helpers.hpp"

TEST(helpers_arg_integral_slots) {
    recomp_context c{};
    c.r4 = 0x11; c.r5 = 0x22; c.r6 = 0x33; c.r7 = 0x44;
    uint32_t a0 = _arg<0, uint32_t>(nullptr, &c);
    uint32_t a1 = _arg<1, uint32_t>(nullptr, &c);
    uint32_t a2 = _arg<2, uint32_t>(nullptr, &c);
    uint32_t a3 = _arg<3, uint32_t>(nullptr, &c);
    CHECK_EQ((uint64_t)a0, (uint64_t)0x11);
    CHECK_EQ((uint64_t)a1, (uint64_t)0x22);
    CHECK_EQ((uint64_t)a2, (uint64_t)0x33);
    CHECK_EQ((uint64_t)a3, (uint64_t)0x44);
}

TEST(helpers_arg_integral_narrowing) {
    recomp_context c{};
    c.r4 = 0xFFFFFF80u;                                   // sits in a0
    int8_t   s8  = _arg<0, int8_t>(nullptr, &c);
    uint8_t  u8  = _arg<0, uint8_t>(nullptr, &c);
    uint16_t u16 = _arg<0, uint16_t>(nullptr, &c);
    CHECK_EQ((uint64_t)(int64_t)s8, (uint64_t)(int64_t)(int8_t)0x80);
    CHECK_EQ((uint64_t)u8, (uint64_t)0x80);
    CHECK_EQ((uint64_t)u16, (uint64_t)0xFF80);
}

TEST(helpers_return_int_and_float) {
    recomp_context c{};
    _return<int32_t>(&c, -5);
    CHECK_EQ((uint64_t)(int64_t)(int32_t)c.r2, (uint64_t)(int64_t)-5);
    _return<uint32_t>(&c, 0xDEADBEEFu);
    CHECK_EQ((uint64_t)(int32_t)c.r2, (uint64_t)(int32_t)0xDEADBEEFu);
    _return<float>(&c, 3.5f);
    CHECK_TRUE(c.f0.fl == 3.5f);
}

TEST(helpers_float_args) {
    recomp_context c{};
    c.f14.fl = 9.25f;                                     // f14 read straight through
    CHECK_TRUE(_arg_float_f14(nullptr, &c) == 9.25f);
    union { uint32_t u; float f; } v; v.f = -1.5f;        // a1 float == r5 bit pattern
    c.r5 = v.u;
    CHECK_TRUE(_arg_float_a1(nullptr, &c) == -1.5f);
}
