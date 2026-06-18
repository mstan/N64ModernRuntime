// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with upstream authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - register_runtime_fragment for CPU-decompressed fragments and
//     for sections DMA-loaded in chunks; HAL-fragment trampoline
//     synthesis at load time.
//   - Content-hash dispatch in register_runtime_fragment (Shape A
//     runtime side); link-time vram alias registration; J-trampoline
//     fallback.
//   - Synthetic-fragment resolver + per-pattern-id candidate filter;
//     recomp_resolve_via_data_context for data-context fragment-vaddr
//     resolution; recomp_addr_in_loaded_variant for variant-presence
//     check; caller-context fragment-vaddr resolution.
//   - func_map shared_mutex (paired with do_send corruption guard
//     in ultramodern); env-driven func_map probe; variant-candidate
//     probe for pattern-bucket addresses.
//   - Eviction fixes (remove old loaded_sections entry, evict prior
//     section's func_map entries before re-registration).
//   - J-trampoline range-match tiebreaker for hash-miss dispatch
//     (resolves Free Battle entry blocker).
//   - Lookup-miss trampoline enriched with addr + trace ring + host
//     backtrace; per-frag relocation logging.
//   - Framework-level libultra ring + RSP watchdog + boot fixes for
//     PokemonStadium boot sequencing.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"
#include "overlays.hpp"
#include "sections.h"
#include "json/json.hpp"
#include <memory>
#include <sstream>
#include <thread>
#include <chrono>

// B3 runtime-JIT tier dependencies. discover_function_bounds lives in
// N64Recomp's private src/analysis.h (not on librecomp's include path) but
// is compiled into the N64Recomp lib librecomp links — forward-declare it.
// The cop0/switch/break handlers are extern "C" in librecomp/src/recomp.cpp.
namespace N64Recomp {
    bool discover_function_bounds(const uint8_t* body, size_t bytes_size,
        uint32_t vram_base, uint32_t entry_offset,
        size_t& size_out, std::string& error_out);
}
extern "C" void cop0_status_write(recomp_context* ctx, gpr value);
extern "C" gpr cop0_status_read(recomp_context* ctx);
extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
extern "C" void do_break(uint32_t vram);

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
// Stadium fragment id -> section_table index, recorded by
// register_runtime_fragment so the symmetric unregister (driven by the
// game's Memmap_ClearFragmentMemmap) can find the section to release and
// reset its section_addresses[] entry. Mirrors gFragments[]: one slot
// per id. See unregister_runtime_fragment.
static std::unordered_map<uint32_t, size_t> runtime_fragment_id_to_section{};
static std::unordered_map<int32_t, recomp_func_t*> func_map{};
// Single-writer / multi-reader lock for func_map. get_function() runs
// on every recompiled-function indirect call (audio thread, gfx thread,
// game thread, all of them, very hot). Mutators are overlay loaders /
// runtime-fragment registrars on the PI thread and (on the game thread
// at frame boundaries). Without this lock, std::unordered_map's rehash
// during insert can return junk to a concurrent find() — observed on
// 2026-05-03 as Stadium quick-battle audio dispatch returning 0 / 0xB8
// from n_alFxPull (handler resolved to wrong host pointer mid-rehash).
// Likely also explains a class of intermittent boot-time aspMain_impl
// SEGV crashes during heavy overlay loading.
static std::shared_mutex func_map_mutex;

static bool section_original_fragment_base(const SectionTableEntry& sec,
                                           uint32_t& base_out);
static void install_section_func_aliases(const SectionTableEntry& section,
                                         int32_t runtime_base,
                                         const FuncEntry& func);
static void erase_section_func_aliases(const SectionTableEntry& section,
                                       int32_t runtime_base,
                                       const FuncEntry& func);
static void erase_fragment_slot_aliases(const SectionTableEntry& section,
                                        int32_t runtime_base,
                                        uint32_t offset);

// Recursive helper: writer entry points like register_runtime_fragment
// call internal helpers (scan_fragment_section_trampolines,
// retry_pending_trampolines, try_install_trampoline) that ALSO touch
// func_map. std::shared_mutex isn't recursive, so guard each via this
// scope helper which only acquires the underlying lock at the
// outermost depth on a given thread. Inner scopes are no-ops.
namespace {
class FuncMapWriteLock {
    bool acquired_ = false;
public:
    FuncMapWriteLock() {
        if (s_write_depth == 0) {
            func_map_mutex.lock();
            acquired_ = true;
        }
        s_write_depth++;
    }
    ~FuncMapWriteLock() {
        s_write_depth--;
        if (acquired_) {
            func_map_mutex.unlock();
        }
    }
private:
    static thread_local int s_write_depth;
};
thread_local int FuncMapWriteLock::s_write_depth = 0;
}  // anon namespace

// Forward declarations — definitions live further down with the rest
// of the host-PC index machinery.
static void pc_index_register(recomp_func_t* func, size_t section_index);
static std::unordered_map<std::string, recomp_func_t*> base_exports{};
static std::unordered_map<std::string, recomp_func_ext_t*> ext_base_exports{};
static std::unordered_map<std::string, size_t> base_events;
static std::unordered_map<uint32_t, recomp_func_t*> manual_patch_symbols_by_vram;

extern "C" {
int32_t* section_addresses = nullptr;
}

void recomp::overlays::register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays) {
    FuncMapWriteLock _fml;
    sections_info = sections;
    overlays_info = overlays;

    // Populate pc_index with EVERY recompiled function across every
    // section, so caller-context resolution can map a host return PC
    // back to the section that hosts it. Statically-loaded base
    // sections never go through load_overlay/register_runtime_fragment,
    // so we'd otherwise miss them entirely. Section_table_index is a
    // valid identifier for static sections too — they just lack a
    // load_order entry, but pc_index_lookup doesn't need that.
    for (size_t si = 0; si < sections.num_code_sections; si++) {
        const SectionTableEntry& sec = sections.code_sections[si];
        for (size_t fi = 0; fi < sec.num_funcs; fi++) {
            pc_index_register(sec.funcs[fi].func, si);
        }
    }
}

void recomp::overlays::register_patches(const char* patch, std::size_t size, SectionTableEntry* sections, size_t num_sections) {
    FuncMapWriteLock _fml;
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
    FuncMapWriteLock _fml;
    func_map[ram] = func;
}

// ── Host-PC → section_table_index index ──────────────────────────────
//
// Lets a host-side stack walker map a return PC inside a recompiled
// MIPS function back to the section that hosts that function. Used
// by recomp_resolve_fragment_via_caller_pc to disambiguate which
// pattern variant a Memmap_GetFragmentVaddr call is coming from
// when the same fragment id has multiple registered variants.
//
// Stored as a flat sorted vector of (host_pc, section_index) so we
// can binary-search by PC. Rebuilt lazily on next lookup after any
// new registration; sort cost amortizes over the steady-state where
// registrations are rare relative to lookups.
struct PcRange {
    uintptr_t host_pc;
    size_t    section_index;
};
static std::vector<PcRange> pc_to_section_sorted{};
static bool pc_index_dirty = false;

static void pc_index_register(recomp_func_t* func, size_t section_index) {
    if (func == nullptr) return;
    pc_to_section_sorted.push_back({(uintptr_t)func, section_index});
    pc_index_dirty = true;
}

static void pc_index_rebuild_if_dirty() {
    if (!pc_index_dirty) return;
    std::sort(pc_to_section_sorted.begin(), pc_to_section_sorted.end(),
        [](const PcRange& a, const PcRange& b) { return a.host_pc < b.host_pc; });
    pc_index_dirty = false;
}

// Given a host return PC, find the section it lives in. Returns
// section_table_index or size_t(-1) if no fragment function contains
// this PC. Uses upper_bound to find the largest function start ≤ pc;
// since we don't track function size, we accept any function within
// a heuristic 64 KiB window (recompiled MIPS functions are well
// below this — typical max is a few KiB of host code per MIPS func).
static size_t pc_index_lookup(uintptr_t pc) {
    pc_index_rebuild_if_dirty();
    if (pc_to_section_sorted.empty()) return size_t(-1);
    auto it = std::upper_bound(pc_to_section_sorted.begin(),
                               pc_to_section_sorted.end(), pc,
        [](uintptr_t lhs, const PcRange& rhs) { return lhs < rhs.host_pc; });
    if (it == pc_to_section_sorted.begin()) return size_t(-1);
    --it;
    if (pc - it->host_pc > 0x10000) return size_t(-1);
    return it->section_index;
}

// Per-section load-order timestamps. Incremented on each successful
// register_runtime_fragment / load_overlay. Used so that when a
// non-bucket caller (e.g. fragment62 at 0x84300000) asks for a
// 0x8FF00000 fragment-vaddr, we can pick the bucket variant that
// was most recently registered BEFORE the caller — i.e., the
// variant that was "live" at the caller's load time and whose
// data layout the caller's R_MIPS_32 relocs were resolved against.
static std::vector<uint64_t> section_load_order{};
static uint64_t next_load_order = 1;

static void record_load_order(size_t section_index) {
    if (section_load_order.size() <= section_index) {
        section_load_order.resize(section_index + 1, 0);
    }
    section_load_order[section_index] = next_load_order++;
}

static uint64_t get_load_order(size_t section_index) {
    if (section_index >= section_load_order.size()) return 0;
    return section_load_order[section_index];
}

void load_overlay(size_t section_table_index, int32_t ram) {
    FuncMapWriteLock _fml;
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
        install_section_func_aliases(section, ram, func);
        pc_index_register(func.func, section_table_index);
    }

    loaded_sections.emplace_back(ram, section_table_index);
    section_addresses[section.index] = ram;
    record_load_order(section_table_index);
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

static bool func_entry_may_be_fragment_jump_slot(const FuncEntry& func) {
    return func.offset >= 0x20 &&
           (func.offset & 7u) == 0 &&
           func.rom_size == 8;
}

static bool func_entry_is_live_fragment_jump_slot(
    uint8_t* rdram,
    const SectionTableEntry& section,
    int32_t runtime_base,
    const FuncEntry& func) {
    if (!func_entry_may_be_fragment_jump_slot(func)) {
        return false;
    }

    uint32_t instr = read_rdram_u32_be(rdram, runtime_base + (int32_t)func.offset);
    uint32_t delay = read_rdram_u32_be(rdram, runtime_base + (int32_t)func.offset + 4);
    if (delay != 0) {
        return false;
    }

    uint32_t opcode = (instr >> 26) & 0x3F;
    if (opcode != 0x02) {
        return false;
    }

    uint32_t target = decode_jal_target(instr, section.ram_addr + func.offset + 4);
    return target != 0;
}

static bool section_original_fragment_base(const SectionTableEntry& sec,
                                           uint32_t& base_out);

// Translate a link-time vram (assigned by the linker when the section
// was built) to its current runtime RAM address. Returns 0 if no
// loaded section covers the link-time address.
static int32_t translate_link_time_to_runtime(uint32_t link_time) {
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        if (link_time >= (uint32_t)ls.loaded_ram_addr &&
            link_time < (uint32_t)ls.loaded_ram_addr + sec.size) {
            return (int32_t)link_time;
        }
        if (link_time >= sec.ram_addr && link_time < sec.ram_addr + sec.size) {
            return ls.loaded_ram_addr + (int32_t)(link_time - sec.ram_addr);
        }
        uint32_t original_base = 0;
        if (section_original_fragment_base(sec, original_base) &&
            link_time >= original_base &&
            link_time < original_base + sec.size) {
            return ls.loaded_ram_addr + (int32_t)(link_time - original_base);
        }
    }
    return 0;
}

// Try to install a trampoline now. Returns true if the link-time
// target resolves to a runtime address that has a recomp_func
// registered. False means the target's section isn't loaded yet (or
// the offset doesn't have a recomp function).
//
// Registers BOTH the runtime address AND the trampoline's link-time
// vram (computed from the trampoline's host section). Stadium's
// recompiled call sites can dispatch through either, depending on
// whether the caller is using a relocated or link-time pointer.
static bool try_install_trampoline(int32_t trampoline_runtime_addr, uint32_t link_time_target) {
    FuncMapWriteLock _fml;
    int32_t runtime_target = translate_link_time_to_runtime(link_time_target);
    if (runtime_target == 0) return false;
    auto it = func_map.find(runtime_target);
    if (it == func_map.end()) return false;
    func_map[trampoline_runtime_addr] = it->second;

    // Also register the trampoline's link-time vram alias. Find the
    // section that owns trampoline_runtime_addr by walking
    // loaded_sections; compute its link_time_vram = section.ram_addr
    // + (trampoline_runtime_addr - section.runtime_addr); and store
    // a func_map entry there too. Without this, callers that resolve
    // the trampoline by link-time vram (e.g. statically-recompiled
    // dispatches) hit a runtime LOOKUP_FUNC miss.
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        int32_t sec_runtime_end = ls.loaded_ram_addr + (int32_t)sec.size;
        if (trampoline_runtime_addr >= ls.loaded_ram_addr &&
            trampoline_runtime_addr < sec_runtime_end) {
            int32_t off = trampoline_runtime_addr - ls.loaded_ram_addr;
            int32_t link_time_vram = (int32_t)sec.ram_addr + off;
            if (link_time_vram != trampoline_runtime_addr) {
                func_map[link_time_vram] = it->second;
                uint32_t original_base = 0;
                if (section_original_fragment_base(sec, original_base)) {
                    func_map[(int32_t)original_base + off] = it->second;
                }
                // Diagnostic — narrate every link-time alias install
                // when the env var probe is targeting this address.
                static const char* probe_s = std::getenv("PSR_FUNC_MAP_PROBE");
                if (probe_s) {
                    uint32_t probe_a = (uint32_t)std::strtoul(probe_s, nullptr, 0);
                    if ((uint32_t)link_time_vram == probe_a) {
                        fprintf(stderr,
                            "[probe] try_install_trampoline INSTALLED func_map[0x%08X] "
                            "(runtime=0x%08X) from section %zu (link=0x%08X)\n",
                            (uint32_t)link_time_vram,
                            (uint32_t)trampoline_runtime_addr,
                            ls.section_table_index,
                            (uint32_t)sec.ram_addr);
                        fflush(stderr);
                    }
                }
            }
            break;
        }
    }
    return true;
}

// Walk the pending list and try to resolve any trampolines whose
// targets are now loaded. Called after every section load.
static void retry_pending_trampolines() {
    FuncMapWriteLock _fml;
    auto it = pending_trampolines.begin();
    while (it != pending_trampolines.end()) {
        if (try_install_trampoline(it->trampoline_runtime_addr, it->link_time_target)) {
            it = pending_trampolines.erase(it);
        } else {
            ++it;
        }
    }
}

// Diagnostic probe — logs whether func_map[probe_addr] is currently
// populated. Used to track when a specific link-time alias is set or
// cleared across the load/eviction lifecycle of multiple fragments.
// Driven by environment variable PSR_FUNC_MAP_PROBE (set to a hex
// vaddr like "0x81000030"). Disabled when the env var is unset.
static void probe_func_map_entry(const char* phase) {
    static int initialized = 0;
    static uint32_t probe_addr = 0;
    if (!initialized) {
        initialized = 1;
        const char* s = std::getenv("PSR_FUNC_MAP_PROBE");
        if (s) {
            probe_addr = (uint32_t)std::strtoul(s, nullptr, 0);
        }
    }
    if (probe_addr == 0) return;
    // Diagnostic probe — called from inside writer scopes (e.g.
    // retry_pending_trampolines) which already hold the exclusive
    // lock. Use the recursive write helper rather than shared_lock to
    // avoid same-thread deadlock against the held exclusive lock.
    FuncMapWriteLock _fml;
    auto it = func_map.find((int32_t)probe_addr);
    fprintf(stderr,
        "[probe] func_map[0x%08X] %s after %s (size=%zu)\n",
        probe_addr,
        (it == func_map.end()) ? "MISSING" : "PRESENT",
        phase,
        func_map.size());
    fflush(stderr);
}

// Scan one fragment-shaped section's trampoline range. `runtime_base`
// is the section's loaded_ram_addr (where it lives in RAM right now).
static void scan_fragment_section_trampolines(uint8_t* rdram, size_t section_index, int32_t runtime_base) {
    FuncMapWriteLock _fml;
    const SectionTableEntry& section = sections_info.code_sections[section_index];

    static const char* probe_s = std::getenv("PSR_FUNC_MAP_PROBE");
    uint32_t probe_a = probe_s ? (uint32_t)std::strtoul(probe_s, nullptr, 0) : 0;
    bool probe_this = (probe_a != 0) &&
        (probe_a >= (uint32_t)section.ram_addr) &&
        (probe_a < (uint32_t)section.ram_addr + section.size);

    // Heuristic: only treat as a fragment if the "FRAGMENT" magic is
    // at runtime_base+8. Cheap and avoids false positives for
    // non-fragment sections (boot, the resident kernel, audio data,
    // patches).
    uint32_t mag_a = read_rdram_u32_be(rdram, runtime_base + 8);
    uint32_t mag_b = read_rdram_u32_be(rdram, runtime_base + 12);
    if (mag_a != 0x46524147 || mag_b != 0x4D454E54) {
        if (probe_this) {
            fprintf(stderr,
                "[probe] scan SKIPPED section %zu (link=0x%08X runtime=0x%08X) — "
                "magic at runtime+8 = 0x%08X 0x%08X (expected 0x46524147 0x4D454E54)\n",
                section_index, (uint32_t)section.ram_addr, (uint32_t)runtime_base,
                mag_a, mag_b);
            fflush(stderr);
        }
        return;
    }

    // The trampoline range is [0x20, first_real_func_offset). The
    // section's func table excludes the textbin region — look for the
    // smallest non-zero offset in the FuncEntry list.
    uint32_t first_func_offset = section.size;
    for (size_t i = 0; i < section.num_funcs; i++) {
        if (func_entry_is_live_fragment_jump_slot(rdram, section, runtime_base, section.funcs[i])) {
            continue;
        }
        uint32_t off = section.funcs[i].offset;
        if (off > 0 && off < first_func_offset) first_func_offset = off;
    }
    if (first_func_offset <= 0x20) return;  // nothing between header and first func

    uint32_t decode_base = section.ram_addr;
    uint32_t original_base = 0;
    if (section_original_fragment_base(section, original_base)) {
        decode_base = original_base;
    }

    // Walk 8-byte slots. Each slot is { instr, nop } per Stadium's
    // convention. Non-conforming slots (no nop in delay slot) are
    // skipped — they're probably padding or data, not trampolines.
    for (uint32_t slot_off = 0x20; slot_off + 8 <= first_func_offset; slot_off += 8) {
        uint32_t instr = read_rdram_u32_be(rdram, runtime_base + (int32_t)slot_off);
        uint32_t delay = read_rdram_u32_be(rdram, runtime_base + (int32_t)slot_off + 4);
        bool probe_slot = probe_this &&
            (uint32_t)(section.ram_addr + slot_off) == probe_a;
        if (delay != 0) {
            if (probe_slot) {
                fprintf(stderr,
                    "[probe] scan slot 0x%X SKIPPED — delay=0x%08X (expected 0)\n",
                    slot_off, delay);
                fflush(stderr);
            }
            continue;
        }

        uint32_t pc_delay = decode_base + slot_off + 4;
        uint32_t target = decode_jal_target(instr, pc_delay);
        if (target == 0) {
            if (probe_slot) {
                fprintf(stderr,
                    "[probe] scan slot 0x%X SKIPPED — instr=0x%08X is not J/JAL\n",
                    slot_off, instr);
                fflush(stderr);
            }
            continue;
        }

        int32_t tramp_addr = runtime_base + (int32_t)slot_off;
        if (probe_slot) {
            fprintf(stderr,
                "[probe] scan slot 0x%X: instr=0x%08X target=0x%08X tramp_runtime=0x%08X\n",
                slot_off, instr, target, (uint32_t)tramp_addr);
            fflush(stderr);
        }
        bool installed = try_install_trampoline(tramp_addr, target);
        if (probe_slot) {
            fprintf(stderr,
                "[probe] scan slot 0x%X try_install_trampoline returned %s\n",
                slot_off, installed ? "true (installed)" : "false (added to pending)");
            fflush(stderr);
        }
        if (!installed) {
            pending_trampolines.push_back({tramp_addr, target});
        }
    }
}

