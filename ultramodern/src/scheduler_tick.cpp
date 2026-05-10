// scheduler_tick.cpp — voluntary preemption primitives for the
// cooperative scheduler. See scheduler_tick.hpp for the design rationale.
//
// The trigger policy (when to actually yield) lives outside this file —
// callers decide. This module just exposes:
//   - init_scheduler_tick:  stash rdram for the no-arg path.
//   - yield_to_any_queued:  swap to whichever game thread sits at the
//                           head of running_queue, regardless of
//                           priority (unlike check_running_queue,
//                           which only swaps to strictly-higher).
//   - ultramodern_scheduler_tick (extern "C"):  drain externals and
//                           call yield_to_any_queued.
//
// Note: the policy choice is intentionally NOT here. An earlier draft
// fired this on a wall-clock budget (>8 ms run = yield); that yielded
// mid-extras.c registry mutation (Memmap_RelocateFragment eviction)
// and corrupted state — the cooperative-scheduler invariant of "no
// preemption inside a host-side critical section" was broken. The
// current shape lets the caller (in PSR: a busy-wait detector inside
// the trace-entry hook) decide when yielding is safe.
//
// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). New file added 2026 by Matthew Stanley.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/scheduler_tick.hpp"

// rdram is a process-wide singleton owned by the runtime. Stashed once
// at preinit so the no-arg tick path can pass it through to ultramodern
// primitives that take RDRAM_ARG. Single-writer (main thread, before
// any game thread spawns) → many-reader (game threads), no sync needed.
static uint8_t* g_sched_tick_rdram = nullptr;

void ultramodern::init_scheduler_tick(RDRAM_ARG1) {
    g_sched_tick_rdram = rdram;
}

void ultramodern::join_scheduler_tick() {
    // No host thread to join.
}

void ultramodern::yield_to_any_queued(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        return;
    }
    PTR(OSThread) next_ = ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
    OSThread* next_thread = TO_PTR(OSThread, next_);
    // Re-queue ourselves at the priority-ordered position then resume
    // the next thread and block on our own context. Same shape as
    // swap_to_thread in scheduling.cpp (file-static, not exposed).
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread());
    TO_PTR(OSThread, ultramodern::this_thread())->state = OSThreadState::QUEUED;
    ultramodern::resume_thread_and_wait(PASS_RDRAM next_thread);
}

extern "C" void ultramodern_scheduler_tick(void) {
    if (!ultramodern::is_game_thread()) return;
    if (g_sched_tick_rdram == nullptr) return;
    // Drain at most one pending external message into its target
    // OSMesgQueue. Non-blocking. Repeat calls drain the rest. This is
    // what unblocks predicate-flipping threads sitting in osRecvMesg
    // waiting for a completion event posted from a host thread.
    ultramodern::wait_for_external_message_timed(g_sched_tick_rdram, 0);
    ultramodern::yield_to_any_queued(g_sched_tick_rdram);
}
