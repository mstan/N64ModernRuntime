#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "recompiler/context.h"
#include "overlays.hpp"
#include "sections.h"

static recomp::overlays::overlay_section_table_data_t sections_info {};
static recomp::overlays::overlays_by_index_t overlays_info {};

static SectionTableEntry* patch_code_sections = nullptr;
size_t num_patch_code_sections = 0;
static std::vector<char> patch_data;

struct LoadedSection {
    int32_t loaded_ram_addr;
    size_t section_table_index;

    LoadedSection(int32_t loaded_ram_addr_, size_t section_table_index_) {
        loaded_ram_addr = loaded_ram_addr_;
        section_table_index = section_table_index_;
    }

    bool operator<(const LoadedSection& rhs) {
        return loaded_ram_addr < rhs.loaded_ram_addr;
    }
};

static std::unordered_map<uint32_t, uint16_t> code_sections_by_rom{};
static std::unordered_map<uint32_t, uint16_t> patch_code_sections_by_rom{};
static std::vector<LoadedSection> loaded_sections{};
static std::unordered_map<int32_t, recomp_func_t*> func_map{};
static std::unordered_map<std::string, recomp_func_t*> base_exports{};
static std::unordered_map<std::string, recomp_func_ext_t*> ext_base_exports{};
static std::unordered_map<std::string, size_t> base_events;
static std::unordered_map<uint32_t, recomp_func_t*> manual_patch_symbols_by_vram;

extern "C" {
int32_t* section_addresses = nullptr;
}

void recomp::overlays::register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays) {
    sections_info = sections;
    overlays_info = overlays;
}

void recomp::overlays::register_patches(const char* patch, std::size_t size, SectionTableEntry* sections, size_t num_sections) {
    patch_code_sections = sections;
    num_patch_code_sections = num_sections;

    patch_data.resize(size);
    std::memcpy(patch_data.data(), patch, size);

    patch_code_sections_by_rom.reserve(num_patch_code_sections);
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        patch_code_sections_by_rom.emplace(patch_code_sections[i].rom_addr, i);
    }
}

void recomp::overlays::register_base_export(const std::string& name, recomp_func_t* func) {
    base_exports.emplace(name, func);
}

void recomp::overlays::register_ext_base_export(const std::string& name, recomp_func_ext_t* func) {
    ext_base_exports.emplace(name, func);
}

void recomp::overlays::register_base_exports(const FunctionExport* export_list) {
    std::unordered_map<uint32_t, recomp_func_t*> patch_func_vram_map{};

    // Iterate over all patch functions to set up a mapping of their vram address.
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const SectionTableEntry* cur_section = &patch_code_sections[patch_section_index];

        for (size_t func_index = 0; func_index < cur_section->num_funcs; func_index++) {
            const FuncEntry* cur_func = &cur_section->funcs[func_index];
            patch_func_vram_map.emplace(cur_section->ram_addr + cur_func->offset, cur_func->func);
        }
    }

    // Iterate over exports, using the vram mapping to create a name mapping.
    for (const FunctionExport* cur_export = &export_list[0]; cur_export->name != nullptr; cur_export++) {
        auto it = patch_func_vram_map.find(cur_export->ram_addr);
        if (it == patch_func_vram_map.end()) {
            assert(false && "Failed to find exported function in patch function sections!");
        }
        base_exports.emplace(cur_export->name, it->second);
    }
}

