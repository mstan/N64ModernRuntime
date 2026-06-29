// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Section descriptor extensions for synthetic-fragment resolution
//     and content-hash dispatch in register_runtime_fragment.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#ifndef __SECTIONS_H__
#define __SECTIONS_H__

#include <stdint.h>
#include "recomp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

typedef struct {
    recomp_func_t* func;
    uint32_t offset;
    uint32_t rom_size;
} FuncEntry;

typedef enum {
    R_MIPS_NONE = 0,
    R_MIPS_16,
    R_MIPS_32,
    R_MIPS_REL32,
    R_MIPS_26,
    R_MIPS_HI16,
    R_MIPS_LO16,
    R_MIPS_GPREL16,
} RelocEntryType;

typedef struct {
    // Offset into the section of the word to relocate.
    uint32_t offset;
    // Reloc addend from the target section's address.
    uint32_t target_section_offset;
    // Index of the target section (indexes into `section_addresses`).
    uint16_t target_section;
    // Relocation type.
    RelocEntryType type;
} RelocEntry;

typedef struct {
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint32_t size;
    FuncEntry *funcs;
    size_t num_funcs;
    RelocEntry* relocs;
    size_t num_relocs;
    size_t index;
    // Content hash for runtime identification. Nonzero only on
    // pattern-synthesized decompressed sections (multiple sections
    // share a link vram and need byte-content matching at registration
    // time to pick the right variant). FNV-1a-64 of the first 0x40
    // bytes of the decompressed body. Zero for ELF sections (which
    // get registered uniquely by ram_addr alone).
    uint64_t content_hash;
    // Original game-side fragment id for pattern variants whose
    // ram_addr was reassigned to a synthetic per-variant identity.
    // Lets the runtime candidate filter know which game id should
    // include this synthetic section as a candidate. 0xFFFFFFFF
    // means "not a synthetic-link pattern variant" (the section is
    // matched the normal way via ram_addr-derived id).
    uint32_t original_pattern_id;
} SectionTableEntry;

typedef struct {
    const char* name;
    uint32_t ram_addr;
} FunctionExport;

typedef struct {
    uint32_t ram_addr;
    recomp_func_t* func;
} ManualPatchSymbol;

#endif
