// scheduler_tick.hpp — voluntary preemption for the cooperative
// scheduler.
//
// Design: a host monitor thread observes how long it has been since
// the currently-running game thread last context-switched (stamped at
// wait_for_resumed return). If that interval exceeds a threshold, the
// monitor sets a read-mostly atomic flag. Game-thread code checks the
// flag from pkmnstadium_trace_entry (one relaxed atomic load + branch
// per recompiled function entry); when set, the game thread drains
// pending external messages and yields to any queued thread.
//
// This neutralizes a class of Stadium softlocks where a game-thread
// busy-waits on a predicate flipped by a same-or-lower-priority
// thread that never gets scheduled. Previously addressed per-site in
// PSR's extras.c (free-battle-modal-softlock, petit-cup-softlock,
// asset-pending-bypass). With this mechanism live, those per-site
// patches retire.
//
// Why this shape: an earlier attempt (engine-fork 5e91259/fe0d2fd,
// reverted 2026-05-09) computed the "stuck" predicate from inside
// pkmnstadium_trace_entry via per-call counters + wall-clock checks.
// The per-call overhead perturbed audio synth timing enough to flip
// a pre-existing audio UAF from rare to ~deterministic. The current
// shape moves all predicate computation to a host monitor thread; the
// game-thread hot path is one relaxed atomic load + branch.
//
// Rollback: ultramodern_voluntary_preemption_set_enabled(0) at startup
// short-circuits the monitor, leaving the trace_entry hot path as a
// single load against a permanently-zero flag.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#ifndef ULTRAMODERN_SCHEDULER_TICK_HPP
#define ULTRAMODERN_SCHEDULER_TICK_HPP

#include "ultramodern/ultra64.h"

namespace ultramodern {

// Start the host monitor thread. Must be called after rdram is set up
// (i.e., after preinit's other initializers). Safe to skip entirely
// if voluntary preemption is disabled — game-thread hot path then sees
// a permanently-zero flag.
void init_scheduler_tick(RDRAM_ARG1);

// Stop the monitor thread. Called from runner shutdown.
void join_scheduler_tick();

// Stamp "last context switch into a game thread" timestamp. Called
// from wait_for_resumed in threads.cpp after the wait completes.
// Inline-able: just a relaxed store of a steady_clock millisecond.
void record_context_switch();

// Swap unconditionally to head of running_queue (unlike
// check_running_queue, which only swaps to strictly-higher priority).
// Used by the scheduler_tick slow path when the monitor has decided
// the current thread is hogging the CPU. Lower-priority threads can
// still flip predicates the high-priority thread is waiting on; this
// gives them a chance to run.
void yield_to_any_queued(RDRAM_ARG1);

} // namespace ultramodern

extern "C" {

// Game-thread fast-path entry. Hot path is one relaxed atomic load +
// branch; returns immediately if no yield was requested. When the
// monitor has set the flag, clears it, drains one pending external
// message, and yields to any queued thread.
//
// Called from pkmnstadium_trace_entry (PSR) at every recompiled
// function entry. Safe to call from any context; no-op outside game
// threads.
void ultramodern_scheduler_tick(void);

// Master enable for voluntary preemption. Runner reads its environment
// at startup and calls this; if disabled, the monitor never sets the
// flag and the trace_entry hot path is a cheap load against a zero.
//
// PSR uses PSR_DISABLE_VOLUNTARY_PREEMPTION=1 to short-circuit if
// audio timing regresses.
void ultramodern_voluntary_preemption_set_enabled(int enable);

} // extern "C"

#endif // ULTRAMODERN_SCHEDULER_TICK_HPP
