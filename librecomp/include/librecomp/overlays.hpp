// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Public surface for register_runtime_fragment (CPU-decompressed
//     fragments) and HAL-fragment trampoline synthesis at load time.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#ifndef __RECOMP_OVERLAYS_H__
#define __RECOMP_OVERLAYS_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <span>
#include "sections.h"

namespace recomp {
    namespace overlays {
        struct overlay_section_table_data_t {
            SectionTableEntry* code_sections;
            size_t num_code_sections;
            size_t total_num_sections;
        };

        struct overlays_by_index_t {
            int* table;
            size_t len;
        };

        void register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays);

        void register_patches(const char* patch_data, size_t patch_size, SectionTableEntry* code_sections, size_t num_sections);
        void register_base_export(const std::string& name, recomp_func_t* func);
        void register_ext_base_export(const std::string& name, recomp_func_ext_t* func);
        void register_base_exports(const FunctionExport* exports);
        void register_base_events(char const* const* event_names);
        void register_manual_patch_symbols(const ManualPatchSymbol* manual_patch_symbols);
        void read_patch_data(uint8_t* rdram, gpr patch_data_address);

        void init_overlays();
        const std::unordered_map<uint32_t, uint16_t>& get_vrom_to_section_map();
        uint32_t get_section_ram_addr(uint16_t code_section_index);
        std::span<const RelocEntry> get_section_relocs(uint16_t code_section_index);
        recomp_func_t* get_func_by_section_rom_function_vram(uint32_t section_rom, uint32_t function_vram);
        bool get_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out);
        recomp_func_t* get_func_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset);
        recomp_func_t* get_base_export(const std::string& export_name);
        recomp_func_ext_t* get_ext_base_export(const std::string& export_name);
        size_t get_base_event_index(const std::string& event_name);
        size_t num_base_events();

        void add_loaded_function(int32_t ram_addr, recomp_func_t* func);

        // Stadium-style runtime trampoline scan. Called by the PI DMA
        // path right after a `load_overlays` returns. Looks for the
        // "header + jump_table + funcs" fragment layout HAL Labs' games
        // use (see project_chunked_overlays_and_trampolines.md in
        // memory) and registers a func_map entry for every J/JAL slot
        // in the textbin region between the header and the first
        // decompiled function. Targets that reference not-yet-loaded
        // sections are queued and resolved when the target section
        // loads. No-op for games that don't use this layout.
        void scan_loaded_fragment_trampolines(uint8_t* rdram, uint32_t rom, int32_t ram_addr, uint32_t size);

        // Stadium-style runtime fragment registration. Called from a
        // Memmap_RelocateFragment hook (see project memory file
        // project_runtime_fragment_registration.md). Stadium loads
        // some fragments via CPU-side yay0 decompression that bypasses
        // PI DMA, so the load_overlays path never sees them. After the
        // fragment is in RDRAM and relocated, this function is
        // responsible for registering all of its FuncEntry rows in
        // func_map at (fragment_ptr + offset). It also runs the
        // trampoline scanner over the fragment's textbin region.
        //
        // `id` is Stadium's encoded fragment id (matches the bucket
        // used in Memmap_GetFragmentVaddr: bits 27:20 of the fragment's
        // link-time vram, minus 0x10). The function looks up the
        // matching section in the recompiled section_table by
        // computing the same bucket from each section's link-time
        // ram_addr.
        void register_runtime_fragment(uint8_t* rdram, uint32_t id, int32_t fragment_ptr);

        // Symmetric counterpart to register_runtime_fragment, called from
        // a Memmap_ClearFragmentMemmap hook. Releases the section last
        // registered for this fragment id and resets its
        // section_addresses[] entry to the link-time literal, so
        // reloc-driven RELOC_HI16/LO16 stop resolving to the stale
        // runtime base once the fragment is no longer resident (matching
        // Memmap_GetFragmentVaddr's NULL-fallback behaviour).
        void unregister_runtime_fragment(uint32_t id);

        struct BasePatchedFunction {
            size_t patch_section;
            size_t function_index;
        };

        std::unordered_map<recomp_func_t*, BasePatchedFunction> get_base_patched_funcs();
        const std::unordered_map<uint32_t, uint16_t>& get_patch_vrom_to_section_map();
        uint32_t get_patch_section_ram_addr(uint16_t patch_code_section_index);
        uint32_t get_patch_section_rom_addr(uint16_t patch_code_section_index);
        const FuncEntry* get_patch_function_entry(uint16_t patch_code_section_index, size_t function_index);
        bool get_patch_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out);
        std::span<const RelocEntry> get_patch_section_relocs(uint16_t patch_code_section_index);
        std::span<const uint8_t> get_patch_binary();
    }
};

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

#endif
