#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "ultramodern/ultramodern.hpp"

static PTR(OSThread) running_queue_impl = NULLPTR;

extern "C" uint32_t ultramodern_running_queue_head(void) {
    return static_cast<uint32_t>(running_queue_impl);
}

namespace sched_log {
    enum Op : uint32_t {
        OP_INSERT = 1,
        OP_POP = 2,
        OP_REMOVE = 3,
        OP_REMOVE_MISS = 4,
        OP_SCHEDULE_RUNNING = 100,
        OP_CHECK_EMPTY = 101,
        OP_CHECK_PEEK = 102,
        OP_CHECK_NO_SWAP = 103,
        OP_CHECK_SWAP = 104,
        OP_SWAP_ENTER = 110,
        OP_SWAP_INSERT_SELF = 111,
        OP_SWAP_WAIT_ENTER = 112,
        OP_SWAP_WAIT_RETURN = 113,
        OP_WAIT_BEGIN = 120,
        OP_WAIT_RETURN = 121,
        OP_RESUME_SIGNAL = 122,
        OP_RUN_NEXT_ENTER = 123,
        OP_RUN_NEXT_EMPTY = 124,
        OP_RUN_NEXT_TARGET = 125,
        OP_RUN_NEXT_WAIT_ENTER = 126,
        OP_RUN_NEXT_WAIT_RETURN = 127,
        OP_YIELD_ANY_ENTER = 130,
        OP_YIELD_ANY_EMPTY = 131,
        OP_YIELD_ANY_TARGET = 132,
        OP_YIELD_ANY_REQUEUE_SELF = 133,
        OP_YIELD_ANY_WAIT_ENTER = 134,
        OP_YIELD_ANY_WAIT_RETURN = 135,
    };

    struct Event {
        uint64_t seq;
        uint64_t ms;
        uint32_t op;
        uint32_t queue;
        uint32_t thread;
        uint32_t current_thread;
        uint32_t head_after;
        uint32_t next_after;
        uint16_t thread_id;
        uint16_t current_thread_id;
        int16_t priority;
        uint16_t pad;
    };

    static_assert(sizeof(Event) == 48, "scheduler queue event size changed");

    constexpr size_t RING_CAP = 65536;
    static Event ring[RING_CAP];
    static std::atomic<uint64_t> next_seq{0};
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    uint64_t now_ms() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    uint16_t thread_id(RDRAM_ARG PTR(OSThread) t_) {
        if (t_ == NULLPTR) {
            return 0;
        }
        return static_cast<uint16_t>(TO_PTR(OSThread, t_)->id);
    }

    int16_t thread_priority(RDRAM_ARG PTR(OSThread) t_) {
        if (t_ == NULLPTR) {
            return 0;
        }
        return static_cast<int16_t>(TO_PTR(OSThread, t_)->priority);
    }

    // Never-evict per-thread state table (indexed by OSThread id). The event
    // ring wraps under the idle/audio cycle's high event rate, so an early
    // park (a thread's last blocked_on_recv) scrolls out before a probe looks.
    // This table keeps each thread's LAST event + last message-queue block
    // permanently, so the full thread state is always queryable regardless of
    // how long the cycle has flooded the ring. (Doctrine: fix the ring to cover
    // the window, never arm-then-capture.)
    struct ThreadState {
        uint32_t valid;
        uint32_t id;
        uint32_t thread;
        int32_t  priority;
        uint32_t last_op;
        uint32_t last_queue;
        uint32_t last_head_after;
        uint64_t last_ms;
        uint32_t last_mq;     // last INSERT into a real msg queue (blocked_on_recv)
        uint64_t last_mq_ms;
        uint64_t count;
    };
    constexpr size_t MAX_TID = 1024;
    static ThreadState thread_states[MAX_TID];

