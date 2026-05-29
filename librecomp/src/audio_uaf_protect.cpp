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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

inline int16_t load_be16(const uint8_t* rdram, uint32_t paddr) {
    return (int16_t)(((uint16_t)rdram[(paddr + 0) ^ 3] << 8) |
                     ((uint16_t)rdram[(paddr + 1) ^ 3]));
}

inline uint8_t load_u8(const uint8_t* rdram, uint32_t paddr) {
    return rdram[paddr ^ 3];
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

// ── Always-on voice-event ring ──────────────────────────────────────
// Per-audio-frame snapshot/diff of the libnaudio voice list. Emits a
// key-on / key-off / sample-change event whenever a physical voice's
// (em_motion, dc_table) pair transitions. Physical N_PVoice structs
// are pool-allocated and have stable addresses for the synth's
// lifetime (they migrate between pFree/pAlloc/pLame lists but don't
// move), so the node vaddr is a stable voice identity to key on.

namespace voice_ring {

// Event taxonomy, onset-centric. libnaudio recycles physical N_PVoice
// structs every audio frame (freed to pFreeList then re-handed-out), so
// raw em_motion 0<->1 transitions are mostly pool churn at ~(active
// voices x frame rate), NOT musical note rate. To correlate against the
// 1-5 Hz tempo-tracking click we classify on the ADPCM decoder's
// first-decode flag and on sample (wavetable) identity changes instead:
//
//   ATTACK  — dc_first==1 observed: a true note attack. The RSP gets
//             A_ADPCM | A_INIT (predictor reset). Should track tempo.
//             Predictor carry captured here exposes the "stale carry at
//             a supposedly-fresh start" smell.
//   REALLOC — a playing physical voice's wavetable changed vs the last
//             non-zero wavetable it held, with dc_first==0: the voice
//             was reassigned to a different sample as a CONTINUE
//             (A_ADPCM, load saved state). If the loaded predictor state
//             is the wrong stream's, this is a click site.
//   KEY_OFF — em_motion 1->0 (kept for liveness; mostly churn).
enum Kind : uint32_t { ATTACK = 0, KEY_OFF = 1, REALLOC = 2 };

// One emitted event. POD; the same layout is mirrored at the query
// site in debug_server.cpp and validated via recomp_voice_event_size().
struct VoiceEvent {
    uint64_t seq;          // monotonic event index
    uint64_t ms;           // host ms since ring t0
    uint64_t pass;         // sampler pass count (~audio frames)
    uint32_t voice_ptr;    // N_PVoice node vaddr (stable voice identity)
    uint32_t kind;         // Kind
    uint32_t em_motion;    // current em_motion (0 = AL_STOPPED)
    uint32_t prev_motion;  // em_motion at previous pass
    uint32_t dc_table;     // current wavetable vaddr
    uint32_t prev_table;   // wavetable vaddr at previous pass
    uint32_t dc_sample;    // current sample position
    uint32_t dc_first;     // first-decode flag (FALSE at reuse = stale predictor risk)
    int32_t  dc_lastsam;   // predictor last-sample
    uint32_t dc_state;     // ADPCM_STATE* vaddr
    int16_t  carry0;       // ADPCM_STATE[14] (predictor carry)
    int16_t  carry1;       // ADPCM_STATE[15] (predictor carry)
    uint32_t wt_base;      // wavetable.base (sample data ptr)
    uint32_t wt_len;       // wavetable.len
    uint32_t wt_type;      // wavetable.type (0=ADPCM, 1=RAW16, 0xFF=unread)
    uint32_t loop_start;   // ADPCM/raw loop start
    uint32_t loop_end;     // loop end
    uint32_t loop_count;   // loop count
};

// Deep enough that pool-churn KEY_OFF events don't crowd out the rarer
// ATTACK/REALLOC signal: ~125 events/s observed, so 16384 holds ~2 min
// of continuous history — ample for a play-through capture session.
constexpr size_t RING_CAP = 16384;
VoiceEvent g_ring[RING_CAP];
std::atomic<uint64_t> g_next_seq{0};
std::mutex g_mtx;  // guards g_ring writes + g_prev + g_pass

std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
inline uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_t0).count();
}

struct PrevState {
    uint32_t em_motion;
    uint32_t dc_table;       // last dc_table seen (may be 0 when stopped)
    uint32_t last_table;     // last NON-ZERO wavetable this voice held
    uint32_t prev_first;     // dc_first at previous pass (attack dedup)
    uint64_t last_seen_pass;
};
std::unordered_map<uint32_t, PrevState> g_prev;  // keyed by voice_ptr
uint64_t g_pass = 0;

