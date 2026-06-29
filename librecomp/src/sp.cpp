// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
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
#include "librecomp/audio_uaf_protect.hpp"

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
        uint32_t wrapper_ptr;   // task_ptr - 0x20, for OSScTask wrappers
        uint32_t suspect;       // bitfield: bad ptr/type/ucode/data shape
        uint32_t task_type;     // task->t.type
        uint32_t task_flags;    // task->t.flags
        uint32_t ucode;         // task->t.ucode
        uint32_t data_ptr;      // task->t.data_ptr
        uint32_t data_size;     // task->t.data_size
        uint32_t output_buff;   // task->t.output_buff
        uint32_t output_buff_size;
        uint32_t wrapper_words[12];
        uint32_t task_words[16];
    };

    // Sized so the ring retains GFX history even after the game thread
    // freezes and the audio thread keeps pumping ~60Hz tasks for ~30s
    // (audio ~1800 events) before SEH dumps. 4096 = generous headroom.
    constexpr size_t RING_CAP = 4096;
    static Event ring[RING_CAP];
    static std::atomic<uint64_t> next_seq{0};
    static std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    static std::atomic<uint32_t> suspect_log_count{0};

    constexpr uint32_t RDRAM_SIZE = 8u * 1024u * 1024u;
    constexpr uint32_t SUSPECT_BAD_TASK_PTR = 1u << 0;
    constexpr uint32_t SUSPECT_BAD_TYPE     = 1u << 1;
    constexpr uint32_t SUSPECT_ZERO_UCODE   = 1u << 2;
    constexpr uint32_t SUSPECT_BAD_UCODE    = 1u << 3;
    constexpr uint32_t SUSPECT_BAD_DATA_PTR = 1u << 4;
    constexpr uint32_t SUSPECT_BAD_DATA_LEN = 1u << 5;
    constexpr uint32_t kMaxTaskDataSize = 0x100000u;

    static inline uint64_t now_ms() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    static inline bool rdram_range(uint32_t vaddr, uint32_t size) {
        uint32_t paddr = vaddr & 0x1FFFFFFFu;
        return paddr < RDRAM_SIZE && size <= RDRAM_SIZE - paddr;
    }

    static inline uint32_t read_rdram_word(uint8_t* rdram, uint32_t vaddr) {
        if (rdram == nullptr || !rdram_range(vaddr, sizeof(uint32_t))) {
            return 0;
        }
        uint32_t paddr = vaddr & 0x1FFFFFFFu;
        return *reinterpret_cast<const uint32_t*>(&rdram[paddr]);
    }

    static inline bool known_task_type(uint32_t type) {
        return type == M_GFXTASK || type == M_AUDTASK || type == M_VIDTASK ||
               type == M_NJPEGTASK || type == 6; // M_HVQMTASK
    }

    static inline bool task_data_shape_sane(const OSTask* task) {
        if (task->t.data_size == 0 || task->t.data_size > kMaxTaskDataSize) {
            return false;
        }
        if (task->t.data_ptr == 0 || !rdram_range(task->t.data_ptr, task->t.data_size)) {
            return false;
        }
        return true;
    }

    static inline bool task_shape_runnable(const OSTask* task) {
        if (task == nullptr || !known_task_type(task->t.type)) {
            return false;
        }

        const bool has_ucode =
            task->t.ucode != 0 && rdram_range(task->t.ucode, 1);
        const bool has_boot =
            task->t.ucode_boot != 0 &&
            task->t.ucode_boot_size != 0 &&
            rdram_range(task->t.ucode_boot, 1);
        if (!has_ucode && !has_boot) {
            return false;
        }

        // Booted custom RSP tasks can reuse the standard OSTask wrapper while
        // leaving fields like data_size in a ucode-specific shape. Let the RSP
        // dispatcher validate the boot signature and route the task.
        if (has_boot) {
            return true;
        }

        return task_data_shape_sane(task);
    }

    static inline bool task_audio_probe_safe(const OSTask* task) {
        return task != nullptr && task->t.ucode != 0 && task_data_shape_sane(task);
    }

    static inline void copy_words(uint8_t* rdram, uint32_t vaddr,
                                  uint32_t* out, size_t count) {
        for (size_t i = 0; i < count; i++) {
            out[i] = read_rdram_word(rdram, vaddr + uint32_t(i * 4));
        }
    }

    inline void record(uint8_t* rdram, const OSTask* task,
                       uint32_t task_ptr, uint32_t mips_ra) {
        const uint64_t s = next_seq.fetch_add(1, std::memory_order_relaxed);
        Event& e = ring[s % RING_CAP];
        std::memset(&e, 0, sizeof(e));
        e.seq = s;
        e.ms = now_ms();
        e.frame = pkmnstadium::dbg::g_frame_count.load(std::memory_order_relaxed);
        e.send_dl = pkmnstadium::dbg::g_send_dl_count.load(std::memory_order_relaxed);
        e.mips_ra = mips_ra;
        e.task_ptr = task_ptr;
        e.wrapper_ptr = task_ptr - 0x20u;
        e.task_type = task ? task->t.type : 0;
        e.task_flags = task ? task->t.flags : 0;
        e.ucode = task ? task->t.ucode : 0;
        e.data_ptr = task ? task->t.data_ptr : 0;
        e.data_size = task ? task->t.data_size : 0;
        e.output_buff = task ? task->t.output_buff : 0;
        e.output_buff_size = task ? task->t.output_buff_size : 0;

        if (!rdram_range(task_ptr, sizeof(OSTask))) {
            e.suspect |= SUSPECT_BAD_TASK_PTR;
        } else {
            copy_words(rdram, task_ptr, e.task_words, 16);
        }

        if (rdram_range(e.wrapper_ptr, sizeof(e.wrapper_words))) {
            copy_words(rdram, e.wrapper_ptr, e.wrapper_words, 12);
        }

        if (!known_task_type(e.task_type)) {
            e.suspect |= SUSPECT_BAD_TYPE;
        }
        if (e.ucode == 0) {
            e.suspect |= SUSPECT_ZERO_UCODE;
        } else if (!rdram_range(e.ucode, 1)) {
            e.suspect |= SUSPECT_BAD_UCODE;
        }
        if (e.data_size == 0 || e.data_size > kMaxTaskDataSize) {
            e.suspect |= SUSPECT_BAD_DATA_LEN;
        }
        uint32_t checked_size = e.data_size > kMaxTaskDataSize ? kMaxTaskDataSize : e.data_size;
        if (e.data_ptr == 0 || !rdram_range(e.data_ptr, checked_size)) {
            e.suspect |= SUSPECT_BAD_DATA_PTR;
        }

        if (e.suspect != 0) {
            uint32_t log_idx = suspect_log_count.fetch_add(1, std::memory_order_relaxed);
            if (log_idx < 32) {
                std::fprintf(stderr,
                    "[sp-task-suspect] seq=%llu suspect=0x%X task=0x%08X wrapper=0x%08X "
                    "ra=0x%08X type=%u flags=0x%X boot=0x%08X boot_size=0x%X "
                    "ucode=0x%08X data=0x%08X size=0x%X out=0x%08X out_size=0x%08X\n",
                    (unsigned long long)e.seq, e.suspect, e.task_ptr, e.wrapper_ptr,
                    e.mips_ra, e.task_type, e.task_flags,
                    task ? (uint32_t)task->t.ucode_boot : 0,
                    task ? task->t.ucode_boot_size : 0,
                    e.ucode, e.data_ptr,
                    e.data_size, e.output_buff, e.output_buff_size);
            }
        }
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
    uint32_t task_vaddr = uint32_t(ctx->r4);
    OSTask* task = sp_task_log::rdram_range(task_vaddr, sizeof(OSTask))
        ? TO_PTR(OSTask, ctx->r4)
        : nullptr;
    sp_task_log::record(rdram, task, task_vaddr, uint32_t(ctx->r31));
    if (task == nullptr) {
        std::fprintf(stderr,
            "[sp-task-invalid] refusing invalid OSTask pointer 0x%08X from ra=0x%08X\n",
            task_vaddr, uint32_t(ctx->r31));
        return;
    }
    if (!sp_task_log::task_shape_runnable(task)) {
        std::fprintf(stderr,
            "[sp-task-invalid] refusing malformed OSTask 0x%08X from ra=0x%08X "
            "type=%u flags=0x%X boot=0x%08X boot_size=0x%X ucode=0x%08X "
            "ucode_size=0x%X data=0x%08X size=0x%X\n",
            task_vaddr, uint32_t(ctx->r31), task->t.type, task->t.flags,
            (uint32_t)task->t.ucode_boot, task->t.ucode_boot_size,
            (uint32_t)task->t.ucode, task->t.ucode_size,
            (uint32_t)task->t.data_ptr, task->t.data_size);
        std::fflush(stderr);
        return;
    }

    // Always-on voice-event ring (music-rate click investigation):
    // sample the libnaudio voice list once per audio frame, on the
    // M_AUDTASK submit, and diff for key-on / key-off / sample-change.
    // Self-gates on PSR_DISABLE_VOICE_RING and on layout registration.
    // M_AUDTASK == 2 (libultra rsp task type).
    if (task && task->t.type == 2 && sp_task_log::task_audio_probe_safe(task)) {
        // data_ptr/data_size = the Acmd command list for this audio frame.
        librecomp_audio_voice_ring_sample(rdram, task->t.data_ptr,
                                          task->t.data_size);
    }
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