void recomp::overlays::scan_loaded_fragment_trampolines(uint8_t* rdram, uint32_t rom, int32_t ram_addr, uint32_t size) {
    FuncMapWriteLock _fml;
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

// Synthetic-vram pool for per-variant pattern-fragment link identities.
// Pattern variants that participate in Path 2 get a unique link-time
// ram_addr from this range (assigned at build time in N64Recomp's
// decompressed_section_pattern handler). The ram_addr's only purposes
// are: (a) to make section_addresses[N] uniquely identify the variant
// at runtime so RELOC_HI16/LO16 macros emit per-variant literals, and
// (b) to look up the variant's runtime buffer in the parallel
// recomp_synthetic_fragments[] table when the game asks
// Memmap_GetFragmentVaddr to resolve a 0xCXXXXXXX literal.
//
// Pool placement: 0xC0000000-0xDFFFFFFF (KSEG2/KSEG3). KSEG0 (0x80)
// and KSEG1 (0xA0) are both used by N64 hardware regions — RSP at
// 0xA4000000, DP at 0xA4100000, etc. — so any sentinel that overlaps
// them risks polluting registration of those engine sections. KSEG2
// and KSEG3 are unused by N64 software, making them the safest
// "obviously invalid in N64-land but valid recomp-side sentinel"
// choice.
//
// Stride 0x100000 = 1 MB per variant — comfortably above the largest
// observed variant size (~286 KB, Stadium's 0x473E0 stadium-models
// fragment). Pool 0xC0000000..0xE0000000 = 512 MB / 512 buckets,
// fitting Stadium's ~279 unique pattern variants with headroom for
// future games.
static constexpr uint32_t kSyntheticPoolBase  = 0xC0000000u;
static constexpr uint32_t kSyntheticPoolEnd   = 0xE0000000u;
static constexpr uint32_t kSyntheticPoolStride = 0x00100000u;
static constexpr size_t   kSyntheticBucketCount =
    (kSyntheticPoolEnd - kSyntheticPoolBase) / kSyntheticPoolStride; // 512

struct SyntheticFragmentSlot {
    uint32_t runtime_base;   // RDRAM addr where the variant currently lives
    uint32_t size;           // variant size
    size_t   section_index;  // index into sections_info.code_sections
    bool     registered;
};
static SyntheticFragmentSlot recomp_synthetic_fragments[kSyntheticBucketCount] = {};

static inline bool is_synthetic_addr(uint32_t addr) {
    return addr >= kSyntheticPoolBase && addr < kSyntheticPoolEnd;
}

static inline size_t synthetic_bucket_idx(uint32_t addr) {
    return size_t((addr - kSyntheticPoolBase) / kSyntheticPoolStride);
}

static bool section_original_fragment_base(const SectionTableEntry& sec,
                                           uint32_t& base_out) {
    if (sec.original_pattern_id == 0xFFFFFFFFu) {
        return false;
    }
    const uint32_t bucket = (sec.original_pattern_id + 0x10u) & 0x0FFu;
    base_out = 0x80000000u | (bucket << 20);
    return true;
}

static void install_section_func_aliases(const SectionTableEntry& section,
                                         int32_t runtime_base,
                                         const FuncEntry& func) {
    if (func.func == nullptr) return;

    const int32_t offset = (int32_t)func.offset;
    func_map[runtime_base + offset] = func.func;
    if ((int32_t)section.ram_addr != runtime_base) {
        func_map[(int32_t)section.ram_addr + offset] = func.func;
    }

    uint32_t original_base = 0;
    if (section_original_fragment_base(section, original_base)) {
        func_map[(int32_t)original_base + offset] = func.func;
    }
}

static void erase_section_func_aliases(const SectionTableEntry& section,
                                       int32_t runtime_base,
                                       const FuncEntry& func) {
    if (func.func == nullptr) return;

    const int32_t offset = (int32_t)func.offset;
    func_map.erase(runtime_base + offset);
    if ((int32_t)section.ram_addr != runtime_base) {
        func_map.erase((int32_t)section.ram_addr + offset);
    }

    uint32_t original_base = 0;
    if (section_original_fragment_base(section, original_base)) {
        func_map.erase((int32_t)original_base + offset);
    }
}

static void erase_fragment_slot_aliases(const SectionTableEntry& section,
                                        int32_t runtime_base,
                                        uint32_t offset) {
    const int32_t signed_offset = (int32_t)offset;
    func_map.erase(runtime_base + signed_offset);
    if ((int32_t)section.ram_addr != runtime_base) {
        func_map.erase((int32_t)section.ram_addr + signed_offset);
    }

    uint32_t original_base = 0;
    if (section_original_fragment_base(section, original_base)) {
        func_map.erase((int32_t)original_base + signed_offset);
    }
}

void recomp::overlays::register_runtime_fragment(uint8_t* rdram, uint32_t id, int32_t fragment_ptr) {
    FuncMapWriteLock _fml;
    if (sections_info.code_sections == nullptr) return;
    if (fragment_ptr == 0) return;

    // Find candidate sections sharing this link-time vram bucket. With
    // pattern-synthesized sections, multiple sections can share a bucket.
    // We collect them all and pick by content hash below.
    //
    // Per-variant synthetic-link-vram sections (ram_addr in the
    // synthetic pool) are added as candidates ONLY when their stored
    // original_pattern_id matches the game-supplied id. Without this
    // filter, every synthetic candidate would be considered for every
    // game-id registration, and a coincidental content-hash match
    // (e.g. live fragment_ptr bytes happening to hash-equal a
    // synthetic variant's hash) would misregister the wrong section
    // for an unrelated game id. The original_pattern_id field is
    // populated at build time from the pattern's canonical bucket.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < sections_info.num_code_sections; i++) {
        const SectionTableEntry& sec = sections_info.code_sections[i];
        if (is_synthetic_addr(uint32_t(sec.ram_addr))) {
            if (sec.original_pattern_id == id && sec.content_hash != 0) {
                candidates.push_back(i);
            }
            continue;
        }
        uint32_t bucket = (sec.ram_addr & 0x0FF00000u) >> 0x14;
        if (bucket < 0x10) continue;
        uint32_t sec_id = bucket - 0x10;
        if (sec_id == id) {
            candidates.push_back(i);
        }
    }

    size_t found_index = (size_t)-1;
    bool has_hashed_candidate = false;
    for (size_t ci : candidates) {
        const SectionTableEntry& sec = sections_info.code_sections[ci];
        if (sec.content_hash != 0) {
            has_hashed_candidate = true;
            break;
        }
    }
    if (candidates.size() == 1 && !has_hashed_candidate) {
        // Common case: exactly one non-content-addressed section per
        // bucket (every static overlay + the single-block
        // decompressed_section path).
        found_index = candidates.front();
    } else if (!candidates.empty()) {
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
            // Hash didn't match any candidate. Before falling back to
            // first-candidate, try to disambiguate via the live
            // fragment's J-trampoline header: if the embedded link_vram
            // falls within a candidate's [ram_addr, ram_addr+size)
            // range, that candidate is the correct one. This rescues
            // the case where multiple candidates share the same
            // bucket-derived id (e.g. fragment58 at 0x84000000 and
            // some other section at 0xA4000040 both map to id=0x30)
            // and content_hash is unset on both (= 0).
            uint32_t hdr_link_vram = 0;
            bool hdr_valid = false;
            if (paddr + 0x14 <= (8u * 1024u * 1024u)) {
                auto rd_be32_p = [&](uint32_t off) -> uint32_t {
                    uint8_t b0 = rdram[(paddr + off + 0) ^ 3];
                    uint8_t b1 = rdram[(paddr + off + 1) ^ 3];
                    uint8_t b2 = rdram[(paddr + off + 2) ^ 3];
                    uint8_t b3 = rdram[(paddr + off + 3) ^ 3];
                    return (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | uint32_t(b3);
                };
                uint32_t j_instr  = rd_be32_p(0x00);
                uint32_t magic_a  = rd_be32_p(0x08);
                uint32_t magic_b  = rd_be32_p(0x0C);
                uint32_t entry_off = rd_be32_p(0x10);
                if (magic_a == 0x46524147u && magic_b == 0x4D454E54u &&
                    ((j_instr >> 26) & 0x3Fu) == 0x02u) {
                    uint32_t j_target = ((j_instr & 0x03FFFFFFu) << 2) | 0x80000000u;
                    hdr_link_vram = j_target - entry_off;
                    hdr_valid = true;
                }
            }
            if (hdr_valid) {
                for (size_t ci : candidates) {
                    const SectionTableEntry& sec = sections_info.code_sections[ci];
                    uint32_t base = uint32_t(sec.ram_addr);
                    uint32_t end  = base + sec.size;
                    if (hdr_link_vram >= base && hdr_link_vram < end) {
                        found_index = ci;
                        fprintf(stderr,
                            "[reg-frag] J-trampoline rescue: live link_vram=0x%08X "
                            "in candidate %zu range [0x%08X..0x%08X) — picked it\n",
                            hdr_link_vram, ci, base, end);
                        fflush(stderr);
                        break;
                    }
                }
            }

            if (found_index == (size_t)-1) {
                // Still no match. Legacy non-content-addressed candidates
                // keep the old bucket fallback after logging. Pattern
                // candidates with content_hash are content-addressed; a
                // mismatch means the live fragment variant is unknown and
                // must stay unregistered until the generator learns it.
                fprintf(stderr,
                    "[reg-frag] no content-hash match for id=0x%X "
                    "fragment_ptr=0x%08X live_hash=0x%016llX "
                    "(%zu candidates) — picking first\n",
                    id, (uint32_t)fragment_ptr,
                    (unsigned long long)live_hash,
                    candidates.size());
                for (size_t ci : candidates) {
                    const SectionTableEntry& sec = sections_info.code_sections[ci];
                    fprintf(stderr,
                        "  cand[%zu] ram_addr=0x%08X size=0x%X "
                        "content_hash=0x%016llX synthetic=%d\n",
                        ci, uint32_t(sec.ram_addr), sec.size,
                        (unsigned long long)sec.content_hash,
                        is_synthetic_addr(uint32_t(sec.ram_addr)) ? 1 : 0);
                }
                if (hdr_valid) {
                    fprintf(stderr,
                        "  live header link_vram=0x%08X (no candidate range matched)\n",
                        hdr_link_vram);
                }
                if (has_hashed_candidate) {
                    fprintf(stderr,
                        "  hashed candidate mismatch is terminal; leaving fragment unregistered\n");
                }
                fflush(stderr);
                size_t dump_size = sections_info.code_sections[candidates.front()].size;
                char path[128];
                // Diagnostic dump written to the working directory (next to the
                // exe for a packaged build) — never an absolute machine path.
                snprintf(path, sizeof(path), "hash_miss_id_%X.bin", id);
                FILE* f = fopen(path, "wb");
                if (f) {
                    if (paddr + dump_size <= (8u * 1024u * 1024u)) {
                        for (size_t i = 0; i < dump_size; i++) {
                            uint8_t b = rdram[(paddr + i) ^ 3];
                            fwrite(&b, 1, 1, f);
                        }
                        fprintf(stderr,
                            "[reg-frag] dumped %zu live bytes to %s for offline ID\n",
                            dump_size, path);
                        fflush(stderr);
                    }
                    fclose(f);
                }
                if (!has_hashed_candidate) {
                    found_index = candidates.front();
                }
            }
        }
    }

    // J-trampoline fallback: when id-based lookup misses, decode the
    // FRAGMENT header at fragment_ptr to recover the link-time vram
    // and try matching by sec.ram_addr instead. Stadium uses different
    // id encodings on different code paths (e.g. relocate-existing
    // vs load-fresh), so even a fragment we successfully recompiled
    // can come in with an "id" we don't recognize. The +0x00 J
    // trampoline + +0x10 entryOffset are the canonical way to find
    // where this fragment expects to live.
    if (found_index == (size_t)-1) {
        const uint32_t paddr = uint32_t(fragment_ptr) & 0x1FFFFFFFu;
        if (paddr + 0x14 <= (8u * 1024u * 1024u)) {
            auto rd_be32 = [&](uint32_t off) -> uint32_t {
                uint8_t b0 = rdram[(paddr + off + 0) ^ 3];
                uint8_t b1 = rdram[(paddr + off + 1) ^ 3];
                uint8_t b2 = rdram[(paddr + off + 2) ^ 3];
                uint8_t b3 = rdram[(paddr + off + 3) ^ 3];
                return (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | uint32_t(b3);
            };
            uint32_t j_instr = rd_be32(0x00);
            uint32_t magic_a = rd_be32(0x08);
            uint32_t magic_b = rd_be32(0x0C);
            uint32_t entry_off = rd_be32(0x10);
            // FRAGMENT magic + J opcode (top 6 bits == 0x02 << 26).
            if (magic_a == 0x46524147u && magic_b == 0x4D454E54u &&
                ((j_instr >> 26) & 0x3Fu) == 0x02u) {
                uint32_t j_target = ((j_instr & 0x03FFFFFFu) << 2) | 0x80000000u;
                uint32_t link_vram = j_target - entry_off;
                for (size_t i = 0; i < sections_info.num_code_sections; i++) {
                    const SectionTableEntry& sec = sections_info.code_sections[i];
                    if (uint32_t(sec.ram_addr) == link_vram) {
                        found_index = i;
                        fprintf(stderr,
                            "[reg-frag] J-trampoline fallback rescued id=0x%X (signed=%d): "
                            "link_vram=0x%08X -> section index %zu (ram_addr=0x%08X size=0x%X)\n",
                            id, (int32_t)id, link_vram, i,
                            uint32_t(sec.ram_addr), sec.size);
                        fflush(stderr);
                        break;
                    }
                }
            }
        }
    }

    if (found_index == (size_t)-1) {
        // Not one of our recompiled fragments. Dump the FRAGMENT
        // header so the caller can identify the source ROM fragment
        // and add the appropriate decompressed_section / split.
        // Layout: +0x00 J trampoline, +0x04 nop, +0x08 "FRAGMENT" magic,
        // +0x14 relocOffset, +0x1C sizeInRam.
        char magic[9] = {};
        uint32_t hdr[8] = {};
        const uint32_t paddr = uint32_t(fragment_ptr) & 0x1FFFFFFFu;
        if (paddr + 0x20 <= (8u * 1024u * 1024u)) {
            for (int i = 0; i < 8; i++) {
                uint8_t b0 = rdram[(paddr + i*4 + 0) ^ 3];
                uint8_t b1 = rdram[(paddr + i*4 + 1) ^ 3];
                uint8_t b2 = rdram[(paddr + i*4 + 2) ^ 3];
                uint8_t b3 = rdram[(paddr + i*4 + 3) ^ 3];
                hdr[i] = (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | uint32_t(b3);
            }
            for (int i = 0; i < 8; i++) magic[i] = (char)rdram[(paddr + 8 + i) ^ 3];
            magic[8] = 0;
            // Sanitize non-printable bytes for stderr.
            for (int i = 0; i < 8; i++) {
                if ((unsigned char)magic[i] < 0x20 || (unsigned char)magic[i] > 0x7E) magic[i] = '.';
            }
        }
        fprintf(stderr,
            "[reg-frag] UNRECOMPILED id=0x%X (signed=%d, bucket=0x%X) at runtime=0x%08X "
            "magic='%s' hdr=[%08X %08X %08X %08X %08X %08X %08X %08X] relocOff=0x%X sizeInRam=0x%X\n",
            id, (int32_t)id, id + 0x10, (uint32_t)fragment_ptr,
            magic, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7],
            hdr[5], hdr[7]);
        fflush(stderr);
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

    // Stadium reuses the same RDRAM buffer for sequentially-loaded
    // pattern variants — e.g. five different intro/menu fragments
    // get decompressed into 0x801D7A30 in turn. Each variant has a
    // DIFFERENT set of internal func offsets. If we just overwrite
    // the new variant's offsets without first evicting the previous
    // variant's, callers that JAL into an offset present in the OLD
    // variant but absent in the NEW variant will dispatch to the
    // OLD variant's function — which is no longer in RAM. That
    // matches the user-visible "Stadium replays the intro instead
    // of advancing" symptom.
    //
    // Evict the previous section's func_map entries (both runtime
    // alias at this fragment_ptr AND link-time alias) before
    // registering the new section's. Iterate loaded_sections to
    // find any prior occupant of this runtime address.
    auto evict_it = loaded_sections.begin();
    while (evict_it != loaded_sections.end()) {
        if (evict_it->loaded_ram_addr != fragment_ptr ||
            evict_it->section_table_index == found_index) {
            ++evict_it;
            continue;
        }
        const SectionTableEntry& old_section =
            sections_info.code_sections[evict_it->section_table_index];
        fprintf(stderr,
            "[reg-frag] EVICTING old section index=%zu (ram_addr=0x%08X size=0x%X) "
            "from runtime=0x%08X before installing new section index=%zu\n",
            evict_it->section_table_index, uint32_t(old_section.ram_addr), old_section.size,
            (uint32_t)fragment_ptr, found_index);
        fflush(stderr);
        for (size_t fi = 0; fi < old_section.num_funcs; fi++) {
            const FuncEntry& fe = old_section.funcs[fi];
            if (fe.func == nullptr) continue;
            erase_section_func_aliases(old_section, fragment_ptr, fe);
            // Erase the OLD section's link-time alias too — for
            // pattern-shared bucket vrams (e.g. 0x8FF00000) this
            // is critical since multiple sections claim the same
            // alias, and the freshly-evicted variant's leftover
            // entries would shadow the new variant if it doesn't
            // have a func at the same offset.
            if ((int32_t)old_section.ram_addr != fragment_ptr) {
                int32_t link_alias = (int32_t)old_section.ram_addr + (int32_t)fe.offset;
                static const char* probe_s = std::getenv("PSR_FUNC_MAP_PROBE");
                if (probe_s) {
                    uint32_t probe_a = (uint32_t)std::strtoul(probe_s, nullptr, 0);
                    if ((uint32_t)link_alias == probe_a) {
                        fprintf(stderr,
                            "[probe] EVICT erasing func_map[0x%08X] "
                            "(old_section=%zu link=0x%08X runtime=0x%08X fe.offset=0x%X)\n",
                            (uint32_t)link_alias,
                            evict_it->section_table_index,
                            (uint32_t)old_section.ram_addr,
                            (uint32_t)fragment_ptr,
                            (uint32_t)fe.offset);
                        fflush(stderr);
                    }
                }
                func_map.erase(link_alias);
            }
        }
        // Also drop trampoline-scanner-installed entries (the J-slot
        // dispatch range from 0x20 up to first real func offset).
        // The scanner re-runs at the bottom of this function for
        // the new section.
        uint32_t first_func_offset = old_section.size;
        for (size_t fi = 0; fi < old_section.num_funcs; fi++) {
            if (func_entry_may_be_fragment_jump_slot(old_section.funcs[fi])) {
                continue;
            }
            uint32_t off = old_section.funcs[fi].offset;
            if (off > 0 && off < first_func_offset) first_func_offset = off;
        }
        for (uint32_t slot_off = 0x20; slot_off + 8 <= first_func_offset; slot_off += 8) {
            erase_fragment_slot_aliases(old_section, fragment_ptr, slot_off);
            if ((int32_t)old_section.ram_addr != fragment_ptr) {
                int32_t link_slot = (int32_t)old_section.ram_addr + (int32_t)slot_off;
                static const char* probe_s = std::getenv("PSR_FUNC_MAP_PROBE");
                if (probe_s) {
                    uint32_t probe_a = (uint32_t)std::strtoul(probe_s, nullptr, 0);
                    if ((uint32_t)link_slot == probe_a) {
                        fprintf(stderr,
                            "[probe] EVICT erasing trampoline-slot func_map[0x%08X] "
                            "(old_section=%zu link=0x%08X runtime=0x%08X slot=0x%X)\n",
                            (uint32_t)link_slot,
                            evict_it->section_table_index,
                            (uint32_t)old_section.ram_addr,
                            (uint32_t)fragment_ptr,
                            slot_off);
                        fflush(stderr);
                    }
                }
                func_map.erase(link_slot);
            }
        }
        // Reset the evicted section's address-table entry back to its
        // link-time vram. The section is no longer resident at this
        // runtime slot (the new fragment is taking it over), so any
        // reloc-driven RELOC_HI16/LO16 referencing it must fall back to
        // the fragment-space literal — matching Memmap_GetFragmentVaddr.
        // Without this the stale runtime base corrupts fragment-id math
        // (((addr & 0x0FF00000) >> 20) - 0x10) for the evicted fragment.
        if (section_addresses != nullptr) {
            section_addresses[old_section.index] = old_section.ram_addr;
        }
        // Remove from loaded_sections so subsequent registrations
        // don't see this entry as a "ghost" still occupying the
        // runtime address. Without this, a re-register would attempt
        // to evict the same already-evicted section repeatedly, and
        // fresh fragments at this same runtime addr would all leave
        // ghost entries behind.
        evict_it = loaded_sections.erase(evict_it);
    }

    // Register every FuncEntry at both the runtime address and the
    // section's link-time vram (mirroring load_overlay). The link-time
    // alias is what scan_fragment_section_trampolines uses to resolve
    // J/JAL targets back to a recomp_func — without it the scan can't
    // install +0x00 dispatch trampolines. The runtime alias is what
    // dispatch goes through after Stadium hands out a fragment_ptr.
    for (size_t fi = 0; fi < section.num_funcs; fi++) {
        const FuncEntry& fe = section.funcs[fi];
        if (fe.func == nullptr) continue;
        install_section_func_aliases(section, fragment_ptr, fe);
        // Track host PC → section_index so caller-context disambiguation
        // can map a return PC back to the variant that hosts it.
        pc_index_register(fe.func, found_index);
    }

    // Update section_addresses so reloc-driven RELOC_HI16/LO16 macros
    // resolve to the runtime base.
    if (section_addresses != nullptr) {
        section_addresses[section.index] = fragment_ptr;
    }

    // Per-variant synthetic-vram registration: if this section has a
    // synthetic link identity (ram_addr in the synthetic pool), populate
    // the parallel table so recomp_resolve_synthetic_fragment() can
    // translate 0xA0XXXXXX literals back to the runtime buffer. This is
    // separate from section_addresses[] above — section_addresses gets
    // set to fragment_ptr (so post-register code emits real RDRAM
    // literals directly), while the synthetic table is the lookup path
    // for any literals that ESCAPE that (pre-register code paths, or
    // any 0xA0XXXXXX baked literal that wasn't routed through
    // section_addresses). Slot is keyed by synthetic bucket index.
    if (is_synthetic_addr(uint32_t(section.ram_addr))) {
        const size_t slot_idx = synthetic_bucket_idx(uint32_t(section.ram_addr));
        if (slot_idx < kSyntheticBucketCount) {
            recomp_synthetic_fragments[slot_idx] = SyntheticFragmentSlot{
                /*runtime_base*/ uint32_t(fragment_ptr),
                /*size*/         section.size,
                /*section_index*/found_index,
                /*registered*/   true,
            };
            fprintf(stderr,
                "[synth-frag] registered slot %zu (link 0x%08X) → "
                "runtime 0x%08X size 0x%X (section index %zu)\n",
                slot_idx, uint32_t(section.ram_addr),
                uint32_t(fragment_ptr), section.size, found_index);
            fflush(stderr);
        } else {
            fprintf(stderr,
                "[synth-frag] FATAL: section ram_addr 0x%08X yields slot "
                "index %zu beyond table capacity %zu — synthetic pool "
                "exhausted, can't register variant\n",
                uint32_t(section.ram_addr), slot_idx, kSyntheticBucketCount);
            fflush(stderr);
            std::abort();
        }
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
    // Remember which section this id mapped to so the game's
    // Memmap_ClearFragmentMemmap(id) can release exactly this section
    // (resetting section_addresses[] back to the link literal). One slot
    // per id, mirroring gFragments[].
    runtime_fragment_id_to_section[id] = found_index;
    record_load_order(found_index);

    // Run the textbin trampoline scanner on the fragment, same as the
    // DMA path. Resolves +0x00 J-slot dispatch and any in-header
    // trampolines that point at sibling fragments.
    scan_fragment_section_trampolines(rdram, found_index, fragment_ptr);
    {
        char phase[96];
        std::snprintf(phase, sizeof(phase),
            "scan section %zu (link=0x%08X runtime=0x%08X)",
            found_index, (uint32_t)section.ram_addr, (uint32_t)fragment_ptr);
        probe_func_map_entry(phase);
    }

    // Pending trampolines may now resolve.
    retry_pending_trampolines();
    probe_func_map_entry("retry_pending");
}

static void load_special_overlay(const SectionTableEntry& section, int32_t ram) {
    FuncMapWriteLock _fml;
    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
    }
}

static void load_patch_functions() {
    FuncMapWriteLock _fml;
    if (patch_code_sections == nullptr) {
        debug_printf("[Patch] No patch section was registered\n");
        return;
    }
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        load_special_overlay(patch_code_sections[i], patch_code_sections[i].ram_addr);
    }
}

void recomp::overlays::read_patch_data(uint8_t* rdram, gpr patch_data_address) {
    FuncMapWriteLock _fml;
    for (size_t i = 0; i < patch_data.size(); i++) {
        MEM_B(i, patch_data_address) = patch_data[i];
    }
}

// Forward declaration — definition is below alongside unload_overlay_by_id.
static void unload_overlay_by_section_index(uint32_t section_table_index);

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size) {
    FuncMapWriteLock _fml;
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
    FuncMapWriteLock _fml;
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    auto find_it = std::find_if(loaded_sections.begin(), loaded_sections.end(), [section_table_index](const LoadedSection& s) { return s.section_table_index == section_table_index; });

    if (find_it != loaded_sections.end()) {
        // Mirror load_overlay: funcs were registered at both the runtime
        // slot address and the section's link-time vram.
        for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
            const auto& func = section.funcs[func_index];
            erase_section_func_aliases(section, find_it->loaded_ram_addr, func);
        }
        // Reset the section's address in the address table
        section_addresses[section.index] = section.ram_addr;
        // Remove the section from the loaded section map
        loaded_sections.erase(find_it);
    }
}

