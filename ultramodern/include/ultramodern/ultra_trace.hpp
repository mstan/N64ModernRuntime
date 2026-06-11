// N64ModernRuntime — this file (and the libultra-call ring it
// declares) was added by Matthew Stanley (mstan fork). Per GPL-3.0
// §5(a), authorship is recorded below; the file is distributed under
// the same GPL-3.0 license as the rest of the project (see COPYING).
//
// Added 2026 by Matthew Stanley:
//   - Always-on ring buffer event struct + LIBRECOMP_ULTRA_TRACE
//     macro + accessor APIs.
//   - 2026-06: moved from librecomp to ultramodern so the runtime's
//     own event-delivery paths (VI retrace, AI, SP/DP/SI done, timer
//     fires) can record into the same ring without an inverted
//     librecomp include. librecomp/include/librecomp/ultra_trace.hpp
//     remains as a compatibility shim for existing consumers.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

/*
 * ultra_trace.hpp — Always-on ring buffer of libultra-call events.
 *
 * Records every entry into a librecomp `*_recomp` wrapper as a
 * structured event: function name, caller PC ($ra), four argument
 * registers (a0..a3 = r4..r7), current guest thread id/pointer,
 * monotonic millisecond timestamp, and a monotonic write sequence.
 *
 * Why this exists:
 *
 *   When a game's boot stalls before producing graphics tasks, the
 *   diagnostic question is always "what libultra call is the game
 *   thread waiting on?" — typically an osRecvMesg whose matching
 *   osSendMesg never fires because some upstream subsystem (EEPROM
 *   probe, controller scan, save block read) didn't complete.
 *
 *   Per the project rule (CLAUDE.md global "ring buffer" rule):
 *   we do NOT arm-then-run a one-shot trace. The ring is recording
 *   continuously from process start (in Release too), and probes
 *   query backward over the window of interest.
 *
 *   This is framework-level — every consumer game (Stadium, future
 *   N+1) gets the same diagnostic surface. Game-specific code stays
 *   in the runner; the recording lives here in the runtime.
 *
 * Event classes recorded:
 *
 *   1. Wrapper entries — every libultra `*_recomp` wrapper records
 *      on entry via LIBRECOMP_ULTRA_TRACE (name = wrapper symbol).
 *   2. Blocking-call returns — osRecvMesg/osSendMesg/osJamMesg also
 *      record on RETURN ("~osRecvMesg.done": pc=caller ra, a0=mq,
 *      a1=result, a2=flags). This makes a WAKE directly visible: a
 *      thread blocked on a queue that never logs a `.done` is still
 *      blocked; one that logs `.done` but never re-enters an os call
 *      has run off into call-free guest code (spin loop).
 *   3. Runtime→guest event deliveries — the runtime records when IT
 *      posts to a guest queue ("~vi_retrace", "~ai_event", "~sp_done",
 *      "~dp_done", "~si_done", "~timer_fire", "~pi_dma_done",
 *      "~flash_done"): pc=0 (no guest caller), a0=mq, a1=msg,
 *      a2=osSendMesg result (0 ok, -1 dropped/full).
 *
 *   Names starting with '~' are synthetic runtime events, not guest
 *   calls. Compiler-runtime math shims (__udivdi3, __ull_div, ...,
 *   in librecomp/src/math_routines.cpp) are deliberately NOT part of
 *   the event class: they are data-plane arithmetic, not OS-boundary
 *   events, and tracing them would evict the entire OS-call history
 *   in a few microseconds of tight math code.
 *
 * How to use from a wrapper:
 *
 *   extern "C" void osEepromProbe_recomp(uint8_t* rdram, recomp_context* ctx) {
 *       LIBRECOMP_ULTRA_TRACE(ctx);
 *       ...
 *   }
 *
 * How to read from a runner:
 *
 *   uint64_t widx = recomp_ultra_trace_write_idx();
 *   uint32_t cap  = recomp_ultra_trace_capacity();
 *   recomp_ultra_trace_event ev{};
 *   for (uint64_t i = widx > N ? widx - N : 0; i < widx; ++i) {
 *       if (recomp_ultra_trace_get(i, &ev)) {
 *           // ev.name, ev.pc, ev.a0..a3, ev.ms, ev.seq
 *       }
 *   }
 */

