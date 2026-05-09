// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with upstream authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Path A: persistent RSP GPRs across run_task calls (paired with
//     RSPRecomp emit changes in upstream-fork branch).
//   - Pre-task hook registry + DMA helper export.
//   - get_last_pc_trail API for live RSP hang inspection.
//   - Post-task DMEM dispatch-table integrity check + cmd dump.
//   - Always-on watchdog + PC trail at every label (paired with
//     RSPRecomp emit).
//   - Framework-level libultra ring + boot-sequence fixes.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <atomic>
#include <cassert>
#include <cstring>
#include <cinttypes>
#include <string>
#include <unordered_map>

#include "rsp.hpp"
#include "librecomp/ultra_trace.hpp"

// Last live RspContext pointer. Captured at every run_pre_task_hook
// call (which runs at the top of every recompiled ucode_func wrapper).
// While that ucode is running, its pc_trail / watchdog_count fields
// are being written by the recompiled code on the game thread; readers
// from other threads (e.g. TCP debug server) snapshot it racily and
// accept brief tearing — acceptable for diagnostics. Cleared back to
// nullptr is intentionally NOT done: when a ucode finishes (Broke or
// Watchdog), the last context's pc_trail still holds the most-recent
// values, which is exactly what get_last_pc_trail callers want.
namespace {
std::atomic<RspContext*> g_last_rsp_context{nullptr};
}

static recomp::rsp::callbacks_t rsp_callbacks {};

void recomp::rsp::set_callbacks(const callbacks_t& callbacks) {
    rsp_callbacks = callbacks;
}

namespace {
// Pre-task hook registry. Keyed by ucode name (== recompiler's
// output_function_name). Lookup happens once per task-run, so even
// a hash map is cheap; using std::unordered_map for clarity.
std::unordered_map<std::string, recomp::rsp::pre_task_hook_t*>& pre_task_hooks() {
    static std::unordered_map<std::string, recomp::rsp::pre_task_hook_t*> m;
    return m;
}
}

void recomp::rsp::set_pre_task_hook(const char* ucode_name,
                                    recomp::rsp::pre_task_hook_t* hook) {
    if (ucode_name == nullptr) return;
    if (hook == nullptr) {
        pre_task_hooks().erase(ucode_name);
    } else {
        pre_task_hooks()[ucode_name] = hook;
    }
}

void recomp::rsp::run_pre_task_hook(uint8_t* rdram, RspContext* ctx,
                                    const char* ucode_name,
                                    uint32_t ucode_addr) {
    // Capture the current ucode's RspContext pointer so external
    // observers can read pc_trail/watchdog_count via get_last_pc_trail()
    // even when no hook is registered for this ucode. release-store so
    // any subsequent acquire-load on g_last_rsp_context sees a fully
    // valid object.
    g_last_rsp_context.store(ctx, std::memory_order_release);

    if (ucode_name == nullptr) return;
    auto it = pre_task_hooks().find(ucode_name);
    if (it == pre_task_hooks().end()) return;
    it->second(rdram, ctx, ucode_name, ucode_addr);
}

bool recomp::rsp::get_last_pc_trail(PcTrailSnapshot* out) {
    if (out == nullptr) return false;
    *out = {};
    RspContext* ctx = g_last_rsp_context.load(std::memory_order_acquire);
    if (ctx == nullptr) {
        out->valid = false;
        return false;
    }
    // Racy reads — the recompiled ucode may be writing pc_trail and
    // watchdog_count concurrently on its game thread. Tearing within a
    // single uint32_t isn't possible on x86_64 for aligned 4-byte
    // accesses, so per-entry tearing won't occur. The trail as a whole
    // can be inconsistent across entries (a slightly older slot mixed
    // with a slightly newer one) but that's acceptable — callers
    // interpret pc_trail as a forensic trail, not a serialized record.
    for (int i = 0; i < 32; i++) out->entries[i] = ctx->pc_trail[i];
    out->idx            = ctx->pc_trail_idx;
    out->watchdog_count = ctx->watchdog_count;
    out->valid          = true;
    return true;
}

void recomp::rsp::dma_rdram_to_dmem_external(uint8_t* rdram,
                                             uint32_t dmem_addr,
                                             uint32_t dram_addr,
                                             uint32_t rd_len) {
    // Forward to the inline DMA helper from rsp.hpp. Exposed because
    // pre-task hooks live in game code that can't easily inline the
    // RSP_MEM_B byte-swap macro (it depends on the librecomp `dmem`
    // global being directly visible).
    dma_rdram_to_dmem(rdram, dmem_addr, dram_addr, rd_len);
}