    void record(RDRAM_ARG uint32_t op,
                PTR(PTR(OSThread)) queue_,
                PTR(OSThread) thread_,
                PTR(OSThread) head_after) {
        const uint64_t s = next_seq.fetch_add(1, std::memory_order_relaxed);
        Event& e = ring[s % RING_CAP];
        e.seq = s;
        e.ms = now_ms();
        e.op = op;
        e.queue = static_cast<uint32_t>(queue_);
        e.thread = static_cast<uint32_t>(thread_);
        e.current_thread = static_cast<uint32_t>(ultramodern::this_thread());
        e.head_after = static_cast<uint32_t>(head_after);
        e.next_after = thread_ != NULLPTR ? static_cast<uint32_t>(TO_PTR(OSThread, thread_)->next) : 0;
        e.thread_id = thread_id(PASS_RDRAM thread_);
        e.current_thread_id = thread_id(PASS_RDRAM ultramodern::this_thread());
        e.priority = thread_priority(PASS_RDRAM thread_);
        e.pad = 0;

        if (e.thread_id < MAX_TID && !(e.thread_id == 0 && thread_ == NULLPTR)) {
            ThreadState& ts = thread_states[e.thread_id];
            ts.valid = 1;
            ts.id = e.thread_id;
            ts.thread = e.thread;
            ts.priority = e.priority;
            ts.last_op = op;
            ts.last_queue = e.queue;
            ts.last_head_after = e.head_after;
            ts.last_ms = e.ms;
            ts.count++;
            // INSERT into a queue that is neither the running queue (0xFFFFFFFF,
            // the running_queue sentinel) nor NULL == blocking on a real msg
            // queue's blocked_on_recv list: that is what the thread is waiting on.
            if (op == OP_INSERT && e.queue != 0xFFFFFFFFu && e.queue != 0) {
                ts.last_mq = e.queue;
                ts.last_mq_ms = e.ms;
            }
        }
    }
}

extern "C" void ultramodern_sched_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using namespace sched_log;
    const uint64_t s = next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available = std::min<size_t>(s, RING_CAP);
    const size_t want = std::min(cap, available);
    Event* out = static_cast<Event*>(out_void);
    const size_t start = (s - want) % RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = ring[(start + i) % RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t ultramodern_sched_event_size(void) {
    return sizeof(sched_log::Event);
}

extern "C" size_t ultramodern_sched_thread_state_size(void) {
    return sizeof(sched_log::ThreadState);
}

// Copy the never-evict per-thread state table. Returns every thread the
// scheduler has ever touched, each with its last event + last msg-queue block.
extern "C" void ultramodern_sched_thread_states_copy(
    void* out_void, size_t cap, size_t* n_written)
{
    using namespace sched_log;
    size_t w = 0;
    if (out_void != nullptr) {
        ThreadState* out = static_cast<ThreadState*>(out_void);
        for (size_t i = 0; i < MAX_TID && w < cap; i++) {
            if (thread_states[i].valid) {
                out[w++] = thread_states[i];
            }
        }
    }
    if (n_written) *n_written = w;
}

static PTR(OSThread)* queue_to_ptr(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == ultramodern::running_queue) {
        return &running_queue_impl;
    }
    return TO_PTR(PTR(OSThread), queue);
}

static PTR(OSThread) queue_head_or_zero(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == NULLPTR) {
        return NULLPTR;
    }
    return *queue_to_ptr(PASS_RDRAM queue);
}

void ultramodern::scheduler_trace_mark(RDRAM_ARG uint32_t op,
                                       PTR(PTR(OSThread)) queue_,
                                       PTR(OSThread) thread_) {
    sched_log::record(PASS_RDRAM op, queue_, thread_, queue_head_or_zero(PASS_RDRAM queue_));
}

// A guest OSThread pointer is valid only if it is a 4-byte-aligned KSEG0 RDRAM
// address. A game that frees an OSThread while it is still linked in a runtime
// queue (e.g. Pokemon Snap's per-game-object process threads, destroyed en masse
// on scene teardown) leaves a garbage `next` in the chain; walking it unvalidated
// dereferences freed/reused memory and faults. Validate before every deref so the
// scheduler degrades gracefully instead of crashing. Strict improvement — the only
// behavior it changes is on an already-corrupt queue, where the alternative is a hard crash.
static inline bool valid_thread_ptr(PTR(OSThread) p) {
    uint32_t v = static_cast<uint32_t>(p);
    return v >= 0x80000000u && v < 0x80800000u && (v & 3u) == 0u;
}

