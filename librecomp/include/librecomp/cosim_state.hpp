#ifndef LIBRECOMP_COSIM_STATE_HPP
#define LIBRECOMP_COSIM_STATE_HPP

// Per-VI-frame differential co-simulation: full-state snapshot hashing.
// See PokemonStadiumRecomp/COSIM.md (Tier 0, tasks T1/T3/T4).
//
// This module hashes the "both-sides" architectural state surface — the state
// that BOTH the recomp and the Ares oracle hold — so a per-frame checkpoint can
// detect (and localize) the first divergence. It is pure/read-only: it never
// mutates guest state.

#include <cstdint>

struct recomp_context;  // defined in recomp.h

namespace recomp::cosim {

// Per-subsystem sub-hashes over the both-sides state surface (COSIM.md §2).
// Splitting by subsystem lets a halt say WHICH subsystem diverged, not just
// THAT something did.
struct SubHashes {
    uint64_t cpu_int;  // r0..r31, hi, lo
    uint64_t cp0;      // cop0_regs[32], status_reg, mips3_float_mode
    uint64_t cp1;      // f0..f31 (the fpr union, hashed on its u64 lane)
    uint64_t rdram;    // the full guest RDRAM (real 8 MB, not the 1 GB window)
};

// One checkpoint row (COSIM.md §6 protocol; §5 is the modeled-cycle axis).
struct Checkpoint {
    uint64_t cp;           // checkpoint index
    uint64_t vis;          // total_vis (VI count) at this quiescent boundary
    uint64_t cycle;        // modeled guest-cycle clock
    uint64_t cpu_retired;  // approximate retired CPU-instruction counter
    SubHashes sub;
    uint64_t chain;        // cumulative order-sensitive chain hash through this cp
};

// Hash the both-sides state surface.
//   ctx        : the recomp CPU/coprocessor context.
//   rdram      : host base pointer to guest RDRAM.
//   rdram_size : the REAL RDRAM size (8 MB); NOT the 1 GB mapped window.
// Host-only fields (f_odd, tailcall_*, host_return_target, dispatch_entry_*)
// and PC are excluded by construction — see COSIM.md §2.
SubHashes hash_state(const recomp_context* ctx, const uint8_t* rdram, uint32_t rdram_size);

// Fold sub-hashes (+ VI count + modeled cycle) into the running chain hash.
// prev_chain is the previous checkpoint's chain (0 at reset). Order-sensitive:
// the same sub-hashes reached via a different history produce a different chain.
uint64_t chain_advance(uint64_t prev_chain, const SubHashes& sub, uint64_t vis, uint64_t cycle);

}  // namespace recomp::cosim

#endif  // LIBRECOMP_COSIM_STATE_HPP
