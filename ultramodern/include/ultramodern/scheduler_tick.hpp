// scheduler_tick.hpp — voluntary preemption primitives for the
// cooperative scheduler.
//
// The cooperative scheduler in this runtime only switches threads when
// the running thread enters a libultra primitive (osRecvMesg, etc.).
// A thread that tight-loops on a memory predicate without entering any
// primitive starves the predicate-flipping thread → softlock. Real N64
// hardware preempts via DP/AI/VI interrupts, so this is a known
// recompiler-side gap.
//
// This module exposes the *primitives* needed to plug that gap. It
// does NOT decide when to fire them — that policy belongs to the
// caller, since "yielding mid-execution" is unsafe inside extras.c
// hooks that mutate global state (e.g. fragment-registry eviction).
// The caller has the context to know when yielding is safe (e.g. a
// trace-entry hook that detects "same predicate function called N
// times in a row" — a tight-loop signature that, by definition, only
// reads memory and doesn't hold critical state).
//
// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). New file added 2026 by Matthew Stanley.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#ifndef __ultramodern_scheduler_tick_HPP__
#define __ultramodern_scheduler_tick_HPP__

#include "ultra64.h"

namespace ultramodern {

// Stash rdram for use by the no-arg tick path. Called once at preinit
// before any game thread is spawned; no synchronization needed for the
// later reads.
void init_scheduler_tick(RDRAM_ARG1);

// Currently a no-op. Kept for symmetry with other init_/join_ pairs.
void join_scheduler_tick();

// Yield the running token to whichever game thread sits at the head of
// running_queue. Unlike check_running_queue (which only swaps to a
// strictly-higher-priority thread), this swaps unconditionally — so it
// can break a busy-wait where the predicate-flipping thread is at the
// same or lower priority. No-op if running_queue is empty.
//
// Safe to call only from a known-safe context: the caller must be at a
// point where switching threads cannot leave global state half-mutated.
// Inside a fragment-registry update, an audio-buffer commit, etc., this
// is unsafe. Inside a trivial predicate read, it is safe.
void yield_to_any_queued(RDRAM_ARG1);

} // namespace ultramodern

#ifdef __cplusplus
extern "C" {
#endif

// Convenience wrapper: drain at most one pending external message into
// its target queue (so blocked receivers can wake), then call
// yield_to_any_queued. Uses the rdram stashed at init_scheduler_tick.
// No-op when not on a game thread or before init.
//
// Same safety contract as yield_to_any_queued: caller decides when it
// is safe to switch threads.
void ultramodern_scheduler_tick(void);

#ifdef __cplusplus
}
#endif

#endif