recomp_func_t* recomp::overlays::get_base_export(const std::string& export_name) {
    auto it = base_exports.find(export_name);
    if (it == base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

recomp_func_ext_t* recomp::overlays::get_ext_base_export(const std::string& export_name) {
    auto it = ext_base_exports.find(export_name);
    if (it == ext_base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

void recomp::overlays::register_base_events(char const* const* event_names) {
    for (size_t event_index = 0; event_names[event_index] != nullptr; event_index++) {
        base_events.emplace(event_names[event_index], event_index);
    }
}

size_t recomp::overlays::get_base_event_index(const std::string& event_name) {
    auto it = base_events.find(event_name);
    if (it == base_events.end()) {
        return (size_t)-1;
    }
    return it->second;
}

size_t recomp::overlays::num_base_events() {
    return base_events.size();
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_vrom_to_section_map() {
    return code_sections_by_rom;
}

uint32_t recomp::overlays::get_section_ram_addr(uint16_t code_section_index) {
    return sections_info.code_sections[code_section_index].ram_addr;
}

std::span<const RelocEntry> recomp::overlays::get_section_relocs(uint16_t code_section_index) {
    if (code_section_index < sections_info.num_code_sections) {
        const auto& section = sections_info.code_sections[code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

void recomp::overlays::add_loaded_function(int32_t ram, recomp_func_t* func) {
    func_map[ram] = func;
}

void load_overlay(size_t section_table_index, int32_t ram) {
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    // Register funcs at BOTH the runtime slot address AND the section's
    // link-time vram. Pokemon Stadium-style fragments link at unique
    // vrams (e.g. 0x82000000) but get DMAed into shared runtime slots
    // (e.g. 0x80114BF0). The kernel's fragment loader calls the freshly
    // loaded fragment ENTRY at the runtime slot address (jal slot+0).
    // Cross-fragment / intra-fragment calls captured statically by the
    // recompiler use link-time vrams. Both must dispatch correctly.
    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
        if (section.ram_addr != ram) {
            func_map[section.ram_addr + func.offset] = func.func;
        }
    }

    loaded_sections.emplace_back(ram, section_table_index);
    section_addresses[section.index] = ram;
}

// ── Fragment trampoline registration ─────────────────────────────────
//
// Some games (Pokemon Stadium and other HAL Labs N64 titles) use
// fragments laid out as:
//
//   +0x000  J entry / nop                  ← bootstrap
//   +0x008  "FRAGMENT" magic + sizes       ← 0x18 bytes of metadata
//   +0x020  J target / nop                 ┐
//   +0x028  J target / nop                 │ jump table — small
//           ...                            │ trampolines, opaque
//   +0x1F8  J target / nop                 ┘ to the recompiler
//   +0xN00  decompiled C functions begin
//
// The middle region is `textbin` in pret/pokestadium's rom.yaml — raw
// MIPS bytes the recompiler doesn't decompile. Stadium calls into the
// trampolines (`jal fragment_base + slot_offset`) to dispatch through
// to functions in *other* fragments. On real hardware the J targets
// get patched at load time by the game's relocator to point at the
// runtime addresses where their target fragments ended up.
//
// In our static recompile, those trampoline addresses are not in
// func_map, so the dispatch lookup-misses. Fix: at PI-DMA time,
// after a section's funcs have been registered, scan the textbin
// region between the header and the first decompiled function. For
// each J/JAL slot, decode the link-time target and translate it to a
// runtime address via `loaded_sections`. Register the trampoline's
// runtime address in func_map → the same recomp_func as the resolved
// target. Slots whose target sections haven't loaded yet go on a
// pending list and are retried after every subsequent section load.

struct PendingTrampoline {
    int32_t  trampoline_runtime_addr;   // RAM address of the J/nop slot
    uint32_t link_time_target;          // absolute link-time vram of target
};
static std::vector<PendingTrampoline> pending_trampolines;

// Bytewise BE u32 read with the rdram XOR-3 swap convention. RDRAM is
// stored XOR-3 swapped per the recompiler's internal layout; a logical
// big-endian word at virtual address V lives in physical bytes
// rdram[(V & 0x1FFFFFFF) ^ 3] high-to-low.
static uint32_t read_rdram_u32_be(uint8_t* rdram, int32_t vaddr) {
    uint32_t paddr = (uint32_t)vaddr & 0x1FFFFFFF;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | rdram[(paddr + i) ^ 3];
    }
    return v;
}

// Decode a J or JAL absolute target. PC of the delay slot supplies
// the high 4 bits per MIPS spec. Returns 0 (an invalid target) if
// the instruction isn't a J/JAL.
static uint32_t decode_jal_target(uint32_t instr, uint32_t pc_delay_slot) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    if (opcode != 0x02 && opcode != 0x03) return 0;     // 02=J, 03=JAL
    uint32_t target_field = instr & 0x03FFFFFF;
    return (pc_delay_slot & 0xF0000000) | (target_field << 2);
}

// Translate a link-time vram (assigned by the linker when the section
// was built) to its current runtime RAM address. Returns 0 if no
// loaded section covers the link-time address.
static int32_t translate_link_time_to_runtime(uint32_t link_time) {
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        if (link_time >= sec.ram_addr && link_time < sec.ram_addr + sec.size) {
            return ls.loaded_ram_addr + (int32_t)(link_time - sec.ram_addr);
        }
    }
    return 0;
}

// Try to install a trampoline now. Returns true if the link-time
// target resolves to a runtime address that has a recomp_func
// registered. False means the target's section isn't loaded yet (or
// the offset doesn't have a recomp function).
static bool try_install_trampoline(int32_t trampoline_runtime_addr, uint32_t link_time_target) {
    int32_t runtime_target = translate_link_time_to_runtime(link_time_target);
    if (runtime_target == 0) return false;
    auto it = func_map.find(runtime_target);
    if (it == func_map.end()) return false;
    func_map[trampoline_runtime_addr] = it->second;
    return true;
}

// Walk the pending list and try to resolve any trampolines whose
// targets are now loaded. Called after every section load.
static void retry_pending_trampolines() {
    auto it = pending_trampolines.begin();
    while (it != pending_trampolines.end()) {
        if (try_install_trampoline(it->trampoline_runtime_addr, it->link_time_target)) {
            it = pending_trampolines.erase(it);
        } else {
            ++it;
        }
    }
}

// Scan one fragment-shaped section's trampoline range. `runtime_base`
// is the section's loaded_ram_addr (where it lives in RAM right now).
static void scan_fragment_section_trampolines(uint8_t* rdram, size_t section_index, int32_t runtime_base) {
    const SectionTableEntry& section = sections_info.code_sections[section_index];

    // Heuristic: only treat as a fragment if the "FRAGMENT" magic is
    // at runtime_base+8. Cheap and avoids false positives for
    // non-fragment sections (boot, the resident kernel, audio data,
    // patches).
    if (read_rdram_u32_be(rdram, runtime_base + 8)  != 0x46524147) return; // "FRAG"
    if (read_rdram_u32_be(rdram, runtime_base + 12) != 0x4D454E54) return; // "MENT"

    // The trampoline range is [0x20, first_real_func_offset). The
    // section's func table excludes the textbin region — look for the
    // smallest non-zero offset in the FuncEntry list.
    uint32_t first_func_offset = section.size;
    for (size_t i = 0; i < section.num_funcs; i++) {
        uint32_t off = section.funcs[i].offset;
        if (off > 0 && off < first_func_offset) first_func_offset = off;
    }
    if (first_func_offset <= 0x20) return;  // nothing between header and first func

    // Walk 8-byte slots. Each slot is { instr, nop } per Stadium's
    // convention. Non-conforming slots (no nop in delay slot) are
    // skipped — they're probably padding or data, not trampolines.
    for (uint32_t slot_off = 0x20; slot_off + 8 <= first_func_offset; slot_off += 8) {
        uint32_t instr = read_rdram_u32_be(rdram, runtime_base + (int32_t)slot_off);
        uint32_t delay = read_rdram_u32_be(rdram, runtime_base + (int32_t)slot_off + 4);
        if (delay != 0) continue;

        uint32_t pc_delay = section.ram_addr + slot_off + 4;
        uint32_t target = decode_jal_target(instr, pc_delay);
        if (target == 0) continue;

        int32_t tramp_addr = runtime_base + (int32_t)slot_off;
        if (!try_install_trampoline(tramp_addr, target)) {
            pending_trampolines.push_back({tramp_addr, target});
        }
    }
}

void recomp::overlays::scan_loaded_fragment_trampolines(uint8_t* rdram, uint32_t rom, int32_t ram_addr, uint32_t size) {
    // Iterate sections we just loaded as part of this DMA. A section
    // counts as "just loaded" if its rom_addr falls in [rom, rom+size)
    // OR if rom falls inside the section (chunked-load case). We're
    // conservative and walk loaded_sections, picking ones whose
    // section.rom_addr overlaps.
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        bool overlaps =
            (sec.rom_addr < rom + size) && (sec.rom_addr + sec.size > rom);
        if (!overlaps) continue;

        scan_fragment_section_trampolines(rdram, ls.section_table_index, ls.loaded_ram_addr);
    }

    // Whether or not any new trampolines were registered, retry any
    // pending ones — the section that just loaded may have been the
    // missing target for trampolines registered earlier.
    retry_pending_trampolines();
}

// ---- Stadium runtime fragment registration -------------------------------
//
// Stadium has two fragment-load paths:
//   (1) PI DMA → load_overlays() → registers funcs in func_map at the
//       runtime address each section was DMA'd to. Trampoline scan also
//       fires here.
//   (2) CPU-side yay0 decompression → bytes appear in RDRAM by direct
//       stores from recompiled libultra/main code, never going through
//       do_dma. The PI-DMA-based hooks above never see these.
//
// Both paths converge on Memmap_RelocateFragment(id, fragment_ptr) before
// the fragment is dispatched. That's the right place to register the
// runtime address mapping for path (2). For path (1), this is a no-op or
// re-registers the same mapping — safe.
//
// Stadium's `id` is the same encoding Memmap_GetFragmentVaddr uses:
//   id = ((link_time_vram & 0x0FF00000) >> 0x14) - 0x10
// To find which section in our static section_table corresponds to this
// id, we apply the same formula to each section.ram_addr (the link-time
// vram baked into the recompiled output) and match.
// FNV-1a 64-bit. Same algorithm the build-time recompiler uses to hash
// pattern-synthesized section bodies — they MUST match for runtime
// content-keyed dispatch to find the right section.
static uint64_t fnv1a_64(const uint8_t* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= uint64_t(data[i]);
        h *= 0x100000001B3ull;
    }
    return h;
}

void recomp::overlays::register_runtime_fragment(uint8_t* rdram, uint32_t id, int32_t fragment_ptr) {
    if (sections_info.code_sections == nullptr) return;
    if (fragment_ptr == 0) return;

    // Find candidate sections sharing this link-time vram bucket. With
    // pattern-synthesized sections, multiple sections can share a bucket.
    // We collect them all and pick by content hash below.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < sections_info.num_code_sections; i++) {
        const SectionTableEntry& sec = sections_info.code_sections[i];
        uint32_t bucket = (sec.ram_addr & 0x0FF00000u) >> 0x14;
        if (bucket < 0x10) continue;
        uint32_t sec_id = bucket - 0x10;
        if (sec_id == id) {
            candidates.push_back(i);
        }
    }

    size_t found_index = (size_t)-1;
    if (candidates.size() == 1) {
        // Common case: exactly one section per bucket (every static
        // overlay + the single-block decompressed_section path).
        found_index = candidates.front();
    } else if (candidates.size() > 1) {
        // Pattern-synthesized case: hash the bytes Stadium just put
        // in RDRAM at fragment_ptr, look up the matching section by
        // content_hash. Window must match the build-time hash window
        // in N64Recomp's decompressed.cpp (currently 0x100, fnv1a-64).
        constexpr size_t HASH_WINDOW = 0x100;
        uint8_t window[HASH_WINDOW];
        const uint32_t paddr = uint32_t(fragment_ptr) & 0x1FFFFFFFu;
        // Read with the recompiler's XOR-3 byte-order convention so
        // we observe the same bytes the build-time hash saw.
        for (size_t i = 0; i < HASH_WINDOW; i++) {
            window[i] = rdram[(paddr + i) ^ 3];
        }
        uint64_t live_hash = fnv1a_64(window, HASH_WINDOW);

        for (size_t ci : candidates) {
            const SectionTableEntry& sec = sections_info.code_sections[ci];
            if (sec.content_hash != 0 && sec.content_hash == live_hash) {
                found_index = ci;
                break;
            }
        }

        if (found_index == (size_t)-1) {
            // Hash didn't match any candidate. Fall back to the first
            // candidate — won't be correct dispatch for this fragment,
            // but lets the run continue. Log so post-mortem can spot
            // the residual ~5% the 0x100-byte window misses.
            fprintf(stderr,
                "[reg-frag] no content-hash match for id=0x%X "
                "fragment_ptr=0x%08X live_hash=0x%016llX "
                "(%zu candidates) — picking first\n",
                id, (uint32_t)fragment_ptr,
                (unsigned long long)live_hash,
                candidates.size());
            fflush(stderr);
            found_index = candidates.front();
        }
    }

    if (found_index == (size_t)-1) {
        // Not one of our recompiled fragments — a HAL fragment present
        // in ROM that pret hasn't split out as code. Stadium has at
        // least one such fragment at ROM 0x1206400 (inside the
        // pokemon_models bin segment, link-vram 0xFF000000), plus
        // an id=0xC0 buddy. Without recompiled C for the body we have
        // no good way to dispatch it. A noop stub at fragment_ptr
        // (return 0 in $v0/$v1) was tried and led to a silent
        // downstream failure — so we leave the lookup-miss trampoline
        // as the visible failure mode. Real fix lives upstream in
        // pret/pokestadium (split this region as a code subsegment).
        fprintf(stderr, "[reg-frag] unrecompiled fragment id=0x%X (link-vram bucket 0x%X) at runtime 0x%08X — needs pret split\n",
                id, id + 0x10, (uint32_t)fragment_ptr); fflush(stderr);
        return;
    }

    const SectionTableEntry& section = sections_info.code_sections[found_index];

    // Diagnostic: log every successful registration so we can correlate
    // load order with crash post-mortem. Single line per fragment, includes
    // the link-time vram bucket so it's clear which pret fragment.
    fprintf(stderr,
        "[reg-frag] id=0x%X bucket=0x%X link=0x%08X runtime=0x%08X size=0x%X\n",
        id, id + 0x10, section.ram_addr, (uint32_t)fragment_ptr, section.size);
    fflush(stderr);

    // Register every FuncEntry at both the runtime address and the
    // section's link-time vram (mirroring load_overlay). The link-time
    // alias is what scan_fragment_section_trampolines uses to resolve
    // J/JAL targets back to a recomp_func — without it the scan can't
    // install +0x00 dispatch trampolines. The runtime alias is what
    // dispatch goes through after Stadium hands out a fragment_ptr.
    for (size_t fi = 0; fi < section.num_funcs; fi++) {
        const FuncEntry& fe = section.funcs[fi];
        if (fe.func == nullptr) continue;
        func_map[fragment_ptr + (int32_t)fe.offset] = fe.func;
        if ((int32_t)section.ram_addr != fragment_ptr) {
            func_map[(int32_t)section.ram_addr + (int32_t)fe.offset] = fe.func;
        }
    }

    // Update section_addresses so reloc-driven RELOC_HI16/LO16 macros
    // resolve to the runtime base.
    if (section_addresses != nullptr) {
        section_addresses[section.index] = fragment_ptr;
    }

    // Track in loaded_sections so unload paths (and the diagnostic dump)
    // see this fragment. Avoid duplicate entries if Memmap_RelocateFragment
    // is invoked more than once for the same id (rare but possible).
    auto find_existing = std::find_if(loaded_sections.begin(), loaded_sections.end(),
        [found_index](const LoadedSection& s) { return s.section_table_index == found_index; });
    if (find_existing == loaded_sections.end()) {
        loaded_sections.emplace_back(fragment_ptr, found_index);
    } else {
        find_existing->loaded_ram_addr = fragment_ptr;
    }

    // Run the textbin trampoline scanner on the fragment, same as the
    // DMA path. Resolves +0x00 J-slot dispatch and any in-header
    // trampolines that point at sibling fragments.
    scan_fragment_section_trampolines(rdram, found_index, fragment_ptr);

    // Pending trampolines may now resolve.
    retry_pending_trampolines();
}

static void load_special_overlay(const SectionTableEntry& section, int32_t ram) {
    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
    }
}

