#include <ultramodern/ultramodern.hpp>
#include "recomp.h"

// Software implementations of the 64-bit integer and int<->float conversions
// that the recompiler emits calls to in place of the corresponding libgcc
// helpers (and a few libultra long-long routines). They could eventually be
// retired by widening the recompiler's own instruction/control-flow coverage.

// Tracing note (ultra_trace): these are intentionally left out of
// LIBRECOMP_ULTRA_TRACE. They are compiler-runtime arithmetic, not
// libultra OS-boundary calls, and a single hot math loop would otherwise
// flood — and evict — the OS-call ring within microseconds while adding cost
// to every 64-bit divide in Release. See ultramodern/ultra_trace.hpp for the
// ring's event taxonomy.

// A 64-bit argument arrives split across two o32 registers (high half, then
// low half); rebuild the value the way each helper consumes it.
static inline uint64_t join64(gpr hi, gpr lo) {
    return (hi << 32) | (lo & 0xFFFFFFFFu);
}

// A 64-bit result is returned through the r2:r3 pair, high half in r2.
static inline void return64(recomp_context* ctx, uint64_t value) {
    ctx->r2 = static_cast<int32_t>(value >> 32);
    ctx->r3 = static_cast<int32_t>(value);
}

extern "C" void __udivdi3_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) / join64(ctx->r6, ctx->r7));
}

extern "C" void __divdi3_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    int64_t a = static_cast<int64_t>(join64(ctx->r4, ctx->r5));
    int64_t b = static_cast<int64_t>(join64(ctx->r6, ctx->r7));
    return64(ctx, static_cast<uint64_t>(a / b));
}

extern "C" void __umoddi3_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) % join64(ctx->r6, ctx->r7));
}

extern "C" void __ull_div_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) / join64(ctx->r6, ctx->r7));
}

extern "C" void __ll_div_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    int64_t a = static_cast<int64_t>(join64(ctx->r4, ctx->r5));
    int64_t b = static_cast<int64_t>(join64(ctx->r6, ctx->r7));
    return64(ctx, static_cast<uint64_t>(a / b));
}

extern "C" void __ll_mul_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) * join64(ctx->r6, ctx->r7));
}

extern "C" void __ull_rem_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) % join64(ctx->r6, ctx->r7));
}

extern "C" void __ull_to_d_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->f0.d = static_cast<double>(join64(ctx->r4, ctx->r5));
}

extern "C" void __ull_to_f_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->f0.fl = static_cast<float>(join64(ctx->r4, ctx->r5));
}

extern "C" void __ull_rshift_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) >> join64(ctx->r6, ctx->r7));
}

extern "C" void __ll_to_f_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->f0.fl = static_cast<float>(static_cast<int64_t>(join64(ctx->r4, ctx->r5)));
}

extern "C" void __f_to_ll_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, static_cast<uint64_t>(static_cast<int64_t>(ctx->f12.fl)));
}

extern "C" void __ll_lshift_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    return64(ctx, join64(ctx->r4, ctx->r5) << join64(ctx->r6, ctx->r7));
}
