// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). The notices below describe the changes and their authorship,
// as required by GPL-3.0 §5(a). Original file copyright remains with the
// upstream N64ModernRuntime authors.
//
// Modified 2026 by Matthew Stanley:
//   - Re-queue externals on full target OSMesgQueue (replaces silent drop
//     in dequeue_external_messages; see commit a14270c).
//   - Defensive boundary check + post-mortem dump in do_send to surface
//     corrupted OSMesgQueue pointers cleanly instead of host-side SEGV
//     (see commit bbd3f79).
//   - Queue-event ring buffer (always-on) + ultramodern_mesg_recent_copy
//     accessor for runner-side diagnostics (commit dd8137d).
//   - Queue-ops mutex + external-pump host thread (Option C). Drains
//     external_messages into target queues on a host timer, regardless
//     of whether any game thread is currently in a libultra primitive.
//     Resolves the cooperative-scheduler softlock class where a game
//     thread tight-loops on a predicate that depends on a completion
//     message that nobody dequeues.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <cstdio>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// Unified post-mortem dump (src/main/post_mortem.cpp) — called from
// the do_send corruption guard so we capture full state when a
// corrupted OSMesgQueue ptr arrives.
extern "C" void psr_post_mortem_dump(const char* reason, void* fault_info);

// ── Queue-event ring buffer ───────────────────────────────────────────
//
// Always-on ring of recent osSendMesg/osRecvMesg/external-enqueue
// events. Used to diagnose softlocks where the game thread blocks on
// a queue and we need to know exactly what was sent/received around
// the freeze. Probes query the ring post-fact via the runner's
// debug_server (`mesg_recent` cmd) — never arm/capture/dump.
namespace mesg_log {
    enum Op : uint8_t {
        OP_SEND_GAME       = 1,  // osSendMesg from game thread
        OP_SEND_EXTERNAL   = 2,  // osSendMesg from non-game thread (queued externally)
        OP_RECV_ENTER      = 3,  // osRecvMesg called
        OP_RECV_BLOCK      = 4,  // osRecvMesg blocked (queue empty + OS_MESG_BLOCK)
        OP_RECV_RETURN_OK  = 5,  // osRecvMesg returned with a message
        OP_EXT_DEQ_OK      = 6,  // dequeue_external_messages drained one
        OP_EXT_DEQ_FULL    = 7,  // dequeue tried to send but target full → re-queued
        OP_DO_SEND_BLOCK   = 8,  // do_send blocked sender on queue-full
    };

    struct Event {
        uint64_t seq;
        uint64_t ms;
        uint32_t mq;          // OSMesgQueue pointer (gpr-style)
        uint32_t msg;         // message value (often pointer)
        uint16_t valid_before;
        uint16_t valid_after;
        uint8_t  op;
        uint8_t  block;       // 1 = OS_MESG_BLOCK, 0 = OS_MESG_NOBLOCK
        uint8_t  game_thread; // 1 = sender/receiver was game thread
        uint8_t  pad;
    };

    // Bumped from 1024 to 65536 to span ~18 minutes of VI ticks at 60Hz —
    // 1024 was getting saturated by VI retraces (60/sec) within ~17 seconds,
    // burying any non-VI events from menu transitions before we could query.
    // 65536 events × 40 bytes ≈ 2.5 MB, acceptable for diagnostics.
    constexpr size_t RING_CAP = 65536;
    static Event ring[RING_CAP];
    static std::atomic<uint64_t> next_seq{0};
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    static inline uint64_t now_ms() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    inline void record(uint8_t op, uint32_t mq, uint32_t msg,
                       uint16_t vb, uint16_t va,
                       bool block, bool game_thread) {
        const uint64_t s = next_seq.fetch_add(1, std::memory_order_relaxed);
        Event& e = ring[s % RING_CAP];
        e.seq = s;
        e.ms = now_ms();
        e.mq = mq;
        e.msg = msg;
        e.valid_before = vb;
        e.valid_after = va;
        e.op = op;
        e.block = block ? 1 : 0;
        e.game_thread = game_thread ? 1 : 0;
        e.pad = 0;
    }
}