static void load_patch_functions() {
    if (patch_code_sections == nullptr) {
        debug_printf("[Patch] No patch section was registered\n");
        return;
    }
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        load_special_overlay(patch_code_sections[i], patch_code_sections[i].ram_addr);
    }
}

void recomp::overlays::read_patch_data(uint8_t* rdram, gpr patch_data_address) {
    for (size_t i = 0; i < patch_data.size(); i++) {
        MEM_B(i, patch_data_address) = patch_data[i];
    }
}

// Forward declaration — definition is below alongside unload_overlay_by_id.
static void unload_overlay_by_section_index(uint32_t section_table_index);

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size) {
    // Two registration patterns coexist here:
    //
    //  (a) A single DMA covers an entire section (small fragments, the
    //      Zelda style — ROM contiguous, runtime address picked by
    //      libultra at load time, single PI transfer for the whole
    //      section). Find all sections fully inside [rom, rom+size)
    //      and register each.
    //
    //  (b) A single SECTION is built up by multiple smaller DMAs
    //      (Pokemon Stadium's 77-fragment system: fragment 17 is
    //      ~58 KB and gets streamed in 0x1000-byte chunks; each chunk
    //      falls *inside* the section). Find any section that the
    //      DMA range overlaps and register it on the first chunk —
    //      subsequent chunks just write more bytes into the same
    //      already-registered section's RAM body.
    //
    // The previous implementation only handled (a) and silently
    // returned when the DMA range fell inside a section (lower >=
    // upper). This worked for libleo / audio chunked reads (they
    // re-DMA into already-registered space) but not for Stadium's
    // chunked initial section loads.

    // Helper: register section at the runtime base implied by this
    // chunk, but only if not already loaded there. Re-registering at
    // the same base is wasted work; loading at a NEW base is a
    // re-relocation handled by unload+reload.
    auto register_if_new = [](size_t section_index, int32_t implied_base) {
        auto find_it = std::find_if(loaded_sections.begin(), loaded_sections.end(),
            [section_index](const LoadedSection& s) { return s.section_table_index == section_index; });
        if (find_it == loaded_sections.end()) {
            // First time this section is being loaded.
            load_overlay(section_index, implied_base);
        } else if (find_it->loaded_ram_addr != implied_base) {
            // Section is already loaded but at a different runtime
            // address — game has relocated it. Unregister the old
            // mapping and re-register at the new base.
            unload_overlay_by_section_index(section_index);
            load_overlay(section_index, implied_base);
        }
        // else: same section already registered at same base; skip.
    };

    auto* sections_begin = &sections_info.code_sections[0];
    auto* sections_end   = &sections_info.code_sections[sections_info.num_code_sections];

    // First handle case (a): sections wholly inside [rom, rom+size).
    // lower = first section with rom_addr >= rom.
    auto lower = std::lower_bound(sections_begin, sections_end, rom,
        [](const SectionTableEntry& entry, uint32_t addr) {
            return entry.rom_addr < addr;
        });
    // upper_a = first section with rom_addr > (rom + size). These are
    // the sections fully contained in [rom, rom+size).
    auto upper_a = std::upper_bound(sections_begin, sections_end, (uint32_t)(rom + size),
        [](uint32_t addr, const SectionTableEntry& entry) {
            return addr < entry.rom_addr;
        });
    for (auto it = lower; it != upper_a; ++it) {
        // Skip sections that don't fully fit in this DMA — those are
        // the chunked-load sections, handled below.
        if (it->rom_addr + it->size > rom + size) continue;
        register_if_new(std::distance(sections_begin, it),
                        it->rom_addr - rom + ram_addr);
    }

    // Now handle case (b): the DMA range may be a chunk of a larger
    // section. The section containing `rom` is either at `lower`
    // (when rom == lower->rom_addr exactly) or at `lower - 1` (when
    // lower advanced past the containing section).
    if (lower != sections_begin) {
        auto* candidate = lower - 1;
        if (rom >= candidate->rom_addr && rom < candidate->rom_addr + candidate->size) {
            int32_t implied_base = (int32_t)ram_addr - (int32_t)(rom - candidate->rom_addr);
            register_if_new(std::distance(sections_begin, candidate), implied_base);
        }
    }
    if (lower != sections_end && lower->rom_addr == rom) {
        // DMA starts exactly at section boundary — but the section
        // might still be larger than the DMA chunk (case (b) flavor
        // where the first chunk of a multi-chunk section is being
        // loaded).
        if (lower->rom_addr + lower->size > rom + size) {
            register_if_new(std::distance(sections_begin, lower),
                            lower->rom_addr - rom + ram_addr);
        }
    }
}

