// Per-VI-frame differential co-simulation: full-state snapshot hashing.
// See PokemonStadiumRecomp/COSIM.md (Tier 0). Pure/read-only over guest state.

#include "librecomp/cosim_state.hpp"

#include "recomp.h"

// XXH_INLINE_ALL: emit xxHash as TU-local static-inline, so this file gets its
// own copy regardless of how other TUs (e.g. librecomp/src/recomp.cpp) include
// xxHash — no duplicate-symbol risk at link, and self-contained for the
// standalone self-test below.
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

// Layout assumptions the hash relies on (COSIM.md §2). If recomp_context
// changes shape, these fire at COMPILE time rather than silently corrupting
// the compared state surface.
static_assert(sizeof(gpr) == 8, "cpu_int hash assumes 8-byte GPRs");
static_assert(sizeof(fpr) == 8, "cp1 hash assumes 8-byte FPRs");

namespace recomp::cosim {

SubHashes hash_state(const recomp_context* ctx, const uint8_t* rdram, uint32_t rdram_size) {
    SubHashes h{};

    // cpu_int: r0..r31 (32 contiguous same-type GPR members), then hi, lo.
    {
        XXH3_state_t s;
        XXH3_64bits_reset(&s);
        XXH3_64bits_update(&s, &ctx->r0, 32 * sizeof(gpr));
        XXH3_64bits_update(&s, &ctx->hi, sizeof(ctx->hi));
        XXH3_64bits_update(&s, &ctx->lo, sizeof(ctx->lo));
        h.cpu_int = XXH3_64bits_digest(&s);
    }

    // cp1: f0..f31 — the fpr union. Hash the raw 8-byte lane so both the
    // single- and double-precision interpretations are covered.
    h.cp1 = XXH3_64bits(&ctx->f0, 32 * sizeof(fpr));

    // cp0: cop0_regs[32], plus status_reg and mips3_float_mode (which live
    // outside the array — see recomp.h). Blind spot: the FP rounding mode is
    // held in the HOST fenv (get/set_cop1_cs), not status_reg, so a rounding
    // divergence is invisible here — documented, not silently dropped.
    {
        XXH3_state_t s;
        XXH3_64bits_reset(&s);
        XXH3_64bits_update(&s, ctx->cop0_regs, sizeof(ctx->cop0_regs));
        XXH3_64bits_update(&s, &ctx->status_reg, sizeof(ctx->status_reg));
        XXH3_64bits_update(&s, &ctx->mips3_float_mode, sizeof(ctx->mips3_float_mode));
        h.cp0 = XXH3_64bits_digest(&s);
    }

    // rdram: the full guest RDRAM. Correctness first (full hash); the
    // dirty-page incremental optimization (COSIM.md §3) lands only after
    // profiling shows this dominates.
    h.rdram = (rdram && rdram_size) ? XXH3_64bits(rdram, rdram_size) : 0;

    return h;
}

uint64_t chain_advance(uint64_t prev_chain, const SubHashes& sub, uint64_t vis, uint64_t cycle) {
    // Order-sensitive: seed with the previous chain so the same sub-hashes
    // reached via a different history yield a different chain hash.
    struct {
        uint64_t prev, cpu_int, cp0, cp1, rdram, vis, cycle;
    } blob{ prev_chain, sub.cpu_int, sub.cp0, sub.cp1, sub.rdram, vis, cycle };
    return XXH3_64bits_withSeed(&blob, sizeof(blob), prev_chain);
}

}  // namespace recomp::cosim

#ifdef COSIM_STATE_SELFTEST
// Standalone proof (COSIM.md T1). Build + run:
//   clang++ -std=c++20 -DCOSIM_STATE_SELFTEST \
//     -I <N64Recomp>/include -I <N64MR>/thirdparty -I <N64MR>/librecomp/include \
//     cosim_state.cpp -o cosim_selftest && ./cosim_selftest
// Verifies: determinism, per-subsystem sensitivity, host-only exclusion, and
// chain order-sensitivity — all against the REAL recomp_context layout.
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fails; } \
    else         { std::printf("ok:   %s\n", (msg)); } \
} while (0)

