// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Framework-level libultra-call ring instrumentation
//     (LIBRECOMP_ULTRA_TRACE) at osEep entry points for runner-side
//     boot sequencing diagnostics.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/ultra_trace.hpp"

#include "ultramodern/ultra64.h"

void save_write(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);
void save_read(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);

constexpr int eeprom_block_size = 8;

// EEPROM access is only valid when the configured save type is one of the
// EEPROM variants; abort loudly otherwise to avoid scribbling on a save of a
// different type.
static void require_eeprom_save() {
    if (!recomp::eeprom_allowed()) {
        ultramodern::error_handling::message_box("Attempted to use EEPROM saving with other save type");
        ULTRAMODERN_QUICK_EXIT();
    }
}

// EEPROM is addressed in fixed 8-byte blocks, so a block index scales to a
// byte offset by the block size.
static uint32_t eeprom_byte_offset(uint8_t block_index) {
    return block_index * eeprom_block_size;
}

extern "C" void osEepromProbe_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // Report the EEPROM capacity libultra should assume for this save type.
    switch (recomp::get_save_type()) {
        case recomp::SaveType::AllowAll:
        case recomp::SaveType::Eep16k:
            ctx->r2 = 0x02; // EEPROM_TYPE_16K
            break;
        case recomp::SaveType::Eep4k:
            ctx->r2 = 0x01; // EEPROM_TYPE_4K
            break;
        default:
            ctx->r2 = 0x00;
            break;
    }
}

extern "C" void osEepromWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_eeprom_save();

    uint8_t block_index = ctx->r5;
    gpr buffer = ctx->r6;

    // The single-block call always transfers exactly one 8-byte block.
    save_write(rdram, buffer, eeprom_byte_offset(block_index), eeprom_block_size);

    ctx->r2 = 0;
}

extern "C" void osEepromLongWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_eeprom_save();

    uint8_t block_index = ctx->r5;
    gpr buffer = ctx->r6;
    int32_t nbytes = ctx->r7;

    assert((nbytes % eeprom_block_size) == 0);

    save_write(rdram, buffer, eeprom_byte_offset(block_index), nbytes);

    ctx->r2 = 0;
}

extern "C" void osEepromRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_eeprom_save();

    uint8_t block_index = ctx->r5;
    gpr buffer = ctx->r6;

    // The single-block call always transfers exactly one 8-byte block.
    save_read(rdram, buffer, eeprom_byte_offset(block_index), eeprom_block_size);

    ctx->r2 = 0;
}

extern "C" void osEepromLongRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    require_eeprom_save();

    uint8_t block_index = ctx->r5;
    gpr buffer = ctx->r6;
    int32_t nbytes = ctx->r7;

    assert((nbytes % eeprom_block_size) == 0);

    save_read(rdram, buffer, eeprom_byte_offset(block_index), nbytes);

    ctx->r2 = 0;
}