// Public dump: copies up to `cap` most-recent events (chronological)
// into `out`. Returns count written via `*n_written` and the global
// write_idx via `*next_seq_out`.
extern "C" void ultramodern_mesg_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using namespace mesg_log;
    const uint64_t s = next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available = std::min<size_t>(s, RING_CAP);
    const size_t want = std::min(cap, available);
    Event* out = static_cast<Event*>(out_void);
    // Copy oldest first: starting index = (s - want) % RING_CAP.
    const size_t start = (s - want) % RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = ring[(start + i) % RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t ultramodern_mesg_event_size(void) {
    return sizeof(mesg_log::Event);
}

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

// Pump-thread wake-signal state (Option C) — defined at file scope so
// enqueue paths can poke the condition variable to keep external-
// message delivery latency sub-millisecond when the pump is idle.
// Definitions are below.
static std::thread g_pump_thread;
static std::atomic<bool> g_pump_shutdown{false};
static std::atomic<bool> g_pump_started{false};
static std::mutex g_pump_wake_mutex;
static std::condition_variable g_pump_wake_cv;

// ── Queue-ops mutex (Option C) ────────────────────────────────────────
//
// Serializes mutations of OSMesgQueue state and the per-queue
// blocked-thread queues + running_queue when they're touched from
// inside do_send / do_recv. Held by:
//   - Game-thread libultra paths (do_send / do_recv) during their
//     critical sections; released across pause-and-wait so other
//     game threads can run.
//   - The external-pump host thread while draining externals into
//     target queues.
// Recursive because do_send is called both directly (from game-thread
// osSendMesg) and indirectly via dequeue_external_messages on the same
// thread; nested locking on the same thread must succeed.
static std::recursive_mutex g_msg_queue_mutex;

void enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam) {
    external_messages.enqueue({mq, msg, jam});
    mesg_log::record(mesg_log::OP_SEND_EXTERNAL,
                     uint32_t(mq), uint32_t(uintptr_t(msg)),
                     0, 0, false, false);
    // Wake the external-pump thread so the message is delivered with
    // sub-millisecond latency rather than waiting for the 2 ms timer.
    // Cheap when the pump isn't running yet (notify_all on an idle CV).
    if (g_pump_started.load(std::memory_order_acquire)) {
        g_pump_wake_cv.notify_one();
    }
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

// Counter for how many external messages have been re-queued after a
// transient target-queue-full failure. Surfaced via the runner's
// status command so a sustained nonzero value indicates a target
// queue is being overrun (receiver thread starved).
static std::atomic<uint64_t> g_external_requeues{0};

void dequeue_external_messages(RDRAM_ARG1) {
    std::lock_guard<std::recursive_mutex> lk(g_msg_queue_mutex);
    QueuedMessage to_send;
    while (external_messages.try_dequeue(to_send)) {
        OSMesgQueue* mq_pre = TO_PTR(OSMesgQueue, to_send.mq);
        const uint16_t vb = uint16_t(mq_pre ? mq_pre->validCount : 0);
        // Try non-blocking send first.
        bool ok = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
        if (!ok) {
            mesg_log::record(mesg_log::OP_EXT_DEQ_FULL,
                             uint32_t(to_send.mq),
                             uint32_t(uintptr_t(to_send.mesg)),
                             vb, vb, false, true);
            // Target OSMesgQueue is full. The original code silently
            // dropped the message here — see git blame on this line.
            // That is wrong: callers (sp_complete / dp_complete /
            // ai_complete / etc) treat enqueue_external_message as
            // reliable delivery, and silent drops produce
            // hard-to-diagnose softlocks when game-thread receivers
            // can't keep up with the host-side event burst rate.
            //
            // Pokemon Stadium hits this on the gfx scheduler queue
            // 0x800A6CD0: audio SP DONE + VI ticks generate ~110
            // events/sec into a cap=16 queue, briefly filling it,
            // and the gfx RDP DONE for Stadium's third boot-logo
            // dlist gets dropped — Game_Thread softlocks waiting for
            // a completion that never propagates.
            //
            // Fix: re-queue at the tail and bail out of this drain
            // pass. We must NOT keep looping (we'd just dequeue and
            // re-enqueue the same blocking message in a tight loop
            // with no chance for the receiver to drain). Letting the
            // caller (the game-thread inside osSendMesg/osRecvMesg)
            // return and continue executing gives the receiver a
            // chance to dequeue, freeing space; the next call into
            // dequeue_external_messages will retry.
            //
            // NB: doesn't apply to OS_MESG_BLOCK semantics — sender
            // explicitly opted-in to blocking, and that path is
            // direct (skips this external queue). Externals are by
            // design fire-and-forget posts from host threads, but
            // "fire-and-forget" must mean "delivered eventually",
            // not "occasionally lost."
            external_messages.enqueue(to_send);
            g_external_requeues.fetch_add(1, std::memory_order_relaxed);
            break;
        } else {
            OSMesgQueue* mq_post = TO_PTR(OSMesgQueue, to_send.mq);
            const uint16_t va = uint16_t(mq_post ? mq_post->validCount : 0);
            mesg_log::record(mesg_log::OP_EXT_DEQ_OK,
                             uint32_t(to_send.mq),
                             uint32_t(uintptr_t(to_send.mesg)),
                             vb, va, false, true);
        }
    }
}

// Public accessor surfaced via the runner's debug_server status cmd.
extern "C" uint64_t ultramodern_external_requeues(void) {
    return g_external_requeues.load(std::memory_order_relaxed);
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG1, u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    // Defensive boundary check: do_send writes through mq->msg[last]
    // which produces a host-pointer SEGV if mq is corrupted (we've
    // observed thread20_rsp / OSScTask scenarios where the task
    // struct's msgQ field at +0x28 reads back garbage). Without this
    // guard the trace just shows "do_send wrote to host 0x65b075d8"
    // which is hard to backtrack. Guard:
    //   1. mq_ vaddr must be in kseg0 RDRAM range.
    //   2. mq->msg buffer pointer must be in kseg0 RDRAM range.
    //   3. mq->msgCount must be > 0 (else mod-by-zero on `last`).
    //   4. mq->first / validCount must be sane (< msgCount).
    // On failure: log to stderr, dump post-mortem, return false. The
    // caller treats this as "send failed" (same as a full queue) which
    // is recoverable; better than SEGV-and-die. The accompanying
    // post-mortem records who held the bad mq_ at the time so we can
    // chase the upstream corruption next iteration.
    auto fail = [&](const char* why, uint32_t a, uint32_t b, uint32_t c) -> bool {
        fprintf(stderr,
            "[do_send] BAD mq_=0x%08X reason=%s a=0x%08X b=0x%08X c=0x%08X "
            "msg=0x%08X jam=%d block=%d — skipping send\n",
            uint32_t(mq_), why, a, b, c,
            uint32_t(uintptr_t(msg)), int(jam), int(block));
        fflush(stderr);
        // Trigger unified post-mortem so we get host-stack + rings +
        // hardware state dumped to last_run_report.json. Use a one-shot
        // guard so a stuck audio thread can't spam dumps.
        static std::atomic<int> dumped{0};
        int prior = dumped.fetch_add(1);
        if (prior == 0) {
            psr_post_mortem_dump("do_send-bad-mq", nullptr);
        }
        return false;
    };
    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
        return fail("mq_-not-kseg0", 0, 0, 0);
    }
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (uint32_t(mq->msg) < 0x80000000u || uint32_t(mq->msg) >= 0x80800000u) {
        return fail("mq.msg-not-kseg0",
                    uint32_t(mq->msg), uint32_t(mq->msgCount), 0);
    }
    if (mq->msgCount <= 0 || mq->msgCount > 0x10000) {
        return fail("mq.msgCount-insane",
                    uint32_t(mq->msg), uint32_t(mq->msgCount), 0);
    }
    if (mq->first < 0 || mq->first >= mq->msgCount
        || mq->validCount < 0 || mq->validCount > mq->msgCount) {
        return fail("mq.first/validCount-insane",
                    uint32_t(mq->first), uint32_t(mq->validCount),
                    uint32_t(mq->msgCount));
    }
    // Mutex held across the queue-state critical section. For the
    // blocking path we release it across run_next_thread_and_wait so
    // other game threads (and the pump) can make progress, then
    // re-acquire and re-check the queue state on wake.
    std::unique_lock<std::recursive_mutex> lk(g_msg_queue_mutex);

    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            mesg_log::record(mesg_log::OP_DO_SEND_BLOCK,
                             uint32_t(mq_), uint32_t(uintptr_t(msg)),
                             uint16_t(mq->validCount), uint16_t(mq->validCount),
                             true, true);
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            lk.unlock();
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
            lk.lock();
        }
    }

    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    std::unique_lock<std::recursive_mutex> lk(g_msg_queue_mutex);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer empty
        while (MQ_IS_EMPTY(mq)) {
            mesg_log::record(mesg_log::OP_RECV_BLOCK,
                             uint32_t(mq_), 0,
                             uint16_t(mq->validCount), uint16_t(mq->validCount),
                             true, true);
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            lk.unlock();
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
            lk.lock();
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }

    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;

    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    const uint16_t vb = uint16_t(mq ? mq->validCount : 0);
    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    const uint16_t va = uint16_t(mq ? mq->validCount : 0);
    mesg_log::record(mesg_log::OP_SEND_GAME,
                     uint32_t(mq_), uint32_t(uintptr_t(msg)),
                     vb, va, flags == OS_MESG_BLOCK, true);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);

    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");

    const uint16_t vb_enter = uint16_t(mq ? mq->validCount : 0);
    mesg_log::record(mesg_log::OP_RECV_ENTER,
                     uint32_t(mq_), 0, vb_enter, vb_enter,
                     flags == OS_MESG_BLOCK, true);

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    const uint16_t va = uint16_t(mq ? mq->validCount : 0);
    if (received) {
        uint32_t got = 0;
        if (msg_ != NULLPTR) {
            got = uint32_t(uintptr_t(*TO_PTR(OSMesg, msg_)));
        }
        mesg_log::record(mesg_log::OP_RECV_RETURN_OK,
                         uint32_t(mq_), got, vb_enter, va,
                         flags == OS_MESG_BLOCK, true);
    }

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}