// Internal helper — unload a section by its section-table index
// (not its overlay id). Both unload_overlay_by_id (public) and
// load_overlays' relocation path (for re-registration) use this.
static void unload_overlay_by_section_index(uint32_t section_table_index) {
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    auto find_it = std::find_if(loaded_sections.begin(), loaded_sections.end(), [section_table_index](const LoadedSection& s) { return s.section_table_index == section_table_index; });

    if (find_it != loaded_sections.end()) {
        // Mirror load_overlay: funcs were registered at both the runtime
        // slot address and the section's link-time vram.
        for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
            const auto& func = section.funcs[func_index];
            func_map.erase(func.offset + find_it->loaded_ram_addr);
            if (section.ram_addr != find_it->loaded_ram_addr) {
                func_map.erase(func.offset + section.ram_addr);
            }
        }
        // Reset the section's address in the address table
        section_addresses[section.index] = section.ram_addr;
        // Remove the section from the loaded section map
        loaded_sections.erase(find_it);
    }
}

extern "C" void unload_overlay_by_id(uint32_t id) {
    uint32_t section_table_index = overlays_info.table[id];
    unload_overlay_by_section_index(section_table_index);
}

extern "C" void load_overlay_by_id(uint32_t id, uint32_t ram_addr) {
    uint32_t section_table_index = overlays_info.table[id];
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];
    int32_t prev_address = section_addresses[section.index];
    if (/*ram_addr >= 0x80000000 && ram_addr < 0x81000000) {*/ prev_address == section.ram_addr) {
        load_overlay(section_table_index, ram_addr);
    }
    else {
        int32_t new_address = prev_address + ram_addr;
        unload_overlay_by_id(id);
        load_overlay(section_table_index, new_address);
    }
}

