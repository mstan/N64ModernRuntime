#ifndef __RECOMP_HELPERS__
#define __RECOMP_HELPERS__

#include <string>

#include "recomp.h"
#include <ultramodern/ultra64.h>

template<int index, typename T>
T _arg(uint8_t* rdram, recomp_context* ctx) {
    static_assert(index < 4, "Only args 0 through 3 supported");
    // The integer argument registers a0..a3 sit contiguously in the
    // context starting at r4, so index them directly.
    const gpr reg_value = (&ctx->r4)[index];

    if constexpr (std::is_same_v<T, float>) {
        if constexpr (index < 2) {
            static_assert(index != 1, "Floats in arg 1 not supported");
            return ctx->f12.fl;
        }
        else {
            // static_assert in else workaround
            [] <bool flag = false>() {
                static_assert(flag, "Floats in a2/a3 not supported");
            }();
        }
    }
    else if constexpr (std::is_pointer_v<T>) {
        static_assert(!std::is_pointer_v<std::remove_pointer_t<T>>, "Double pointers not supported");
        return TO_PTR(std::remove_pointer_t<T>, reg_value);
    }
    else if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= 4, "64-bit args not supported");
        return static_cast<T>(reg_value);
    }
    else {
        // static_assert in else workaround
        [] <bool flag = false>() {
            static_assert(flag, "Unsupported type");
        }();
    }
}

inline float _arg_float_a1(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    // a1 carries the float's raw bit pattern in an integer register;
    // reinterpret those bits as a float via a shared-storage union.
    union {
        u32 word;
        float value;
    } conv;
    conv.word = _arg<1, u32>(rdram, ctx);
    return conv.value;
}

inline float _arg_float_f14(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    return ctx->f14.fl;
}

template <int arg_index>
std::string _arg_string(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) guest_str = _arg<arg_index, PTR(char)>(rdram, ctx);

    // The string lives byteswapped in rdram, so read it one byte at a
    // time through MEM_B and accumulate until the terminating NUL.
    std::string result;
    for (size_t i = 0; ; i++) {
        char c = static_cast<char>(MEM_B(guest_str, i));
        if (c == 0x00) {
            break;
        }
        result += c;
    }

    return result;
}

template <typename T>
void _return(recomp_context* ctx, T val) {
    static_assert(sizeof(T) <= 4 && "Only 32-bit value returns supported currently");
    if constexpr (std::is_same_v<T, float>) {
        // Float results are reported through f0.
        ctx->f0.fl = val;
    }
    else if constexpr (std::is_integral_v<T> && sizeof(T) <= 4) {
        // Integer results are reported (sign-extended) through r2.
        ctx->r2 = static_cast<int32_t>(val);
    }
    else {
        // static_assert in else workaround
        [] <bool flag = false>() {
            static_assert(flag, "Unsupported type");
        }();
    }
}

#endif
