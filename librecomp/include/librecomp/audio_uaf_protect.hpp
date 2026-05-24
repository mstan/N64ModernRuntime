// Part of N64ModernRuntime's librecomp subsystem (added by Matthew
// Stanley in mstan fork; not present upstream). Distributed under
// the project's GPL-3.0 (see ../../../COPYING).
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#ifndef __LIBRECOMP_AUDIO_UAF_PROTECT__
#define __LIBRECOMP_AUDIO_UAF_PROTECT__

#include <cstdint>

// libnaudio voice UAF protection.
//
// Background: an N64 game using libaudio / libnaudio (Nintendo's audio
// library + N-Audio 2.0) can free pool memory that backs an active
// voice's wavetable struct. The synth still holds a pointer to that
// memory and reads it on the next audio frame, hitting freed bytes —
// UAF crashes in n_alAdpcmPull / n_alLoadParam.
//
// This module silences such voices selectively (range-scoped) or
// wholesale before the freeing happens. The mechanism is generic
// across libnaudio games; the per-game layout (synth pointer location
// and struct offsets) is registered at startup.
//
// Walk strategy: libaudio holds physical voices on three intrusive
// circular ALLink lists embedded in the synth — pFreeList,
// pAllocList, pLameList. Allocated voices live on pAllocList; voices
// queued to be freed live on pLameList. Walking these two lists
// catches every voice the synth might still pull from on the next
// audio frame, regardless of bus routing.

namespace librecomp::audio_uaf {
    // Per-game layout. All vaddrs are kseg0 (0x80xxxxxx).
    //
    // Walk: synth = *(n_syn_var_vaddr)
    //       list_head = synth + alloc_list_offset (or lame_list_offset)
    //       node = *(list_head)            // ALLink.next
    //       while node != list_head:       // circular sentinel
    //           voice = node               // ALLink is at voice offset 0
    //           voice->dc_table at +voice_dc_table_offset
    //           voice->em_motion at +voice_em_motion_offset
    //           node = *(node)
    struct VoiceLayout {
        // Vaddr of the global pointer holding the synth (e.g. n_syn).
        uint32_t n_syn_var_vaddr;

        // Offsets of ALLink list heads within the synth struct.
        // For libnaudio N_ALSynth: pAllocList=0x0C, pLameList=0x14.
        // Set to 0xFFFFFFFF to skip a list.
        uint32_t alloc_list_offset;
        uint32_t lame_list_offset;

        // Offsets within the per-voice struct (PVoice / N_PVoice).
        // The walker assumes the voice's intrusive ALLink is at the
        // start of the voice (offset 0), which is true for libaudio
        // and libnaudio.
        uint32_t voice_em_motion_offset;
        uint32_t voice_dc_table_offset;

        // AL_STOPPED sentinel (libaudio: 0). Written to em_motion to
        // silence a voice.
        uint32_t voice_em_motion_stopped_value;

        // Sanity cap on linked-list iteration. Defends against cycles
        // and torn / pre-init reads. The walk aborts after this many
        // nodes per list.
        uint32_t max_voice_count;
    };

    // Register the layout. Call once at startup, before any silencer
    // call. Subsequent calls overwrite (last write wins).
    void register_voice_layout(const VoiceLayout& layout);

    // Whether a layout has been registered.
    bool layout_registered();

    // Secondary voice-table layout: array-style voice tables held by
    // game-side code on top of libnaudio. Distinct from VoiceLayout
    // (which describes libnaudio's intrusive ALLink lists). Some games
    // route audio through a high-level synth that maintains its own
    // per-voice struct array, in addition to the libnaudio synth.
    // Those high-level structs can also hold pointers into pool memory
    // that gets freed, so a UAF on the game-side path is possible
    // independently of libnaudio.
    //
    // Walk strategy: array of fixed-stride voice structs at a
    // game-known vaddr/count. For each voice, follow a deref chain
    // (sequence of u32 loads at offsets from the prior step's
    // address) to land on a wavetable / pool pointer. If that pointer
    // falls in the freed range, write silence_value at silence_field
    // within the voice struct to make the game's per-voice processor
    // skip the voice on the next frame.
    //
    // Chain semantics: cur = voice_base; for each step s in 0..N-1:
    //   addr = cur + chain_offsets[s]
    //   cur = *(u32*)addr           // load u32 from rdram[addr]
    // After all steps, `cur` is the final pointer to range-check.
    // chain_step_count = 0 means treat voice_base itself as the
    // pointer (uncommon; included for symmetry).
    struct SecondaryVoiceTableLayout {
        // Where the voice count lives (read as u32).
        uint32_t count_vaddr;
        // Where the array pointer lives (read as u32 to get array base vaddr).
        uint32_t array_ptr_vaddr;
        // Bytes per voice.
        uint16_t voice_size;
        // Safety cap on iteration (defends against count corruption).
        uint16_t max_voice_count;
        // Deref chain — up to 4 steps; chain_step_count must be <= 4.
        uint8_t  chain_step_count;
        uint8_t  chain_offsets[4];
        // Field within the voice to write silence_value to.
        uint16_t silence_field_offset;
        // Value to write to silence (typically 0).
        uint32_t silence_value;
    };

    // Register a secondary voice-table layout. Independent of the
    // libnaudio layout; both can be registered.
    void register_secondary_voice_table(const SecondaryVoiceTableLayout& layout);

    // Whether a secondary table has been registered.
    bool secondary_table_registered();
}

extern "C" {
    // Silence libaudio voices whose dc_table pointer falls in
    // [start_vaddr, end_vaddr). Returns the number of voices
    // silenced, or a negative error code if the registered layout is
    // invalid / pointers are out of RDRAM range.
    //
    // Intended to be called from a hook just before the game frees
    // pool memory in [start, end). After this call, voices that
    // referenced wavetable structs in the freed range will read
    // em_motion=AL_STOPPED and skip the synth path until reassigned.
    int librecomp_audio_uaf_silence_voices_in_range(
        uint8_t* rdram, uint32_t start_vaddr, uint32_t end_vaddr);

    // Silence ALL libaudio voices regardless of dc_table location.
    // For total-scene-cleanup hooks where every voice should stop.
    int librecomp_audio_uaf_silence_all_voices(uint8_t* rdram);

    // Silence game-side secondary voices (e.g. high-level synth voice
    // arrays) whose chained wavetable / pool pointer falls in
    // [start_vaddr, end_vaddr). Returns the number of voices silenced,
    // 0 if no secondary table is registered or none matched, or a
    // negative error code on layout-resolution failure.
    int librecomp_audio_uaf_silence_secondary_in_range(
        uint8_t* rdram, uint32_t start_vaddr, uint32_t end_vaddr);
}

#endif