extern "C" void unload_overlays(int32_t ram_addr, uint32_t size) {
    for (auto it = loaded_sections.begin(); it != loaded_sections.end();) {
        const auto& section = sections_info.code_sections[it->section_table_index];

        // Check if the unloaded region overlaps with the loaded section
        if (ram_addr < (it->loaded_ram_addr + section.size) && (ram_addr + size) >= it->loaded_ram_addr) {
            // Check if the section isn't entirely in the loaded region
            if (ram_addr > it->loaded_ram_addr || (ram_addr + size) < (it->loaded_ram_addr + section.size)) {
                fprintf(stderr,
                    "Cannot partially unload section\n"
                    "  rom: 0x%08X size: 0x%08X loaded_addr: 0x%08X\n"
                    "  unloaded_ram: 0x%08X unloaded_size : 0x%08X\n",
                        section.rom_addr, section.size, it->loaded_ram_addr, ram_addr, size);
                assert(false);
                std::exit(EXIT_FAILURE);
            }
            // Mirror load_overlay: funcs were registered at both the
            // runtime slot address and the section's link-time vram.
            for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
                const auto& func = section.funcs[func_index];
                func_map.erase(func.offset + it->loaded_ram_addr);
                if (section.ram_addr != it->loaded_ram_addr) {
                    func_map.erase(func.offset + section.ram_addr);
                }
            }
            // Reset the section's address in the address table
            section_addresses[section.index] = section.ram_addr;
            // Remove the section from the loaded section map
            it = loaded_sections.erase(it);
            // Skip incrementing the iterator
            continue;
        }
        ++it;
    }
}

