// N64ModernRuntime — this file (and the libultra-call ring it
// declares) was added by Matthew Stanley (mstan fork). Per GPL-3.0
// §5(a), authorship is recorded below; the file is distributed under
// the same GPL-3.0 license as the rest of the project (see COPYING).
//
// Added 2026 by Matthew Stanley:
//   - Always-on ring buffer event struct + LIBRECOMP_ULTRA_TRACE
//     macro + ultramodern_ultra_recent_copy accessor.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

/*
 * ultra_trace.hpp — Always-on ring buffer of libultra-call events.
 *
 * Records every entry into a librecomp `*_recomp` wrapper as a
 * structured event: function name, caller PC ($ra), four argument
 * registers (a0..a3 = r4..r7), monotonic millisecond timestamp,
 * and a monotonic write sequence.
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
 *   N+1) gets the same diagnostic surface. Stadium-specific code
 *   stays in the runner; the recording lives here in librecomp.
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

#ifndef LIBRECOMP_ULTRA_TRACE_HPP
#define LIBRECOMP_ULTRA_TRACE_HPP

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

#endif /* LIBRECOMP_ULTRA_TRACE_HPP */
