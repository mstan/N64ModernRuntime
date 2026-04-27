/*
 * ultra_trace.cpp — implementation of the libultra-call ring.
 *
 * Lock-free fixed-size ring. Single atomic write index; each
 * recorder fetch-adds to claim a slot, then writes the slot fields
 * and finally publishes its `seq`. Readers compare the stored seq
 * to the requested idx to detect overwrites.
 *
 * No backpressure, no eviction logic — the slot at (idx & mask) is
 * simply the most recent writer's record. With a 4096-deep ring
 * this gives ~4 ms of history at typical libultra call rates,
 * which is far more than the wall-clock window between an LLM
 * probe firing and the boot event we want to inspect.
 */

#include "librecomp/ultra_trace.hpp"

#include <atomic>
#include <chrono>
#include <cstring>

namespace {

constexpr uint32_t kCapacity = 16384; /* power of two — sliding window */
constexpr uint32_t kMask     = kCapacity - 1;

/* Boot-history capacity. This is a SEPARATE buffer that fills once
 * from process start and then stops. Lets us answer "what did each
 * thread do during boot?" no matter how long the game has been
 * running in steady state. 32K events is enough for a typical
 * libultra boot (osCreate*, osStartThread, osViSetEvent,
 * osContInit, osEepromProbe, etc. — order of hundreds to a few
 * thousand events). Memory cost is ~2.4MB once. */
constexpr uint32_t kBootCapacity = 32768;

struct Slot {
    /* Marker that the writer has finished filling the slot. We
     * publish `seq` last with a release store; readers do an
     * acquire load before trusting the rest of the fields. */
    std::atomic<uint64_t> seq{UINT64_MAX};
    char     name[40];
    uint32_t pc;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint64_t ms;
};

Slot                       g_ring[kCapacity];
Slot                       g_boot[kBootCapacity];
std::atomic<uint64_t>      g_write_idx{0};
std::atomic<uint32_t>      g_boot_count{0};

/* Captured at first wrapper entry. We store as void* via atomic
 * because there's no portable atomic<unsigned char*> with the
 * narrow contract we need; a raw pointer assignment is atomic on
 * x86_64 anyway. */
std::atomic<unsigned char*> g_rdram{nullptr};

/* Set on first record so timestamps are relative to "interesting
 * time started" rather than process epoch. Readers don't need to
 * know the absolute origin; they just compare millis between
 * events. */
std::atomic<uint64_t>      g_origin_ns{0};

uint64_t now_ns() {
    return (uint64_t)std::chrono::steady_clock::now()
                       .time_since_epoch().count();
}

} /* namespace */

extern "C" void recomp_ultra_trace_record(const char* name,
                                          uint32_t pc,
                                          uint32_t a0, uint32_t a1,
                                          uint32_t a2, uint32_t a3) {
    /* First-record initializes origin. Many threads may race here;
     * compare_exchange ensures only one wins and the rest see the
     * established value. */
    uint64_t origin = g_origin_ns.load(std::memory_order_relaxed);
    if (origin == 0) {
        uint64_t now = now_ns();
        if (g_origin_ns.compare_exchange_strong(origin, now,
                                                std::memory_order_relaxed)) {
            origin = now;
        }
    }

    uint64_t seq = g_write_idx.fetch_add(1, std::memory_order_relaxed);
    uint64_t ms  = (now_ns() - origin) / 1000000ull;

    auto fill = [&](Slot& s) {
        s.seq.store(UINT64_MAX, std::memory_order_relaxed);
        if (name != nullptr) {
            std::strncpy(s.name, name, sizeof(s.name) - 1);
            s.name[sizeof(s.name) - 1] = '\0';
        } else {
            s.name[0] = '\0';
        }
        s.pc = pc;
        s.a0 = a0;
        s.a1 = a1;
        s.a2 = a2;
        s.a3 = a3;
        s.ms = ms;
        s.seq.store(seq, std::memory_order_release);
    };

    /* Sliding ring (recent window). */
    fill(g_ring[seq & kMask]);

    /* Boot snapshot — fill once, never evict. */
    uint32_t b = g_boot_count.fetch_add(1, std::memory_order_relaxed);
    if (b < kBootCapacity) {
        fill(g_boot[b]);
    }
}

extern "C" void recomp_runtime_set_rdram(unsigned char* rdram) {
    unsigned char* expected = nullptr;
    g_rdram.compare_exchange_strong(expected, rdram,
                                    std::memory_order_relaxed);
}

extern "C" unsigned char* recomp_runtime_get_rdram(void) {
    return g_rdram.load(std::memory_order_relaxed);
}

extern "C" uint64_t recomp_ultra_trace_write_idx(void) {
    return g_write_idx.load(std::memory_order_relaxed);
}

extern "C" uint32_t recomp_ultra_trace_capacity(void) {
    return kCapacity;
}

extern "C" int recomp_ultra_trace_get(uint64_t idx,
                                      recomp_ultra_trace_event* out) {
    if (out == nullptr) return 0;
    Slot& s = g_ring[idx & kMask];
    uint64_t got = s.seq.load(std::memory_order_acquire);

    /* Always copy whatever's in the slot — partial info beats
     * none for a diagnostic. Caller decides if it's trustworthy
     * via the returned validity flag. */
    std::memcpy(out->name, s.name, sizeof(out->name));
    out->pc  = s.pc;
    out->a0  = s.a0;
    out->a1  = s.a1;
    out->a2  = s.a2;
    out->a3  = s.a3;
    out->ms  = s.ms;
    out->seq = got;

    /* Valid iff the slot still holds the requested index AND the
     * writer finished publishing (seq != sentinel). */
    return (got == idx) ? 1 : 0;
}

extern "C" uint32_t recomp_ultra_trace_boot_count(void) {
    uint32_t c = g_boot_count.load(std::memory_order_relaxed);
    /* Saturate to capacity for callers; they shouldn't iterate
     * past kBootCapacity even if many more events occurred. */
    return c < kBootCapacity ? c : kBootCapacity;
}

extern "C" uint32_t recomp_ultra_trace_boot_capacity(void) {
    return kBootCapacity;
}

extern "C" int recomp_ultra_trace_boot_get(uint32_t pos,
                                           recomp_ultra_trace_event* out) {
    if (out == nullptr) return 0;
    if (pos >= kBootCapacity) return 0;
    Slot& s = g_boot[pos];
    uint64_t got = s.seq.load(std::memory_order_acquire);

    std::memcpy(out->name, s.name, sizeof(out->name));
    out->pc  = s.pc;
    out->a0  = s.a0;
    out->a1  = s.a1;
    out->a2  = s.a2;
    out->a3  = s.a3;
    out->ms  = s.ms;
    out->seq = got;

    return (got != UINT64_MAX) ? 1 : 0;
}