extern "C" void unload_overlay_by_id(uint32_t id) {
    FuncMapWriteLock _fml;
    uint32_t section_table_index = overlays_info.table[id];
    unload_overlay_by_section_index(section_table_index);
}

extern "C" void load_overlay_by_id(uint32_t id, uint32_t ram_addr) {
    FuncMapWriteLock _fml;
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
    FuncMapWriteLock _fml;
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
                erase_section_func_aliases(section, it->loaded_ram_addr, func);
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

// Symmetric counterpart to register_runtime_fragment, driven by the
// game's Memmap_ClearFragmentMemmap(id). Once the game clears
// gFragments[id], the fragment is no longer resident, so its code
// section must stop claiming a runtime address. unload_overlay_by_
// section_index resets section_addresses[section.index] back to the
// section's link-time ram_addr, which makes reloc-driven RELOC_HI16/
// LO16 fall back to the fragment-space literal (e.g. 0x8D000000) —
// exactly what Memmap_GetFragmentVaddr does when gFragments[id].vaddr
// is NULL.
//
// Without this, section_addresses[] kept the stale runtime base of the
// last load. Fragment code that extracts a fragment id from such an
// address with ((addr & 0x0FF00000) >> 20) - 0x10 then computed a wrong
// (often negative) id; passed to func_80004454 -> Memmap_RelocateFragment
// -> Memmap_SetFragmentMap it indexed gFragments[] out of bounds and
// clobbered gSegments[] (the menu cursor/icon "sparkle" corruption).
void recomp::overlays::unregister_runtime_fragment(uint32_t id) {
    FuncMapWriteLock _fml;
    auto it = runtime_fragment_id_to_section.find(id);
    if (it == runtime_fragment_id_to_section.end()) {
        return;
    }
    // Reset section_addresses + drop func_map/loaded_sections entries for
    // this section. Idempotent: a second clear with no intervening
    // register finds nothing in loaded_sections but re-asserts the link
    // literal, which is harmless. The id->section entry is intentionally
    // retained as "last section for this id" (re-register overwrites it).
    unload_overlay_by_section_index((uint32_t)it->second);
}

extern "C" void recomp_unregister_runtime_fragment(uint32_t id) {
    recomp::overlays::unregister_runtime_fragment(id);
}

// Verification probe: for a Stadium fragment id, report the section it
// last registered to, that section's current section_addresses[] value,
// and its link-time ram_addr. Returns 0 if the id was never registered
// (or has been released). Used by the debug server to confirm
// section_addresses falls back to the link literal after a clear.
extern "C" int recomp_debug_runtime_fragment(uint32_t id,
                                             uint32_t* out_section_index,
                                             int32_t* out_section_addr,
                                             int32_t* out_link_addr) {
    auto it = runtime_fragment_id_to_section.find(id);
    if (it == runtime_fragment_id_to_section.end()) {
        return 0;
    }
    size_t found_index = it->second;
    const SectionTableEntry& section = sections_info.code_sections[found_index];
    if (out_section_index) *out_section_index = (uint32_t)section.index;
    if (out_section_addr)  *out_section_addr  =
        (section_addresses != nullptr) ? section_addresses[section.index] : 0;
    if (out_link_addr)     *out_link_addr     = (int32_t)section.ram_addr;
    return 1;
}

// Defined later in this file; init_overlays warm-starts the fragment tier from
// the persisted coverage manifest (FRAGMENT_TIERS.md §3/§9).
static bool frag_jit_enabled();
static void ensure_jit_cache_reserved();
static void load_fragment_manifest();

void recomp::overlays::init_overlays() {
    FuncMapWriteLock _fml;
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

    // B4: warm-start the content-keyed fragment tier from the persisted coverage
    // manifest (no-ops unless PSR_FRAG_JIT is armed). Pre-seeds g_frag_cands with
    // fn=null candidates that re-key lazily against live RAM at first dispatch.
    // FuncMapWriteLock above already holds func_map_mutex, so g_frag_cands writes
    // are safe here.
    if (frag_jit_enabled()) {
        ensure_jit_cache_reserved();
        load_fragment_manifest();
    }
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
// Not a stub: the trampoline doesn't simulate behavior — it surfaces
// "execution reached unimplemented code" with full address context.
// Surfaces are richer than std::exit().
// Set by get_function on a lookup miss; consumed by the trampoline
// when the bogus pointer is actually invoked.
static int32_t g_last_lookup_miss_addr = 0;

static FILE* open_last_error_log(const char* mode) {
    // Working-directory-relative (next to the exe for a packaged build); fall
    // back to a build/ subdir if one exists. Never an absolute machine path.
    FILE* f = fopen("last_error.log", mode);
    if (f == nullptr) {
        f = fopen("build/last_error.log", mode);
    }
    return f;
}

static uint32_t rdram_offset_for_vaddr(uint32_t vaddr) {
    return (uint32_t)(((uint64_t)vaddr - 0xFFFFFFFF80000000ull) & 0x3FFFFFFFull);
}

static uint32_t read_rdram_u32_macro_order(uint8_t* rdram, uint32_t vaddr) {
    if (rdram == nullptr) {
        return 0;
    }

    uint32_t value = 0;
    memcpy(&value, rdram + rdram_offset_for_vaddr(vaddr), sizeof(value));
    return value;
}

static uint32_t read_rdram_u32_xor_be_for_diag(uint8_t* rdram, uint32_t vaddr) {
    if (rdram == nullptr) {
        return 0;
    }

    uint32_t paddr = rdram_offset_for_vaddr(vaddr);
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) {
        value = (value << 8) | rdram[(paddr + i) ^ 3];
    }
    return value;
}

static void dump_lookup_addr_classification(FILE* f, uint32_t addr) {
    if (f == nullptr || sections_info.code_sections == nullptr) {
        return;
    }

    bool matched = false;
    fprintf(f, "  address classification for 0x%08X:\n", addr);
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        uint32_t runtime_base = (uint32_t)ls.loaded_ram_addr;
        uint32_t link_base = (uint32_t)sec.ram_addr;
        if (addr >= runtime_base && addr < runtime_base + sec.size) {
            fprintf(f,
                "    runtime code section index=%zu link=0x%08X runtime=0x%08X size=0x%X offset=0x%X load_order=%llu\n",
                ls.section_table_index, link_base, runtime_base, sec.size,
                addr - runtime_base,
                (unsigned long long)get_load_order(ls.section_table_index));
            matched = true;
        }
        if (addr >= link_base && addr < link_base + sec.size) {
            fprintf(f,
                "    link-time code section index=%zu link=0x%08X runtime=0x%08X size=0x%X offset=0x%X load_order=%llu\n",
                ls.section_table_index, link_base, runtime_base, sec.size,
                addr - link_base,
                (unsigned long long)get_load_order(ls.section_table_index));
            matched = true;
        }
    }
    if (!matched) {
        fprintf(f, "    no loaded code section contains this as runtime or link-time address\n");
    }
}

static void dump_lookup_memory_window(FILE* f, uint8_t* rdram, const char* label, uint32_t center) {
    if (f == nullptr || rdram == nullptr) {
        return;
    }

    uint32_t start = (center - 0x20u) & ~3u;
    fprintf(f, "  %s window around 0x%08X:\n", label, center);
    for (uint32_t row = 0; row < 8; row++) {
        uint32_t addr = start + row * 0x10u;
        fprintf(f, "    %08X:", addr);
        for (uint32_t col = 0; col < 4; col++) {
            uint32_t cur = addr + col * 4u;
            fprintf(f, " %08X/%08X",
                read_rdram_u32_macro_order(rdram, cur),
                read_rdram_u32_xor_be_for_diag(rdram, cur));
        }
        fprintf(f, "\n");
    }
    fprintf(f, "    values are macro-order/xor-be u32\n");
}

static void dump_lookup_context(FILE* f, uint8_t* rdram, recomp_context* ctx) {
    if (f == nullptr || ctx == nullptr) {
        return;
    }

    const uint64_t regs[32] = {
        ctx->r0,  ctx->r1,  ctx->r2,  ctx->r3,
        ctx->r4,  ctx->r5,  ctx->r6,  ctx->r7,
        ctx->r8,  ctx->r9,  ctx->r10, ctx->r11,
        ctx->r12, ctx->r13, ctx->r14, ctx->r15,
        ctx->r16, ctx->r17, ctx->r18, ctx->r19,
        ctx->r20, ctx->r21, ctx->r22, ctx->r23,
        ctx->r24, ctx->r25, ctx->r26, ctx->r27,
        ctx->r28, ctx->r29, ctx->r30, ctx->r31,
    };
    static const char* names[32] = {
        "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
    };

    fprintf(f, "  ctx registers:\n");
    for (uint32_t i = 0; i < 32; i += 4) {
        fprintf(f,
            "    %-2s=%016llX %-2s=%016llX %-2s=%016llX %-2s=%016llX\n",
            names[i + 0], (unsigned long long)regs[i + 0],
            names[i + 1], (unsigned long long)regs[i + 1],
            names[i + 2], (unsigned long long)regs[i + 2],
            names[i + 3], (unsigned long long)regs[i + 3]);
    }
    fprintf(f,
        "    hi=%016llX lo=%016llX tail_pending=%u tail_target=0x%08X tail_func=%p\n",
        (unsigned long long)ctx->hi,
        (unsigned long long)ctx->lo,
        ctx->tailcall_pending,
        ctx->tailcall_target,
        (void*)ctx->tailcall_func);

    const uint32_t gp = (uint32_t)ctx->r28;
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t gb_dispatch_slot = gp + 0x53C8u;
    fprintf(f,
        "  diagnostic slots:\n"
        "    gp+0x53C4 @ %08X = %08X/%08X\n"
        "    gp+0x53C8 @ %08X = %08X/%08X\n"
        "    gp+0x53CC @ %08X = %08X/%08X\n"
        "    gp+0x55AC @ %08X = %08X/%08X\n"
        "    sp+0x088 @ %08X = %08X/%08X\n"
        "    sp+0x08C @ %08X = %08X/%08X\n"
        "    sp+0x090 @ %08X = %08X/%08X\n",
        gp + 0x53C4u, read_rdram_u32_macro_order(rdram, gp + 0x53C4u), read_rdram_u32_xor_be_for_diag(rdram, gp + 0x53C4u),
        gb_dispatch_slot, read_rdram_u32_macro_order(rdram, gb_dispatch_slot), read_rdram_u32_xor_be_for_diag(rdram, gb_dispatch_slot),
        gp + 0x53CCu, read_rdram_u32_macro_order(rdram, gp + 0x53CCu), read_rdram_u32_xor_be_for_diag(rdram, gp + 0x53CCu),
        gp + 0x55ACu, read_rdram_u32_macro_order(rdram, gp + 0x55ACu), read_rdram_u32_xor_be_for_diag(rdram, gp + 0x55ACu),
        sp + 0x88u, read_rdram_u32_macro_order(rdram, sp + 0x88u), read_rdram_u32_xor_be_for_diag(rdram, sp + 0x88u),
        sp + 0x8Cu, read_rdram_u32_macro_order(rdram, sp + 0x8Cu), read_rdram_u32_xor_be_for_diag(rdram, sp + 0x8Cu),
        sp + 0x90u, read_rdram_u32_macro_order(rdram, sp + 0x90u), read_rdram_u32_xor_be_for_diag(rdram, sp + 0x90u));
    if (section_addresses != nullptr && sections_info.num_code_sections > 9) {
        uint32_t section9_base = (uint32_t)section_addresses[9];
        fprintf(f,
            "    section[9] base=%08X target+0xA204=%08X target+0xA1CC=%08X\n",
            section9_base,
            section9_base + 0xA204u,
            section9_base + 0xA1CCu);
    }

    dump_lookup_memory_window(f, rdram, "bad target", (uint32_t)g_last_lookup_miss_addr);
    dump_lookup_memory_window(f, rdram, "sp", (uint32_t)ctx->r29);
    dump_lookup_memory_window(f, rdram, "sp+0x8C", (uint32_t)ctx->r29 + 0x8Cu);
    dump_lookup_memory_window(f, rdram, "gp", (uint32_t)ctx->r28);
    dump_lookup_memory_window(f, rdram, "gp+0x53C8", gb_dispatch_slot);
    dump_lookup_memory_window(f, rdram, "v1", (uint32_t)ctx->r3);
    dump_lookup_memory_window(f, rdram, "a2", (uint32_t)ctx->r6);
    dump_lookup_memory_window(f, rdram, "at", (uint32_t)ctx->r1);
}

// ---------------------------------------------------------------------
// Runtime overlay-discovery capture (Track B1 / C-capture).
//
// A get_function miss is almost always an undiscovered function entry
// inside a decompressed fragment — an indirect jalr the static
// recompiler never saw (FINDINGS.md CRASH-001). Instead of only
// aborting, we record enough ground-truth per UNIQUE missing address to
// drive the static fold-back (tools/fold_captures.py ->
// game.toml force_function_vrams):
//   - the missing vaddr
//   - the enclosing loaded section's content_hash + offset-in-section
//     (content_hash is stable across the synthetic per-variant link
//      addresses, so it is the real key the fold-back joins on)
//   - whether the offset sits on a reloc (the miss is a relocated
//     POINTER site whose true target is section[target]:target_offset)
//     vs. plain interior CODE (an undiscovered entry to seed here)
//
// Loud-but-not-destructive: each unique address logs ONCE (stderr +
// last_error.log + a captures-file rewrite). Repeat hits of the same
// address only bump an in-memory counter — no per-hit stderr spam and
// no I/O flood for table-walk callers that hit the trampoline in a loop.
// ---------------------------------------------------------------------
struct LookupCapture {
    uint32_t missed_addr = 0;
    uint64_t hit_count = 0;
    bool enclosing_found = false;
    const char* match_kind = "none";   // "runtime" | "link" | "none"
    size_t section_index = 0;
    uint32_t link_base = 0;
    uint32_t runtime_base = 0;
    uint32_t section_size = 0;
    uint32_t offset_in_section = 0;
    uint64_t content_hash = 0;
    uint32_t original_pattern_id = 0xFFFFFFFFu;
    uint64_t load_order = 0;
    bool offset_is_known_func = false; // offset matches an existing FuncEntry start
    bool reloc_at_offset = false;
    const char* reloc_type = "";
    uint16_t reloc_target_section = 0;
    uint32_t reloc_target_offset = 0;
    const char* classification = "unknown"; // see classify_lookup_capture
};

// Per-tier execution counters. Today only two tiers exist (static
// dispatch hit, lookup miss). Names for the rest of the self-healing
// arc (interpreter / JIT / disk-shard) are added as those tiers land so
// the coverage report stays forward-compatible.
static std::atomic<uint64_t> g_tier_static_hits{0};
static std::atomic<uint64_t> g_tier_lookup_misses{0};
// Misses resolved by the interior-return self-heal (dispatched into the
// resident enclosing function via ctx->dispatch_entry_target instead of
// aborting). A miss can recur (the interior point is never registered in
// func_map), so self_heals >> unique_missed_addrs is expected and healthy.
static std::atomic<uint64_t> g_tier_self_heals{0};
// Misses that could NOT be self-healed (no resident enclosing function) and
// fell through to the B3 JIT tier / loud abort.
static std::atomic<uint64_t> g_tier_self_heal_misses{0};
// B3 runtime-JIT tier: functions compiled live from their resident rdram
// image when neither static dispatch nor self-heal could resolve them (a
// TRUE gap — genuinely unrecompiled code). jit_compiles succeed; jit_failures
// fell through to the loud abort.
static std::atomic<uint64_t> g_tier_jit_compiles{0};
static std::atomic<uint64_t> g_tier_jit_failures{0};
// B-interp tier: fragment-interior entries B3 can't handle are run by the
// R4300i interpreter (mips_interp.cpp) instead of aborting — the correctness
// floor (FRAGMENT_TIERS.md). interp_runs = times the interpreter carried a miss.
static std::atomic<uint64_t> g_tier_interp_runs{0};
// Content-keyed fragment tier (FRAGMENT_TIERS.md §8): distinct content-keyed
// fragment candidates discovered. Steps 1-2 track them (execution stays on the
// interpreter floor); native shards + the differential gate are steps 3+.
// Counts unique (addr, content) pairs registered in g_frag_cands.
static std::atomic<uint64_t> g_tier_frag_candidates{0};
// B4 manifest persistence (FRAGMENT_TIERS.md §3/§9): candidates re-loaded from
// the on-disk coverage manifest at init (pre-seeded fn=null, re-keyed lazily),
// and candidates written out to it this session.
static std::atomic<uint64_t> g_tier_frag_reloaded{0};
static std::atomic<uint64_t> g_tier_frag_persisted{0};
// Slice 3 (native fragment exec + shadow-diff gate, FRAGMENT_TIERS.md §8.4).
static std::atomic<uint64_t> g_tier_frag_diff_clean{0};   // diff passes that matched the interpreter
static std::atomic<uint64_t> g_tier_frag_diff_diverge{0}; // diff passes that diverged (trust reset)
static std::atomic<uint64_t> g_tier_frag_device_touch{0}; // candidates pinned to interp (made a native call)
static std::atomic<uint64_t> g_tier_frag_promoted{0};     // candidates that reached BUDGET clean passes
static std::atomic<uint64_t> g_tier_frag_native_runs{0};  // live native executions of a promoted candidate