uint8_t dmem[0x1000];
uint16_t rspReciprocals[512];
uint16_t rspInverseSquareRoots[512];

// From Ares emulator. For license details, see rsp_vu.h
void recomp::rsp::constants_init() {
    rspReciprocals[0] = u16(~0);
    for (u16 index = 1; index < 512; index++) {
        u64 a = index + 512;
        u64 b = (u64(1) << 34) / a;
        rspReciprocals[index] = u16((b + 1) >> 8);
    }

    for (u16 index = 0; index < 512; index++) {
        u64 a = (index + 512) >> ((index % 2 == 1) ? 1 : 0);
        u64 b = 1 << 17;
        //find the largest b where b < 1.0 / sqrt(a)
        while (a * (b + 1) * (b + 1) < (u64(1) << 44)) b++;
        rspInverseSquareRoots[index] = u16(b >> 1);
    }
}

// Runs a recompiled RSP microcode
bool recomp::rsp::run_task(uint8_t* rdram, const OSTask* task) {
    assert(rsp_callbacks.get_rsp_microcode != nullptr);
    RspUcodeFunc* ucode_func = rsp_callbacks.get_rsp_microcode(task);

    if (ucode_func == nullptr) {
        fprintf(stderr, "No registered RSP ucode for %" PRIu32 " (returned `nullptr`)\n", task->t.type);
        return false;
    }

    // Load the OSTask into DMEM
    memcpy(&dmem[0xFC0], task, sizeof(OSTask));

    // Load the ucode data into DMEM
    dma_rdram_to_dmem(rdram, 0x0000, task->t.ucode_data, 0xF80 - 1);

    // Record run_task entry into the libultra ring so probes can
    // attribute "RSP code went in, never came out" without needing
    // an extra trace channel. Args: task type, ucode vaddr,
    // ucode_data vaddr, data_ptr.
    recomp_runtime_set_rdram(rdram);
    recomp_ultra_trace_record(
        "rsp_run_task_enter", 0,
        (uint32_t)task->t.type,
        (uint32_t)task->t.ucode,
        (uint32_t)task->t.ucode_data,
        (uint32_t)task->t.data_ptr);

    // Run the ucode
    RspExitReason exit_reason = ucode_func(rdram, task->t.ucode);

    // Audio dispatch-table integrity check. After every audio task we
    // expect DMEM[0..0x1F] to still hold Stadium's aspMain dispatch
    // halfword table. Handlers writing to DMEM via mis-computed offsets
    // (e.g. an envelope/resampler write whose dest wraps past 0x1000)
    // will silently overwrite the table. Once overwritten, subsequent
    // cmd dispatches read garbage and route to wrong handlers, which
    // sounds like the post-title clicking. Catch the FIRST corrupting
    // task: dump its full cmd list to a file so we can simulate the
    // handlers offline and identify the culprit.
    {
        // M_AUDTASK = 2 (libultra OSTask::type for aspMain).
        if (task->t.type == 2) {
            static const uint16_t kExpectedTable[16] = {
                0x10EC, 0x139C, 0x119C, 0x1A64, 0x11C8, 0x17EC, 0x1208, 0x0000,
                0x0000, 0x127C, 0x1348, 0x1248, 0x1C84, 0x12D4, 0x02B0, 0x1384,
            };
            // RSP halfword reads use XOR-3 byte order: byte at addr
            // (idx & ~3) | ((idx & 3) ^ 3). For halfword at byte
            // offset 2*i, the high byte sits at (2*i)^3 in the
            // host-side dmem array, low at (2*i+1)^3.
            // Triage: skip slots 7 and 8. Empirically, L_1384 (handler
            // for opcode 0x0F) writes 4 bytes to DMEM[0x0E..0x11] via
            // `sw $1, 0xE($zero)` on every 0x0F cmd; this overlaps
            // dispatch slots 7 and 8 (which Stadium's table populates
            // with 0x0000). The chunk streams we've sampled do not
            // contain opcodes 0x07 or 0x08, so dispatch never reads
            // those slots and the write is asymptomatic. Whether that's
            // by design or an accident in the microcode is unverified
            // — we skip the slots here only to surface OTHER, unknown
            // corruption sources. If a path is ever observed sending
            // opcode 0x07 or 0x08, the L_1384 write becomes a real bug
            // and this skip should be removed.
            int first_bad_slot = -1;
            uint16_t first_bad_actual = 0;
            for (int i = 0; i < 16; i++) {
                if (i == 7 || i == 8) continue;
                uint8_t hi = dmem[(2*i)     ^ 3];
                uint8_t lo = dmem[(2*i + 1) ^ 3];
                uint16_t actual = (uint16_t)((hi << 8) | lo);
                if (actual != kExpectedTable[i]) {
                    first_bad_slot = i;
                    first_bad_actual = actual;
                    break;
                }
            }
            static bool s_dumped = false;
            if (first_bad_slot >= 0 && !s_dumped) {
                fprintf(stderr,
                    "[dispatch-corrupt] task corrupted dispatch table: "
                    "first_bad slot[%d] actual=0x%04X expected=0x%04X "
                    "data_ptr=0x%08X data_size=0x%X\n",
                    first_bad_slot, first_bad_actual,
                    kExpectedTable[first_bad_slot],
                    (uint32_t)task->t.data_ptr,
                    (uint32_t)task->t.data_size);
                // Also dump entire DMEM[0..0x1F] for context.
                fprintf(stderr, "[dispatch-corrupt] DMEM[0..0x1F]: ");
                for (int j = 0; j < 32; j++) {
                    fprintf(stderr, "%02X%s", dmem[j ^ 3], (j & 1) ? " " : "");
                }
                fprintf(stderr, "\n");
                // Save the cmd list to a file for offline simulation.
                FILE* f = fopen("audio_task_corrupt.bin", "wb");
                if (f) {
                    uint32_t paddr = (uint32_t)task->t.data_ptr & 0xFFFFFF;
                    uint32_t size  = (uint32_t)task->t.data_size;
                    if (paddr + size <= 0x800000) {
                        fwrite(rdram + paddr, 1, size, f);
                        fprintf(stderr,
                            "[dispatch-corrupt] cmd list (%u bytes) saved to "
                            "audio_task_corrupt.bin\n", size);
                    }
                    fclose(f);
                }
                fflush(stderr);
                s_dumped = true;  // only dump the first corrupting task
            }
        }
    }

    // Pair the entry record with an exit record. If exit doesn't
    // appear in the ring for a recorded entry, ucode_func hung.
    recomp_ultra_trace_record(
        "rsp_run_task_exit", (uint32_t)exit_reason,
        (uint32_t)task->t.type,
        (uint32_t)task->t.ucode,
        (uint32_t)task->t.ucode_data,
        (uint32_t)task->t.data_ptr);

    // Ensure that the ucode exited correctly
    if (exit_reason != RspExitReason::Broke) {
        if (exit_reason == RspExitReason::Watchdog) {
            // Hung ucode. The recompiler-emitted watchdog tripped
            // after ~100M basic-block transitions. The last 32
            // PCs visited are in dedicated thread-local context;
            // the runtime can't get to them without taking the
            // address of the impl wrapper's static, but we DO get
            // the entry args, so we route the diagnostic into the
            // libultra ring as a paired event the user can query.
            fprintf(stderr,
                "RSP ucode type=%" PRIu32 " ucode=0x%08" PRIX32
                " WATCHDOG TRIPPED — basic-block transitions exceeded "
                "threshold. ucode is hung. Inspect via "
                "tcp libultra_recent for `rsp_run_task_exit` "
                "(reason=%d) and `rsp_pc_trail_*` records below.\n",
                task->t.type, (uint32_t)task->t.ucode,
                static_cast<int>(exit_reason));
            // The pc_trail lives inside the per-ucode static
            // thread_local RspContext, which we can't reach from
            // this layer. Instead, a follow-up emit change pushes
            // the trail to the libultra ring at the watchdog trip
            // point itself (in process_instruction's emit). This
            // function just notes the exit so the entry/exit
            // pairing in the ring stays clean.
            return false;
        }
        fprintf(stderr, "RSP ucode %" PRIu32 " exited unexpectedly. exit_reason: %i\n", task->t.type, static_cast<int>(exit_reason));
        assert(exit_reason == RspExitReason::Broke);
        return false;
    }

    return true;
}
