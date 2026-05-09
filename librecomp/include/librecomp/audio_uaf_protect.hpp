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
}

#endif