// Env gate evaluated once. true = ring active.
bool enabled() {
    static const bool e = [] {
        const char* v = std::getenv("PSR_DISABLE_VOICE_RING");
        bool disabled = (v && v[0] != '\0' && v[0] != '0');
        std::fprintf(stderr,
            "[voice-ring] %s (set PSR_DISABLE_VOICE_RING=1 to disable)\n",
            disabled ? "DISABLED" : "ENABLED");
        std::fflush(stderr);
        return !disabled;
    }();
    return e;
}

// Push one event into the ring. Caller holds g_mtx.
void push(const VoiceEvent& ev_in) {
    const uint64_t s = g_next_seq.load(std::memory_order_relaxed);
    VoiceEvent ev = ev_in;
    ev.seq = s;
    ev.ms = now_ms();
    ev.pass = g_pass;
    g_ring[s % RING_CAP] = ev;
    g_next_seq.store(s + 1, std::memory_order_release);
}

// Read the per-voice fields and (if a wavetable is bound) its sample
// identity + loop into `ev`. Returns with ev populated. Anchored on the
// registered libnaudio VoiceLayout. Caller has validated voice_p is in
// rdram.
void read_voice(const uint8_t* rdram,
                const librecomp::audio_uaf::VoiceLayout& L,
                uint32_t voice_v, VoiceEvent& ev) {
    const uint32_t vp = to_paddr(voice_v);
    ev.voice_ptr  = voice_v;
    ev.em_motion  = load_be32(rdram, vp + L.voice_em_motion_offset);
    ev.dc_table   = load_be32(rdram, vp + L.voice_dc_table_offset);
    ev.dc_sample  = L.voice_dc_sample_offset
                        ? load_be32(rdram, vp + L.voice_dc_sample_offset) : 0;
    ev.dc_first   = L.voice_dc_first_offset
                        ? load_be32(rdram, vp + L.voice_dc_first_offset) : 0;
    ev.dc_lastsam = L.voice_dc_lastsam_offset
                        ? (int32_t)load_be32(rdram, vp + L.voice_dc_lastsam_offset) : 0;
    ev.dc_state   = L.voice_dc_state_offset
                        ? load_be32(rdram, vp + L.voice_dc_state_offset) : 0;

    // Predictor carry: last two s16 of the 16-entry ADPCM_STATE buffer.
    ev.carry0 = ev.carry1 = 0;
    if (in_rdram_kseg0(ev.dc_state)) {
        const uint32_t sp = to_paddr(ev.dc_state);
        ev.carry0 = load_be16(rdram, sp + 14 * 2);
        ev.carry1 = load_be16(rdram, sp + 15 * 2);
    }

    // Wavetable identity + loop, when a sample is bound.
    ev.wt_base = ev.wt_len = 0;
    ev.wt_type = 0xFF;
    ev.loop_start = ev.loop_end = ev.loop_count = 0;
    if (in_rdram_kseg0(ev.dc_table)) {
        const uint32_t wp = to_paddr(ev.dc_table);
        ev.wt_base = load_be32(rdram, wp + 0x00);  // base
        ev.wt_len  = load_be32(rdram, wp + 0x04);  // len
        ev.wt_type = load_u8(rdram, wp + 0x08);    // type
        const uint32_t loop_v = load_be32(rdram, wp + 0x0C);  // waveInfo.loop
        if (in_rdram_kseg0(loop_v)) {
            const uint32_t lp = to_paddr(loop_v);
            ev.loop_start = load_be32(rdram, lp + 0x00);
            ev.loop_end   = load_be32(rdram, lp + 0x04);
            ev.loop_count = load_be32(rdram, lp + 0x08);
        }
    }
}

