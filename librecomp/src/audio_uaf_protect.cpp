// Part of N64ModernRuntime's librecomp subsystem (added by Matthew
// Stanley in mstan fork; not present upstream). Distributed under
// the project's GPL-3.0 (see ../../COPYING).
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

// Generic libaudio voice UAF protection. See header for design notes.
//
// Implementation is intentionally simple: a single static layout
// config, non-atomic em_motion writes (32-bit aligned, naturally
// atomic on x86/ARM for the values we use), and a bounded walk over
// each ALLink list.

#include "librecomp/audio_uaf_protect.hpp"

#include <atomic>
#include <cstdio>

namespace {

constexpr uint32_t kSkipList = 0xFFFFFFFFu;

struct State {
    std::atomic<bool> registered{false};
    librecomp::audio_uaf::VoiceLayout layout{};
};

State& state() {
    static State s;
    return s;
}

struct SecondaryState {
    std::atomic<bool> registered{false};
    librecomp::audio_uaf::SecondaryVoiceTableLayout layout{};
};

SecondaryState& secondary_state() {
    static SecondaryState s;
    return s;
}

inline bool in_rdram_kseg0(uint32_t vaddr) {
    // RDRAM kseg0: [0x80000000, 0x80800000) — 8 MiB max with
    // expansion pak.
    return vaddr >= 0x80000000u && vaddr < 0x80800000u;
}

inline uint32_t to_paddr(uint32_t vaddr) {
    return vaddr & 0x1FFFFFFFu;
}

// Big-endian 32-bit load/store that account for the host's
// word-swapped rdram layout (the recompiler stores RDRAM with bytes
// XOR-3 within each 4-byte word for fast 32-bit access).
inline uint32_t load_be32(const uint8_t* rdram, uint32_t paddr) {
    return ((uint32_t)rdram[(paddr + 0) ^ 3] << 24) |
           ((uint32_t)rdram[(paddr + 1) ^ 3] << 16) |
           ((uint32_t)rdram[(paddr + 2) ^ 3] <<  8) |
           ((uint32_t)rdram[(paddr + 3) ^ 3]);
}

inline void store_be32(uint8_t* rdram, uint32_t paddr, uint32_t val) {
    rdram[(paddr + 0) ^ 3] = (uint8_t)(val >> 24);
    rdram[(paddr + 1) ^ 3] = (uint8_t)(val >> 16);
    rdram[(paddr + 2) ^ 3] = (uint8_t)(val >>  8);
    rdram[(paddr + 3) ^ 3] = (uint8_t)(val);
}

// Resolve the synth pointer. Returns 0 (not in rdram) if invalid.
uint32_t resolve_synth(const uint8_t* rdram) {
    auto& st = state();
    if (!st.registered.load(std::memory_order_acquire)) return 0;
    const auto& L = st.layout;
    if (!in_rdram_kseg0(L.n_syn_var_vaddr)) return 0;
    uint32_t synth_v = load_be32(rdram, to_paddr(L.n_syn_var_vaddr));
    if (!in_rdram_kseg0(synth_v)) return 0;
    return synth_v;
}

// Visitor invoked for each voice node. Returns true to mark "silenced
// this voice", false otherwise. The caller manages the running count.
template <typename Visit>
int walk_list(uint8_t* rdram, uint32_t synth_v, uint32_t list_offset,
              uint32_t max_voices, Visit visit) {
    if (list_offset == kSkipList) return 0;
    uint32_t list_head_v = synth_v + list_offset;
    if (!in_rdram_kseg0(list_head_v)) return 0;
    uint32_t node_v = load_be32(rdram, to_paddr(list_head_v));
    int silenced = 0;
    uint32_t iters = 0;
    while (node_v != list_head_v && in_rdram_kseg0(node_v)
           && iters < max_voices) {
        if (visit(node_v)) silenced++;
        // ALLink.next is at voice offset 0 (intrusive list, ALLink is
        // first member of PVoice / N_PVoice).
        node_v = load_be32(rdram, to_paddr(node_v));
        iters++;
    }
    return silenced;
}

}  // namespace

namespace librecomp::audio_uaf {

void register_voice_layout(const VoiceLayout& layout) {
    auto& st = state();
    st.layout = layout;
    st.registered.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[audio-uaf] voice layout registered: n_syn_var=0x%08X "
        "alloc_list_off=0x%X lame_list_off=0x%X "
        "em_motion_off=0x%X dc_table_off=0x%X stopped=0x%X "
        "max_voices=%u\n",
        layout.n_syn_var_vaddr, layout.alloc_list_offset,
        layout.lame_list_offset, layout.voice_em_motion_offset,
        layout.voice_dc_table_offset,
        layout.voice_em_motion_stopped_value, layout.max_voice_count);
    std::fflush(stderr);
}

bool layout_registered() {
    return state().registered.load(std::memory_order_acquire);
}

