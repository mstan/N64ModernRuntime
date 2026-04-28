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