void ultramodern::thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) added_) {
    // The queue is an intrusive singly-linked list kept in descending priority
    // order. Advance to the first link whose thread does not outrank the new one
    // and splice the new thread in ahead of it.
    PTR(OSThread)* link = queue_to_ptr(PASS_RDRAM queue_);
    OSThread* added = TO_PTR(OSThread, added_);
    debug_printf("[Thread Queue] thread %d -> queue 0x%08X\n", added->id, (uintptr_t)queue_);
    uint32_t guard = 0;
    while (*link && valid_thread_ptr(*link) && ++guard <= 0x40000u && TO_PTR(OSThread, *link)->priority > added->priority) {
        link = &TO_PTR(OSThread, *link)->next;
    }
    // If we stopped on a corrupt link (not NULL, not a valid pointer), sever the
    // bad tail here so we splice ahead of it rather than dereferencing garbage.
    if (*link != NULLPTR && !valid_thread_ptr(*link)) {
        *link = NULLPTR;
    }
    added->next = *link;
    added->queue = queue_;
    *link = added_;
    sched_log::record(PASS_RDRAM sched_log::OP_INSERT, queue_, added_, *queue_to_ptr(PASS_RDRAM queue_));

    // Trace the queue contents after the insertion.
    debug_printf("  Contains:");
    link = queue_to_ptr(PASS_RDRAM queue_);
    while (*link) {
        debug_printf("%d (%d) ", TO_PTR(OSThread, *link)->id, TO_PTR(OSThread, *link)->priority);
        link = &TO_PTR(OSThread, *link)->next;
    }
    debug_printf("\n");
}

PTR(OSThread) ultramodern::thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    // The highest-priority thread is at the head; detach and return it.
    PTR(OSThread)* head = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) popped = *head;
    // Guard against a corrupt head (freed/reused OSThread) — treat as empty.
    if (!valid_thread_ptr(popped)) {
        *head = NULLPTR;
        return NULLPTR;
    }
    *head = TO_PTR(OSThread, popped)->next;
    TO_PTR(OSThread, popped)->queue = NULLPTR;
    sched_log::record(PASS_RDRAM sched_log::OP_POP, queue_, popped, *head);
    debug_printf("[Thread Queue] popped thread %d from queue 0x%08X\n", TO_PTR(OSThread, popped)->id, (uintptr_t)queue_);
    return popped;
}

bool ultramodern::thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    // Guard against a garbage queue pointer (queue_to_ptr would build a wild host
    // pointer that faults on the first deref). The running queue is a sentinel
    // backed by a host static, so it is always valid.
    if (queue_ != ultramodern::running_queue && !valid_thread_ptr(queue_)) {
        sched_log::record(PASS_RDRAM sched_log::OP_REMOVE_MISS, queue_, t_, NULLPTR);
        return false;
    }

    // Scan the list for the target and unlink it if found.
    PTR(OSThread)* link = queue_to_ptr(PASS_RDRAM queue_);
    uint32_t guard = 0;
    while (*link != NULLPTR) {
        PTR(OSThread) cur = *link;
        if (!valid_thread_ptr(cur) || ++guard > 0x40000u) {
            // Corrupt/oversized chain (a freed/reused OSThread). Sever here so a
            // later walk of this queue can't re-fault, and report not-found.
            *link = NULLPTR;
            sched_log::record(PASS_RDRAM sched_log::OP_REMOVE_MISS, queue_, t_, NULLPTR);
            return false;
        }
        if (cur == t_) {
            *link = TO_PTR(OSThread, cur)->next;
            TO_PTR(OSThread, cur)->queue = NULLPTR;  // clear on removal, matching thread_queue_pop
            sched_log::record(PASS_RDRAM sched_log::OP_REMOVE, queue_, t_, *queue_to_ptr(PASS_RDRAM queue_));
            return true;
        }
        link = &TO_PTR(OSThread, cur)->next;
    }

    // Target was not present in this queue.
    sched_log::record(PASS_RDRAM sched_log::OP_REMOVE_MISS, queue_, t_, *queue_to_ptr(PASS_RDRAM queue_));
    return false;
}

bool ultramodern::thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    return *queue_to_ptr(PASS_RDRAM queue_) == NULLPTR;
}

PTR(OSThread) ultramodern::thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    return *queue_to_ptr(PASS_RDRAM queue_);
}
