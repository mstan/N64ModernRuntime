#include <array>
#include "ultramodern/ultra_trace.hpp"
#include <cassert>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"

// TODO move this out into ultramodern code

constexpr uint32_t flash_size = 1024 * 1024 / 8; // 1Mbit
constexpr uint32_t page_size = 128;
constexpr uint32_t pages_per_sector = 128;
constexpr uint32_t page_count = flash_size / page_size;
constexpr uint32_t sector_size = page_size * pages_per_sector;
constexpr uint32_t sector_count = flash_size / sector_size;

void save_write_ptr(const void* in, uint32_t offset, uint32_t count);
void save_write(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);
void save_read(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);
void save_clear(uint32_t start, uint32_t size, char value);

// Staging area for a single page program: osFlashWriteBuffer fills it,
// osFlashWriteArray commits it to the backing save at the chosen page.
std::array<char, page_size> write_buffer;

// Each osFlash* entry point is only valid when the game's configured save
// type is FlashRAM; bail loudly otherwise rather than silently corrupting a
// differently-typed save file.
static void require_flashram_save() {
    if (!recomp::flashram_allowed()) {
        ultramodern::error_handling::message_box("Attempted to use FlashRAM saving with other save type");
        ULTRAMODERN_QUICK_EXIT();
    }
}

extern "C" void osFlashInit_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    ctx->r2 = recomp::flash_handle;
}

extern "C" void osFlashReadStatus_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    PTR(u8) flash_status = ctx->r4;
    MEM_B(0, flash_status) = 0;
}

extern "C" void osFlashReadId_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    PTR(u32) flash_type = ctx->r4;
    PTR(u32) flash_maker = ctx->r5;

    // Report an MX_B/D part so libultra picks 0x80-byte page addressing, which
    // lines up with the 128-byte page program/read granularity used here.
    MEM_W(0, flash_type) = 0x11118001;
    MEM_W(0, flash_maker) = 0x00C2001D;
}

extern "C" void osFlashClearStatus_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    // No status latch is modeled, so there is nothing to clear.
}

extern "C" void osFlashAllErase_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    save_clear(0, ultramodern::save_size, 0xFF);
    ctx->r2 = 0;
}

extern "C" void osFlashAllEraseThrough_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    save_clear(0, ultramodern::save_size, 0xFF);
    ctx->r2 = 0;
}

// erase one page worth of storage; the libultra name says "sector" but the
// granularity here is a page.
static void flash_erase_page(recomp_context* ctx) {
    uint32_t page_num = (uint32_t)ctx->r4;

    // Reject an out-of-range page rather than clearing past the save.
    if (page_num >= page_count) {
        ctx->r2 = -1;
        return;
    }

    save_clear(page_num * page_size, page_size, 0xFF);
    ctx->r2 = 0;
}

extern "C" void osFlashSectorErase_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();
    flash_erase_page(ctx);
}

extern "C" void osFlashSectorEraseThrough_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();
    flash_erase_page(ctx);
}

extern "C" void osFlashCheckEraseEnd_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    // Erases here run synchronously, so the erase is always already done.
    ctx->r2 = 0; // FLASH_STATUS_ERASE_OK
}

extern "C" void osFlashWriteBuffer_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    PTR(void) dramAddr = ctx->r6;
    PTR(OSMesgQueue) mq = ctx->r7;

    // Latch one page of data from guest RDRAM into the staging buffer.
    for (size_t i = 0; i < page_size; i++) {
        write_buffer[i] = MEM_B(i, dramAddr);
    }

    // Signal completion to the caller's queue.
    s32 sent = osSendMesg(PASS_RDRAM mq, 0, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~flash_done", 0, (uint32_t)mq, 0, (uint32_t)sent, 0);

    ctx->r2 = 0;
}

extern "C" void osFlashWriteArray_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    uint32_t page_num = ctx->r4;

    // Flush the staged page into the backing save.
    save_write_ptr(write_buffer.data(), page_num * page_size, page_size);

    ctx->r2 = 0;
}

extern "C" void osFlashReadArray_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    uint32_t page_num = ctx->r6;
    PTR(void) dramAddr = ctx->r7;
    // The remaining two arguments are passed on the stack.
    uint32_t n_pages = MEM_W(0x10, ctx->r29);
    PTR(OSMesgQueue) mq = MEM_W(0x14, ctx->r29);

    // Pull the requested page span from the backing save into guest RDRAM.
    save_read(PASS_RDRAM dramAddr, page_num * page_size, n_pages * page_size);

    // Signal completion to the caller's queue.
    s32 sent = osSendMesg(PASS_RDRAM mq, 0, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~flash_done", 0, (uint32_t)mq, 1, (uint32_t)sent, 0);

    ctx->r2 = 0;
}

extern "C" void osFlashChange_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_flashram_save();

    // Switching between multiple flash banks is unsupported.
    assert(false);
}
