#ifndef __RSP_H__
#define __RSP_H__

#include <cstdio>

#include "rsp_vu.hpp"
#include "recomp.h"
#include "ultramodern/ultra64.h"

// TODO: Move these to recomp namespace?

enum class RspExitReason {
    Invalid,
    Broke,
    ImemOverrun,
    UnhandledJumpTarget,
    Unsupported,
    SwapOverlay,
    UnhandledResumeTarget,
    // Watchdog tripped — the recompiled microcode executed more
    // basic-block transitions than `RSP_WATCHDOG_THRESHOLD` (set at
    // emit time, default ~100M, ~1.6s at native RSP speed). The
    // ucode is almost certainly stuck in an infinite loop. The
    // last 32 PCs visited are stored in `RspContext::pc_trail`,
    // newest at `(pc_trail_idx - 1) & 31`. Treat as a fatal exit
    // and dump diagnostic state.
    Watchdog
};

struct RspContext {
    uint32_t      r1,  r2,  r3,  r4,  r5,  r6,  r7,
             r8,  r9,  r10, r11, r12, r13, r14, r15,
             r16, r17, r18, r19, r20, r21, r22, r23,
             r24, r25, r26, r27, r28, r29, r30, r31;
    uint32_t dma_mem_address;
    uint32_t dma_dram_address;
    uint32_t jump_target;
    RSP rsp;
    uint32_t resume_address;
    bool resume_delay;
    // Watchdog state. Per-label emit increments watchdog_count and
    // appends current PC to pc_trail (ring of 32 entries). If
    // watchdog_count exceeds RSP_WATCHDOG_THRESHOLD, the ucode
    // returns RspExitReason::Watchdog. Per-task struct, so the
    // counter resets across run_task calls (a long-running task
    // legitimately can have many basic-block transitions).
    uint64_t watchdog_count;
    uint32_t pc_trail_idx;
    uint32_t pc_trail[32];
};

using RspUcodeFunc = RspExitReason(uint8_t* rdram, uint32_t ucode_addr);

extern uint8_t dmem[];
extern uint16_t rspReciprocals[512];
extern uint16_t rspInverseSquareRoots[512];

#define RSP_MEM_B(offset, addr) \
    (*reinterpret_cast<int8_t*>(dmem + (0xFFF & (((offset) + (addr)) ^ 3))))

#define RSP_MEM_BU(offset, addr) \
    (*reinterpret_cast<uint8_t*>(dmem + (0xFFF & (((offset) + (addr)) ^ 3))))

static inline uint32_t RSP_MEM_W_LOAD(uint32_t offset, uint32_t addr) {
    uint32_t out;
    for (int i = 0; i < 4; i++) {
        reinterpret_cast<uint8_t*>(&out)[i ^ 3] = RSP_MEM_BU(offset + i, addr);
    }
    return out;
}

static inline void RSP_MEM_W_STORE(uint32_t offset, uint32_t addr, uint32_t val) {
    for (int i = 0; i < 4; i++) {
        RSP_MEM_BU(offset + i, addr) = reinterpret_cast<uint8_t*>(&val)[i ^ 3];
    }
}

static inline uint32_t RSP_MEM_HU_LOAD(uint32_t offset, uint32_t addr) {
    uint16_t out;
    for (int i = 0; i < 2; i++) {
        reinterpret_cast<uint8_t*>(&out)[(i + 2) ^ 3] = RSP_MEM_BU(offset + i, addr);
    }
    return out;
}

static inline uint32_t RSP_MEM_H_LOAD(uint32_t offset, uint32_t addr) {
    int16_t out;
    for (int i = 0; i < 2; i++) {
        reinterpret_cast<uint8_t*>(&out)[(i + 2) ^ 3] = RSP_MEM_BU(offset + i, addr);
    }
    return out;
}

static inline void RSP_MEM_H_STORE(uint32_t offset, uint32_t addr, uint32_t val) {
    for (int i = 0; i < 2; i++) {
        RSP_MEM_BU(offset + i, addr) = reinterpret_cast<uint8_t*>(&val)[(i + 2) ^ 3];
    }
}

#define RSP_ADD32(a, b) \
    ((int32_t)((a) + (b)))

#define RSP_SUB32(a, b) \
    ((int32_t)((a) - (b)))

#define RSP_SIGNED(val) \
    ((int32_t)(val))

#define SET_DMA_MEM(mem_addr) dma_mem_address = (mem_addr)
#define SET_DMA_DRAM(dram_addr) dma_dram_address = (dram_addr)
#define DO_DMA_READ(rd_len) dma_rdram_to_dmem(rdram, dma_mem_address, dma_dram_address, (rd_len))
#define DO_DMA_WRITE(wr_len) dma_dmem_to_rdram(rdram, dma_mem_address, dma_dram_address, (wr_len))

