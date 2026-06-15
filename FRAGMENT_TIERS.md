# FRAGMENT_TIERS.md — runtime fragment-discovery tier architecture (n64)

Status: **design + in-progress.** The n64 counterpart to psxrecomp's `SLJIT.md`.
The *spine* (interpreter floor, content-keyed multi-candidate dispatch, portable
coverage manifest, non-comingled caches, release fold-back) is shared with PSX —
see `psxrecomp/SLJIT.md` §2/§4. This doc records only the n64-specific shape and
plan.

"Fragment" = the n64 term for a DMA-loaded / decompressed code region at a runtime
address (PSX calls these "overlays"; same concept).

---

## 0. Why this exists

PokemonStadium2Recomp boots, inits, and reaches the first frame, then **aborts on
one dispatch miss** (`0x801451A0`): an indirectly-reached *fragment-interior*
entry that the static recompiler never discovered. Today the runtime tiers are:

```
B1 capture → B2 self-heal (interior RETURNS only) → B3 sljit JIT (resident-static
ONLY) → loud abort
```

A fragment code-**entry** is not an interior return (B2 misses it) and not in a
resident static section (B3's allowlist excludes it — that allowlist exists
because address-keyed JIT *crashed* on reused fragment arenas). So it aborts.
That violates the project's hard requirement #1: **a miss must be safe, not
fatal.**

---

## 1. n64 is the mirror image of psxrecomp