void recomp::overlays::init_overlays() {
    func_map.clear();
    section_addresses = (int32_t *)calloc(sections_info.total_num_sections, sizeof(int32_t));

    // Sort the executable sections by rom address
    std::sort(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections],
        [](const SectionTableEntry& a, const SectionTableEntry& b) {
            return a.rom_addr < b.rom_addr;
        }
    );

    for (size_t section_index = 0; section_index < sections_info.num_code_sections; section_index++) {
        SectionTableEntry* code_section = &sections_info.code_sections[section_index];

        section_addresses[sections_info.code_sections[section_index].index] = code_section->ram_addr;
        code_sections_by_rom[code_section->rom_addr] = section_index;        
    }

    load_patch_functions();
}

// Finds a function given a section's index and the function's offset into the section.
bool recomp::overlays::get_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (code_section_index >= sections_info.num_code_sections) {
        return false;
    }

    SectionTableEntry* section = &sections_info.code_sections[code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

void recomp::overlays::register_manual_patch_symbols(const ManualPatchSymbol* manual_patch_symbols) {
    for (size_t i = 0; manual_patch_symbols[i].func != nullptr; i++) {
        if (!manual_patch_symbols_by_vram.emplace(manual_patch_symbols[i].ram_addr, manual_patch_symbols[i].func).second) {
            printf("Duplicate manual patch symbol address: %08X\n", manual_patch_symbols[i].ram_addr);
            ultramodern::error_handling::message_box("Duplicate manual patch symbol address (syms.ld)!");
            assert(false && "Duplicate manual patch symbol address (syms.ld)!");
            ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
        }
    }
}

// TODO use N64Recomp::is_manual_patch_symbol instead after updating submodule.
bool is_manual_patch_symbol(uint32_t vram) {
    return vram >= 0x8F000000 && vram < 0x90000000;
}

// Finds a function given a section's index and the function's offset into the section and returns its native pointer.
recomp_func_t* recomp::overlays::get_func_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset) {
    FuncEntry entry;
    
    if (get_func_entry_by_section_index_function_offset(code_section_index, function_offset, entry)) {
        return entry.func;
    }

    if (code_section_index == N64Recomp::SectionAbsolute && is_manual_patch_symbol(function_offset)) {
        auto find_it = manual_patch_symbols_by_vram.find(function_offset);
        if (find_it != manual_patch_symbols_by_vram.end()) {
            return find_it->second;
        }
    }

    return nullptr;
}