static inline void dma_rdram_to_dmem(uint8_t* rdram, uint32_t dmem_addr, uint32_t dram_addr, uint32_t rd_len) {
    rd_len += 1; // Read length is inclusive
    dram_addr &= 0xFFFFF8;
    assert(dmem_addr + rd_len <= 0x1000);
    for (uint32_t i = 0; i < rd_len; i++) {
        RSP_MEM_B(i, dmem_addr) = MEM_B(0, (int64_t)(int32_t)(dram_addr + i + 0x80000000));
    }
}

static inline void dma_dmem_to_rdram(uint8_t* rdram, uint32_t dmem_addr, uint32_t dram_addr, uint32_t wr_len) {
    wr_len += 1; // Write length is inclusive
    dram_addr &= 0xFFFFF8;
    assert(dmem_addr + wr_len <= 0x1000);
    for (uint32_t i = 0; i < wr_len; i++) {
        MEM_B(0, (int64_t)(int32_t)(dram_addr + i + 0x80000000)) = RSP_MEM_B(i, dmem_addr);
    }
}

namespace recomp {
    namespace rsp {
        struct callbacks_t {
            using get_rsp_microcode_t = RspUcodeFunc*(const OSTask* task);

            /**
             * Return a function pointer to the corresponding RSP microcode function for the given `task_type`.
             *
             * The full OSTask (`task` parameter) is passed in case the `task_type` number is not enough information to distinguish out the exact microcode function.
             *
             * This function is allowed to return `nullptr` if no microcode matches the specified task. In this case a message will be printed to stderr and the program will exit.
             */
            get_rsp_microcode_t* get_rsp_microcode;
        };

        void set_callbacks(const callbacks_t& callbacks);

        void constants_init();

        bool run_task(uint8_t* rdram, const OSTask* task);

        /**
         * Pre-task hook signature. Called by the recompiled ucode legacy
         * wrapper after the OSTask has been loaded into DMEM[0xFC0..],
         * ucode_data has been DMA'd into DMEM[0..0xF7F], and persistent
         * RspContext is constructed — but BEFORE the ucode entry at
         * PC 0x1000 begins executing.
         *
         * This hook exists to replicate parts of rspboot's behavior that
         * the HLE doesn't simulate. On real hardware, rspboot does work
         * before the ucode entry that leaves specific GPRs and DMA-engine
         * registers in known states, plus may pre-load command data into
         * DMEM beyond ucode_data. Different ucodes depend on different
         * subsets of this prep; the hook lets each game register the
         * setup its specific ucode-version expects.
         *
         * Register one hook per ucode (keyed by ucode_name = the
         * recompiler's output_function_name). Pass nullptr to clear.
         *
         * Per the project's "framework-level" rule: this is the
         * recommended escape hatch when an ucode's static recompilation
         * happens to compile correctly but its boot path expects state
         * that only rspboot would have set on real hardware.
         */
        using pre_task_hook_t = void(uint8_t* rdram, RspContext* ctx,
                                     const char* ucode_name,
                                     uint32_t ucode_addr);

        /**
         * Register a pre-task hook keyed by ucode name. ucode_name must
         * match the recompiler's output_function_name. The pointer is
         * stored as-is; ensure storage outlives any future task run.
         */
        void set_pre_task_hook(const char* ucode_name, pre_task_hook_t* hook);

        /**
         * Called by the recompiled ucode wrapper. Looks up the
         * registered hook by name and invokes it if present. Returns
         * silently if no hook is registered — the typical case for
         * ucodes that don't depend on rspboot residue.
         */
        void run_pre_task_hook(uint8_t* rdram, RspContext* ctx,
                               const char* ucode_name,
                               uint32_t ucode_addr);

        /**
         * RDRAM to DMEM DMA helper exposed for use inside pre_task hooks.
         * Same semantics as the static inline `dma_rdram_to_dmem` above:
         * `rd_len` is the inclusive byte count - 1 (to match the RSP DMA
         * register encoding).
         */
        void dma_rdram_to_dmem_external(uint8_t* rdram, uint32_t dmem_addr,
                                        uint32_t dram_addr, uint32_t rd_len);

        /**
         * Snapshot of the watchdog state for the most-recently-launched
         * ucode. Populated by get_last_pc_trail(). Stable layout —
         * external diagnostic harnesses (e.g. tools/diff_aspmain.py)
         * deserialize this. `idx` indexes a ring of 32 PCs; the newest
         * is at `(idx - 1) & 31`. `valid` is false before any ucode
         * has been launched in this process.
         */
        struct PcTrailSnapshot {
            uint32_t entries[32];
            uint32_t idx;
            uint64_t watchdog_count;
            bool     valid;
        };

        /**
         * Read the live pc_trail of whichever ucode last started.
         * Returns true if `valid` is set in `*out`. The read is racy
         * with respect to the ucode thread's writes — callers should
         * treat the result as a forensic trail (per-entry coherent on
         * x86_64; whole-trail consistency not guaranteed).
         *
         * Survives ucode exit: when a ucode finishes (Broke or
         * Watchdog), the snapshot still reflects the last-written
         * pc_trail values, which is the useful state for hang
         * post-mortems.
         */
        bool get_last_pc_trail(PcTrailSnapshot* out);
    }
}

#endif
