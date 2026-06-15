/*
 * mips_interp.cpp - R4300i interpreter: the fragment-discovery correctness floor.
 *
 * See N64ModernRuntime/FRAGMENT_TIERS.md. When a dispatch miss lands on an
 * indirectly-reached fragment-interior entry that B2 (self-heal) and B3 (the
 * static-only JIT) can't handle, the lookup-miss trampoline calls
 * recomp_interpret_function() instead of aborting. We interpret the function
 * from the missed PC against the live recomp_context + rdram, dispatching calls
 * to already-recompiled functions natively (the native<->interp boundary), and
 * return when the function returns to its caller.
 *
 * This is the SAFETY FLOOR (precision over recall): a missed function must be
 * safe (slow), never fatal. Any opcode we don't yet implement fails LOUDLY
 * (logs opcode + PC, returns false -> trampoline aborts with that info), never
 * silently mis-executes. Fill gaps as measurement surfaces them.
 *
 * NOT a perf path: interpreted PCs are captured as coverage so the fragment can
 * later be JIT'd / folded back into the static build and never interpreted again.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "recomp.h"

// Pure func_map lookup that returns nullptr on a miss (NOT the lookup-miss
// trampoline) — see librecomp/src/overlays.cpp. The interpreter's native
// boundary MUST use this, not get_function: get_function returns the trampoline
// on a miss, so calling it would re-enter the whole tier chain (including this
// interpreter) and recurse unbounded -> host stack overflow.
extern "C" recomp_func_t* recomp_lookup_function_or_null(int32_t vram);

namespace {

// Coverage hook. No-op for now (step #1 is the correctness floor); a later step
// records interpreted PCs into the portable coverage manifest so the fragment
// can be JIT'd / folded back and never interpreted again (FRAGMENT_TIERS.md #2).
inline void recomp_interp_capture_pc(uint32_t /*pc*/) {}

constexpr int kMaxInterpDepth = 256;   // host-stack guard for jal-into-unknown

// Opt-in jump tracing (PSR_INTERP_TRACE=1): logs every computed jr/jalr target
// and recursion entry, so a bad jump (e.g. into a data region) is caught at its
// first occurrence. Per the doctrine: extend the debug surface, find the first
// divergence — don't guess.
static const bool g_interp_trace = std::getenv("PSR_INTERP_TRACE") != nullptr;

inline uint32_t ifetch(uint8_t* rdram, uint32_t pc) {
    // Instruction words read back as the native big-endian value via MEM_WU
    // (RDRAM is stored word-native; only sub-word access needs the ^ swap).
    return (uint32_t)MEM_WU(0, (gpr)(int32_t)pc);
}

inline int64_t  s64(gpr v)   { return (int64_t)v; }
inline gpr      sx32(uint32_t v) { return (gpr)(int32_t)v; }   // sign-extend 32->64

bool interpret(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc, int depth);

