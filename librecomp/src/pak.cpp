// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with upstream authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Framework-level libultra-call ring instrumentation
//     (LIBRECOMP_ULTRA_TRACE) at osPfs entry points for runner-side
//     boot sequencing diagnostics.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"
#include "librecomp/ultra_trace.hpp"

extern "C" void osPfsInitPak_recomp(uint8_t * rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsAllocateFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsDeleteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsFileState_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsFindFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsChecker_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 1; // PFS_ERR_NOPACK
}

extern "C" void osPfsNumFiles_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    s32* max_files = _arg<1, s32*>(rdram, ctx);
    s32* files_used = _arg<2, s32*>(rdram, ctx);

    *max_files = 0;
    *files_used = 0;

    _return<s32>(ctx, 1); // PFS_ERR_NOPACK
}

extern "C" void osPfsRepairId_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    _return<s32>(ctx, 1); // PFS_ERR_NOPACK
}

extern "C" void osPfsIsPlug_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // s32 osPfsIsPlug(OSMesgQueue* mq, u8* pattern_out)
    // Reports which controller ports have an accessory attached, as a
    // bitmap in *pattern_out. Controller and Transfer Paks share this
    // libultra presence scan; the project callback identifies the pak
    // type before raw accessory I/O handles the device protocol.
    PTR(u8) pattern_out = _arg<1, PTR(u8)>(rdram, ctx);
    if (pattern_out != NULLPTR) {
        u8 pattern = 0;
        for (int controller = 0; controller < 4; controller++) {
            const ultramodern::input::connected_device_info_t device_info =
                ultramodern::input::get_connected_device_info(controller);
            if (device_info.connected_pak != ultramodern::input::Pak::None) {
                pattern |= static_cast<u8>(1 << controller);
            }
        }
        MEM_B(0, pattern_out) = pattern;
    }
    _return<s32>(ctx, 0);
}