// R4300i interpreter entry (librecomp/src/mips_interp.cpp). Runs the function at
// start_pc against the live ctx/rdram; returns true on a clean return, false if
// it hit an unimplemented opcode (logged) so the caller aborts loudly.
extern "C" bool recomp_interpret_function(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc);
// JIT outputs MUST outlive the program: the native code address is baked into
// func_map and the generated code bakes in its string/jumptable/section-addr
// pointers. Freeing any of these would UAF. Guarded by func_map_mutex.
struct JitEntry {
    std::unique_ptr<N64Recomp::LiveGeneratorOutput> output;
    std::unique_ptr<int32_t[]> section_addrs;
};
static std::vector<JitEntry> g_jit_entries;

// ── B3 in-game execution test harness ────────────────────────────────────
// Functions deliberately evicted from func_map (recomp_debug_jit_evict_all_
// resident) so their next INDIRECT call lookup-misses and routes through the
// dispatch tiers (self-heal → B3). Saves the original static func so that if
// B3 can't recover a given function, the trampoline restores + runs the static
// version instead of aborting — keeping the test non-destructive. Guarded by
// func_map_mutex (same lock as func_map).
static std::unordered_map<int32_t, recomp_func_t*> g_evicted_funcs;

// ── Content-keyed fragment-JIT candidate registry (FRAGMENT_TIERS.md §8) ───
// Makes B3 fragment-eligible SAFELY. Fragment (overlay-arena) addresses are
// reused by different content over a session, so a JIT keyed by address alone
// would run stale code (which is why addr_in_resident_static_section excludes
// fragments today). Instead key each candidate by the fnv1a-64 hash of the live
// code bytes it was built from, keep a candidate CHAIN per runtime address (the
// same address legitimately hosts several distinct fragment contents — e.g. the
// intro variants that share one arena slot), and pick per dispatch the candidate
// whose hash still matches the live bytes. A content that stops recurring simply
// stops matching and is skipped, so the chain is bounded by the number of
// DISTINCT contents at that address, not by reload count. Fragment JITs are
// NEVER placed in func_map (that is the unsafe address-keyed fast path) — they
// live only here, reached through the content-keyed tier in the lookup-miss
// trampoline. Guarded by func_map_mutex (same lock as func_map / g_jit_entries).
struct FragmentCandidate {
    uint32_t  addr = 0;            // runtime entry (overlay-arena vaddr)
    uint64_t  content_hash = 0;    // fnv1a_64 of live code [code_lo, code_lo+code_len)
    uint32_t  code_lo = 0;         // = addr
    uint32_t  code_len = 0;        // discovered func_size (bytes)
    recomp_func_t* fn = nullptr;   // JIT output (kept alive in g_jit_entries); null until step 3
    uint32_t  diff_passes = 0;     // consecutive clean differentials vs interpreter (step 3+)
    bool      device_touch = false;// touched HLE/side-effecting state -> interp-only (step 3+)
    bool      blacklisted = false; // self-mod / repeatedly-divergent -> never native (step 3+)
};
static std::unordered_map<uint32_t, std::vector<FragmentCandidate>> g_frag_cands;

// Default ON. The content-keyed fragment tier is the whole point — transparent,
// always-on coverage of fragment (overlay-arena) misses. PSR_FRAG_JIT=0 is a
// kill switch for debugging only. Validated default-on: self-test proves the
// native path; PMS + Stadium 1 + Stadium 2 run with 0 divergences / 0 regress.
static bool frag_jit_enabled() {
    static const bool enabled = []{
        const char* v = std::getenv("PSR_FRAG_JIT");
        return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' ||
                       v[0] == 'f' || v[0] == 'F'));
    }();
    return enabled;
}

// fnv1a_64 over the live code bytes at [addr, addr+len), read in the recompiler's
// XOR-3 byte order — same algorithm/convention as fnv1a_64() and the section
// content hash, computed inline to avoid a per-dispatch temp buffer.
static uint64_t fragment_live_code_hash(uint8_t* rdram, uint32_t addr, uint32_t len) {
    const uint32_t paddr = addr & 0x1FFFFFFFu;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len; i++) {
        h ^= uint64_t(rdram[(paddr + i) ^ 3]);
        h *= 0x00000100000001B3ull;
    }
    return h;
}

static std::mutex g_lookup_capture_mutex;
static std::map<uint32_t, LookupCapture> g_lookup_captures;

static const char* reloc_type_name(RelocEntryType t) {
    switch (t) {
        case R_MIPS_NONE:    return "R_MIPS_NONE";
        case R_MIPS_16:      return "R_MIPS_16";
        case R_MIPS_32:      return "R_MIPS_32";
        case R_MIPS_REL32:   return "R_MIPS_REL32";
        case R_MIPS_26:      return "R_MIPS_26";
        case R_MIPS_HI16:    return "R_MIPS_HI16";
        case R_MIPS_LO16:    return "R_MIPS_LO16";
        case R_MIPS_GPREL16: return "R_MIPS_GPREL16";
        default:             return "R_MIPS_?";
    }
}

// Fill `out` from the static section tables. Caller must hold at least a
// shared lock on func_map_mutex (loaded_sections is mutated only under
// the writer lock). Mirrors dump_lookup_addr_classification but returns
// structured data and adds reloc / known-func discrimination.
static void classify_lookup_capture(uint32_t addr, LookupCapture& out) {
    out.missed_addr = addr;
    if (sections_info.code_sections == nullptr) {
        out.classification = "no-sections";
        return;
    }
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
        uint32_t runtime_base = (uint32_t)ls.loaded_ram_addr;
        uint32_t link_base = (uint32_t)sec.ram_addr;
        bool in_runtime = (addr >= runtime_base && addr < runtime_base + sec.size);
        bool in_link    = (addr >= link_base && addr < link_base + sec.size);
        if (!in_runtime && !in_link) continue;

        out.enclosing_found = true;
        out.match_kind = in_runtime ? "runtime" : "link";
        out.section_index = ls.section_table_index;
        out.link_base = link_base;
        out.runtime_base = runtime_base;
        out.section_size = sec.size;
        out.offset_in_section = in_runtime ? (addr - runtime_base) : (addr - link_base);
        out.content_hash = sec.content_hash;
        out.original_pattern_id = sec.original_pattern_id;
        out.load_order = get_load_order(ls.section_table_index);

        // Known function start? A miss on a known start would point at a
        // different bug (eviction / aliasing), not a discovery gap.
        for (size_t i = 0; i < sec.num_funcs; i++) {
            if (sec.funcs[i].offset == out.offset_in_section) {
                out.offset_is_known_func = true;
                break;
            }
        }
        // Reloc at this offset => the miss address is a relocated POINTER
        // site, not a code entry; the true call target is
        // section[target_section] : target_section_offset.
        for (size_t i = 0; i < sec.num_relocs; i++) {
            if (sec.relocs[i].offset == out.offset_in_section) {
                out.reloc_at_offset = true;
                out.reloc_type = reloc_type_name(sec.relocs[i].type);
                out.reloc_target_section = sec.relocs[i].target_section;
                out.reloc_target_offset = sec.relocs[i].target_section_offset;
                break;
            }
        }
        out.classification = out.reloc_at_offset ? "pointer-site" : "code-entry";
        return; // first enclosing section wins
    }
    out.classification = "outside-loaded";
}

// ── Fragment-JIT cache layout (FRAGMENT_TIERS.md §3/§9) ───────────────────
// Three never-comingled trees under the build dir:
//   coverage/            portable, arch-independent coverage currency
//                        (runtime_captures.json + the fragment manifest).
//   jit/<arch>-<abi>/    RESERVED for a future v2 sljit blob cache. n64 v1
//                        does NOT persist JIT bytes: the LiveGenerator emits
//                        final, position-dependent machine code (host pointers
//                        for funcs / jump tables / string literals baked in by
//                        sljit_generate_code), so a blob can't be reloaded into
//                        another process. v1 re-JITs from the coverage manifest
//                        instead — see §5.3 / §9. The dir is created with a
//                        README so its reserved purpose is self-documenting.
//   generated/           the static fold-back C (the optimized shipped tier).
// The arch-abi tag namespaces any per-arch derived cache so a blob built for
// one target can never load on another.
#if defined(_M_X64) || defined(__x86_64__)
  #if defined(_WIN32)
    #define N64_FRAG_ARCH_ABI "x86_64-win64"
  #else
    #define N64_FRAG_ARCH_ABI "x86_64-sysv"
  #endif
#elif defined(_M_ARM64) || defined(__aarch64__)
  #if defined(_WIN32)
    #define N64_FRAG_ARCH_ABI "aarch64-win64"
  #else
    #define N64_FRAG_ARCH_ABI "aarch64-sysv"
  #endif
#else
  #define N64_FRAG_ARCH_ABI "unknown-arch"
#endif
// Bump when a LiveGenerator codegen change would invalidate a persisted
// manifest's bounds/hash assumptions (the manifest stamps this; a mismatch
// makes the loader ignore a stale manifest rather than re-key against it).
#define N64_FRAG_CODEGEN_VER 1u

// Cache root: "build" if that directory exists (the normal run layout), else
// "." (exe-relative fallback). Computed once.
static const std::string& cache_root() {
    static const std::string root = []{
        std::error_code ec;
        if (std::filesystem::is_directory("build", ec)) {
            return std::string("build");
        }
        return std::string(".");
    }();
    return root;
}

