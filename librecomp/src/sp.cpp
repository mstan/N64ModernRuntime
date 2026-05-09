// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with upstream authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Always-on osSpTaskStartGo ring (sp_task_log) for runner-side
//     diagnostics — captures task type, ucode, data_ptr, sizes per
//     submission so post-mortem can identify the last gfx/audio task.
//   - Framework-level libultra-call ring + boot-sequence fixes.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "librecomp/ultra_trace.hpp"

// External counters from debug_server (frame, send_dl) so we can
// timestamp each task submission with the runner's view of progress.
namespace pkmnstadium { namespace dbg {
    extern std::atomic<uint64_t> g_frame_count;
    extern std::atomic<uint64_t> g_send_dl_count;
    extern std::atomic<uint64_t> g_send_dl_gfx_count;
}}

// ── osSpTaskStartGo event ring ───────────────────────────────────────
// Always-on ring of every osSpTaskStartGo call. Used to identify the
// LAST gfx task submitted before send_dl freezes — answers the
// gfx-submit-freeze question in slice 1.
namespace sp_task_log {
    struct Event {
        uint64_t seq;
        uint64_t ms;
        uint64_t frame;
        uint64_t send_dl;
        uint32_t mips_ra;       // ctx->r31 — caller PC in MIPS
        uint32_t task_ptr;      // gpr passed in $a0
        uint32_t task_type;     // task->t.type
        uint32_t task_flags;    // task->t.flags
        uint32_t ucode;         // task->t.ucode
        uint32_t data_ptr;      // task->t.data_ptr
        uint32_t data_size;     // task->t.data_size
        uint32_t output_buff;   // task->t.output_buff
        uint32_t output_buff_size;
    };

    // Sized so the ring retains GFX history even after the game thread
    // freezes and the audio thread keeps pumping ~60Hz tasks for ~30s
    // (audio ~1800 events) before SEH dumps. 4096 = generous headroom.
    constexpr size_t RING_CAP = 4096;
    static Event ring[RING_CAP];
    static std::atomic<uint64_t> next_seq{0};
    static std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();

    static inline uint64_t now_ms() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    inline void record(const OSTask* task, uint32_t task_ptr, uint32_t mips_ra) {
        const uint64_t s = next_seq.fetch_add(1, std::memory_order_relaxed);
        Event& e = ring[s % RING_CAP];
        e.seq = s;
        e.ms = now_ms();
        e.frame = pkmnstadium::dbg::g_frame_count.load(std::memory_order_relaxed);
        e.send_dl = pkmnstadium::dbg::g_send_dl_count.load(std::memory_order_relaxed);
        e.mips_ra = mips_ra;
        e.task_ptr = task_ptr;
        e.task_type = task ? task->t.type : 0;
        e.task_flags = task ? task->t.flags : 0;
        e.ucode = task ? task->t.ucode : 0;
        e.data_ptr = task ? task->t.data_ptr : 0;
        e.data_size = task ? task->t.data_size : 0;
        e.output_buff = task ? task->t.output_buff : 0;
        e.output_buff_size = task ? task->t.output_buff_size : 0;
    }
}

extern "C" void recomp_sp_task_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using namespace sp_task_log;
    const uint64_t s = next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available = (s < RING_CAP) ? size_t(s) : RING_CAP;
    const size_t want = (cap < available) ? cap : available;
    Event* out = static_cast<Event*>(out_void);
    const size_t start = (s - want) % RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = ring[(start + i) % RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t recomp_sp_task_event_size(void) {
    return sizeof(sp_task_log::Event);
}

extern "C" void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // Nothing to do here
}

bool dump_frame = false;

extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    OSTask* task = TO_PTR(OSTask, ctx->r4);
    sp_task_log::record(task, uint32_t(ctx->r4), uint32_t(ctx->r31));
    // For debugging
    if (dump_frame) {
        char addr_str[32];
        constexpr size_t ram_size = 0x800000;
        std::unique_ptr<char[]> ram_unswapped = std::make_unique<char[]>(ram_size);
        snprintf(addr_str, sizeof(addr_str) - 1, "%08X", task->t.data_ptr);
        addr_str[sizeof(addr_str) - 1] = '\0';
        std::ofstream dump_file{ "ramdump" + std::string{ addr_str } + ".bin", std::ios::binary};

        for (size_t i = 0; i < ram_size; i++) {
            ram_unswapped[i] = rdram[i ^ 3];
        }

        dump_file.write(ram_unswapped.get(), ram_size);
        dump_frame = false;
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}

extern "C" void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // Ignore yield requests (acts as if the task completed before it received the yield request)
}

extern "C" void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // Task yield requests are ignored, so always return 0 as tasks will never be yielded
    ctx->r2 = 0;
}

extern "C" void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    assert(false);
}