// Finds a function given a section's rom address and the function's vram address.
recomp_func_t* recomp::overlays::get_func_by_section_rom_function_vram(uint32_t section_rom, uint32_t function_vram) {
    auto find_section_it = code_sections_by_rom.find(section_rom);
    if (find_section_it == code_sections_by_rom.end()) {
        return nullptr;
    }

    SectionTableEntry* section = &sections_info.code_sections[find_section_it->second];
    int32_t func_offset = function_vram - section->ram_addr;
    
    return get_func_by_section_index_function_offset(find_section_it->second, func_offset);
}

// Tolerant-emit companion: when an indirect call lands at an address
// the recompiler didn't generate a function for, log it (file + stderr)
// and return a "trampoline" that aborts loudly when called — instead
// of immediately killing the process. The caller still crashes if it
// actually invokes the function pointer; but the lookup itself
// returns, so static initializers / table walks finish cleanly.
//
// Per project principles: not a stub. The trampoline doesn't simulate
// behavior — it surfaces "execution reached unimplemented code" with
// full address context. Surfaces are richer than std::exit().
static void unhandled_lookup_trampoline(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    fprintf(stderr, "[recomp] lookup-miss trampoline reached — aborting\n");
    std::abort();
}

// C-linkage wrapper for the runtime fragment registrar so it can be
// invoked from a recompiled-function text hook (game.toml [[patches.hook]]).
// Hooks run inside the recompiled function with `rdram` and `ctx` in
// scope; see the Memmap_RelocateFragment hooks in pret/pokestadium's
// game.toml.
extern "C" void recomp_register_runtime_fragment(uint8_t* rdram, uint32_t id, int32_t fragment_ptr) {
    recomp::overlays::register_runtime_fragment(rdram, id, fragment_ptr);
}