// One sampler pass: walk pAllocList + pLameList, diff each voice
// against its previous state, and emit transition events. Then sweep
// g_prev for voices that were playing last pass but vanished from the
// lists this pass (treated as key-off). Caller holds g_mtx.
void sample_pass(uint8_t* rdram,
                 const librecomp::audio_uaf::VoiceLayout& L,
                 uint32_t synth_v) {
    g_pass++;
    const uint32_t stopped = L.voice_em_motion_stopped_value;

    auto visit_voice = [&](uint32_t voice_v) {
        VoiceEvent ev{};
        read_voice(rdram, L, voice_v, ev);

        auto it = g_prev.find(voice_v);
        const bool known = (it != g_prev.end());
        const uint32_t prev_motion = known ? it->second.em_motion : stopped;
        const uint32_t prev_table  = known ? it->second.dc_table  : 0;
        const uint32_t last_table  = known ? it->second.last_table : 0;
        const uint32_t prev_first  = known ? it->second.prev_first : 0;
        ev.prev_motion = prev_motion;
        ev.prev_table  = prev_table;

        const bool was_playing = known && (prev_motion != stopped);
        const bool is_playing  = (ev.em_motion != stopped);
        const bool table_ok    = in_rdram_kseg0(ev.dc_table);

        // Classify, most-significant first. Pool churn (a physical voice
        // resuming the same sample it already held) is intentionally NOT
        // emitted — it would swamp the ring at the frame rate.
        if (ev.dc_first == 1u && prev_first != 1u) {
            // True note attack — predictor reset on the RSP.
            ev.kind = ATTACK;
            ev.prev_table = last_table;  // report the sample being displaced
            push(ev);
        } else if (is_playing && table_ok && last_table != 0
                   && ev.dc_table != last_table && ev.dc_first == 0u) {
            // Physical voice reassigned to a different sample as a CONTINUE
            // (load saved predictor state) — stale-state click suspect.
            ev.kind = REALLOC;
            ev.prev_table = last_table;
            push(ev);
        } else if (!is_playing && was_playing) {
            ev.kind = KEY_OFF;
            push(ev);
        }

        const uint32_t new_last_table =
            (table_ok && ev.dc_table != 0) ? ev.dc_table : last_table;
        g_prev[voice_v] = PrevState{ ev.em_motion, ev.dc_table,
                                     new_last_table, ev.dc_first, g_pass };
    };

    // Reuse the existing bounded walker for both lists.
    walk_list(rdram, synth_v, L.alloc_list_offset, L.max_voice_count,
              [&](uint32_t v) { visit_voice(v); return false; });
    walk_list(rdram, synth_v, L.lame_list_offset, L.max_voice_count,
              [&](uint32_t v) { visit_voice(v); return false; });

    // Voices that were playing but disappeared from both lists this
    // pass: emit a synthetic key-off and mark stopped. Prune long-stale
    // entries so the map stays bounded (voice_ptr space is small but
    // pools can be torn down + re-created across scenes).
    for (auto it = g_prev.begin(); it != g_prev.end();) {
        if (it->second.last_seen_pass == g_pass) { ++it; continue; }
        if (it->second.em_motion != stopped) {
            VoiceEvent ev{};
            ev.voice_ptr  = it->first;
            ev.kind       = KEY_OFF;
            ev.em_motion  = stopped;
            ev.prev_motion = it->second.em_motion;
            ev.prev_table  = it->second.dc_table;
            ev.wt_type    = 0xFF;
            push(ev);
        }
        if (g_pass - it->second.last_seen_pass > 600) {
            it = g_prev.erase(it);  // ~10 s unseen at 60 Hz: drop identity
        } else {
            it->second.em_motion = stopped;  // remember it's gone
            ++it;
        }
    }
}

}  // namespace voice_ring

// ── ADPCM-decode command-list scan ──────────────────────────────────
// Parse the audio command list (Acmd array at the M_AUDTASK's data_ptr,
// fully built in RDRAM and not yet consumed by the RSP at submit time)
// to recover the GROUND-TRUTH A_INIT decision per ADPCM decode. This is
// the decision the task-time voice ring CANNOT see, because n_alAdpcmPull
// clears dc_first immediately after building the command (n_load.c:322).
//
// Command encoding (n_abi.h):
//   n_aLoadADPCM: w0 = A_LOADADPCM<<24 | count;  w1 = book_phys
//   n_aADPCMdec : w0 = A_ADPCM<<24 | state_phys; w1 = flags<<28 | ...
//     flags bit A_INIT(0x1): predictor reset (fresh attack). A CONTINUE
//     (A_INIT clear) loads the saved predictor state from state_phys.
// The LOADADPCM that precedes a decode carries that voice's book (a
// per-sample identity). A decode whose book changed vs the last decode
// on the same state buffer but is A_CONTINUE => the RSP loads the
// PREVIOUS sample's predictor => audible click. That is `suspect`.

