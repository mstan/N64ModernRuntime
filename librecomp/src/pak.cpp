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
    // Reports which controller ports have a memory pak attached, as a
    // bitmap in *pattern_out. With no controller paks emulated, every
    // port reports "no pak" (pattern = 0). Return 0 for success, which
    // is what libultra reports when the SI poll completed cleanly even
    // though no paks were detected.
    u8* pattern_out = _arg<1, u8*>(rdram, ctx);
    if (pattern_out != nullptr) {
        *pattern_out = 0;
    }
    _return<s32>(ctx, 0);
}