extern "C" recomp_func_t * get_function(int32_t addr) {
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        FILE* f = fopen("F:/Projects/PokemonStadiumRecomp/build/last_error.log", "a");
        if (f) {
            fprintf(f, "\n=== get_function lookup miss: 0x%08X ===\n", addr);
            fclose(f);
        }
        fprintf(stderr, "[Warn] get_function lookup miss: 0x%08X — returning trampoline\n", addr);
        fflush(stderr);
        return unhandled_lookup_trampoline;
    }
    return func_find->second;
}

std::unordered_map<recomp_func_t*, recomp::overlays::BasePatchedFunction> recomp::overlays::get_base_patched_funcs() {
    std::unordered_map<recomp_func_t*, BasePatchedFunction> ret{};

    // Collect the set of all functions in the patches.
    std::unordered_map<recomp_func_t*, BasePatchedFunction> all_patch_funcs{};
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const auto& patch_section = patch_code_sections[patch_section_index];
        for (size_t func_index = 0; func_index < patch_section.num_funcs; func_index++) {
            all_patch_funcs.emplace(patch_section.funcs[func_index].func, BasePatchedFunction{ .patch_section = patch_section_index, .function_index = func_index });
        }
    }

    // Check every vanilla function against the full patch function set.
    // Any functions in both are patched.
    for (size_t code_section_index = 0; code_section_index < sections_info.num_code_sections; code_section_index++) {
        const auto& code_section = sections_info.code_sections[code_section_index];
        for (size_t func_index = 0; func_index < code_section.num_funcs; func_index++) {
            recomp_func_t* cur_func = code_section.funcs[func_index].func;
            // If this function also exists in the patches function set then it's a vanilla function that was patched.
            auto find_it = all_patch_funcs.find(cur_func);
            if (find_it != all_patch_funcs.end()) {
                ret.emplace(cur_func, find_it->second);
            }
        }
    }

    return ret;
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_patch_vrom_to_section_map() {
    return patch_code_sections_by_rom;
}

uint32_t recomp::overlays::get_patch_section_ram_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].ram_addr;
    }
    assert(false);
    return -1;
}

uint32_t recomp::overlays::get_patch_section_rom_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].rom_addr;
    }
    assert(false);
    return -1;
}

const FuncEntry* recomp::overlays::get_patch_function_entry(uint16_t patch_code_section_index, size_t function_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        if (function_index < section.num_funcs) {
            return &section.funcs[function_index];
        }
    }
    assert(false);
    return nullptr;
}

// Finds a base patched function given a patch section's index and the function's offset into the section.
bool recomp::overlays::get_patch_func_entry_by_section_index_function_offset(uint16_t patch_code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (patch_code_section_index >= num_patch_code_sections) {
        return false;
    }

    SectionTableEntry* section = &patch_code_sections[patch_code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

std::span<const RelocEntry> recomp::overlays::get_patch_section_relocs(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

std::span<const uint8_t> recomp::overlays::get_patch_binary() {
    return std::span{ reinterpret_cast<const uint8_t*>(patch_data.data()), patch_data.size() };
}