#ifndef ULTRAMODERN_ULTRA_TRACE_HPP
#define ULTRAMODERN_ULTRA_TRACE_HPP

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-size record so the ring is a flat array — no allocator on
 * the hot path. Name is truncated to 39 chars + NUL; that fits
 * every libultra symbol we wrap. */
typedef struct recomp_ultra_trace_event {
    char     name[40];
    uint32_t pc;       /* caller PC = ctx->r31 (ra) at wrapper entry */
    uint32_t a0;       /* ctx->r4 */
    uint32_t a1;       /* ctx->r5 */
    uint32_t a2;       /* ctx->r6 */
    uint32_t a3;       /* ctx->r7 */
    uint32_t thread;   /* current OSThread guest pointer, or 0 */
    uint16_t thread_id;/* current OSThread id, or 0 outside guest threads */
    uint16_t pad;
    uint64_t ms;       /* milliseconds since first record */
    uint64_t seq;      /* monotonic; equals the index this record
                        * was assigned. Lets readers detect that a
                        * slot was overwritten between get() and the
                        * widx they were told. */
} recomp_ultra_trace_event;

/* Record an event. Lock-free. Safe to call from any thread. */
void recomp_ultra_trace_record(const char* name,
                               uint32_t pc,
                               uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3);

/* Monotonic count of records ever written. Use as upper bound when
 * walking the ring. */
uint64_t recomp_ultra_trace_write_idx(void);

/* Power-of-two capacity of the ring. */
uint32_t recomp_ultra_trace_capacity(void);

/* Copy the record at logical index `idx` (i.e. the seq it was
 * assigned). Returns false if the slot has been overwritten — the
 * stored seq doesn't match. Always reads the slot into `out` so
 * callers can decide to keep partial info if they want. */
int recomp_ultra_trace_get(uint64_t idx, recomp_ultra_trace_event* out);

/* Set the rdram base pointer. Called from wrapper-side glue so
 * external probes can read game memory without needing direct
 * access to the runtime's internal allocator. Idempotent — first
 * call wins, subsequent calls are no-ops. */
void recomp_runtime_set_rdram(unsigned char* rdram);

/* Returns the rdram base pointer captured at first wrapper entry,
 * or NULL if not yet set. */
unsigned char* recomp_runtime_get_rdram(void);

/* Boot snapshot — a parallel non-evicting buffer that captures the
 * first N events from process start and then stops recording.
 * Lets a probe answer "what happened at boot?" no matter how long
 * the game has been running in steady state.
 *
 *   pos in [0, recomp_ultra_trace_boot_count()) is filled.
 *   pos in [recomp_ultra_trace_boot_count(), recomp_ultra_trace_boot_capacity())
 *     is empty (never reached). */
uint32_t recomp_ultra_trace_boot_count(void);
uint32_t recomp_ultra_trace_boot_capacity(void);
int      recomp_ultra_trace_boot_get(uint32_t pos,
                                     recomp_ultra_trace_event* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef __cplusplus

/* C++ convenience: pass a recomp_context* and the wrapper picks up
 * ra/a0/a1/a2/a3 itself. Defined inline so we don't need to pull
 * the recomp_context layout into this header — the macro stays in
 * the user's translation unit where recomp.h is already included. */
#define LIBRECOMP_ULTRA_TRACE(ctx)                                     \
    do {                                                               \
        ::recomp_runtime_set_rdram((unsigned char*)(rdram));           \
        ::recomp_ultra_trace_record(                                   \
            __func__,                                                  \
            (uint32_t)(ctx)->r31,                                      \
            (uint32_t)(ctx)->r4,                                       \
            (uint32_t)(ctx)->r5,                                       \
            (uint32_t)(ctx)->r6,                                       \
            (uint32_t)(ctx)->r7);                                      \
    } while (0)

#endif /* __cplusplus */

#endif /* ULTRAMODERN_ULTRA_TRACE_HPP */