namespace adpcm_ring {

constexpr uint32_t A_ADPCM_OP     = 1;
constexpr uint32_t A_LOADADPCM_OP = 11;
constexpr uint32_t A_INIT_FLAG    = 0x1;

struct AdpcmEvent {
    uint64_t seq;
    uint64_t ms;
    uint64_t frame;      // audio task scan count
    uint32_t state;      // dc_state phys addr (voice predictor buffer)
    uint32_t book;       // book phys for THIS decode (sample identity)
    uint32_t prev_book;  // last book seen on this state buffer
    uint32_t flags;      // raw 4-bit flags field
    uint32_t init;       // flags & A_INIT (1 = predictor reset)
    uint32_t count;      // c field (sample count for the decode)
    uint32_t suspect;    // book changed && CONTINUE (stale-predictor click)
};

constexpr size_t RING_CAP = 16384;
AdpcmEvent g_ring[RING_CAP];
std::atomic<uint64_t> g_next_seq{0};
std::mutex g_mtx;
uint64_t g_frame = 0;
std::unordered_map<uint32_t, uint32_t> g_last_book;  // state_phys -> last book

void push(const AdpcmEvent& in) {
    const uint64_t s = g_next_seq.load(std::memory_order_relaxed);
    AdpcmEvent e = in;
    e.seq = s;
    e.ms = voice_ring::now_ms();
    e.frame = g_frame;
    g_ring[s % RING_CAP] = e;
    g_next_seq.store(s + 1, std::memory_order_release);
}

// Parse one audio command list. Caller holds g_mtx.
void scan(uint8_t* rdram, uint32_t cmd_vaddr, uint32_t cmd_size_bytes) {
    if (!in_rdram_kseg0(cmd_vaddr)) return;
    g_frame++;
    const uint32_t cmd_p = to_paddr(cmd_vaddr);
    // Bound the walk: cap commands and clamp size to RDRAM.
    uint32_t n_cmds = cmd_size_bytes / 8;
    if (n_cmds > 8192) n_cmds = 8192;
    uint32_t cur_book = 0;
    for (uint32_t i = 0; i < n_cmds; i++) {
        const uint32_t off = i * 8;
        if (!in_rdram_kseg0(0x80000000u | ((cmd_p + off) & 0x1FFFFFFFu))) break;
        const uint32_t w0 = load_be32(rdram, cmd_p + off);
        const uint32_t w1 = load_be32(rdram, cmd_p + off + 4);
        const uint32_t op = (w0 >> 24) & 0xFFu;
        if (op == A_LOADADPCM_OP) {
            cur_book = w1;  // book phys
        } else if (op == A_ADPCM_OP) {
            AdpcmEvent e{};
            e.state = w0 & 0x00FFFFFFu;
            e.book  = cur_book;
            e.flags = (w1 >> 28) & 0xFu;
            e.init  = (e.flags & A_INIT_FLAG) ? 1u : 0u;
            e.count = (w1 >> 16) & 0xFFFu;
            auto it = g_last_book.find(e.state);
            const uint32_t prev = (it != g_last_book.end()) ? it->second : 0u;
            e.prev_book = prev;
            e.suspect = (prev != 0u && cur_book != 0u && cur_book != prev
                         && e.init == 0u) ? 1u : 0u;
            push(e);
            if (cur_book != 0u) g_last_book[e.state] = cur_book;
        }
    }
}

}  // namespace adpcm_ring

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

// ── Voice-event ring C API ──────────────────────────────────────────

extern "C" void librecomp_audio_voice_ring_sample(
    uint8_t* rdram, uint32_t cmd_list_vaddr, uint32_t cmd_list_size)
{
    if (!voice_ring::enabled()) return;
    // Command-list scan is independent of synth layout (it parses the
    // task's Acmd buffer directly), so do it even if the voice list
    // isn't resolvable yet.
    if (cmd_list_vaddr != 0 && cmd_list_size != 0) {
        std::lock_guard<std::mutex> lk(adpcm_ring::g_mtx);
        adpcm_ring::scan(rdram, cmd_list_vaddr, cmd_list_size);
    }
    uint32_t synth_v = resolve_synth(rdram);   // 0 if no layout / not booted
    if (!synth_v) return;
    const auto& L = state().layout;
    std::lock_guard<std::mutex> lk(voice_ring::g_mtx);
    voice_ring::sample_pass(rdram, L, synth_v);
}

extern "C" void recomp_adpcm_decode_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using adpcm_ring::AdpcmEvent;
    std::lock_guard<std::mutex> lk(adpcm_ring::g_mtx);
    const uint64_t s = adpcm_ring::g_next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available =
        (s < adpcm_ring::RING_CAP) ? size_t(s) : adpcm_ring::RING_CAP;
    const size_t want = (cap < available) ? cap : available;
    AdpcmEvent* out = static_cast<AdpcmEvent*>(out_void);
    const size_t start = (s - want) % adpcm_ring::RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = adpcm_ring::g_ring[(start + i) % adpcm_ring::RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t recomp_adpcm_decode_event_size(void) {
    return sizeof(adpcm_ring::AdpcmEvent);
}

extern "C" void recomp_voice_events_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using voice_ring::VoiceEvent;
    // Take the lock first so `s` and the ring contents are consistent
    // against a concurrent sampler push on the audio thread.
    std::lock_guard<std::mutex> lk(voice_ring::g_mtx);
    const uint64_t s = voice_ring::g_next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available =
        (s < voice_ring::RING_CAP) ? size_t(s) : voice_ring::RING_CAP;
    const size_t want = (cap < available) ? cap : available;
    VoiceEvent* out = static_cast<VoiceEvent*>(out_void);
    const size_t start = (s - want) % voice_ring::RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = voice_ring::g_ring[(start + i) % voice_ring::RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t recomp_voice_event_size(void) {
    return sizeof(voice_ring::VoiceEvent);
}