// ── External-pump host thread (Option C) ──────────────────────────────
//
// Runs continuously after preinit. Wakes on either a 2 ms timer or as
// soon as enqueue_external_message signals the wake-event, then drains
// external_messages into target queues by calling
// dequeue_external_messages (which acquires g_msg_queue_mutex
// internally).
//
// Why this is needed: ultramodern is a cooperative scheduler — a game
// thread that tight-loops on a memory predicate (e.g. polling
// validCount on a queue waiting for a DP-complete) never returns to
// the scheduler, so dequeue_external_messages from within
// osSendMesg/osRecvMesg never gets called. The completion is stuck in
// external_messages forever; the predicate never flips. The pump
// thread breaks the deadlock by delivering externals from a host
// context independent of game-thread cooperation.
//
// 2 ms tick is a balance: the busy-wait fix needs sub-frame latency
// (the game polls 60+ times per second), but waking too aggressively
// burns CPU. 2 ms gives ~500 wakes/sec — well below frame cadence,
// well below Win32 timer resolution waste.
static void external_pump_thread_func(uint8_t* rdram) {
    using namespace std::chrono_literals;
    while (!g_pump_shutdown.load(std::memory_order_acquire)) {
        // Wait either for a wake-signal (new external posted) or
        // for the 2 ms timer, whichever comes first. Coalescing
        // wakes is fine — the drain loop pulls everything currently
        // queued each pass.
        {
            std::unique_lock<std::mutex> lk(g_pump_wake_mutex);
            g_pump_wake_cv.wait_for(lk, 2ms);
        }
        if (g_pump_shutdown.load(std::memory_order_acquire)) break;
        dequeue_external_messages(PASS_RDRAM1);
    }
}

namespace ultramodern {
    void init_external_pump(RDRAM_ARG1) {
        bool expected = false;
        if (!g_pump_started.compare_exchange_strong(expected, true)) {
            return;  // already started
        }
        g_pump_shutdown.store(false, std::memory_order_release);
        g_pump_thread = std::thread(external_pump_thread_func, rdram);
    }

    void join_external_pump() {
        if (!g_pump_started.load(std::memory_order_acquire)) return;
        g_pump_shutdown.store(true, std::memory_order_release);
        g_pump_wake_cv.notify_all();
        if (g_pump_thread.joinable()) g_pump_thread.join();
        g_pump_started.store(false, std::memory_order_release);
    }
}  // namespace ultramodern