void register_secondary_voice_table(const SecondaryVoiceTableLayout& layout) {
    auto& st = secondary_state();
    st.layout = layout;
    st.registered.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "[audio-uaf-2nd] secondary voice table registered: "
        "count@0x%08X array@0x%08X size=%u max=%u chain_steps=%u "
        "silence_off=%u silence_val=0x%X\n",
        layout.count_vaddr, layout.array_ptr_vaddr,
        (unsigned)layout.voice_size, (unsigned)layout.max_voice_count,
        (unsigned)layout.chain_step_count,
        (unsigned)layout.silence_field_offset,
        layout.silence_value);
    std::fflush(stderr);
}

bool secondary_table_registered() {
    return secondary_state().registered.load(std::memory_order_acquire);
}

}  // namespace librecomp::audio_uaf

extern "C" int librecomp_audio_uaf_silence_voices_in_range(
    uint8_t* rdram, uint32_t start_vaddr, uint32_t end_vaddr)
{
    if (start_vaddr >= end_vaddr) return 0;
    uint32_t synth_v = resolve_synth(rdram);
    if (!synth_v) return -1;
    const auto& L = state().layout;

    auto visit = [&](uint32_t voice_v) -> bool {
        uint32_t voice_p = to_paddr(voice_v);
        uint32_t dc_table = load_be32(rdram, voice_p + L.voice_dc_table_offset);
        if (dc_table >= start_vaddr && dc_table < end_vaddr) {
            store_be32(rdram, voice_p + L.voice_em_motion_offset,
                       L.voice_em_motion_stopped_value);
            return true;
        }
        return false;
    };

    int silenced = 0;
    silenced += walk_list(rdram, synth_v, L.alloc_list_offset,
                          L.max_voice_count, visit);
    silenced += walk_list(rdram, synth_v, L.lame_list_offset,
                          L.max_voice_count, visit);

    if (silenced > 0) {
        std::fprintf(stderr,
            "[audio-uaf] silenced %d voice(s) whose dc_table fell in "
            "[0x%08X, 0x%08X)\n",
            silenced, start_vaddr, end_vaddr);
        std::fflush(stderr);
    }
    return silenced;
}

extern "C" int librecomp_audio_uaf_silence_all_voices(uint8_t* rdram) {
    uint32_t synth_v = resolve_synth(rdram);
    if (!synth_v) return -1;
    const auto& L = state().layout;

    auto visit = [&](uint32_t voice_v) -> bool {
        uint32_t voice_p = to_paddr(voice_v);
        store_be32(rdram, voice_p + L.voice_em_motion_offset,
                   L.voice_em_motion_stopped_value);
        return true;
    };

    int silenced = 0;
    silenced += walk_list(rdram, synth_v, L.alloc_list_offset,
                          L.max_voice_count, visit);
    silenced += walk_list(rdram, synth_v, L.lame_list_offset,
                          L.max_voice_count, visit);
    return silenced;
}

extern "C" int librecomp_audio_uaf_silence_secondary_in_range(
    uint8_t* rdram, uint32_t start_vaddr, uint32_t end_vaddr)
{
    if (start_vaddr >= end_vaddr) return 0;
    auto& st = secondary_state();
    if (!st.registered.load(std::memory_order_acquire)) return 0;
    const auto& L = st.layout;
    if (L.chain_step_count > 4) return -1;

    if (!in_rdram_kseg0(L.count_vaddr) ||
        !in_rdram_kseg0(L.array_ptr_vaddr)) return -1;
    uint32_t count = load_be32(rdram, to_paddr(L.count_vaddr));
    if (count == 0 || count > L.max_voice_count) return 0;

    uint32_t array_v = load_be32(rdram, to_paddr(L.array_ptr_vaddr));
    if (!in_rdram_kseg0(array_v)) return 0;
    uint32_t array_p = to_paddr(array_v);

    int silenced = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t voice_p = array_p + (uint32_t)i * (uint32_t)L.voice_size;
        uint32_t cur_v = 0x80000000u | (voice_p & 0x1FFFFFFFu);

        bool ok = true;
        for (uint8_t s = 0; s < L.chain_step_count; s++) {
            uint32_t step_addr = cur_v + (uint32_t)L.chain_offsets[s];
            if (!in_rdram_kseg0(step_addr)) { ok = false; break; }
            cur_v = load_be32(rdram, to_paddr(step_addr));
            if (s + 1u < L.chain_step_count && !in_rdram_kseg0(cur_v)) {
                ok = false; break;
            }
        }
        if (!ok) continue;

        if (cur_v >= start_vaddr && cur_v < end_vaddr) {
            store_be32(rdram, voice_p + (uint32_t)L.silence_field_offset,
                       L.silence_value);
            silenced++;
        }
    }

    if (silenced > 0) {
        std::fprintf(stderr,
            "[audio-uaf-2nd] silenced %d secondary voice(s) whose chained "
            "pointer fell in [0x%08X, 0x%08X)\n",
            silenced, start_vaddr, end_vaddr);
        std::fflush(stderr);
    }
    return silenced;
}