| | psxrecomp | n64recomp |
|---|---|---|
| self-contained JIT (sljit) | ❌ net-new codegen | ✅ **exists** (B3 / `LiveRecomp` / `LiveGenerator`) |
| generator abstraction | ❌ | ✅ **exists** (`N64Recomp/include/recompiler/generator.h`: `CGenerator` + `LiveGenerator`) |
| optimized "dev" backend | ✅ spawn-gcc→DLL | ◻️ **build-time static recompile** (N64Recomp→C→clang) = the natural optimized tier; a *runtime* spawn-clang→DLL dev path is cheap to add (C emitter exists) but not the long pole |
| interpreter floor (#1) | ✅ dirty-RAM interp | ❌ **MISSING — aborts** ← the gap |
| cache separation (#3) | ⚠️ flat | ❌ none yet (B4 unbuilt) |

So n64 already has the hard part psxrecomp lacks (the in-process JIT + generator
abstraction). What n64 is missing is the **interpreter floor** and **content-keyed
fragment dispatch + cache separation**.

---

## 2. Target architecture (shared spine)

```
fragment dispatch miss
  → interpreter (FLOOR: always correct, never fatal)         [#1, NEW]
  → meanwhile/after: content-keyed JIT compile (sljit, B3)    [#2, exists; extend to fragments]
      registered as a content-hash candidate, multi-candidate per address,
      live-byte validated per dispatch, write-invalidated on overwrite
  → persist coverage manifest mid-session                     [#3, NEW: B4 groundwork]
  → release: fold accumulated coverage into the static build  [optimized tier, Track C]
```

Backend selection mirrors `SLJIT.md` §1: sljit is the self-contained default;
the optimized path is the static fold-back (and optionally a runtime
spawn-clang→DLL dev backend later). Interpreter is always the floor.

---

## 3. Cache layout — NO COMINGLING (same rule as SLJIT.md §4)

```
build/coverage/            # neutral, portable, contributable currency
                           #   (evolve runtime_captures.json + the force_function_vrams fold manifest)
build/jit/<arch>-<abi>/    # optional sljit blob cache (v2; v1 re-JITs from coverage — sub-ms)
generated/                 # the static fold-back C = the optimized/shipped tier
```

`coverage/` is the only shared/contributed thing; `jit/` is per-arch derived;
`generated/` is the optimized shipped form. Never crossed. (n64's blob-persistence
caveat is identical to SLJIT.md §5.3: prefer re-JIT-from-manifest over persisting
JIT bytes, to avoid the per-process relocation/symbol-rebind problem.)

---

## 4. The content-keying fix for fragment address reuse

B3's resident-static allowlist (`addr_in_resident_static_section`) exists because
JIT keyed by *address* crashed on reused fragment arenas (a relocated fragment's
address is reused by different content over time). psxrecomp's solved answer
(SLJIT.md §2, design `overlay-recompilation-design.md` §8) is the same one to
adopt here:

- key compiled fragment code by **content hash**, not address;
- keep a **candidate chain per runtime address** (the same address legitimately
  hosts different fragment content over a session);
- **re-validate the live code bytes on every dispatch** (`cand_crc`-style) and run
  the candidate that matches;
- **write-invalidate** a candidate when its code pages are overwritten (fragment
  reload / self-mod).

With that, B3 can be made fragment-eligible *safely* — the allowlist is replaced
by content validation, not by exclusion.

---

## 5. Priority order (dependency-first)

1. **Interpreter floor (#1)** — a fragment lookup-miss interprets from that PC
   instead of aborting. *Backend-independent; unblocks the `0x801451A0` abort
   today.* ← **in progress.**
2. **Cache namespacing + mid-session manifest persistence (#3).**
3. **Content-keyed, fragment-eligible sljit (#2 extension)** — replace B3's
   allowlist with content-hash + multi-candidate + live-byte validation.
4. **Release fold-back (Track C)** — re-run N64Recomp on accumulated coverage for
   the optimized shipped baseline (already partially present via
   `force_function_vrams`).
5. *(optional)* runtime spawn-clang→DLL dev backend + selection policy, mirroring
   `SLJIT.md` §1.

---

## 6. Interpreter (step 1) — design

A small R4300i interpreter in librecomp, invoked from the lookup-miss trampoline
(`librecomp/src/overlays.cpp`) in place of the abort:

- **Entry:** `void recomp_interpret_from(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc)`.
  Interprets from `start_pc` until control returns to the caller (a `jr $ra`
  whose target equals the host return target) — then returns out, and the
  recompiled caller resumes natively. This is the **native↔interp call contract**
  (psxrecomp learned this the hard way as "Bug A"): the boundary must preserve the
  caller's return continuation exactly.
- **Native boundary:** when interpretation hits a `jal`/`jalr`/`j` whose target IS
  a known recompiled function (`func_map` hit), **call the native function**
  (don't interpret into it) — fast and avoids re-interpreting compiled code. Only
  genuinely-unknown targets are interpreted.
- **Coverage capture:** every PC interpreted is recorded into the coverage
  manifest (executed-PC = the strongest discovery signal, per
  `overlay-recompilation-design.md` §2.3), so the fragment can later be JIT'd /
  folded back. This is how "interpret once → never interpret again" is realized.
- **Opcode coverage:** integer ALU, loads/stores (via the same MEM_* path),
  branches + delay slots, jumps, mult/div + hi/lo, and **COP1/FPU** (Stadium is a
  3D game — floats are on the hot path, not optional). COP0 / TLB / cache are
  HLE-vestigial no-ops (same policy as `recomp_unhandled_instruction`).
- **Precision-over-recall:** the interpreter is the *safety floor*, so an
  unhandled opcode must fail **loudly** (not silently mis-execute) — it's a real
  gap to fill, surfaced via the post-mortem.

---

## 7. Open questions / risks

- **Native↔interp call contract** correctness (returns, delay slots, tail calls).
  Mirror the discipline in `overlay-recompilation-design.md` §9.
- **Interpreter completeness** — FPU + 64-bit ops must be right; a wrong result is
  silent corruption. Validate against native where a function is dual-available.
- **Perf** — the interpreter is the slow floor; the point is *correctness first*,
  then promote hot interpreted PCs to JIT (B3) and eventually fold back. A
  fragment should interpret only until it's compiled.
- Everything in `SLJIT.md` §9 about content-keying, blob persistence, and arch
  coverage applies symmetrically.