// Ensure <root>/<sub> exists and return its path. <sub> may contain '/'.
static std::string cache_subdir(const std::string& sub) {
    std::string dir = cache_root() + "/" + sub;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static FILE* open_runtime_captures(const char* mode) {
    const bool writing = (mode[0] == 'w' || mode[0] == 'a');
    if (writing) {
        std::string path = cache_subdir("coverage") + "/runtime_captures.json";
        FILE* f = fopen(path.c_str(), mode);
        if (f != nullptr) {
            return f;
        }
        // coverage/ unwritable — fall back to the legacy locations.
    } else {
        std::string covpath = cache_root() + "/coverage/runtime_captures.json";
        FILE* f = fopen(covpath.c_str(), mode);
        if (f != nullptr) {
            return f;
        }
    }
    FILE* f = fopen("build/runtime_captures.json", mode);
    if (f == nullptr) {
        f = fopen("runtime_captures.json", mode);
    }
    return f;
}

// Rewrite the whole captures file from the in-memory map. Cheap: the map
// holds one entry per UNIQUE missing address (a handful in practice).
// Called only when a new unique address appears, or once at abort —
// never per repeat hit. Caller holds g_lookup_capture_mutex.
static void write_runtime_captures_locked() {
    FILE* f = open_runtime_captures("w");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"coverage\": {\n");
    fprintf(f, "    \"static_dispatch_hits\": %llu,\n", (unsigned long long)g_tier_static_hits.load());
    fprintf(f, "    \"lookup_misses\": %llu,\n", (unsigned long long)g_tier_lookup_misses.load());
    fprintf(f, "    \"self_heals\": %llu,\n", (unsigned long long)g_tier_self_heals.load());
    fprintf(f, "    \"self_heal_misses\": %llu,\n", (unsigned long long)g_tier_self_heal_misses.load());
    fprintf(f, "    \"jit_compiles\": %llu,\n", (unsigned long long)g_tier_jit_compiles.load());
    fprintf(f, "    \"jit_failures\": %llu,\n", (unsigned long long)g_tier_jit_failures.load());
    fprintf(f, "    \"interp_runs\": %llu,\n", (unsigned long long)g_tier_interp_runs.load());
    fprintf(f, "    \"frag_candidates\": %llu,\n", (unsigned long long)g_tier_frag_candidates.load());
    fprintf(f, "    \"frag_reloaded\": %llu,\n", (unsigned long long)g_tier_frag_reloaded.load());
    fprintf(f, "    \"frag_persisted\": %llu,\n", (unsigned long long)g_tier_frag_persisted.load());
    fprintf(f, "    \"frag_diff_clean\": %llu,\n", (unsigned long long)g_tier_frag_diff_clean.load());
    fprintf(f, "    \"frag_diff_diverge\": %llu,\n", (unsigned long long)g_tier_frag_diff_diverge.load());
    fprintf(f, "    \"frag_device_touch\": %llu,\n", (unsigned long long)g_tier_frag_device_touch.load());
    fprintf(f, "    \"frag_promoted\": %llu,\n", (unsigned long long)g_tier_frag_promoted.load());
    fprintf(f, "    \"frag_native_runs\": %llu,\n", (unsigned long long)g_tier_frag_native_runs.load());
    fprintf(f, "    \"unique_missed_addrs\": %zu\n", g_lookup_captures.size());
    fprintf(f, "  },\n");
    fprintf(f, "  \"misses\": [\n");
    size_t i = 0;
    for (const auto& kv : g_lookup_captures) {
        const LookupCapture& c = kv.second;
        ++i;
        fprintf(f,
            "    {\n"
            "      \"missed_addr\": \"0x%08X\",\n"
            "      \"hit_count\": %llu,\n"
            "      \"classification\": \"%s\",\n"
            "      \"enclosing_found\": %s,\n"
            "      \"match_kind\": \"%s\",\n"
            "      \"section_index\": %zu,\n"
            "      \"link_base\": \"0x%08X\",\n"
            "      \"runtime_base\": \"0x%08X\",\n"
            "      \"section_size\": \"0x%X\",\n"
            "      \"offset_in_section\": \"0x%X\",\n"
            "      \"content_hash\": \"0x%016llX\",\n"
            "      \"original_pattern_id\": \"0x%X\",\n"
            "      \"load_order\": %llu,\n"
            "      \"offset_is_known_func\": %s,\n"
            "      \"reloc_at_offset\": %s,\n"
            "      \"reloc_type\": \"%s\",\n"
            "      \"reloc_target_section\": %u,\n"
            "      \"reloc_target_offset\": \"0x%X\"\n"
            "    }%s\n",
            c.missed_addr,
            (unsigned long long)c.hit_count,
            c.classification,
            c.enclosing_found ? "true" : "false",
            c.match_kind,
            c.section_index,
            c.link_base,
            c.runtime_base,
            c.section_size,
            c.offset_in_section,
            (unsigned long long)c.content_hash,
            c.original_pattern_id,
            (unsigned long long)c.load_order,
            c.offset_is_known_func ? "true" : "false",
            c.reloc_at_offset ? "true" : "false",
            c.reloc_type,
            (unsigned)c.reloc_target_section,
            c.reloc_target_offset,
            (i < g_lookup_captures.size()) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

// ── B4: fragment coverage manifest persistence (FRAGMENT_TIERS.md §3/§9) ───
// Cross-session persistence of the content-keyed fragment candidate set. We do
// NOT persist native JIT bytes (the LiveGenerator emits position-dependent code
// — see the jit/ note above); instead we persist the arch-independent coverage
// currency (entry addr + content hash + discovered bounds) and RE-JIT lazily
// from it. At init the manifest pre-seeds g_frag_cands with fn=null candidates;
// the existing per-dispatch live-byte revalidation (have_live_match) re-keys
// each entry against live RAM before it is ever used, so a reloaded candidate is
// exactly as safe as a freshly discovered one. Value before native exec lands
// (slice 3): coverage accumulates across sessions for Track C static fold-back,
// and warm-starts the validation budget once native promotion exists.
#define N64_FRAG_MANIFEST_FORMAT_VER 1u

// Persistence sub-switch: on by default whenever the fragment tier is armed;
// PSR_FRAG_CACHE=0 keeps the live tier but disables disk read/write (A/B).
static bool frag_cache_enabled() {
    static const bool enabled = []{
        const char* v = std::getenv("PSR_FRAG_CACHE");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Rewrite the whole manifest from the in-memory g_frag_cands. Cheap (a handful
// of entries) and mirrors write_runtime_captures_locked's rewrite-on-new model.
// Caller holds func_map_mutex (read access to g_frag_cands).
static void persist_fragment_manifest_locked() {
    if (!frag_cache_enabled()) {
        return;
    }
    std::string path = cache_subdir("coverage") + "/fragment_manifest.json";
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        return;
    }
    size_t total = 0;
    for (const auto& kv : g_frag_cands) {
        total += kv.second.size();
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"format_ver\": %u,\n", (unsigned)N64_FRAG_MANIFEST_FORMAT_VER);
    fprintf(f, "  \"codegen_ver\": %u,\n", (unsigned)N64_FRAG_CODEGEN_VER);
    fprintf(f, "  \"arch_abi\": \"%s\",\n", N64_FRAG_ARCH_ABI);
    fprintf(f, "  \"candidates\": [\n");
    size_t i = 0;
    for (const auto& kv : g_frag_cands) {
        for (const auto& c : kv.second) {
            ++i;
            fprintf(f,
                "    {\"addr\": \"0x%08X\", \"content_hash\": \"0x%016llX\", "
                "\"code_lo\": \"0x%08X\", \"code_len\": %u}%s\n",
                c.addr, (unsigned long long)c.content_hash,
                c.code_lo, c.code_len, (i < total) ? "," : "");
        }
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    g_tier_frag_persisted.store((uint64_t)total, std::memory_order_relaxed);
}

// Create the reserved jit/<arch-abi>/ blob-cache dir + a self-documenting
// README explaining why n64 v1 leaves it empty. Idempotent; called once at init.
static void ensure_jit_cache_reserved() {
    std::string dir = cache_subdir(std::string("jit/") + N64_FRAG_ARCH_ABI);
    std::string readme = dir + "/README.txt";
    std::error_code ec;
    if (std::filesystem::exists(readme, ec)) {
        return;
    }
    FILE* f = fopen(readme.c_str(), "w");
    if (f == nullptr) {
        return;
    }
    fprintf(f,
        "Reserved for a future v2 sljit blob cache (arch-abi: %s).\n\n"
        "n64 v1 intentionally does NOT persist JIT bytes here. The LiveRecomp\n"
        "LiveGenerator emits final, position-dependent machine code (host pointers\n"
        "for functions, jump tables, string literals, and the executable offset are\n"
        "baked in by sljit_generate_code), so a serialized blob cannot be reloaded\n"
        "into another process. Fragment coverage is persisted instead as the\n"
        "arch-independent manifest in ../../coverage/fragment_manifest.json, and the\n"
        "JIT is regenerated from it on demand. See FRAGMENT_TIERS.md sections 3/9.\n",
        N64_FRAG_ARCH_ABI);
    fclose(f);
}

// Reload the persisted fragment manifest at init: pre-seed g_frag_cands with
// fn=null candidates (re-keyed lazily against live RAM at first dispatch). Stale
// manifests (format/codegen/arch mismatch) are ignored wholesale. Caller holds
// the func_map write lock (init_overlays' FuncMapWriteLock) so the g_frag_cands
// writes are safe.
static void load_fragment_manifest() {
    if (!frag_jit_enabled() || !frag_cache_enabled()) {
        return;
    }
    std::string path = cache_root() + "/coverage/fragment_manifest.json";
    std::ifstream in(path);
    if (!in.good()) {
        return;
    }
    // Whole parse+validate+load is wrapped: the manifest is a contributable,
    // potentially hand-edited file, so a wrong-typed field must be tolerated
    // (ignore the manifest), never crash the boot. Next session rewrites it.
    try {
    nlohmann::json j;
    in >> j;
    if (!j.is_object()) {
        return;
    }
    if (j.value("format_ver", 0u) != N64_FRAG_MANIFEST_FORMAT_VER ||
        j.value("codegen_ver", 0u) != N64_FRAG_CODEGEN_VER ||
        j.value("arch_abi", std::string{}) != std::string(N64_FRAG_ARCH_ABI)) {
        return; // stale — a codegen/arch change invalidates the discovered bounds.
    }
    auto cands_it = j.find("candidates");
    if (cands_it == j.end() || !cands_it->is_array()) {
        return;
    }
    for (const auto& e : *cands_it) {
        if (!e.is_object()) {
            continue;
        }
        auto get_u64 = [&](const char* key, uint64_t def) -> uint64_t {
            auto it = e.find(key);
            if (it == e.end()) return def;
            if (it->is_string()) {
                return strtoull(it->get<std::string>().c_str(), nullptr, 0);
            }
            if (it->is_number_unsigned()) return it->get<uint64_t>();
            if (it->is_number_integer())  return (uint64_t)it->get<int64_t>();
            return def;
        };
        FragmentCandidate c;
        c.addr         = (uint32_t)get_u64("addr", 0);
        c.content_hash = get_u64("content_hash", 0);
        c.code_lo      = (uint32_t)get_u64("code_lo", c.addr);
        c.code_len     = (uint32_t)get_u64("code_len", 0);
        c.fn           = nullptr; // re-JIT'd lazily; never trust a reloaded ptr.
        if (c.addr == 0 || c.code_len == 0 || c.content_hash == 0) {
            continue;
        }
        // De-dup: skip if an identical (addr, content_hash) is already present.
        auto& chain = g_frag_cands[c.addr];
        bool dup = false;
        for (const auto& existing : chain) {
            if (existing.content_hash == c.content_hash) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        chain.push_back(c);
        g_tier_frag_reloaded.fetch_add(1, std::memory_order_relaxed);
    }
    } catch (...) {
        return; // malformed manifest — ignore wholesale, never crash the boot.
    }
}

// ── Slice 3: native fragment execution + shadow-diff validation gate ───────
// (FRAGMENT_TIERS.md §8.4) A content-keyed candidate is JIT'd but NEVER trusted
// blindly. Before it runs live it must pass the same-state differential vs the
// interpreter oracle BUDGET consecutive times; until then the *interpreter*
// result is what's committed, so an unvalidated/wrong shard never affects the
// game. Default ON (requires frag_jit, which is also default-on). The shadow-diff
// gate makes always-on SAFE BY CONSTRUCTION: only safe-leaf candidates are ever
// JIT'd, the interpreter result is committed until BUDGET consecutive clean
// diffs, and device-touchers are pinned — so a wrong/unvalidated shard can never
// affect the game even running by default. PSR_FRAG_NATIVE=0 disables (debug).
static bool frag_native_enabled() {
    static const bool enabled = []{
        const char* v = std::getenv("PSR_FRAG_NATIVE");
        return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' ||
                       v[0] == 'f' || v[0] == 'F'));
    }();
    return enabled;
}
static uint32_t frag_diff_budget() {
    static const uint32_t budget = []{
        const char* v = std::getenv("PSR_FRAG_DIFF_BUDGET");
        uint32_t b = v ? (uint32_t)strtoul(v, nullptr, 10) : 0;
        return b ? b : 8u;   // default: 8 consecutive clean passes before live-native
    }();
    return budget;
}

// Device-touch detector. While a shadow diff's PASS 1 interprets the candidate,
// the interpreter bumps this whenever it makes a NATIVE call (jal/jalr/j into a
// recompiled function — see mips_interp.cpp). Such a call can mutate host-side
// state OUTSIDE the rdram block (scheduler / gfx submit / save) that we cannot
// snapshot-restore, so any candidate that touches one is pinned to the
// interpreter forever. Thread-local: two threads may diff concurrently.
static thread_local bool t_shadow_active = false;
static thread_local bool t_shadow_touched_native = false;
extern "C" int  recomp_shadow_diff_active(void)           { return t_shadow_active ? 1 : 0; }
extern "C" void recomp_shadow_diff_note_native_call(void) { t_shadow_touched_native = true; }

// Eligibility: a candidate may only ever run native if its body contains NO
// outgoing control transfer — no jal/jalr (call), no j to outside its own
// bounds (tail call), and no computed jr (only `jr $ra` = return is allowed).
// Such a "safe leaf" is a pure register+RAM transformation on ALL inputs, so
// the same-state differential fully captures its behavior and a promoted shard
// can never reach an unvalidated, side-effecting path. Conservative on purpose
// (precision over recall): a jumptable (computed jr) or any call keeps the
// fragment on the interpreter floor. The dynamic PASS-1 detector is the
// backstop for anything this static scan can't see (e.g. data misread as code).
static bool fragment_is_safe_leaf(uint8_t* rdram, uint32_t code_lo, uint32_t code_len) {
    const uint32_t pbase = code_lo & 0x1FFFFFFFu;
    for (uint32_t off = 0; off + 4 <= code_len; off += 4) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            w = (w << 8) | rdram[((pbase + off + b) ^ 3)];
        }
        const uint32_t op = w >> 26;
        if (op == 0x03) {                       // jal
            return false;
        }
        if (op == 0x02) {                        // j — local jump or tail call
            const uint32_t pcv = code_lo + off;
            const uint32_t tgt = (pcv & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (tgt < code_lo || tgt >= code_lo + code_len) {
                return false;                    // j out of bounds = tail call
            }
        }
        if (op == 0x00) {
            const uint32_t fn = w & 0x3F;
            if (fn == 0x09) {                    // jalr = call
                return false;
            }
            if (fn == 0x08) {                    // jr
                const uint32_t rs = (w >> 21) & 0x1F;
                if (rs != 31) {                  // computed jr (jumptable/tailcall)
                    return false;
                }
            }
        }
    }
    return true;
}

// Compare the architectural state two passes produced (GPRs r1..r31, FPRs by
// raw bits, hi/lo). The host-side scaffolding (tailcall_*, dispatch/return
// targets, f_odd, cop0) is intentionally excluded — it is control-plane plumbing
// that legitimately differs and is not part of the function's data result.
static bool shadow_regs_equal(const recomp_context* a, const recomp_context* b) {
    const gpr* ra = &a->r0; const gpr* rb = &b->r0;
    for (int i = 1; i < 32; i++) {               // r0 is hardwired 0
        if (ra[i] != rb[i]) return false;
    }
    const fpr* fa = &a->f0; const fpr* fb = &b->f0;
    for (int i = 0; i < 32; i++) {
        if (fa[i].u64 != fb[i].u64) return false;
    }
    return a->hi == b->hi && a->lo == b->lo;
}

// One same-state differential pass (FRAGMENT_TIERS.md §8.4). PASS 1 interprets
// (authoritative) with the device detector armed; PASS 2 runs the native shard
// from the identical input; we compare GPR/FPR/hi-lo + the full 8 MiB RAM region
// and ALWAYS commit the interpreter result (the native pass is discarded). The
// caller holds NO lock (this interprets + runs guest code). rdram[0,0x800000)
// is the kseg0 RAM region — every interpreter memory access stays inside the
// mapped block, so snapshotting that region captures all restorable state.
struct ShadowDiffOutcome { bool interp_ok; bool device_touch; bool ran_native; bool clean; };
static ShadowDiffOutcome run_shadow_diff(uint8_t* rdram, recomp_context* ctx,
                                         uint32_t addr, recomp_func_t* fn) {
    constexpr size_t RAM = 0x800000;
    static thread_local std::vector<uint8_t> ram0, ramI;
    ram0.resize(RAM);
    ramI.resize(RAM);
    ShadowDiffOutcome o{};

    const recomp_context ctx0 = *ctx;
    std::memcpy(ram0.data(), rdram, RAM);

    // PASS 1 — interpreter first (authoritative), device detector armed.
    t_shadow_touched_native = false;
    t_shadow_active = true;
    const bool ok = recomp_interpret_function(rdram, ctx, addr);
    t_shadow_active = false;
    o.interp_ok = ok;
    if (!ok) {
        // Could not even interpret: undo our mutations and let the normal interp
        // tier re-run it and abort loudly with the proper diagnostics.
        *ctx = ctx0;
        std::memcpy(rdram, ram0.data(), RAM);
        return o;
    }
    if (t_shadow_touched_native) {
        // Touched a native call -> possible unrestorable side effect. Keep the
        // interpreter result live (already applied) and pin the candidate.
        o.device_touch = true;
        return o;
    }

    // Clean leaf: save the interp result, restore the start state, run native.
    const recomp_context ctxI = *ctx;
    std::memcpy(ramI.data(), rdram, RAM);
    *ctx = ctx0;
    std::memcpy(rdram, ram0.data(), RAM);

    // PASS 2 — native shard from the identical input.
    fn(rdram, ctx);
    o.ran_native = true;
    o.clean = shadow_regs_equal(ctx, &ctxI) &&
              (std::memcmp(rdram, ramI.data(), RAM) == 0);

    // COMMIT the interpreter result; the native pass is discarded entirely.
    *ctx = ctxI;
    std::memcpy(rdram, ramI.data(), RAM);
    return o;
}

// Apply a differential outcome to the candidate at (faddr, content_hash), under
// the write lock. Updates trust (diff_passes), pins device-touchers, and bumps
// the surfaced counters. Re-finds by content hash because the chain may have
// changed while the diff ran unlocked.
static void frag_apply_diff_outcome(uint32_t faddr, uint64_t content_hash,
                                    const ShadowDiffOutcome& o) {
    const uint32_t budget = frag_diff_budget();
    std::unique_lock<std::shared_mutex> lock(func_map_mutex);
    auto it = g_frag_cands.find(faddr);
    if (it == g_frag_cands.end()) {
        return;
    }
    for (auto& c : it->second) {
        if (c.content_hash != content_hash) {
            continue;
        }
        if (o.device_touch) {
            if (!c.device_touch) {
                g_tier_frag_device_touch.fetch_add(1, std::memory_order_relaxed);
            }
            c.device_touch = true;
        } else if (o.ran_native) {
            if (o.clean) {
                g_tier_frag_diff_clean.fetch_add(1, std::memory_order_relaxed);
                if (c.diff_passes < budget) {
                    c.diff_passes++;
                    if (c.diff_passes == budget) {
                        g_tier_frag_promoted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                g_tier_frag_diff_diverge.fetch_add(1, std::memory_order_relaxed);
                c.diff_passes = 0;   // an intermittently-wrong shard never accrues trust
            }
        }
        return;
    }
}

// Trace-ring queries (defined in extras.c — game-side instrumentation).
extern "C" {
    uint64_t pkmnstadium_trace_write_idx(void);
    const char* pkmnstadium_trace_at(uint64_t idx);
    uint32_t pkmnstadium_trace_capacity(void);
    // Unified post-mortem dump (src/main/post_mortem.cpp). Called on
    // controlled aborts (lookup-miss trampoline) so we get the same
    // last_run_report.json + last_run_input_history.json + RDRAM dump
    // that SEH crashes get.
    void psr_post_mortem_dump(const char* reason, void* fault_info);
}

// Set by get_function immediately before returning the trampoline, read by
// the trampoline on the SAME thread's immediate call. Thread-local so two
// threads missing concurrently can't cross their self-heal targets (the
// shared g_last_lookup_miss_addr is kept for the abort-path diagnostics).
static thread_local uint32_t g_self_heal_addr = 0;

// Expose the current in-flight lookup-miss RUNTIME address (this thread).
// A graceful-skip stub (generated by recompile_or_stub; it calls
// recomp_unhandled_instruction with "unrecompilable_func") only knows its own
// LINK vram. When that stub is reached via the self-heal dispatch for a
// relocated-fragment miss, the link vram is NOT what's resident — the real
// code is at the address the lookup missed on. The project's
// recomp_unhandled_instruction reads this so it interprets the loaded runtime
// address instead of decoding unrelated bytes at the link vram. 0 if no miss
// is in flight on this thread.
extern "C" uint32_t recomp_current_lookup_miss_addr(void) { return g_self_heal_addr; }

// Interior-return self-heal lookup: a get_function miss on an address
// INTERIOR to a resident fragment function (never a registered start) is
// resolvable by dispatching into that function via ctx->dispatch_entry_target.
// The N64Recomp jal-return pass guarantees every return point has a
// `case <link_vram>: goto L_<link_vram>` in the enclosing function's body, so
// the resume is exact. Returns true + fills host / link_target (the LINK vram
// the dispatch switch is keyed on) / func_start_link if a resident enclosing
// function is found. Caller must hold >= a shared lock on func_map_mutex
// (reads loaded_sections + sections_info). O(total loaded funcs); misses are
// rare so the linear scan is acceptable.
// B3 eligibility (POSITIVE allowlist). True only if `addr` lies inside an
// always-resident STATIC code section — one whose link base (ram_addr) is its
// fixed runtime home in resident kseg0 and is not synthetic. That is the only
// place B3's literal-compile model holds ("resident code, link==runtime, all
// relocations baked as absolute addresses"). Runtime-loaded overlay fragments
// (link bases like 0x82xxxxxx / 0xC0xxxxxx, or synthetic, loaded into the
// overlay arena above the static section) violate that model — JITing one
// produces wrong code and crashed the process in testing. We deliberately use
// the STABLE section table (not the dynamic loaded_sections list) so the check
// can't be defeated by a fragment that is transiently absent from
// loaded_sections during a load/unload race. Caller must hold func_map_mutex.
static bool addr_in_resident_static_section(uint32_t addr) {
    if (sections_info.code_sections == nullptr) {
        return false;
    }
    for (size_t i = 0; i < sections_info.num_code_sections; i++) {
        const SectionTableEntry& sec = sections_info.code_sections[i];
        const uint32_t base = (uint32_t)sec.ram_addr;
        // Resident static section: non-synthetic, link base in resident kseg0,
        // address inside its link range. Overlay sections have high/synthetic
        // link bases and are excluded; overlay-arena runtime addresses fall in
        // no static section's link range and are likewise excluded.
        if (is_synthetic_addr(base)) {
            continue;
        }
        if (base < 0x80000000u || base >= 0x80800000u) {
            continue;
        }
        if (addr >= base && addr < base + sec.size) {
            return true;
        }
    }
    return false;
}

static bool find_resident_enclosing_function(uint32_t addr,
                                             recomp_func_t*& host_out,
                                             uint32_t& link_target_out,
                                             uint32_t& func_start_link_out) {
    if (sections_info.code_sections == nullptr) {
        return false;
    }
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        const uint32_t runtime_base = (uint32_t)ls.loaded_ram_addr;
        const uint32_t synth_base = (uint32_t)sec.ram_addr;
        uint32_t link_base = synth_base;
        if (is_synthetic_addr(synth_base)) {
            uint32_t orig = 0;
            if (section_original_fragment_base(sec, orig)) {
                link_base = orig;
            }
        }
        // The miss address may be expressed in the section's link, runtime,
        // or synthetic base; pick whichever range contains it.
        uint32_t base = 0;
        bool matched = false;
        if (addr >= link_base && addr < link_base + sec.size) {
            base = link_base; matched = true;
        } else if (addr >= runtime_base && addr < runtime_base + sec.size) {
            base = runtime_base; matched = true;
        } else if (addr >= synth_base && addr < synth_base + sec.size) {
            base = synth_base; matched = true;
        }
        if (!matched) {
            continue;
        }
        const uint32_t offset = addr - base;
        for (size_t i = 0; i < sec.num_funcs; i++) {
            const FuncEntry& fe = sec.funcs[i];
            if (fe.func == nullptr) {
                continue;
            }
            if (offset >= fe.offset && offset < fe.offset + fe.rom_size) {
                host_out = fe.func;
                link_target_out = link_base + offset;
                func_start_link_out = link_base + fe.offset;
                return true;
            }
        }
    }
    return false;
}

// Defined later in this file; needed as a LiveGeneratorInputs callback so
// JIT-compiled code resolves its own calls through the same dispatch path.
extern "C" recomp_func_t* get_function(int32_t addr);

// ── B3: runtime LiveRecomp JIT tier ──────────────────────────────────────
// Last resort when a miss is neither a registered static function nor an
// interior point of a resident recompiled function (self-heal) — i.e. a TRUE
// gap: a function whose code is loaded in rdram but was never recompiled at
// build time. JIT it live from its resident rdram image via N64Recomp's
// LiveRecomp (sljit) backend, register it in func_map, and return it so every
// future call takes the static fast path. For PMS this never fires (all
// misses are interior points of already-compiled code, handled by self-heal),
// but the framework targets many future games which WILL have real gaps.
//
// Only resident main-RDRAM addresses (kseg0 8 MiB) are eligible — that is the
// only place a loaded code image exists to read. Overlay link addresses
// (e.g. PMS 0x82xxxxxx) are out of that range and are rejected (no OOB read).
// Returns the native function and registers it, or nullptr + err on failure.
// Two orthogonal flags:
//   keep=false  compiles + validates then DISCARDS (frees output); returns a
//               non-null success sentinel (never call it). Used by the pipeline
//               validation probe.
//   keep=true, register_in_map=false  keeps the code alive (in g_jit_entries)
//               and returns a CALLABLE function, but does NOT touch func_map.
//               Used by the content-keyed fragment tier (slice 3): a fragment
//               shard must never sit in the address-keyed fast path.
//   keep=true, register_in_map=true   the B3 resident-static path: kept alive
//               AND registered so all future calls hit the static fast path.
//
// NOTE: this is the *inner* worker. It can hard-fault (segfault) inside the
// sljit/LiveGenerator codegen on a malformed or unsupported function — the
// input is untrusted guest bytes. It is ALWAYS called through the SEH-guarded
// jit_compile_function wrapper below, never directly, so such a fault becomes
// a graceful failure instead of taking down the process.
static recomp_func_t* jit_compile_inner(uint32_t vram, uint8_t* rdram,
                                         std::string& err,
                                         bool keep, bool register_in_map,
                                         size_t* out_func_size,
                                         size_t* out_code_size) {
    using namespace N64Recomp;

    // Eligibility: code must be physically resident in kseg0 rdram.
    if (vram < 0x80000000u || vram >= 0x80800000u) {
        err = "address not in resident kseg0 rdram (8 MiB) — no loaded "
              "code image to JIT (likely an unloaded overlay link address)";
        return nullptr;
    }

    // 1. Snapshot a generous window of the loaded code (big-endian words;
    //    rdram is XOR-3 byte-swapped), clamped to stay inside the 8 MiB buffer.
    constexpr uint32_t RDRAM_END = 0x00800000u;
    const uint32_t pbase = vram & 0x1FFFFFFFu;
    uint32_t window = 0x8000u;  // 32 KiB upper bound on a single function
    if (pbase + window > RDRAM_END) {
        window = RDRAM_END - pbase;
    }
    if (window < 8) {
        err = "too close to end of rdram to read a function";
        return nullptr;
    }
    std::vector<uint8_t> body(window);
    for (uint32_t i = 0; i < window; i++) {
        body[i] = rdram[(pbase + i) ^ 3];
    }

    // 2. Discover the function bounds (BFS control-flow walk).
    size_t func_size = 0;
    if (!discover_function_bounds(body.data(), body.size(), vram, 0,
                                  func_size, err)) {
        return nullptr;  // err populated by discover_function_bounds
    }
    if (func_size < 8 || (func_size & 3u) != 0 || func_size > window) {
        err = "implausible discovered function size";
        return nullptr;
    }

    // 3. Word array the recompiler consumes. Function::words is host
    //    little-endian: recompile_function_impl byteswaps each word back to
    //    big-endian before handing it to rabbitizer (matching how the offline
    //    path and the live-recompiler test populate words). So we must store
    //    the host-native read of the four big-endian bytes here, NOT a
    //    big-endian-packed value — packing big-endian made byteswap produce
    //    garbage instructions (e.g. a real `sw` decoded as a bogus `jal`),
    //    which silently compiled wrong code and crashed the generator on
    //    float-heavy functions.
    std::vector<uint32_t> words(func_size / 4);
    for (size_t w = 0; w < words.size(); w++) {
        const uint8_t* p = body.data() + w * 4;
        words[w] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                   (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    // 4. Minimal single-section, single-function, no-reloc context. The
    //    resident code already has all relocations applied (absolute
    //    addresses baked in), so compile it literally; every call resolves
    //    at runtime through get_function (use_lookup_for_all_function_calls).
    Context ctx{};
    Section sec{};
    sec.rom_addr = 0;
    sec.ram_addr = vram;
    sec.size = (uint32_t)func_size;
    sec.name = "jit";
    sec.executable = true;
    ctx.sections.push_back(std::move(sec));
    ctx.functions.emplace_back(vram, 0u, words,
                               "jit_" + std::to_string(vram), (uint16_t)0);
    ctx.section_functions.push_back(std::vector<size_t>{0});
    ctx.use_lookup_for_all_function_calls = true;

    // 5. Persistent per-JIT section-address array (generated code may bake
    //    its address). One section; address = its runtime vram.
    auto section_addrs = std::make_unique<int32_t[]>(1);
    section_addrs[0] = (int32_t)vram;

    LiveGeneratorInputs inputs{};
    inputs.base_event_index = 0;
    inputs.cop0_status_write = cop0_status_write;
    inputs.cop0_status_read = cop0_status_read;
    inputs.switch_error = switch_error;
    inputs.do_break = do_break;
    inputs.get_function = get_function;
    inputs.syscall_handler = nullptr;
    inputs.pause_self = nullptr;     // vanilla overlay funcs don't pause_self
    inputs.trigger_event = nullptr;  // ...or trigger mod events
    inputs.reference_section_addresses = section_addrs.get();
    inputs.local_section_addresses = section_addrs.get();
    inputs.run_hook = nullptr;
    inputs.original_section_indices = std::vector<size_t>{0};

    LiveGenerator generator{ ctx.functions.size(), inputs };
    std::ostringstream dummy_ostream;
    // Must have one entry per section: recompile_function_impl writes
    // jal/jalr link targets into static_funcs_out[section_index]. An empty
    // span here is an out-of-bounds write (crash) on the first in-section
    // call. We discard the contents, but the storage must exist.
    std::vector<std::vector<uint32_t>> dummy_static_funcs(ctx.sections.size());
    if (!recompile_function_live(generator, ctx, 0, dummy_ostream,
                                 dummy_static_funcs, false)) {
        err = "recompile_function_live failed (unsupported instruction / "
              "jump table / reference symbol)";
        return nullptr;
    }
    auto output = std::make_unique<LiveGeneratorOutput>(generator.finish());
    if (!output->good || output->functions.empty() ||
        output->functions[0] == nullptr) {
        err = "live generator produced no usable function";
        return nullptr;
    }

    recomp_func_t* jitted = output->functions[0];
    if (out_func_size) *out_func_size = func_size;
    if (out_code_size) *out_code_size = output->code_size;
    if (!keep) {
        // Validation path: proven good; discard (output + section_addrs freed
        // here). Return a non-null sentinel meaning "compiled OK, not kept".
        // The caller must NOT call it.
        return reinterpret_cast<recomp_func_t*>(0x1);
    }
    // 6. Keep output + section-addr array alive for the program lifetime.
    //    register_in_map=true (B3) also puts it in func_map so all future calls
    //    hit the static fast path; the fragment tier (slice 3) keeps it alive
    //    but OUT of func_map (it is reached only through the content-keyed,
    //    per-dispatch-validated path).
    {
        std::unique_lock<std::shared_mutex> lock(func_map_mutex);
        g_jit_entries.push_back(
            JitEntry{ std::move(output), std::move(section_addrs) });
        if (register_in_map) {
            func_map[(int32_t)vram] = jitted;
        }
    }
    return jitted;
}

// SEH guard (crash safety). jit_compile_inner compiles untrusted guest bytes
// through sljit and can hard-fault; this frame holds only reference/pointer/
// POD locals (no C++ object unwinding), so __try/__except is legal here. A
// structured exception (access violation, illegal instruction, etc.) becomes
// a graceful failure. On a guarded fault the inner frame's C++ objects (sljit
// compiler, vectors) leak (SEH skips C++ unwinding under /EHsc); acceptable on
// this rare path. NOTE: SEH cannot catch a HANG — see the watchdog below.
static recomp_func_t* jit_compile_seh(uint32_t vram, uint8_t* rdram,
                                      std::string& err, bool keep, bool register_in_map,
                                      size_t* out_func_size,
                                      size_t* out_code_size) {
#ifdef _WIN32
    __try {
        return jit_compile_inner(vram, rdram, err, keep, register_in_map,
                                 out_func_size, out_code_size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        err.assign("JIT compile hard-faulted (SEH-guarded; unsupported "
                   "function shape)");
        if (out_func_size) *out_func_size = 0;
        if (out_code_size) *out_code_size = 0;
        return nullptr;
    }
#else
    return jit_compile_inner(vram, rdram, err, keep, register_in_map,
                             out_func_size, out_code_size);
#endif
}

// Shared state between the watchdog and its big-stack worker.
struct JitWorkerShared {
    recomp_func_t* fn = nullptr;
    std::string err;
    size_t fs = 0, cs = 0;
    std::atomic<bool> done{false};
    uint32_t vram = 0;
    uint8_t* rdram = nullptr;
    bool keep = true;
    bool register_in_map = true;
};

#ifdef _WIN32
static DWORD WINAPI jit_worker_proc(LPVOID param) {
    std::shared_ptr<JitWorkerShared>* holder =
        reinterpret_cast<std::shared_ptr<JitWorkerShared>*>(param);
    std::shared_ptr<JitWorkerShared> s = *holder;
    delete holder;
    std::string e;
    size_t fs = 0, cs = 0;
    recomp_func_t* fn = jit_compile_seh(s->vram, s->rdram, e, s->keep, s->register_in_map, &fs, &cs);
    s->fn = fn;
    s->err = std::move(e);
    s->fs = fs;
    s->cs = cs;
    s->done.store(true, std::memory_order_release);
    return 0;
}
#endif

// Watchdog + the only entry point callers use. The N64Recomp live recompiler
// can (a) recurse deeply in its sljit codegen — overflowing a normal thread
// stack and crashing — (b) infinite-loop, or (c) segfault on pathological
// functions. None may freeze or kill the game thread. So the compile runs on a
// dedicated worker with a LARGE reserved stack (kills the deep-recursion
// overflow → it completes or hits the watchdog), under the SEH guard (catches
// AVs), with a wall-clock budget (catches a true infinite loop: on timeout the
// worker is abandoned — bounded, rare; in the real tier the trampoline aborts
// right after, ending the process). The worker registers func_map under its
// own lock on success; the game thread holds no lock here, so no deadlock.
static recomp_func_t* jit_compile_function(uint32_t vram, uint8_t* rdram,
                                           std::string& err,
                                           bool keep = true,
                                           bool register_in_map = true,
                                           size_t* out_func_size = nullptr,
                                           size_t* out_code_size = nullptr) {
    auto sh = std::make_shared<JitWorkerShared>();
    sh->vram = vram;
    sh->rdram = rdram;
    sh->keep = keep;
    sh->register_in_map = register_in_map;

#ifdef _WIN32
    // 256 MiB reserved (not committed) stack — address space only; pages
    // commit lazily as the recursion descends.
    constexpr SIZE_T JIT_STACK_RESERVE = SIZE_T(256) * 1024 * 1024;
    auto* holder = new std::shared_ptr<JitWorkerShared>(sh);
    HANDLE h = CreateThread(nullptr, JIT_STACK_RESERVE, jit_worker_proc,
                            holder, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h == nullptr) {
        delete holder;
        err.assign("JIT worker thread creation failed");
        if (out_func_size) *out_func_size = 0;
        if (out_code_size) *out_code_size = 0;
        return nullptr;
    }

    constexpr int BUDGET_MS = 2000;
    bool finished = false;
    for (int i = 0; i < BUDGET_MS / 5; i++) {
        if (sh->done.load(std::memory_order_acquire)) { finished = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!finished) {
        CloseHandle(h);  // abandon a hung/runaway compile (true infinite loop)
        err.assign("JIT compile exceeded time budget (watchdog) — function "
                   "triggers a live-recompiler hang");
        if (out_func_size) *out_func_size = 0;
        if (out_code_size) *out_code_size = 0;
        return nullptr;
    }
    CloseHandle(h);
#else
    std::thread worker([sh]() {
        std::string e; size_t fs = 0, cs = 0;
        recomp_func_t* fn = jit_compile_seh(sh->vram, sh->rdram, e,
                                            sh->keep, sh->register_in_map, &fs, &cs);
        sh->fn = fn; sh->err = std::move(e); sh->fs = fs; sh->cs = cs;
        sh->done.store(true, std::memory_order_release);
    });
    worker.join();
#endif
    err = sh->err;
    if (out_func_size) *out_func_size = sh->fs;
    if (out_code_size) *out_code_size = sh->cs;
    return sh->fn;
}

// The runtime JIT tier is DEFAULT ON (set PSR_JIT_TIER=0 to force-disable).
// Two classes of bug were found and fixed:
//   1. Compile-correctness: the historical "live recompiler hangs/crashes" was
//      a red herring — jit_compile_inner packed Function::words big-endian, but
//      recompile_function_impl byteswaps each word back for rabbitizer, so every
//      function compiled from garbage; plus an empty static_funcs_out span. With
//      those fixed, 32/32 sampled resident functions JIT cleanly and sqrt
//      executes numerically correct.
//   2. Eligibility: a forced-eviction torture test showed B3 would also attempt
//      OVERLAY-ARENA addresses (relocated/loaded fragments living above the
//      static section), whose runtime image violates B3's literal-compile model
//      — JITing one crashed the process. Fixed by the positive allowlist in
//      addr_in_resident_static_section (B3 eligible ONLY inside the always-
//      resident static code section). With it, the same torture test no longer
//      hard-crashes: overlay gaps take the graceful loud abort, exactly as the
//      pre-B3 era did.
// So default-on is strictly >= the old loud-abort behavior for a true gap: a
// STATIC-section gap is recompiled + run (recovery); an overlay gap falls through
// to the same graceful abort as before. JIT work runs on a 256 MiB-reserved
// worker thread guarded by SEH + a 2 s watchdog. PSR_JIT_TIER=0 restores opt-out.
static bool jit_tier_enabled() {
    static const bool enabled = [](){
        const char* v = std::getenv("PSR_JIT_TIER");
        // Default ON; only an explicit 0/false/no disables it.
        if (v == nullptr) return true;
        return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' ||
                 v[0] == 'n' || v[0] == 'N');
    }();
    return enabled;
}

static void unhandled_lookup_trampoline(uint8_t* rdram, recomp_context* ctx) {
    // ── Interior-return self-heal (tier 2 before the loud abort) ─────────
    // The dominant miss class is a return address interior to a resident
    // fragment function (the tailcall-bubble dispatcher re-enters via
    // get_function(return_addr); mid-function points are never registered).
    // Resolve by dispatching into the enclosing function instead of aborting.
    // get_function already captured this miss (deduped, loud-once) for
    // fold-back telemetry, so the self-heal stays silent.
    {
        const uint32_t addr = g_self_heal_addr;
        recomp_func_t* host = nullptr;
        uint32_t link_target = 0;
        uint32_t func_start_link = 0;
        bool found = false;
        {
            std::shared_lock<std::shared_mutex> lock(func_map_mutex);
            found = find_resident_enclosing_function(
                addr, host, link_target, func_start_link);
        }
        if (found && host != nullptr) {
            g_tier_self_heals.fetch_add(1, std::memory_order_relaxed);
            // Resume at the interior link vram via the generated dispatch
            // switch; if the miss was actually a function start, enter
            // normally (dispatch_entry_target = 0). Release the func_map
            // lock before calling host — it may re-enter get_function.
            const uint32_t saved = ctx->dispatch_entry_target;
            ctx->dispatch_entry_target =
                (link_target == func_start_link) ? 0u : link_target;
            host(rdram, ctx);
            ctx->dispatch_entry_target = saved;
            return;
        }
    }
    g_tier_self_heal_misses.fetch_add(1, std::memory_order_relaxed);

    // ── B3: runtime JIT tier (tier 3 before the loud abort) ──────────────
    // No registered function and no resident enclosing function — a TRUE gap.
    // JIT the function from its resident rdram image, register it, and run it.
    // DEFAULT ON (PSR_JIT_TIER=0 to force-disable). Eligible ONLY for static-
    // section gaps (addr_in_resident_static_section); overlay-arena addresses
    // are excluded and fall through to the graceful loud abort. On success we
    // recover; on failure we fall through to the same loud abort as before.
    bool jit_eligible = false;
    if (jit_tier_enabled()) {
        // Eligibility: B3 only handles genuine STATIC-section gaps. Overlay
        // arena / loaded-fragment addresses violate its literal-compile model
        // (see addr_in_resident_static_section) and JITing one crashes — those
        // fall through to the graceful loud abort below.
        {
            std::shared_lock<std::shared_mutex> lock(func_map_mutex);
            jit_eligible =
                addr_in_resident_static_section((uint32_t)g_self_heal_addr);
        }
        if (!jit_eligible) {
            fprintf(stderr,
                "[recomp] 0x%08X is not in a resident static section — B3 not "
                "eligible (overlay/fragment gap), skipping JIT\n",
                (uint32_t)g_self_heal_addr);
            fflush(stderr);
        }
    }
    if (jit_tier_enabled() && jit_eligible) {
        std::string jit_err;
        recomp_func_t* jitted =
            jit_compile_function((uint32_t)g_self_heal_addr, rdram, jit_err);
        if (jitted != nullptr) {
            g_tier_jit_compiles.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr,
                "[recomp] JIT-compiled missing function 0x%08X — registered; "
                "future calls take the static fast path\n",
                (uint32_t)g_self_heal_addr);
            fflush(stderr);
            jitted(rdram, ctx);
            return;
        }
        g_tier_jit_failures.fetch_add(1, std::memory_order_relaxed);
        fprintf(stderr,
            "[recomp] JIT of 0x%08X not possible (%s) — aborting\n",
            (uint32_t)g_self_heal_addr, jit_err.c_str());
        fflush(stderr);
    }

    // B3 in-game test harness: if this address was deliberately evicted and
    // B3 couldn't recover it (above), restore the saved static function and
    // run it instead of aborting — so the evict-all test can never crash the
    // game. (B3 SUCCESS already returned above, having re-registered vram.)
    {
        recomp_func_t* restore = nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(func_map_mutex);
            auto it = g_evicted_funcs.find((int32_t)g_self_heal_addr);
            if (it != g_evicted_funcs.end()) {
                restore = it->second;
                func_map[(int32_t)g_self_heal_addr] = restore;  // re-register
                g_evicted_funcs.erase(it);
            }
        }
        if (restore != nullptr) {
            fprintf(stderr,
                "[jit-evict-test] 0x%08X not handled by B3 — restored static "
                "func (no abort)\n", (uint32_t)g_self_heal_addr);
            fflush(stderr);
            restore(rdram, ctx);
            return;
        }
    }

    // ── Content-keyed fragment tier (FRAGMENT_TIERS.md §8) ───────────────
    // For a fragment (non-resident-static) miss, discover its bounds and track
    // a content-keyed candidate in g_frag_cands. With PSR_FRAG_JIT alone (slice
    // 1-2) this is pure bookkeeping (discover + key + persist) and execution
    // falls through to the interpreter floor below. With PSR_FRAG_NATIVE also
    // set (slice 3) a safe-leaf candidate is JIT'd (kept alive, NOT in func_map)
    // and run through the same-state differential gate; once it passes BUDGET
    // consecutive clean diffs vs the interpreter it runs native live. Until then
    // the interpreter result is what's committed, so an unvalidated shard never
    // affects the game. Gated OFF unless PSR_FRAG_JIT=1.
    //
    // !t_shadow_active: never re-enter the fragment tier from inside a shadow
    // diff. A safe-leaf candidate makes no calls so its diff can't reach here,
    // but if the static leaf check is ever wrong (data misread as code) the
    // PASS-1 interpreter could make a native call that lookup-misses back into
    // this trampoline — a nested miss during a diff just interprets instead.
    if (frag_jit_enabled() && !t_shadow_active) {
        const uint32_t faddr = (uint32_t)g_self_heal_addr;
        if (faddr >= 0x80000000u && faddr < 0x80800000u) {
            bool resident_static;
            {
                std::shared_lock<std::shared_mutex> lock(func_map_mutex);
                resident_static = addr_in_resident_static_section(faddr);
            }
            if (!resident_static) {
                // Find a candidate whose content still matches the live bytes,
                // copying out the fields we need so we can act WITHOUT the lock
                // (the diff/native run must not hold func_map_mutex).
                bool have_live = false, live_dt = false, live_bl = false;
                uint64_t live_hash = 0;
                uint32_t live_lo = 0, live_len = 0, live_passes = 0;
                recomp_func_t* live_fn = nullptr;
                {
                    std::shared_lock<std::shared_mutex> lock(func_map_mutex);
                    auto it = g_frag_cands.find(faddr);
                    if (it != g_frag_cands.end()) {
                        for (const auto& c : it->second) {
                            if (c.code_len > 0 &&
                                fragment_live_code_hash(rdram, c.code_lo, c.code_len)
                                    == c.content_hash) {
                                have_live = true;   live_hash = c.content_hash;
                                live_lo = c.code_lo; live_len = c.code_len;
                                live_fn = c.fn;     live_passes = c.diff_passes;
                                live_dt = c.device_touch; live_bl = c.blacklisted;
                                break;
                            }
                        }
                    }
                }

                if (!have_live) {
                    // New content at this address: discover bounds + key it.
                    const uint32_t pbase = faddr & 0x1FFFFFFFu;
                    constexpr uint32_t RDRAM_END = 0x00800000u;
                    uint32_t window = 0x8000u;
                    if (pbase + window > RDRAM_END) window = RDRAM_END - pbase;
                    if (window >= 8) {
                        std::vector<uint8_t> fbody(window);
                        for (uint32_t i = 0; i < window; i++) {
                            fbody[i] = rdram[(pbase + i) ^ 3];
                        }
                        size_t fsz = 0;
                        std::string ferr;
                        if (N64Recomp::discover_function_bounds(
                                fbody.data(), fbody.size(), faddr, 0, fsz, ferr) &&
                            fsz >= 8 && (fsz & 3u) == 0 && fsz <= window) {
                            FragmentCandidate c;
                            c.addr = faddr;
                            c.code_lo = faddr;
                            c.code_len = (uint32_t)fsz;
                            c.content_hash =
                                fragment_live_code_hash(rdram, faddr, (uint32_t)fsz);
                            // Slice 3: JIT a safe-leaf candidate now (kept alive,
                            // NOT registered in func_map). A non-leaf or JIT
                            // failure leaves fn=null -> stays on the interpreter.
                            if (frag_native_enabled()) {
                                if (fragment_is_safe_leaf(rdram, faddr, (uint32_t)fsz)) {
                                    std::string jerr; size_t js = 0, cs = 0;
                                    recomp_func_t* fn = jit_compile_function(
                                        faddr, rdram, jerr, /*keep=*/true,
                                        /*register_in_map=*/false, &js, &cs);
                                    if (fn) {
                                        c.fn = fn;
                                    }
                                } else {
                                    c.device_touch = true; // outgoing control -> never native
                                }
                            }
                            {
                                std::unique_lock<std::shared_mutex> lock(func_map_mutex);
                                g_frag_cands[faddr].push_back(c);
                                // Persist the updated candidate set to the
                                // coverage manifest (cross-session re-JIT
                                // currency). Done under the lock since it reads
                                // g_frag_cands; cheap (rewrite of a handful).
                                persist_fragment_manifest_locked();
                            }
                            g_tier_frag_candidates.fetch_add(
                                1, std::memory_order_relaxed);
                            if (c.device_touch) {
                                g_tier_frag_device_touch.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            // Reflect the new candidate into the live_* locals so
                            // the execution decision below uses it this dispatch.
                            have_live = true; live_hash = c.content_hash;
                            live_lo = c.code_lo; live_len = c.code_len;
                            live_fn = c.fn; live_passes = 0;
                            live_dt = c.device_touch; live_bl = false;
                        }
                    }
                } else if (frag_native_enabled() && live_fn == nullptr &&
                           !live_dt && !live_bl && live_len > 0) {
                    // Live candidate with no native shard yet (e.g. reloaded from
                    // the manifest, or a prior JIT was deferred): JIT it now.
                    if (fragment_is_safe_leaf(rdram, live_lo, live_len)) {
                        std::string jerr; size_t js = 0, cs = 0;
                        recomp_func_t* fn = jit_compile_function(
                            faddr, rdram, jerr, /*keep=*/true,
                            /*register_in_map=*/false, &js, &cs);
                        std::unique_lock<std::shared_mutex> lock(func_map_mutex);
                        auto it = g_frag_cands.find(faddr);
                        if (it != g_frag_cands.end()) {
                            for (auto& c : it->second) {
                                if (c.content_hash != live_hash) continue;
                                if (fn) { c.fn = fn; live_fn = fn; }
                                break;
                            }
                        }
                    } else {
                        std::unique_lock<std::shared_mutex> lock(func_map_mutex);
                        auto it = g_frag_cands.find(faddr);
                        if (it != g_frag_cands.end()) {
                            for (auto& c : it->second) {
                                if (c.content_hash != live_hash) continue;
                                if (!c.device_touch) {
                                    g_tier_frag_device_touch.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                                c.device_touch = true; live_dt = true;
                                break;
                            }
                        }
                    }
                }

                // Slice 3 execution decision (only when native is armed).
                if (frag_native_enabled() && have_live) {
                    if (live_fn && !live_dt && !live_bl &&
                        live_passes >= frag_diff_budget()) {
                        // PROMOTED: BUDGET clean diffs passed — run native live.
                        live_fn(rdram, ctx);
                        g_tier_frag_native_runs.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    if (live_fn && !live_dt && !live_bl) {
                        // Validating: one differential pass (interp result committed).
                        ShadowDiffOutcome o = run_shadow_diff(rdram, ctx, faddr, live_fn);
                        if (o.interp_ok) {
                            frag_apply_diff_outcome(faddr, live_hash, o);
                            g_tier_interp_runs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        // interp failed inside the diff -> fall through to the
                        // interp tier below for the loud abort + diagnostics.
                    } else if (live_dt || live_bl) {
                        // Pinned to the interpreter (device touch / blacklisted).
                        if (recomp_interpret_function(rdram, ctx, faddr)) {
                            g_tier_interp_runs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                    }
                }
            }
        }
    }

    // B-interp tier (FRAGMENT_TIERS.md #1): B3 above only handles resident
    // static-section gaps; an indirectly-reached fragment-interior entry is
    // ineligible there. Rather than abort, INTERPRET the function from the
    // missed address against the live ctx/rdram — the correctness floor. A
    // missed function is thus safe (slow), not fatal. Disable: PSR_INTERP_TIER=0.
    {
        static const bool interp_enabled = []{
            const char* e = std::getenv("PSR_INTERP_TIER");
            return !(e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N'));
        }();
        if (interp_enabled) {
            if (recomp_interpret_function(rdram, ctx, (uint32_t)g_self_heal_addr)) {
                g_tier_interp_runs.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            fprintf(stderr,
                "[recomp] interpreter could not run 0x%08X (unimplemented op above) "
                "— aborting\n", (uint32_t)g_self_heal_addr);
            fflush(stderr);
        }
    }

    fprintf(stderr,
        "[recomp] lookup-miss trampoline reached — aborting\n"
        "  bad function pointer: 0x%08X\n",
        g_last_lookup_miss_addr);
    FILE* f = open_last_error_log("a");
    if (f) {
        fprintf(f,
            "\n=== lookup-miss trampoline reached (post-call) ===\n"
            "  bad function pointer: 0x%08X\n",
            g_last_lookup_miss_addr);
        {
            std::shared_lock<std::shared_mutex> lock(func_map_mutex);
            dump_lookup_addr_classification(f, (uint32_t)g_last_lookup_miss_addr);
        }
        dump_lookup_context(f, rdram, ctx);
#ifdef _WIN32
        // Host stack backtrace — the immediate caller of the trampoline
        // is the recompiled function that invoked the bad pointer.
        // Symbol resolution gives source-file + line of that caller, so
        // we can identify which recompiled MIPS instruction was the
        // indirect call site.
        HANDLE proc = GetCurrentProcess();
        SymInitialize(proc, NULL, TRUE);
        void* frames[24];
        USHORT n = CaptureStackBackTrace(0, 24, frames, NULL);
        char symbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        fprintf(f, "  host backtrace (caller of trampoline = bad-call site):\n");
        for (USHORT i = 0; i < n; i++) {
            DWORD64 disp64 = 0;
            DWORD disp32 = 0;
            const char* name = "?";
            if (SymFromAddr(proc, (DWORD64)frames[i], &disp64, sym)) name = sym->Name;
            const char* file = "?"; DWORD lineno = 0;
            if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &disp32, &line)) {
                file = line.FileName; lineno = line.LineNumber;
            }
            fprintf(f, "    #%02u 0x%016llX %s (%s:%lu)\n",
                i, (unsigned long long)(uintptr_t)frames[i], name, file, lineno);
        }
#endif
        // Dump last 64 trace ring entries so we can see who called
        // into the bogus function pointer.
        uint64_t cap  = (uint64_t)pkmnstadium_trace_capacity();
        uint64_t widx = pkmnstadium_trace_write_idx();
        fprintf(f, "  trace ring (write_idx=%llu, capacity=%llu):\n",
            (unsigned long long)widx, (unsigned long long)cap);
        if (cap > 0) {
            uint64_t n = (widx < 64) ? widx : 64;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t slot = (widx - n + i) % cap;
                const char* name = pkmnstadium_trace_at(slot);
                fprintf(f, "    %4llu: %s\n",
                    (unsigned long long)slot, name ? name : "(null)");
            }
        }
        fclose(f);
    }
    // Persist final capture counts before aborting. Live updates only
    // fire on the first sighting of each address, so this flush captures
    // the accurate hit_count accumulated since.
    {
        std::lock_guard<std::mutex> clk(g_lookup_capture_mutex);
        write_runtime_captures_locked();
    }
    // Trigger the unified post-mortem so build/last_run_report.json +
    // build/last_run_input_history.json + RDRAM dump capture context
    // for this controlled abort, the same way SEH crashes do. Without
    // this the lookup-miss path is invisible to the diagnostic tools
    // and we lose the input history needed to replay-repro.
    psr_post_mortem_dump("lookup-miss-trampoline", nullptr);
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

// Given a fragment-space link vaddr AND the host PC of the caller,
// resolve the link vaddr against an appropriate variant. Strategy:
//
//   1. If the caller's section is itself a variant in this bucket
//      AND covers the offset → resolve against caller. Best case;
//      "the variant the calling code lives in."
//
//   2. Otherwise, find the bucket variant most recently registered
//      BEFORE the caller's section. Idea: that's the variant that
//      was "live" when the caller was loaded, and the caller's
//      embedded fragment-vaddr literals were resolved against it
//      at fragment relocation time.
//
//   3. If neither finds a variant whose size covers the offset,
//      return 0 (let the game's native resolution stand).
//
// Returns the resolved runtime address, or 0 if no variant found.
// Test whether `addr` lies inside ANY currently-loaded variant of the
// given link-time bucket (= section.ram_addr & 0xFFF00000 high bits).
// Returns 1 if some loaded section L has L.ram_addr == bucket and
// addr ∈ [L.loaded_ram_addr, L.loaded_ram_addr + L.size); 0 otherwise.
extern "C" int recomp_addr_in_loaded_variant(uint32_t bucket, uint32_t addr) {
    if (sections_info.code_sections == nullptr) return 0;
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        if (uint32_t(sec.ram_addr) != bucket) continue;
        uint32_t base = (uint32_t)ls.loaded_ram_addr;
        if (addr >= base && addr < base + sec.size) return 1;
    }
    return 0;
}

extern "C" int32_t recomp_resolve_fragment_via_caller_pc(
    uint32_t link_vaddr, uintptr_t caller_pc)
{
    if (link_vaddr < 0x81000000u || link_vaddr >= 0x90000000u) return 0;
    const uint32_t bucket = link_vaddr & 0xFFF00000u;
    const uint32_t offset = link_vaddr & 0x000FFFFFu;
    if (sections_info.code_sections == nullptr) return 0;

    const size_t caller_idx = pc_index_lookup(caller_pc);
    if (caller_idx == size_t(-1)) return 0;
    const SectionTableEntry& caller_sec =
        sections_info.code_sections[caller_idx];

    // Strategy 1: caller is a variant of this bucket.
    if (uint32_t(caller_sec.ram_addr) == bucket && offset < caller_sec.size) {
        for (const auto& ls : loaded_sections) {
            if (ls.section_table_index == caller_idx) {
                return ls.loaded_ram_addr + int32_t(offset);
            }
        }
    }

    // Strategy 2: caller is NOT a bucket variant. Find the bucket
    // variant most-recently registered BEFORE the caller, whose
    // size covers the offset.
    const uint64_t caller_order = get_load_order(caller_idx);
    if (caller_order == 0) return 0;
    int32_t  best_addr = 0;
    uint64_t best_order = 0;
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        if (uint32_t(sec.ram_addr) != bucket) continue;
        if (offset >= sec.size) continue;
        const uint64_t order = get_load_order(ls.section_table_index);
        if (order == 0 || order >= caller_order) continue;
        if (order > best_order) {
            best_order = order;
            best_addr  = ls.loaded_ram_addr + int32_t(offset);
        }
    }
    return best_addr;
}

// Offset-aware fragment-vaddr lookup. Given a fragment-space link
// vaddr (e.g. 0x8FF0ABFC), find the loaded variant whose link-time
// ram_addr matches the high bits AND whose size covers the offset.
// Returns the runtime address (loaded_ram_addr + offset) if a match
// is found; returns 0 if no variant covers this offset.
//
// Why this exists: when multiple decompressed_section_pattern
// variants share a single link-time vram bucket (e.g. all stadium-
// models pattern fragments at 0x8FF00000), the game's gFragments
// table holds a single pointer per id — so it's clobbered on every
// new registration. A consumer asking for offset 0xABFC of fragment
// 0xEF gets resolved against whichever variant happened to be
// registered last, ignoring whether that variant actually contains
// data at offset 0xABFC. This function lets game-side hooks do the
// "find a variant whose [base, base+size) actually contains the
// requested offset" walk that the game's single-pointer dispatch
// can't.
//
// Tiebreak: prefer the SMALLEST variant whose size strictly contains
// the requested offset. Empirical observation in Pokemon Stadium:
// pattern-fragment variants registered for vram 0x8FF00000 each
// correspond to a specific stadium-models sub-asset, and consumers
// (e.g. fragment62's process_geo_layout calls) target the smallest
// variant that covers their offset. Larger variants are stale
// leftovers from previous loads that haven't been evicted.
//
// If multiple variants tie on size (rare), pick the most recently
// registered (back-to-front in loaded_sections).
extern "C" int32_t recomp_lookup_fragment_offset(uint32_t link_vaddr) {
    if (link_vaddr < 0x81000000u || link_vaddr >= 0x90000000u) return 0;
    const uint32_t bucket = link_vaddr & 0xFFF00000u;
    const uint32_t offset = link_vaddr & 0x000FFFFFu;
    if (sections_info.code_sections == nullptr) return 0;

    int32_t best_addr = 0;
    uint32_t best_size = ~0u;
    for (auto it = loaded_sections.rbegin();
         it != loaded_sections.rend(); ++it) {
        const SectionTableEntry& sec =
            sections_info.code_sections[it->section_table_index];
        if (uint32_t(sec.ram_addr) != bucket) continue;
        if (offset >= sec.size) continue;
        if (sec.size < best_size) {
            best_size = sec.size;
            best_addr = it->loaded_ram_addr + int32_t(offset);
        }
    }
    return best_addr;
}

// Data-context-driven fragment-vaddr resolution.
//
// `link_vaddr` is a fragment-space address (e.g. 0x8FF0ABFC) the game
// is asking to convert to a runtime address. `data_ctx_addr` is an
// RDRAM pointer into whatever data the GAME is currently walking
// (e.g. process_geo_layout's gGeoLayoutCommand) at the moment of
// the call.
//
// Why this is the right resolution rule for pattern-fragment buckets:
// in the original game, the geo walker only ever walks the variant
// pointed to by gFragments[id]. Embedded 0x8FF0XXXX literals in that
// variant's command stream are intended to refer back into the same
// variant. The recompiler breaks this implicit invariant by keeping
// multiple variants host-resident concurrently with the same id, so
// gFragments[id] becomes ambiguous — but the data the walker is
// CURRENTLY reading is unambiguous: data_ctx_addr lies in exactly
// one variant's [loaded_ram_addr, +size) range. Using THAT variant
// as the resolution context restores the original game's semantics
// without any heuristics.
//
// Returns the resolved runtime address, or 0 if no variant whose
// link-time bucket matches and whose size covers `offset` contains
// `data_ctx_addr` (caller falls back to game's native answer).
// Synthetic-fragment resolver. Translates a synthetic-pool address
// (0xA0000000..0xC0000000) to its current runtime buffer + offset.
//
// Diagnostic policy: if the synthetic address is in-range but the
// table slot is empty, this is a hard error — the variant's code
// emitted a synthetic literal but the variant has not been registered
// at runtime yet. Logs loudly and aborts deterministically. This
// surfaces (a) ordering bugs where a variant's code runs before the
// game decompresses + registers it, and (b) variants whose
// content-hash failed to match at registration so the synthetic slot
// never got populated. Both are bugs we want surfaced, not masked.
//
// Returns 0 if `addr` is not in the synthetic range. Otherwise either
// returns the resolved RDRAM address (slot registered) or aborts
// (slot empty).
extern "C" int32_t recomp_resolve_synthetic_fragment(uint32_t addr) {
    if (!is_synthetic_addr(addr)) return 0;

    const size_t slot_idx = synthetic_bucket_idx(addr);
    const uint32_t offset = addr & 0x000FFFFFu;

    if (slot_idx >= kSyntheticBucketCount) {
        fprintf(stderr,
            "[synth-frag] FATAL: addr 0x%08X synthetic-bucket index %zu "
            "exceeds table size %zu\n",
            addr, slot_idx, kSyntheticBucketCount);
        fflush(stderr);
        std::abort();
    }

    const SyntheticFragmentSlot& slot = recomp_synthetic_fragments[slot_idx];

    static volatile int s_n_logged = 0;
    int log_idx = __atomic_fetch_add(&s_n_logged, 1, __ATOMIC_RELAXED);

    if (!slot.registered) {
        fprintf(stderr,
            "[synth-frag] FATAL: addr 0x%08X (slot %zu, offset 0x%X) — "
            "table slot is empty (variant not yet registered or content-"
            "hash mismatch). Aborting deterministically per the no-stubs "
            "policy: a synthetic literal escaping resolution means the "
            "variant's code ran before its register or registration "
            "failed silently.\n",
            addr, slot_idx, offset);
        fflush(stderr);
        std::abort();
    }

    if (offset >= slot.size) {
        fprintf(stderr,
            "[synth-frag] FATAL: addr 0x%08X offset 0x%X >= variant "
            "size 0x%X (slot %zu, runtime_base 0x%08X)\n",
            addr, offset, slot.size, slot_idx, slot.runtime_base);
        fflush(stderr);
        std::abort();
    }

    const uint32_t resolved = slot.runtime_base + offset;
    if (log_idx < 16) {
        fprintf(stderr,
            "[synth-frag] resolve in=0x%08X slot=%zu runtime_base=0x%08X "
            "offset=0x%X → 0x%08X\n",
            addr, slot_idx, slot.runtime_base, offset, resolved);
        fflush(stderr);
    }
    return int32_t(resolved);
}

// Geo-layout command size table for Pokemon Stadium (from
// disasm/src/geo_layout.h). Indexed by cmd byte; each entry is the
// number of bytes that command consumes from the stream. Used by the
// structural-parse probe below to walk N commands forward through a
// candidate variant's bytes and confirm they parse as coherent geo
// data, not random bytes.
//
// Stadium-specific. Lives here only as diagnostic infrastructure for
// the build-time binding-table generation pass. Do NOT bake "parse
// Pokemon Stadium geo commands" into the generic recompiler core.
static const uint8_t kStadiumGeoCmdSize[0x27] = {
    /*0x00 branch_and_link*/ 0x08, /*0x01 end*/ 0x04,
    /*0x02 jump*/            0x08, /*0x03 branch*/           0x08,
    /*0x04 return*/          0x04, /*0x05 open_node*/        0x04,
    /*0x06 close_node*/      0x04, /*0x07*/                  0x08,
    /*0x08*/                 0x0C, /*0x09*/                  0x04,
    /*0x0A*/                 0x08, /*0x0B*/                  0x18,
    /*0x0C*/                 0x04, /*0x0D*/                  0x04,
    /*0x0E*/                 0x04, /*0x0F*/                  0x04,
    /*0x10*/                 0x04, /*0x11*/                  0x04,
    /*0x12*/                 0x04, /*0x13*/                  0x08,
    /*0x14*/                 0x0C, /*0x15*/                  0x0C,
    /*0x16*/                 0x04, /*0x17*/                  0x14,
    /*0x18*/                 0x08, /*0x19*/                  0x08,
    /*0x1A*/                 0x04, /*0x1B*/                  0x10,
    /*0x1C*/                 0x10, /*0x1D*/                  0x1C,
    /*0x1E*/                 0x08, /*0x1F*/                  0x18,
    /*0x20*/                 0x14, /*0x21*/                  0x10,
    /*0x22*/                 0x08, /*0x23*/                  0x10,
    /*0x24*/                 0x04, /*0x25*/                  0x04,
    /*0x26*/                 0x14,
};

// Walk up to `max_steps` geo commands forward starting at variant's
// (runtime_base + offset). Returns the number of commands successfully
// parsed. A "successful parse" requires:
//   - cmd byte < 0x27 (in jumptable range)
//   - body+offset+cmd_size remains within the variant's [base, +size)
//   - cmd 0x01 (end) terminates the walk and counts as success
// Returns 0 if even the first byte isn't a valid opcode.
static uint32_t walk_geo_commands(uint8_t* rdram,
                                  uint32_t runtime_base,
                                  uint32_t variant_size,
                                  uint32_t offset,
                                  uint32_t max_steps)
{
    uint32_t cur = offset;
    uint32_t steps = 0;
    while (steps < max_steps && cur < variant_size) {
        const uint32_t paddr = (runtime_base + cur) & 0x1FFFFFFFu;
        if (paddr >= 8u * 1024u * 1024u) break;
        const uint8_t cmd = rdram[paddr ^ 3];
        if (cmd >= 0x27) break;
        const uint8_t sz = kStadiumGeoCmdSize[cmd];
        if (sz == 0) break;
        if (cur + sz > variant_size) break;
        steps++;
        if (cmd == 0x01) break;  // end terminates
        cur += sz;
    }
    return steps;
}

// Diagnostic-only: dump every loaded pattern variant whose size > offset,
// peek 16 bytes at runtime_base + offset, parse N=8 geo commands
// forward to validate structural shape, and flag the variant where
// parsing stays consistent. Used to answer "which variant does
// fragment62 actually want for offset 0xABFC?" with high confidence.
//
// Caller is responsible for passing the canonical pattern bucket
// (e.g. 0x8FF00000) — we don't require it to come from a synthetic
// addr. Fires fully every call (not rate-limited) — caller should
// gate to one-shot externally.
extern "C" void recomp_diag_dump_variant_candidates_for_offset(
    uint8_t* rdram,
    uint32_t pattern_id,
    uint32_t offset)
{
    if (sections_info.code_sections == nullptr) return;
    constexpr uint32_t kStructSteps = 8;
    fprintf(stderr,
        "[variant-probe] pattern_id=0x%X offset=0x%X — variants whose "
        "size covers offset, with structural parse N=%u:\n",
        pattern_id, offset, kStructSteps);
    uint32_t shown = 0;
    uint32_t winners = 0;
    size_t winner_section_idx = size_t(-1);
    uint32_t winner_link = 0;
    uint32_t winner_runtime = 0;
    uint32_t winner_steps = 0;
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        if (sec.original_pattern_id != pattern_id) continue;
        if (offset >= sec.size) continue;
        const uint32_t base = uint32_t(ls.loaded_ram_addr);
        const uint32_t paddr = (base + offset) & 0x1FFFFFFFu;
        if (paddr + 16 > 8u * 1024u * 1024u) continue;
        uint8_t bytes[16];
        for (size_t i = 0; i < 16; i++) {
            bytes[i] = rdram[(paddr + i) ^ 3];
        }
        const uint8_t cmd_byte = bytes[0];
        const uint32_t parsed = walk_geo_commands(
            rdram, base, sec.size, offset, kStructSteps);
        const bool full_parse = (parsed >= kStructSteps);
        const bool any_parse  = (parsed > 0);
        const char* tag = full_parse ? "WINNER"
                        : any_parse  ? "partial"
                        :              "noise";
        fprintf(stderr,
            "  section_idx=%zu link=0x%08X runtime=0x%08X size=0x%X "
            "hash=0x%016llX cmd=0x%02X parsed=%u/%u %s "
            "bytes=%02X%02X%02X%02X %02X%02X%02X%02X "
            "%02X%02X%02X%02X %02X%02X%02X%02X\n",
            sec.index, uint32_t(sec.ram_addr), base, sec.size,
            (unsigned long long)sec.content_hash,
            cmd_byte, parsed, kStructSteps, tag,
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        shown++;
        if (full_parse) {
            winners++;
            winner_section_idx = sec.index;
            winner_link = uint32_t(sec.ram_addr);
            winner_runtime = base;
            winner_steps = parsed;
        }
    }
    if (winners == 1) {
        fprintf(stderr,
            "[variant-probe] CENSUS pattern=0x%X offset=0x%X winner=section_idx=%zu "
            "link=0x%08X runtime=0x%08X parsed=%u/%u (single)\n",
            pattern_id, offset, winner_section_idx,
            winner_link, winner_runtime, winner_steps, kStructSteps);
    } else if (winners == 0) {
        fprintf(stderr,
            "[variant-probe] CENSUS pattern=0x%X offset=0x%X NONE "
            "(%u variants checked, none parsed full %u steps — "
            "literal probably isn't geo data)\n",
            pattern_id, offset, shown, kStructSteps);
    } else {
        fprintf(stderr,
            "[variant-probe] CENSUS pattern=0x%X offset=0x%X AMBIGUOUS "
            "(%u variants checked, %u parsed full %u steps)\n",
            pattern_id, offset, shown, winners, kStructSteps);
    }
    fflush(stderr);
}

// Diagnostic-only iterator over registered variants of a pattern_id.
// Used by Option-C probes that need to ask "of all currently-resident
// variants of stadium_models, which one provides valid geo data at
// the offset fragment62 wants?" Walks loaded_sections, filters to
// sections whose original_pattern_id matches, and returns the i-th
// match's runtime metadata. Returns 1 on success, 0 if no i-th
// match exists.
extern "C" int recomp_get_pattern_variant_info(
    uint32_t pattern_id,
    uint32_t idx,
    uint32_t* out_runtime_base,
    uint32_t* out_size,
    uint32_t* out_synthetic_link,
    uint32_t* out_section_index)
{
    if (sections_info.code_sections == nullptr) return 0;
    uint32_t seen = 0;
    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        if (sec.original_pattern_id != pattern_id) continue;
        if (seen == idx) {
            if (out_runtime_base) *out_runtime_base = uint32_t(ls.loaded_ram_addr);
            if (out_size)         *out_size         = sec.size;
            if (out_synthetic_link) *out_synthetic_link = uint32_t(sec.ram_addr);
            if (out_section_index) *out_section_index = uint32_t(sec.index);
            return 1;
        }
        seen++;
    }
    return 0;
}

extern "C" int32_t recomp_resolve_via_data_context(
    uint32_t link_vaddr, uint32_t data_ctx_addr)
{
    if (link_vaddr < 0x81000000u || link_vaddr >= 0x90000000u) return 0;
    if (data_ctx_addr == 0) return 0;
    const uint32_t bucket = link_vaddr & 0xFFF00000u;
    const uint32_t offset = link_vaddr & 0x000FFFFFu;
    if (sections_info.code_sections == nullptr) return 0;

    for (const auto& ls : loaded_sections) {
        const SectionTableEntry& sec =
            sections_info.code_sections[ls.section_table_index];
        const uint32_t base = (uint32_t)ls.loaded_ram_addr;
        if (data_ctx_addr < base || data_ctx_addr >= base + sec.size) {
            continue;
        }
        // Walker is inside this variant. Only useful if the variant
        // covers the requested bucket+offset.
        if (uint32_t(sec.ram_addr) != bucket) return 0;
        if (offset >= sec.size) return 0;
        return ls.loaded_ram_addr + int32_t(offset);
    }
    return 0;
}

// ── Fragment-vaddr resolution helpers ─────────────────────────────
//
// Games that ship variant-aware fragments (Pokemon Stadium pattern-bucket
// fragments at the 0x8FF00000 link-vram, etc.) need a runtime resolver
// to disambiguate a single game-side fragment id between multiple
// concurrently-loaded variants. The original game maintained an implicit
// invariant: while walking variant X's data, gFragments[X.id] points to
// X's buffer, and embedded link-vaddr literals resolve against X. In the
// recompiler, multiple variants are host-resident concurrently, so the
// single-pointer-per-id model becomes ambiguous.
//
// The three helpers above (recomp_addr_in_loaded_variant,
// recomp_resolve_synthetic_fragment, recomp_resolve_via_data_context)
// each handle one piece of that disambiguation. recomp_resolve_fragment_vaddr
// below orchestrates them in priority order so a game-side hook on the
// fragment-vaddr-resolver function is a one-liner.
//
// Pairs with librecomp_fragment_input_push / _pop for the typical
// entry-hook-saves-input / exit-hook-uses-input pattern. The TLS stack
// handles recursive calls.

namespace {
    constexpr int kFragmentInputStackDepth = 16;
    thread_local uint32_t s_fragment_input_stack[kFragmentInputStackDepth];
    thread_local int      s_fragment_input_sp = 0;
}

extern "C" void librecomp_fragment_input_push(uint32_t input) {
    if (s_fragment_input_sp < kFragmentInputStackDepth) {
        s_fragment_input_stack[s_fragment_input_sp] = input;
    }
    s_fragment_input_sp++;
}

extern "C" uint32_t librecomp_fragment_input_pop(void) {
    s_fragment_input_sp--;
    int idx = (s_fragment_input_sp >= 0 && s_fragment_input_sp < kFragmentInputStackDepth)
        ? s_fragment_input_sp : 0;
    return s_fragment_input_stack[idx];
}

// Unified fragment-vaddr resolver. Pops the matching push_input from
// the TLS stack, runs the 3-step orchestration, returns the resolved
// vaddr (or game_result if no override applies).
//
// Game-side hook usage (toml):
//   [[patches.hook]]              # entry
//   func = "<game's resolver>"
//   before_vram = <entry>
//   text = "librecomp_fragment_input_push((uint32_t)ctx->r4);"
//
//   [[patches.hook]]              # exit
//   func = "<game's resolver>"
//   before_vram = <exit>
//   text = "ctx->r4 = librecomp_fragment_resolve_exit(
//             (uint32_t)ctx->r4, (uint32_t)MEM_W(0, <walker_state_vaddr>));"
extern "C" uint32_t librecomp_fragment_resolve_exit(
    uint32_t game_result, uint32_t data_ctx_addr)
{
    const uint32_t input = librecomp_fragment_input_pop();

    // Step 1: synthetic-fragment pool (highest priority). For inputs in
    // the per-variant synthetic-vram range, resolve via the parallel
    // recomp_synthetic_fragments[] table, bypassing the game's native
    // gFragments[id]. The native game path returned the input unchanged
    // here because the input is outside the range the game recognizes
    // — we substitute our resolution.
    {
        int32_t synth = recomp_resolve_synthetic_fragment(input);
        if (synth != 0) {
            return (uint32_t)synth;
        }
    }

    // Bounded scope: only the known-ambiguous 0x8FF00000 bucket (the
    // pattern-bucket family). Other inputs use the game's native answer
    // untouched — single-variant fragments need no disambiguation.
    if ((input & 0xFFF00000u) != 0x8FF00000u) return game_result;
    if (input < 0x81000000u || input >= 0x90000000u) return game_result;

    // Step 2: trust the game's answer when it lands inside a registered
    // variant of this bucket. Steady-state walks where gFragments[id]
    // happens to point to the correct variant.
    if (recomp_addr_in_loaded_variant(input & 0xFFF00000u, game_result)) {
        return game_result;
    }

    // Step 3: data-context resolution. The walker's current data
    // pointer lives in exactly one variant's RDRAM buffer — resolve
    // against that variant.
    int32_t resolved = recomp_resolve_via_data_context(input, data_ctx_addr);
    if (resolved != 0) return (uint32_t)resolved;

    // Step 4: fall back to the game's answer. Deliberately do NOT pick
    // a variant by heuristic — the underlying issue would be a missing
    // variant load in the game-state orchestration, and a heuristic
    // pick would silently mask it. The lookup-miss in callers IS the
    // diagnostic.
    return game_result;
}

// Pure func_map query for the interpreter's native-boundary check: returns the
// real recompiled function at `addr`, or nullptr if none. Unlike get_function,
// it returns nullptr (NOT the lookup-miss trampoline) on a miss and has no
// capture/log side effects — so the interpreter can decide "call native vs
// interpret" without re-entering the trampoline (which would recurse unbounded).
extern "C" recomp_func_t* recomp_lookup_function_or_null(int32_t addr) {
    std::shared_lock<std::shared_mutex> lock(func_map_mutex);
    auto it = func_map.find(addr);
    return (it != func_map.end()) ? it->second : nullptr;
}

extern "C" recomp_func_t * get_function(int32_t addr) {
    std::shared_lock<std::shared_mutex> lock(func_map_mutex);
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        g_tier_lookup_misses.fetch_add(1, std::memory_order_relaxed);

        // Capture ground-truth for this miss, deduped by address. First
        // sighting: classify, log loudly, refresh the captures file.
        // Repeat hits: silent counter bump only (no spam, no I/O flood).
        bool first_sighting = false;
        {
            std::lock_guard<std::mutex> clk(g_lookup_capture_mutex);
            auto emplaced = g_lookup_captures.try_emplace((uint32_t)addr);
            LookupCapture& cap = emplaced.first->second;
            if (emplaced.second) {
                classify_lookup_capture((uint32_t)addr, cap);
                first_sighting = true;
            }
            cap.hit_count++;
            if (first_sighting) {
                write_runtime_captures_locked();
            }
        }

        if (first_sighting) {
            FILE* f = open_last_error_log("a");
            if (f) {
                fprintf(f, "\n=== get_function lookup miss: 0x%08X ===\n", addr);
                dump_lookup_addr_classification(f, (uint32_t)addr);
                fclose(f);
            }
            fprintf(stderr,
                "[Warn] get_function lookup miss: 0x%08X — captured (see runtime_captures.json), returning trampoline\n",
                addr);
            fflush(stderr);
        }
        // Stash for the trampoline so post-call diagnostics print
        // *which* address was missing, not just "something bad happened".
        // g_self_heal_addr is thread-local (race-free across concurrent
        // missing threads); g_last_lookup_miss_addr stays for the abort dump.
        g_last_lookup_miss_addr = addr;
        g_self_heal_addr = (uint32_t)addr;
        return unhandled_lookup_trampoline;
    }
    g_tier_static_hits.fetch_add(1, std::memory_order_relaxed);
    return func_find->second;
}

// ---------------------------------------------------------------------
// Debug-only hooks to exercise the always-on lookup-miss capture without
// a real crash (PMS has no benign misses — a real miss IS a crash). Both
// are invoked only from explicit debug-server commands; they never run on
// their own. See PocketMonstersStadiumRecomp/src/main/debug_server.cpp.
// ---------------------------------------------------------------------

// Dump the live loaded-section table to build/loaded_sections.json so a
// probe can pick a guaranteed-interior offset (code-entry) and a
// guaranteed reloc offset (pointer-site) from REAL section data rather
// than guessing addresses.
extern "C" void recomp_debug_dump_loaded_sections(void) {
    FILE* f = fopen("build/loaded_sections.json", "w");
    if (f == nullptr) {
        f = fopen("loaded_sections.json", "w");
    }
    if (f == nullptr) {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(func_map_mutex);
    fprintf(f, "[\n");
    bool first = true;
    if (sections_info.code_sections != nullptr) {
        for (const auto& ls : loaded_sections) {
            const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
            uint32_t runtime_base = (uint32_t)ls.loaded_ram_addr;

            // A guaranteed-interior, non-4-aligned offset: cannot be a
            // (4-aligned) function start and cannot match a (4-aligned)
            // reloc offset -> probing it yields a "code-entry" miss.
            uint32_t code_off = (sec.size > 8) ? (((sec.size / 2) & ~3u) | 1u) : 1u;

            // The first reloc offset that is NOT a function start: probing
            // it yields a "pointer-site" miss (reloc present, but the
            // address is not a registered function so get_function misses).
            std::unordered_set<uint32_t> func_starts;
            func_starts.reserve(sec.num_funcs * 2);
            for (size_t i = 0; i < sec.num_funcs; i++) {
                func_starts.insert(sec.funcs[i].offset);
            }
            uint32_t ptr_off = 0xFFFFFFFFu;
            for (size_t i = 0; i < sec.num_relocs; i++) {
                uint32_t off = sec.relocs[i].offset;
                if (off < sec.size && func_starts.find(off) == func_starts.end()) {
                    ptr_off = off;
                    break;
                }
            }

            char ptr_field[24];
            if (ptr_off == 0xFFFFFFFFu) {
                snprintf(ptr_field, sizeof(ptr_field), "null");
            } else {
                snprintf(ptr_field, sizeof(ptr_field), "\"0x%08X\"", runtime_base + ptr_off);
            }
            fprintf(f,
                "%s  {\"index\":%zu,\"runtime_base\":\"0x%08X\",\"link_base\":\"0x%08X\","
                "\"size\":\"0x%X\",\"content_hash\":\"0x%016llX\",\"num_funcs\":%zu,"
                "\"num_relocs\":%zu,\"probe_code_addr\":\"0x%08X\",\"probe_pointer_addr\":%s}",
                first ? "" : ",\n",
                ls.section_table_index, runtime_base, sec.ram_addr,
                sec.size, (unsigned long long)sec.content_hash, sec.num_funcs,
                sec.num_relocs, runtime_base + code_off, ptr_field);
            first = false;
        }
    }
    fprintf(f, "\n]\n");
    fclose(f);
}

// Force a get_function lookup for `addr` to drive the capture pipeline.
// Returns 1 if the lookup missed (capture fired), 0 if it resolved to a
// real function. Does NOT invoke the returned pointer, so a miss records
// + returns without aborting.
extern "C" int recomp_debug_probe_lookup(uint32_t addr) {
    recomp_func_t* f = get_function((int32_t)addr);
    return (f == unhandled_lookup_trampoline) ? 1 : 0;
}

// Forced-JIT validation hook (B3). Compile-only: exercises the full runtime
// JIT pipeline (rdram read -> discover bounds -> Context -> LiveRecomp ->
// native code) on a RESIDENT function without registering it, so the live
// func_map is untouched. Returns 0 on success, 1 on failure; writes the
// discovered function size, generated code size, and any error to *out_*.
// Pick a resident main-RDRAM function (0x80000000-0x807FFFFF) — overlay link
// addresses have no loaded image and are (correctly) rejected.
extern "C" unsigned char* recomp_runtime_get_rdram(void);
extern "C" int recomp_debug_jit_test(uint32_t vram,
                                     uint32_t* out_func_size,
                                     uint32_t* out_code_size,
                                     char* out_err, size_t out_err_cap) {
    uint8_t* rdram = recomp_runtime_get_rdram();
    if (out_func_size) *out_func_size = 0;
    if (out_code_size) *out_code_size = 0;
    if (out_err && out_err_cap) out_err[0] = '\0';
    if (rdram == nullptr) {
        if (out_err && out_err_cap) {
            std::strncpy(out_err, "rdram not available", out_err_cap - 1);
        }
        return 1;
    }
    std::string err;
    size_t func_size = 0, code_size = 0;
    recomp_func_t* res = jit_compile_function(
        vram, rdram, err, /*keep=*/false, /*register_in_map=*/false, &func_size, &code_size);
    if (out_func_size) *out_func_size = (uint32_t)func_size;
    if (out_code_size) *out_code_size = (uint32_t)code_size;
    if (res == nullptr) {
        if (out_err && out_err_cap) {
            std::strncpy(out_err, err.c_str(), out_err_cap - 1);
            out_err[out_err_cap - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

// B3 in-game execution test: evict every RESIDENT kseg0 function from func_map
// (saving each so the trampoline can restore it on B3 failure). Direct jal
// calls in static code are direct C calls and bypass func_map, so they keep
// working; only INDIRECT calls (jalr / function-pointer dispatch) lookup-miss
// and route through the dispatch tiers — for a resident whole-function miss
// self-heal returns false, so B3 JITs + runs + re-registers it. If after this
// the game keeps running correctly, B3's successful compiles all EXECUTE
// correctly on real gameplay call sites. Returns the number evicted.
extern "C" int recomp_debug_jit_evict_all_resident(void) {
    int n = 0;
    std::unique_lock<std::shared_mutex> lock(func_map_mutex);
    for (auto it = func_map.begin(); it != func_map.end(); ) {
        const uint32_t a = (uint32_t)it->first;
        if (a >= 0x80000000u && a < 0x80800000u) {
            g_evicted_funcs[it->first] = it->second;
            it = func_map.erase(it);
            n++;
        } else {
            ++it;
        }
    }
    fprintf(stderr, "[jit-evict-test] evicted %d resident functions from "
                    "func_map; indirect calls will now route through B3\n", n);
    fflush(stderr);
    return n;
}

// ── Slice 3 native-execution self-test (FRAGMENT_TIERS.md §8.7) ────────────
// The in-game fragment candidates on Stadium 2 are all non-leaf (correctly
// pinned to the interpreter), so the live workload can't exercise the native
// promotion/execution path — the same structural gap B3 had. This on-demand
// probe proves that path end-to-end on a hand-built register-only leaf in a
// PRIVATE 8 MiB scratch buffer (never the live game RAM, so it's safe to run at
// any time and is single-threaded by construction). It runs the exact slice-3
// pipeline — fragment_is_safe_leaf -> jit_compile_function(keep, no-register) ->
// run_shadow_diff to promotion -> a direct promoted native call — and checks
// native == interpreter == the known arithmetic result. Returns a JSON string.
extern "C" const char* recomp_debug_frag_native_selftest(void) {
    static thread_local std::string out;
    // Known leaf: addu $v0,$a0,$a1 ; jr $ra ; nop   (register-only, no calls).
    static const uint32_t LEAF[3] = { 0x00851021u, 0x03E00008u, 0x00000000u };
    constexpr uint32_t SCRATCH_VRAM = 0x80001000u;
    constexpr size_t   RAM = 0x800000;
    constexpr uint32_t A = 0x12340000u, B = 0x0000ABCDu;   // -> r4 ($a0), r5 ($a1)
    const uint32_t EXPECT = A + B;                          // addu result in r2 ($v0)

    std::vector<uint8_t> scratch(RAM, 0);
    uint8_t* sr = scratch.data();
    const uint32_t pbase = SCRATCH_VRAM & 0x1FFFFFFFu;
    for (int i = 0; i < 3; i++) {                           // write big-endian-by-word via ^3 byte order
        const uint32_t w = LEAF[i];
        for (int b = 0; b < 4; b++) {
            sr[((pbase + i * 4 + b) ^ 3)] = (uint8_t)(w >> (24 - 8 * b));
        }
    }
    const uint32_t code_len = 12;

    const bool leaf_ok = fragment_is_safe_leaf(sr, SCRATCH_VRAM, code_len);

    std::string jerr; size_t fs = 0, cs = 0;
    recomp_func_t* fn = jit_compile_function(SCRATCH_VRAM, sr, jerr, /*keep=*/true,
                                             /*register_in_map=*/false, &fs, &cs);
    const bool jit_ok = (fn != nullptr);

    const uint32_t budget = frag_diff_budget();
    uint32_t clean = 0, diverge = 0, device = 0, interp_fail = 0;
    bool result_ok = true;
    if (jit_ok && leaf_ok) {
        for (uint32_t i = 0; i < budget; i++) {
            recomp_context ctx{};
            ctx.r4  = (gpr)(int32_t)A;
            ctx.r5  = (gpr)(int32_t)B;
            ctx.r31 = 0x80000000u;                         // return target (jr $ra)
            ShadowDiffOutcome o = run_shadow_diff(sr, &ctx, SCRATCH_VRAM, fn);
            if (!o.interp_ok)  { interp_fail++; break; }
            if (o.device_touch){ device++;      break; }
            if (o.clean) clean++; else diverge++;
            if ((uint32_t)ctx.r2 != EXPECT) result_ok = false; // committed interp result
        }
    }
    // Promoted path: call the native shard directly on fresh input.
    bool native_run_ok = false;
    if (jit_ok && leaf_ok) {
        recomp_context ctx{};
        ctx.r4 = (gpr)(int32_t)A; ctx.r5 = (gpr)(int32_t)B; ctx.r31 = 0x80000000u;
        fn(sr, &ctx);
        native_run_ok = ((uint32_t)ctx.r2 == EXPECT);
    }

    const bool pass = leaf_ok && jit_ok && (clean == budget) && (diverge == 0) &&
                      (device == 0) && (interp_fail == 0) && result_ok && native_run_ok;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"safe_leaf\":%s,\"jit_ok\":%s,\"budget\":%u,\"clean\":%u,"
        "\"diverge\":%u,\"device_touch\":%u,\"interp_fail\":%u,"
        "\"interp_result_ok\":%s,\"native_run_ok\":%s,\"expect\":\"0x%08X\"}",
        pass ? "true" : "false", leaf_ok ? "true" : "false", jit_ok ? "true" : "false",
        budget, clean, diverge, device, interp_fail,
        result_ok ? "true" : "false", native_run_ok ? "true" : "false", EXPECT);
    out.assign(buf);
    fprintf(stderr, "[frag-selftest] %s\n", out.c_str());
    fflush(stderr);
    return out.c_str();
}

// Pick a CURRENTLY-loaded section with a reloc offset that is not a
// function start and probe runtime_base+offset (a "pointer-site" miss).
// Computes the target against the live table just before probing, so it
// can't be defeated by fragment eviction the way a stale dumped address
// can. Writes the chosen address to *out_addr; returns 1 on a miss.
extern "C" int recomp_debug_probe_pointer_site(uint32_t* out_addr) {
    uint32_t target = 0;
    {
        std::shared_lock<std::shared_mutex> lock(func_map_mutex);
        if (sections_info.code_sections != nullptr) {
            for (const auto& ls : loaded_sections) {
                const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
                if (sec.num_relocs == 0 || sec.num_funcs == 0) {
                    continue;
                }
                std::unordered_set<uint32_t> starts;
                starts.reserve(sec.num_funcs * 2);
                for (size_t i = 0; i < sec.num_funcs; i++) {
                    starts.insert(sec.funcs[i].offset);
                }
                for (size_t i = 0; i < sec.num_relocs && target == 0; i++) {
                    uint32_t off = sec.relocs[i].offset;
                    if (off < sec.size && starts.find(off) == starts.end()) {
                        target = (uint32_t)ls.loaded_ram_addr + off;
                    }
                }
                if (target != 0) {
                    break;
                }
            }
        }
    } // release shared lock before get_function re-locks
    if (out_addr != nullptr) {
        *out_addr = target;
    }
    if (target == 0) {
        return 0;
    }
    recomp_func_t* f = get_function((int32_t)target);
    return (f == unhandled_lookup_trampoline) ? 1 : 0;
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