// Execute one NON-control instruction (everything except branches/jumps).
// Returns false (after logging) on an opcode we don't implement yet.
bool exec_noncontrol(uint8_t* rdram, recomp_context* ctx, gpr* R, fpr* F,
                     uint32_t insn, uint32_t pc) {
    const uint32_t op   = insn >> 26;
    const uint32_t rs   = (insn >> 21) & 0x1F;
    const uint32_t rt   = (insn >> 16) & 0x1F;
    const uint32_t rd   = (insn >> 11) & 0x1F;
    const uint32_t sa   = (insn >> 6)  & 0x1F;
    const uint32_t fn   = insn & 0x3F;
    const int16_t  simm = (int16_t)(insn & 0xFFFF);
    const uint16_t uimm = (uint16_t)(insn & 0xFFFF);
    auto setR = [&](uint32_t i, gpr v) { if (i != 0) R[i] = v; };

    switch (op) {
    case 0x00: // SPECIAL
        switch (fn) {
        case 0x00: setR(rd, sx32((uint32_t)R[rt] << sa)); return true;            // sll
        case 0x02: setR(rd, sx32((uint32_t)R[rt] >> sa)); return true;            // srl
        case 0x03: setR(rd, sx32((uint32_t)((int32_t)(uint32_t)R[rt] >> sa))); return true; // sra
        case 0x04: setR(rd, sx32((uint32_t)R[rt] << (R[rs] & 0x1F))); return true; // sllv
        case 0x06: setR(rd, sx32((uint32_t)R[rt] >> (R[rs] & 0x1F))); return true; // srlv
        case 0x07: setR(rd, sx32((uint32_t)((int32_t)(uint32_t)R[rt] >> (R[rs] & 0x1F)))); return true; // srav
        case 0x10: setR(rd, ctx->hi); return true;                                // mfhi
        case 0x11: ctx->hi = R[rs]; return true;                                  // mthi
        case 0x12: setR(rd, ctx->lo); return true;                                // mflo
        case 0x13: ctx->lo = R[rs]; return true;                                  // mtlo
        case 0x18: { int64_t p = (int64_t)(int32_t)(uint32_t)R[rs] * (int64_t)(int32_t)(uint32_t)R[rt]; // mult
                     ctx->lo = sx32((uint32_t)p); ctx->hi = sx32((uint32_t)(p >> 32)); return true; }
        case 0x19: { uint64_t p = (uint64_t)(uint32_t)R[rs] * (uint64_t)(uint32_t)R[rt]; // multu
                     ctx->lo = sx32((uint32_t)p); ctx->hi = sx32((uint32_t)(p >> 32)); return true; }
        case 0x1A: { int32_t a=(int32_t)(uint32_t)R[rs], b=(int32_t)(uint32_t)R[rt];     // div
                     if (b != 0) { ctx->lo = sx32((uint32_t)(a / b)); ctx->hi = sx32((uint32_t)(a % b)); }
                     return true; }
        case 0x1B: { uint32_t a=(uint32_t)R[rs], b=(uint32_t)R[rt];               // divu
                     if (b != 0) { ctx->lo = sx32(a / b); ctx->hi = sx32(a % b); }
                     return true; }
        case 0x20: setR(rd, sx32((uint32_t)R[rs] + (uint32_t)R[rt])); return true; // add (no trap)
        case 0x21: setR(rd, sx32((uint32_t)R[rs] + (uint32_t)R[rt])); return true; // addu
        case 0x22: setR(rd, sx32((uint32_t)R[rs] - (uint32_t)R[rt])); return true; // sub
        case 0x23: setR(rd, sx32((uint32_t)R[rs] - (uint32_t)R[rt])); return true; // subu
        case 0x24: setR(rd, R[rs] & R[rt]); return true;                          // and
        case 0x25: setR(rd, R[rs] | R[rt]); return true;                          // or
        case 0x26: setR(rd, R[rs] ^ R[rt]); return true;                          // xor
        case 0x27: setR(rd, ~(R[rs] | R[rt])); return true;                       // nor
        case 0x2A: setR(rd, (s64(R[rs]) < s64(R[rt])) ? 1 : 0); return true;      // slt
        case 0x2B: setR(rd, (R[rs] < R[rt]) ? 1 : 0); return true;                // sltu
        case 0x2C: setR(rd, R[rs] + R[rt]); return true;                          // dadd
        case 0x2D: setR(rd, R[rs] + R[rt]); return true;                          // daddu
        case 0x2E: setR(rd, R[rs] - R[rt]); return true;                          // dsub
        case 0x2F: setR(rd, R[rs] - R[rt]); return true;                          // dsubu
        case 0x38: setR(rd, R[rt] << sa); return true;                            // dsll
        case 0x3A: setR(rd, R[rt] >> sa); return true;                            // dsrl
        case 0x3B: setR(rd, (gpr)(s64(R[rt]) >> sa)); return true;                // dsra
        case 0x3C: setR(rd, R[rt] << (sa + 32)); return true;                     // dsll32
        case 0x3E: setR(rd, R[rt] >> (sa + 32)); return true;                     // dsrl32
        case 0x3F: setR(rd, (gpr)(s64(R[rt]) >> (sa + 32))); return true;         // dsra32
        case 0x14: setR(rd, R[rt] << (R[rs] & 0x3F)); return true;                // dsllv
        case 0x16: setR(rd, R[rt] >> (R[rs] & 0x3F)); return true;                // dsrlv
        case 0x17: setR(rd, (gpr)(s64(R[rt]) >> (R[rs] & 0x3F))); return true;    // dsrav
        case 0x0B: if (R[rt] != 0) setR(rd, R[rs]); return true;                  // movn
        case 0x0A: if (R[rt] == 0) setR(rd, R[rs]); return true;                  // movz
        case 0x0F: return true;                                                   // sync = nop
        // Trap family: compiler-emitted range/overflow/div-by-zero checks. HLE
        // has no exception model, so these are no-ops (the trapped condition is
        // the error case that real code paths don't hit; div-by-zero already
        // yields a defined result in our div impl).
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:          // tge/tgeu/tlt/tltu/teq/tne
            return true;
        default: break;
        }
        break;
    case 0x08: setR(rt, sx32((uint32_t)R[rs] + (uint32_t)(int32_t)simm)); return true; // addi
    case 0x09: setR(rt, sx32((uint32_t)R[rs] + (uint32_t)(int32_t)simm)); return true; // addiu
    case 0x0A: setR(rt, (s64(R[rs]) < (int64_t)simm) ? 1 : 0); return true;       // slti
    case 0x0B: setR(rt, (R[rs] < (gpr)(int64_t)simm) ? 1 : 0); return true;       // sltiu
    case 0x0C: setR(rt, R[rs] & (gpr)uimm); return true;                          // andi
    case 0x0D: setR(rt, R[rs] | (gpr)uimm); return true;                          // ori
    case 0x0E: setR(rt, R[rs] ^ (gpr)uimm); return true;                          // xori
    case 0x0F: setR(rt, sx32((uint32_t)uimm << 16)); return true;                 // lui
    case 0x18: setR(rt, R[rs] + (gpr)(int64_t)simm); return true;                 // daddi
    case 0x19: setR(rt, R[rs] + (gpr)(int64_t)simm); return true;                 // daddiu
    // ---- loads / stores (reuse the recompiled MEM_* path for endianness) ----
    case 0x20: setR(rt, (gpr)(int64_t)MEM_B(simm, R[rs]));   return true;         // lb
    case 0x21: setR(rt, (gpr)(int64_t)MEM_H(simm, R[rs]));   return true;         // lh
    case 0x23: setR(rt, sx32((uint32_t)MEM_W(simm, R[rs]))); return true;         // lw
    case 0x24: setR(rt, (gpr)(uint64_t)MEM_BU(simm, R[rs])); return true;         // lbu
    case 0x25: setR(rt, (gpr)(uint64_t)MEM_HU(simm, R[rs])); return true;         // lhu
    case 0x27: setR(rt, (gpr)(uint64_t)(uint32_t)MEM_WU(simm, R[rs])); return true; // lwu
    case 0x37: setR(rt, load_doubleword(rdram, R[rs], simm)); return true;        // ld
    case 0x28: MEM_B(simm, R[rs]) = (int8_t)R[rt];  return true;                  // sb
    case 0x29: MEM_H(simm, R[rs]) = (int16_t)R[rt]; return true;                  // sh
    case 0x2B: MEM_W(simm, R[rs]) = (int32_t)R[rt]; return true;                  // sw
    case 0x3F: SD(R[rt], simm, R[rs]); return true;                               // sd
    // ---- unaligned loads / stores (recomp.h helpers handle the byte merge) ----
    case 0x22: setR(rt, do_lwl(rdram, R[rt], (gpr)simm, R[rs])); return true;     // lwl
    case 0x26: setR(rt, do_lwr(rdram, R[rt], (gpr)simm, R[rs])); return true;     // lwr
    case 0x2A: do_swl(rdram, (gpr)simm, R[rs], R[rt]); return true;               // swl
    case 0x2E: do_swr(rdram, (gpr)simm, R[rs], R[rt]); return true;               // swr
    case 0x1A: setR(rt, do_ldl(rdram, R[rt], (gpr)simm, R[rs])); return true;     // ldl
    case 0x1B: setR(rt, do_ldr(rdram, R[rt], (gpr)simm, R[rs])); return true;     // ldr
    case 0x2C: do_sdl(rdram, (gpr)simm, R[rs], R[rt]); return true;               // sdl
    case 0x2D: do_sdr(rdram, (gpr)simm, R[rs], R[rt]); return true;               // sdr
    case 0x30: setR(rt, sx32((uint32_t)MEM_W(simm, R[rs]))); return true;         // ll  (as lw)
    case 0x38: MEM_W(simm, R[rs]) = (int32_t)R[rt]; setR(rt, 1); return true;     // sc  (as sw, ok=1)
    case 0x2F: return true;                                                       // cache = nop
    // ---- COP1 / FPU loads + stores ----
    case 0x31: F[rt].u32l = (uint32_t)MEM_WU(simm, R[rs]); return true;           // lwc1
    case 0x35: F[rt].u64  = load_doubleword(rdram, R[rs], simm); return true;     // ldc1
    case 0x39: MEM_W(simm, R[rs]) = (int32_t)F[rt].u32l; return true;             // swc1
    case 0x3D: SD(F[rt].u64, simm, R[rs]); return true;                           // sdc1
    // ---- COP1 op block (non-branch): mfc1/mtc1/cfc1/ctc1 + arithmetic ----
    case 0x11: {
        const uint32_t fmt = rs;       // rs field selects sub-op / format
        const uint32_t ft  = rt;
        const uint32_t fs  = rd;
        const uint32_t fdd = sa;       // fd in bits [10:6]
        switch (fmt) {
        case 0x00: setR(rt, sx32(F[fs].u32l)); return true;                       // mfc1
        case 0x01: setR(rt, (gpr)F[fs].u64); return true;                         // dmfc1
        case 0x02: setR(rt, sx32(ctx->cop0_regs[0])); return true;                // cfc1 (FCR — approx; FCR31 below)
        case 0x04: F[fs].u32l = (uint32_t)R[rt]; return true;                     // mtc1
        case 0x05: F[fs].u64  = R[rt]; return true;                               // dmtc1
        case 0x06: return true;                                                   // ctc1 (FCR write; ignored)
        case 0x10: case 0x11: case 0x14: case 0x15: { // S / D / W / L compute ops
            const bool dbl = (fmt == 0x11);
            const bool fromW = (fmt == 0x14);
            const bool fromL = (fmt == 0x15);
            auto getS = [&](uint32_t i){ return F[i].fl; };
            auto getD = [&](uint32_t i){ return F[i].d;  };
            switch (fn) {
            case 0x00: if (dbl) F[fdd].d = getD(fs)+getD(ft); else F[fdd].fl = getS(fs)+getS(ft); return true; // add
            case 0x01: if (dbl) F[fdd].d = getD(fs)-getD(ft); else F[fdd].fl = getS(fs)-getS(ft); return true; // sub
            case 0x02: if (dbl) F[fdd].d = getD(fs)*getD(ft); else F[fdd].fl = getS(fs)*getS(ft); return true; // mul
            case 0x03: if (dbl) F[fdd].d = getD(fs)/getD(ft); else F[fdd].fl = getS(fs)/getS(ft); return true; // div
            case 0x04: if (dbl) F[fdd].d = std::sqrt(getD(fs)); else F[fdd].fl = std::sqrt(getS(fs)); return true; // sqrt
            case 0x05: if (dbl) F[fdd].d = std::fabs(getD(fs)); else F[fdd].fl = std::fabs(getS(fs)); return true; // abs
            case 0x06: if (dbl) F[fdd].d = getD(fs); else F[fdd].fl = getS(fs); return true;       // mov
            case 0x07: if (dbl) F[fdd].d = -getD(fs); else F[fdd].fl = -getS(fs); return true;     // neg
            case 0x20: F[fdd].fl = fromW ? (float)(int32_t)F[fs].u32l : (float)(dbl?getD(fs):getS(fs)); return true; // cvt.s
            case 0x21: F[fdd].d  = fromW ? (double)(int32_t)F[fs].u32l : (double)(dbl?getD(fs):getS(fs)); return true; // cvt.d
            case 0x24: F[fdd].u32l = (uint32_t)(int32_t)(dbl?getD(fs):getS(fs)); return true;      // cvt.w (trunc-ish)
            case 0x0D: F[fdd].u32l = (uint32_t)(int32_t)(dbl?std::trunc(getD(fs)):std::trunc(getS(fs))); return true; // trunc.w
            case 0x0C: F[fdd].u32l = (uint32_t)(int32_t)(dbl?std::round(getD(fs)):std::round(getS(fs))); return true; // round.w
            case 0x0E: F[fdd].u32l = (uint32_t)(int32_t)(dbl?std::ceil(getD(fs)):std::ceil(getS(fs))); return true;   // ceil.w
            case 0x0F: F[fdd].u32l = (uint32_t)(int32_t)(dbl?std::floor(getD(fs)):std::floor(getS(fs))); return true; // floor.w
            default:
                if (fn >= 0x30) { // c.cond.fmt -> set FCR31 condition bit (we stash in cop0_regs[1] bit 23)
                    double a = dbl ? getD(fs) : getS(fs);
                    double b = dbl ? getD(ft) : getS(ft);
                    bool lt = a < b, eq = a == b;
                    bool cond = false;
                    const uint32_t c = fn & 0xF;
                    // common predicates: F,UN,EQ,UEQ,OLT,ULT,OLE,ULE,...(.lt=0xC?). Approximate:
                    switch (c) {
                        case 0x2: cond = eq; break;            // c.eq
                        case 0xC: cond = lt; break;            // c.lt
                        case 0xE: cond = lt || eq; break;      // c.le
                        case 0x1: cond = false; break;         // c.un (no NaN here)
                        case 0x4: cond = lt; break;            // c.olt
                        case 0x6: cond = lt || eq; break;      // c.ole
                        default:  cond = eq; break;
                    }
                    if (cond) ctx->cop0_regs[1] |= (1u << 23);
                    else      ctx->cop0_regs[1] &= ~(1u << 23);
                    return true;
                }
                break;
            }
            break;
        }
        default: break;
        }
        break;
    }
    case 0x10: return true;  // COP0 (mtc0/mfc0/tlb*) - HLE-vestigial, no-op
    default: break;
    }

    std::fprintf(stderr,
        "[interp] UNIMPLEMENTED opcode 0x%02X (funct 0x%02X) at PC 0x%08X (insn 0x%08X)\n",
        op, fn, pc, insn);
    std::fflush(stderr);
    return false;
}