int main() {
    using namespace recomp::cosim;

    recomp_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    std::vector<uint8_t> rd(64 * 1024, 0);
    const uint32_t sz = static_cast<uint32_t>(rd.size());

    const SubHashes base  = hash_state(&ctx, rd.data(), sz);
    const SubHashes base2 = hash_state(&ctx, rd.data(), sz);
    CHECK(std::memcmp(&base, &base2, sizeof(base)) == 0, "deterministic: same state -> same hash");

    // GPR write -> only cpu_int.
    ctx.r5 = 0xDEADBEEFull;
    SubHashes hg = hash_state(&ctx, rd.data(), sz);
    CHECK(hg.cpu_int != base.cpu_int, "GPR write changes cpu_int");
    CHECK(hg.cp0 == base.cp0 && hg.cp1 == base.cp1 && hg.rdram == base.rdram,
          "GPR write does NOT touch cp0/cp1/rdram");

    // FPR write -> only cp1.
    ctx.f10.u64 = 0x1122334455667788ull;
    SubHashes hf = hash_state(&ctx, rd.data(), sz);
    CHECK(hf.cp1 != hg.cp1, "FPR write changes cp1");
    CHECK(hf.cpu_int == hg.cpu_int && hf.cp0 == hg.cp0, "FPR write does NOT touch cpu_int/cp0");

    // COP0 write -> only cp0.
    ctx.cop0_regs[9] = 0x55;  // Count
    SubHashes hc = hash_state(&ctx, rd.data(), sz);
    CHECK(hc.cp0 != hf.cp0, "cop0 write changes cp0");
    CHECK(hc.cpu_int == hf.cpu_int && hc.cp1 == hf.cp1, "cop0 write does NOT touch cpu_int/cp1");

    // Host-only fields -> NOTHING moves (they are excluded by construction).
    ctx.tailcall_target = 0x80001234;
    ctx.tailcall_pending = 1;
    ctx.host_return_target = 0x80005678;
    ctx.dispatch_entry_rejected = 1;
    ctx.f_odd = reinterpret_cast<uint32_t*>(0x1234);
    SubHashes hh = hash_state(&ctx, rd.data(), sz);
    CHECK(std::memcmp(&hh, &hc, sizeof(hh)) == 0,
          "host-only fields (tailcall_*/host_return/dispatch/f_odd) are EXCLUDED");

    // RDRAM write -> only rdram.
    rd[1000] = 0xAB;
    SubHashes hr = hash_state(&ctx, rd.data(), sz);
    CHECK(hr.rdram != hh.rdram, "RDRAM write changes rdram sub-hash");
    CHECK(hr.cpu_int == hh.cpu_int && hr.cp0 == hh.cp0 && hr.cp1 == hh.cp1,
          "RDRAM write does NOT touch registers");

    // Chain is order-sensitive.
    const uint64_t c1  = chain_advance(0,  base, 1, 0);
    const uint64_t c2  = chain_advance(c1, hr,   2, 0);
    const uint64_t c2b = chain_advance(0,  hr,   2, 0);
    CHECK(c2 != c2b, "chain is history-dependent (order-sensitive)");

    // Modeled-cycle axis participates in the chain (reserved but wired).
    const uint64_t cyc0 = chain_advance(c1, hr, 2, 0);
    const uint64_t cyc1 = chain_advance(c1, hr, 2, 100);
    CHECK(cyc0 != cyc1, "modeled cycle_count participates in the chain hash");

    std::printf("\n%s (%d failure%s)\n",
                g_fails ? "SELFTEST FAILED" : "SELFTEST PASSED",
                g_fails, g_fails == 1 ? "" : "s");
    return g_fails ? 1 : 0;
}
#endif  // COSIM_STATE_SELFTEST
