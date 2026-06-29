// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Framework-level libultra-call ring instrumentation
//     (LIBRECOMP_ULTRA_TRACE) at osVi entry points for runner-side
//     boot sequencing diagnostics.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "helpers.hpp"
#include "librecomp/ultra_trace.hpp"

extern "C" void osViSetYScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViSetYScale(ctx->f12.fl);
}

extern "C" void osViSetXScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ;
}

extern "C" void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViBlack((uint32_t)ctx->r4);
}

extern "C" void osViRepeatLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViRepeatLine(_arg<0, u8>(rdram, ctx));
}

extern "C" void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViSetSpecialFeatures((uint32_t)ctx->r4);
}

extern "C" void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = (gpr)(int32_t)osViGetCurrentFramebuffer();
}

extern "C" void osViGetNextFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = (gpr)(int32_t)osViGetNextFramebuffer();
}

extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViSwapBuffer(rdram, (int32_t)ctx->r4);
}

extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern uint64_t total_vis;

extern "C" void wait_one_frame(uint8_t* rdram, recomp_context* ctx) {
    uint64_t cur_vis = total_vis;
    while (cur_vis == total_vis) {
        std::this_thread::yield();
    }
}
