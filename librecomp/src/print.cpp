#include <vector>
#include "ultramodern/ultra_trace.hpp"

#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "euc-jp.hpp"

extern "C" void __checkHardware_msp_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 0;
}

extern "C" void __checkHardware_kmc_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 0;
}

extern "C" void __checkHardware_isv_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 0;
}

extern "C" void __osInitialize_msp_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void __osInitialize_kmc_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void __osInitialize_isv_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void isPrintfInit_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void __osRdbSend_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    gpr guest_buf = ctx->r4;
    size_t size = ctx->r5;

    // Copy the guest-side byte run into a NUL-terminated host buffer and emit
    // it to stdout; report the number of bytes consumed back to the caller.
    std::unique_ptr<char[]> host_text = std::make_unique<char[]>(size + 1);
    for (size_t i = 0; i < size; i++) {
        host_text[i] = MEM_B(i, guest_buf);
    }
    host_text[size] = '\0';

    fwrite(host_text.get(), 1, size, stdout);

    ctx->r2 = size;
}

extern "C" void is_proutSyncPrintf_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // The IS-Viewer sync-printf sink is intentionally a no-op here: the
    // payload (guest buffer in r5, length in r6) is discarded and the call is
    // reported as fully handled. A newline-buffered, EUC-JP-decoded variant
    // was prototyped but is not enabled.
    ctx->r2 = 1;
}
