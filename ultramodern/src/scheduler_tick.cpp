// scheduler_tick.cpp — voluntary preemption implementation. See
// scheduler_tick.hpp for the design rationale.
//
// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). New file added 2026 by Matthew Stanley.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <thread>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/scheduler_tick.hpp"

extern "C" uint32_t recomp_rsp_consume_preempt_skip_budget(void);

namespace {

// Read-mostly flag set by host monitor when no game-thread context
// switch has happened in kStuckThresholdMs. Cleared by the game thread
// when it services the yield. Hot path is one relaxed atomic load per
// recompiled function entry.
std::atomic<uint8_t> g_should_yield{0};

// Wallclock (steady_clock) milliseconds of the most recent context
// switch into a game thread. Stamped at wait_for_resumed return.
// Read by the host monitor.
std::atomic<uint64_t> g_last_switch_ms{0};

// Monitor thread control.
std::atomic<bool> g_monitor_exit{false};
std::atomic<bool> g_monitor_started{false};
std::thread g_monitor_thread;

// rdram stashed at init time so the no-arg slow path can pass it
// through to ultramodern primitives that take RDRAM_ARG. Single-writer
// (main thread, before any game thread spawns), many-reader (game
// threads); no sync needed beyond the relaxed-load.
uint8_t* g_rdram = nullptr;

// Threshold over which the monitor considers the current game thread
// stuck. 200 ms is well above any legitimate per-frame budget at 30 fps
// (33 ms) or 60 fps (16 ms), but small enough that stuck busy-waits
// resolve within a few hundred ms of becoming stuck.
constexpr uint64_t kStuckThresholdMs = 200;

// Monitor wake interval. Short enough that detection latency past the
// threshold is ~kMonitorIntervalMs.
constexpr uint64_t kMonitorIntervalMs = 50;

// Master enable. Off → monitor never sets the flag; the trace_entry hot
// path becomes a cheap load against a permanently-zero atomic.
std::atomic<bool> g_enabled{true};

constexpr uint64_t kRecompMemBase = 0xFFFFFFFF80000000ull;
constexpr uint32_t kRecompMemMask = 0x3FFFFFFFu;
constexpr uint64_t kSpStatusAddr = 0xFFFFFFFFA4040010ull;
constexpr uint32_t kSpStatusSignalMask = 0x00007F80u;
constexpr OSPri kHardwarePollPriority = 80;

inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

uint32_t load_mem_word(uint8_t* rdram, uint64_t vaddr) {
    return *reinterpret_cast<uint32_t*>(
        rdram + ((vaddr - kRecompMemBase) & kRecompMemMask));
}

void monitor_func() {
    using namespace std::chrono;
    while (!g_monitor_exit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(milliseconds(kMonitorIntervalMs));

        if (!g_enabled.load(std::memory_order_relaxed)) continue;

        uint64_t last = g_last_switch_ms.load(std::memory_order_relaxed);
        if (last == 0) continue; // No game thread has resumed yet.

        if (g_should_yield.load(std::memory_order_relaxed)) continue; // Already pending.

        uint64_t now = now_ms();
        if (now - last > kStuckThresholdMs) {
            g_should_yield.store(1, std::memory_order_relaxed);
        }
    }
}

} // namespace

void ultramodern::init_scheduler_tick(RDRAM_ARG1) {
    g_rdram = rdram;
    if (g_monitor_started.exchange(true, std::memory_order_relaxed)) return;
    g_monitor_exit.store(false, std::memory_order_relaxed);
    g_monitor_thread = std::thread{monitor_func};
}

void ultramodern::join_scheduler_tick() {
    if (!g_monitor_started.load(std::memory_order_relaxed)) return;
    g_monitor_exit.store(true, std::memory_order_relaxed);
    if (g_monitor_thread.joinable()) {
        g_monitor_thread.join();
    }
    g_monitor_started.store(false, std::memory_order_relaxed);
}

void ultramodern::record_context_switch() {
    g_last_switch_ms.store(now_ms(), std::memory_order_relaxed);
}

void ultramodern::yield_to_any_queued(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        return;
    }
    PTR(OSThread) next_ = ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
    OSThread* next_thread = TO_PTR(OSThread, next_);
    // Re-queue ourselves at the priority-ordered position then resume
    // the next thread and block on our own context. Same shape as the
    // file-static swap_to_thread in scheduling.cpp.
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread());
    TO_PTR(OSThread, ultramodern::this_thread())->state = OSThreadState::QUEUED;
    ultramodern::resume_thread_and_wait(PASS_RDRAM next_thread);
}

extern "C" void ultramodern_voluntary_preemption_set_enabled(int enable) {
    g_enabled.store(enable != 0, std::memory_order_relaxed);
}

extern "C" void ultramodern_scheduler_tick(void) {
    // Hot path: relaxed atomic loads + branches. Returns immediately
    // when no yield or externally-posted message is pending.
    if (__builtin_expect(g_should_yield.load(std::memory_order_relaxed) == 0, 1)) {
        if (!ultramodern::external_message_pending()) {
            return;
        }
        if (!ultramodern::is_game_thread()) return;
        if (g_rdram == nullptr) return;
        uint8_t* rdram = g_rdram;

        ultramodern::wait_for_external_message_timed(rdram, 0);
        ultramodern::check_running_queue(rdram);
        return;
    }
    // Slow path. Defensive: skip if called outside a game thread, or
    // before init_scheduler_tick stashed rdram.
    if (!ultramodern::is_game_thread()) return;
    if (g_rdram == nullptr) return;
    uint8_t* rdram = g_rdram;

    // Clear the flag *before* yielding. If we cleared it after, a
    // monitor wake during our yield could re-set it and we'd miss the
    // intended single-shot semantics.
    g_should_yield.store(0, std::memory_order_relaxed);

    // Drain one pending external message into its target OSMesgQueue.
    // Non-blocking. This is what unblocks predicate-flipping threads
    // sitting in osRecvMesg waiting for a completion event posted from
    // a host thread (audio task complete, DP/SP/VI completions, etc.).
    ultramodern::wait_for_external_message_timed(rdram, 0);

    // If an RSP signal bit is already visible to CPU-side MMIO polling,
    // let the current busy-waiting thread consume it immediately instead
    // of queueing that thread behind unrelated game work. This preserves
    // the voluntary-preemption escape hatch for unsatisfied waits while
    // avoiding starvation at the exact moment an HLE/recompiled RSP task
    // has published the predicate the loop is spinning on.
    if ((load_mem_word(rdram, kSpStatusAddr) & kSpStatusSignalMask) != 0) {
        PTR(OSThread) self = ultramodern::this_thread();
        bool high_priority_poll = self != NULLPTR &&
            TO_PTR(OSThread, self)->priority >= kHardwarePollPriority;
        if (high_priority_poll || recomp_rsp_consume_preempt_skip_budget() != 0) {
            return;
        }
    }

    // Swap to head of running_queue regardless of priority. The stuck
    // thread is by definition busy-waiting on a predicate that some
    // other thread (often same-or-lower priority) needs to flip.
    ultramodern::yield_to_any_queued(rdram);
}