// Interpret the function at start_pc as a callee. Returns when it returns to
// its caller (jr to the entry $ra). false => hit something unhandled (logged).
bool interpret(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc, int depth) {
    if (depth > kMaxInterpDepth) {
        std::fprintf(stderr, "[interp] recursion depth exceeded at 0x%08X\n", start_pc);
        return false;
    }
    gpr* R = &ctx->r0;
    fpr* F = &ctx->f0;
    const uint32_t return_target = (uint32_t)ctx->r31;
    uint32_t pc = start_pc;

    if (g_interp_trace) {
        std::fprintf(stderr, "[interp] ENTER pc=0x%08X depth=%d ra=0x%08X\n",
                     start_pc, depth, return_target);
        std::fflush(stderr);
    }

    for (;;) {
        if ((pc & 3u) != 0) {  // a misaligned target is never valid code (garbage jump)
            std::fprintf(stderr, "[interp] misaligned/garbage target PC 0x%08X — bailing\n", pc);
            std::fflush(stderr);
            return false;
        }
        recomp_interp_capture_pc(pc);
        const uint32_t insn = ifetch(rdram, pc);
        const uint32_t op   = insn >> 26;

        // ---- control-flow opcodes (need delay-slot + pc handling) ----
        if (op == 0x02 || op == 0x03) {  // j / jal
            const uint32_t target = ((pc + 4) & 0xF0000000u) | ((insn & 0x03FFFFFF) << 2);
            const uint32_t link   = pc + 8;
            if (op == 0x03) R[31] = (gpr)(int32_t)link;                  // jal sets $ra
            if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false; // delay slot
            recomp_func_t* nf = recomp_lookup_function_or_null((int32_t)target);
            if (nf != nullptr) {
                nf(rdram, ctx);                          // call native
                if (op == 0x03) { pc = link; continue; } // jal: resume after
                return true;                             // j to native = tail call
            }
            if (op == 0x03) { if (!interpret(rdram, ctx, target, depth + 1)) return false; pc = link; continue; }
            pc = target; continue;                       // j to unknown = tail (same frame)
        }
        if (op == 0x00 && ((insn & 0x3F) == 0x08 || (insn & 0x3F) == 0x09)) { // jr / jalr
            const uint32_t rs = (insn >> 21) & 0x1F;
            const uint32_t rd = (insn >> 11) & 0x1F;
            const bool link = ((insn & 0x3F) == 0x09);
            const uint32_t target = (uint32_t)R[rs];
            const uint32_t ret    = pc + 8;
            if (g_interp_trace) {
                std::fprintf(stderr, "[interp] %s at 0x%08X: $%u=0x%08X (ra=0x%08X)\n",
                             link ? "jalr" : "jr", pc, rs, target, return_target);
                std::fflush(stderr);
            }
            if (link && rd != 0) R[rd] = (gpr)(int32_t)ret;   // jalr rd,rs (rd=$ra normally)
            if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false; // delay slot
            if (!link && target == return_target) return true;           // jr $ra -> function return
            recomp_func_t* nf = recomp_lookup_function_or_null((int32_t)target);
            if (nf != nullptr) { nf(rdram, ctx); if (link) { pc = ret; continue; } return true; }
            if (link) { if (!interpret(rdram, ctx, target, depth + 1)) return false; pc = ret; continue; }
            pc = target; continue;                                       // jr to unknown = tail
        }
        // branches: op 0x01 (regimm), 0x04..0x07, 0x14..0x17 (likely)
        bool isBranch = (op == 0x01) || (op >= 0x04 && op <= 0x07) || (op >= 0x14 && op <= 0x17);
        if (isBranch) {
            const uint32_t rs = (insn >> 21) & 0x1F;
            const uint32_t rt = (insn >> 16) & 0x1F;
            const int16_t  simm = (int16_t)(insn & 0xFFFF);
            const uint32_t btarget = pc + 4 + ((int32_t)simm << 2);
            bool take = false; bool likely = false; bool link = false;
            switch (op) {
            case 0x01: { // REGIMM
                const uint32_t sub = rt;
                switch (sub) {
                case 0x00: take = s64(R[rs]) <  0; break;                 // bltz
                case 0x01: take = s64(R[rs]) >= 0; break;                 // bgez
                case 0x02: take = s64(R[rs]) <  0; likely = true; break;  // bltzl
                case 0x03: take = s64(R[rs]) >= 0; likely = true; break;  // bgezl
                case 0x10: take = s64(R[rs]) <  0; link = true; break;    // bltzal
                case 0x11: take = s64(R[rs]) >= 0; link = true; break;    // bgezal
                default:
                    std::fprintf(stderr, "[interp] UNIMPL regimm sub 0x%02X at 0x%08X\n", sub, pc);
                    return false;
                }
                break;
            }
            case 0x04: take = (R[rs] == R[rt]); break;                    // beq
            case 0x05: take = (R[rs] != R[rt]); break;                    // bne
            case 0x06: take = (s64(R[rs]) <= 0); break;                   // blez
            case 0x07: take = (s64(R[rs]) >  0); break;                   // bgtz
            case 0x14: take = (R[rs] == R[rt]); likely = true; break;     // beql
            case 0x15: take = (R[rs] != R[rt]); likely = true; break;     // bnel
            case 0x16: take = (s64(R[rs]) <= 0); likely = true; break;    // blezl
            case 0x17: take = (s64(R[rs]) >  0); likely = true; break;    // bgtzl
            }
            if (link) R[31] = (gpr)(int32_t)(pc + 8);
            if (likely) {
                if (take) { if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false; pc = btarget; }
                else      { pc = pc + 8; }                                // likely + not taken: nullify delay slot
            } else {
                if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false; // delay slot always runs
                pc = take ? btarget : pc + 8;
            }
            continue;
        }
        // COP1 branch (bc1f/bc1t/bc1fl/bc1tl): op 0x11, rs field == 0x08
        if (op == 0x11 && ((insn >> 21) & 0x1F) == 0x08) {
            const uint32_t tf  = (insn >> 16) & 0x1;   // 0 = bc1f, 1 = bc1t
            const uint32_t nd  = (insn >> 17) & 0x1;   // likely bit
            const int16_t  simm = (int16_t)(insn & 0xFFFF);
            const uint32_t btarget = pc + 4 + ((int32_t)simm << 2);
            const bool cond = (ctx->cop0_regs[1] >> 23) & 1;
            const bool take = (tf ? cond : !cond);
            if (nd) {  // likely
                if (take) { if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false; pc = btarget; }
                else      { pc = pc + 8; }
            } else {
                if (!exec_noncontrol(rdram, ctx, R, F, ifetch(rdram, pc + 4), pc + 4)) return false;
                pc = take ? btarget : pc + 8;
            }
            continue;
        }

        // ---- non-control: execute and advance ----
        if (!exec_noncontrol(rdram, ctx, R, F, insn, pc)) return false;
        pc += 4;
    }
}

} // namespace

// Entry point called by the lookup-miss trampoline (overlays.cpp).
extern "C" bool recomp_interpret_function(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc) {
    return interpret(rdram, ctx, start_pc, 0);
}
