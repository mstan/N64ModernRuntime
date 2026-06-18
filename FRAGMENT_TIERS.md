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
                           #   runtime_captures.json     (live session diagnostic + miss coverage)
                           #   fragment_manifest.json    (content-keyed candidate currency; B4)
                           #   (later: the force_function_vrams fold manifest)
build/jit/<arch>-<abi>/    # RESERVED for a future v2 sljit blob cache. EMPTY in v1
                           #   (a README.txt explains why); v1 re-JITs from the
                           #   coverage manifest instead — sub-ms.
generated/                 # the static fold-back C = the optimized/shipped tier
```

`coverage/` is the only shared/contributed thing; `jit/` is per-arch derived;
`generated/` is the optimized shipped form. Never crossed. (n64's blob-persistence
caveat is identical to SLJIT.md §5.3: prefer re-JIT-from-manifest over persisting
JIT bytes, to avoid the per-process relocation/symbol-rebind problem.)

**Implemented 2026-06-18** (`librecomp/src/overlays.cpp`): `runtime_captures.json`
moved into `coverage/` (readers `tools/ps2_drive.py` + `_verify_frag.ps1` updated,
with read-fallback to the legacy `build/` and cwd paths). Host arch-abi tag
(`N64_FRAG_ARCH_ABI`, e.g. `x86_64-win64`) + `N64_FRAG_CODEGEN_VER` namespace the
derived cache. `cache_subdir()` (`std::filesystem::create_directories`) materializes
each tree on demand.

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

1. ✅ **Interpreter floor (#1)** — a fragment lookup-miss interprets from that PC
   instead of aborting. *Backend-independent; unblocks the `0x801451A0` abort
   today.*
2. ✅ **Cache namespacing + mid-session manifest persistence (#3).** [§3, §9]
3. ✅ **Content-keyed, fragment-eligible sljit (#2 extension)** — replace B3's
   allowlist with content-hash + multi-candidate + live-byte validation +
   shadow-diff gate. [slice 3, §8.7; gated `PSR_FRAG_NATIVE`, OFF by default]
4. ◻️ **Release fold-back (Track C)** — re-run N64Recomp on accumulated coverage
   for the optimized shipped baseline (already partially present via
   `force_function_vrams`).
5. ◻️ *(optional)* runtime spawn-clang→DLL dev backend + selection policy,
   mirroring `SLJIT.md` §1.

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

## 8. Content-keyed fragment dispatch — concrete implementation plan (2026-06-17)

Grounded in the actual `librecomp/src/overlays.cpp` dispatch code. The current
lookup-miss flow (`unhandled_lookup_trampoline`, ~line 2542) is:

```
get_function(addr) miss → trampoline:
  tier 2  self-heal      find_resident_enclosing_function → dispatch interior   (resident recompiled fns)
  tier 3  B3 JIT         addr_in_resident_static_section ONLY → jit + func_map   (resident-static gaps)
  (evict-test restore)
  tier 4  interpreter    recomp_interpret_function(addr)                         (FLOOR — fragments land here)
  → loud abort
```

Fragments fall to **tier 4 (interpreter) forever** — correct but slow. Goal:
let B3 JIT fragments **safely**, replacing `addr_in_resident_static_section`'s
exclusion with psxrecomp's content-keyed model (§4). This is the n64 analogue of
`overlay_loader.c`'s `Candidate` chain + `cand_crc` + `run_shadow_diff`.

### 8.1 Why fragments can't just go in `func_map`

n64 dispatch is `func_map[addr]` → direct call (no per-call validation). B3
today *registers* its JIT in `func_map[vram]` (line 2383) → permanent direct
fast path. That's **address-keyed** and unsafe for fragments: the overlay arena
address is **reused by different content** over a session, so a func baked for
content A runs when content B is loaded. (This is exactly why
`addr_in_resident_static_section` excludes them — "JITing one produces wrong code
and crashed.")

**Fix:** fragment JITs are NOT put in `func_map`. They live in a separate
candidate registry; fragment addresses therefore keep lookup-missing → re-enter
the trampoline → the new content-keyed tier validates per call. (Same as psx:
overlays never sit in a direct map; every dispatch goes through the validator.)

### 8.2 New data structures (`overlays.cpp`)

```cpp
struct FragmentCandidate {
    uint32_t  addr;          // runtime entry (overlay-arena vaddr)
    uint64_t  content_hash;  // XXH3 of the live code bytes at [code_lo,code_lo+code_len)
    uint32_t  code_lo;       // = addr (paddr of the JIT'd body)
    uint32_t  code_len;      // discovered func_size
    recomp_func_t* fn;       // JIT output (storage kept alive in g_jit_entries)
    uint32_t  diff_passes;   // consecutive clean differentials vs interpreter
    bool      blacklisted;   // self-mod / repeatedly-divergent → never run native
};
// chain per runtime address (the same addr legitimately hosts different content)
static std::unordered_map<uint32_t, std::vector<FragmentCandidate>> g_frag_cands; // func_map_mutex
```

### 8.3 New tier in the trampoline (between tier 3 and tier 4), gated OFF by default

`PSR_FRAG_JIT=1` arms it; unset → today's behavior (fragments interpret). Engages
only when `!addr_in_resident_static_section(addr)` AND `addr` is inside a
currently-loaded fragment section (so the bytes are valid and the range is known):

```
content-keyed fragment tier (addr = g_self_heal_addr):
  1. discover_function_bounds(live rdram @ addr) → func_size           (reuse jit_compile_inner's walk)
  2. h = XXH3(live code [addr, addr+func_size))
  3. walk g_frag_cands[addr]:
       for cand: if XXH3(cand range) == cand.content_hash == h:        (live-byte revalidation, cand_crc analogue)
            if cand.blacklisted: skip
            if cand.diff_passes >= BUDGET: cand.fn(rdram, ctx); return  (trusted → native)
            else: run DIFFERENTIAL (8.4); return
  4. no live match → JIT a fresh candidate (jit_compile_function, keep_and_register=FALSE so it
       does NOT touch func_map), push {h, addr, func_size, fn, diff_passes=0}; run DIFFERENTIAL; return
  (on any JIT failure → fall through to tier 4 interpreter, unchanged)
```

So `func_map` is never mutated for fragments; the fast path is "validated
candidate → native," everything else is interpreter.

### 8.4 The validation gate (the crux — differential vs the interpreter floor)

Fragment-JIT correctness is **not proven** (unlike resident-static B3, which is
32/32 verified). So a fresh candidate must pass the same-state differential vs
`mips_interp` (the oracle, already present) before it runs live — precision over
recall. **Fork RESOLVED 2026-06-17: port the proven psx `run_shadow_diff`
verbatim** (`overlay_loader.c:1282`; it ran 0-divergence over hundreds of shadow
calls and fixed the one open crash). It is option (A) but *simpler* — it snapshots
the **whole** working RAM, not touched-ranges:

```
run_shadow_diff(ctx, cand, addr):                         # n64 port
  snapshot: ctx0 = *ctx; copy ALL rdram (8 MiB) → ram0
  PASS 1 — INTERPRETER FIRST (authoritative), device-detector armed:
       recomp_interpret_function(rdram, ctx, addr)
       if it touched device/HLE state (n64's "device touch", see below):
           cand.device_touch = 1                          # pin to interpreter forever
           keep interp result live; ABANDON native pass; return   # never double-exec HLE
  device-free → save interp result: ctxI = *ctx; ram → ramI
  restore: *ctx = ctx0; ram0 → rdram
  PASS 2 — NATIVE shard from identical input: cand.fn(rdram, ctx)
  save native result: ctxN = *ctx; rdram → ramN
  compare ctxN vs ctxI (gprs/fprs/hi-lo) + memcmp(ramN, ramI):
      clean    → cand.diff_passes++  (cap at BUDGET)
      diverge  → cand.diff_passes = 0  (+record first divergence detail)
  COMMIT interp result: *ctx = ctxI; ramI → rdram        # native always discarded here
```

Promotion = BUDGET **consecutive** clean passes (divergence resets to 0, so an
intermittently-wrong shard never accumulates trust). Until promoted, the
**interpreter result is the one committed**, so an un-validated/wrong candidate
never affects the game. Per-function isolation: during the diff, nested fragment
calls run via the interpreter on BOTH passes (n64 equivalent of psx's
`s_native_exec=0`) so the diff isolates THIS shard's codegen, not the call tree.

**n64's "device touch" = the HLE boundary.** psx watches MMIO; n64's analogue is
the native↔interp boundary calling an HLE `osXxx` (scheduler/message-queue/gfx-
submit/save-file side effects that live OUTSIDE rdram and can't be snapshot-
restored). Detect it the same way: arm a counter that bumps whenever the
interpreter's native-call boundary (`recomp_lookup_function_or_null` hit) invokes
a function flagged as HLE/side-effecting during PASS 1; if it bumped, set
`device_touch` and pin the candidate to the interpreter. Pure-compute fragments
(geometry/math — the JIT-worthy majority) don't trip it; HLE-touching ones stay
on the floor, safe by construction. (Cost: PASS 1+2 each snapshot/restore 8 MiB
— heavy, but only on the rare not-yet-promoted path; a promoted candidate runs
native directly with just the per-call XXH3 revalidation.)

### 8.5 Write-invalidation (self-mod / fragment reload)

Fragments are DMA-reloaded into the arena. The existing
`Memmap_ClearFragmentMemmap` / `register_runtime_fragment` / eviction path
(`overlays.cpp` ~1191 "EVICT") is the hook: when a fragment section is
cleared/reloaded, drop `g_frag_cands` entries whose `[code_lo,code_len)` overlaps
the reloaded range. Per-call live-byte revalidation (8.3 step 3) is the backstop
— a stale candidate simply fails its hash and is skipped.

### 8.6 Rollout (additive, gated, measured — psx §7 mirror)

1. ✅ Data structures + content hash + candidate registry (additive, dead until wired). [slice 1]
2. ✅ Wire the tier behind `PSR_FRAG_JIT`, candidate path runs the **interpreter**
   result live (no native yet) — proves discovery/keying/invalidation with zero
   risk. [slice 1-2; + B4 manifest persistence, §9]
3. ✅ **Add the differential gate (8.4); native runs only after BUDGET clean
   passes. [slice 3, 2026-06-18 — see §8.7.]**
4. ◻️ Measure on Stadium 2's fragment workload (it's fragment-dominated): count
   candidates, diff passes, divergences, native promotions vs interp.
5. ◻️ Re-verify PMS + Stadium 1 (engine change → fork branches) before default-on.

Default-on is gated on step 4 showing 0 divergences over a substantial run +
user sign-off — same bar psx used.

**FLIPPED DEFAULT-ON 2026-06-18 (user-directed).** `frag_jit_enabled()` and
`frag_native_enabled()` now default ON; `PSR_FRAG_JIT=0` / `PSR_FRAG_NATIVE=0`
are debug kill-switches. Validated across all three games with no env vars set:
Stadium 2 discovers + content-keys 2 candidates and pins both (non-leaf), 0
divergences, manifest persisted, self-test 8/8; PMS (frame 11249) and Stadium 1
(frame 1299) boot clean with 0 misses (benign — full static coverage). Safe by
construction: the shadow-diff commits the interpreter result until BUDGET clean
passes, only safe-leaf candidates are eligible, device-touchers are pinned.
CAVEAT: no REAL game fragment has been promoted+run native yet (Stadium 2's are
non-leaf; the siblings have 0 misses), so the live-native path is proven only by
the synthetic self-test leaf — the shadow-diff guards the first real one.

### 8.7 Slice 3 as built (2026-06-18) — safe-leaf v1

A second sub-gate `PSR_FRAG_NATIVE=1` (requires `PSR_FRAG_JIT`) arms native
execution; with `PSR_FRAG_JIT` alone the tier is unchanged (slice 1-2 bookkeeping
+ interpreter floor). When armed:

- A new candidate is JIT'd via `jit_compile_function(keep=true,
  register_in_map=false)` — a new JIT mode that keeps the shard alive in
  `g_jit_entries` but NEVER puts it in `func_map` (the address-keyed fast path is
  exactly what's unsafe for reused fragment arenas). The shard is reached only
  through the content-keyed, per-dispatch-validated tier.
- **Safe-leaf eligibility** (`fragment_is_safe_leaf`): a candidate is JIT/native-
  eligible only if its body has NO outgoing control transfer — no `jal`/`jalr`
  (call), no `j` outside its own bounds (tail call), no computed `jr` (only
  `jr $ra` = return). Such a function is a pure register+RAM transformation on
  **all** inputs, so the diff fully captures it and a promoted shard can never
  reach an unvalidated, side-effecting path. Non-leaf → pinned to interp
  (`device_touch`), never JIT'd. Precision over recall.
- `run_shadow_diff` ports psx verbatim but snapshots/compares only the 8 MiB
  kseg0 RAM region (`rdram[0,0x800000)`) + GPR/FPR/hi-lo — sound because every
  interpreter memory access (incl. MMIO) indexes the one mapped block
  (`RECOMP_MEM_MASK`), there is no device handler in the mem path, and the only
  non-restorable side effect is a native CALL. **Device-touch = any native call
  during PASS 1**, detected via the `recomp_shadow_diff_active` /
  `recomp_shadow_diff_note_native_call` hooks at `mips_interp.cpp`'s two
  `recomp_lookup_function_or_null → nf` sites (a dynamic backstop to the static
  leaf check). The whole tier is skipped while `t_shadow_active` (no nested diff).
- Promotion = `PSR_FRAG_DIFF_BUDGET` (default 8) **consecutive** clean passes;
  any divergence resets to 0. Until promoted the interpreter result is committed
  (native discarded), so an un-promoted/wrong shard never affects the game.
- Counters in `runtime_captures.json`: `frag_diff_clean` / `frag_diff_diverge` /
  `frag_device_touch` / `frag_promoted` / `frag_native_runs`.

**Known v1 limits** (broaden later, not blockers): only leaf fragments promote
(non-leaf — most fragments that call helpers — stay on the interpreter, correct
but unaccelerated); the 8 MiB snapshot is heavy but only on the rare un-promoted
path. Broadening to non-leaf needs the psx `s_native_exec=0` model (route nested
calls through the interpreter on both passes) + an HLE side-effect choke-point
detector — deferred.

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

## 9. B4 disk persistence — re-JIT-from-manifest, NOT a blob cache (2026-06-18)

**Decision (settles task #3 / §5 step 2): n64 persists a coverage manifest and
re-JITs from it; it does NOT persist native JIT bytes.** psxrecomp's
`persist_sljit_shard`/`scan_sljit_cache_dir` serialize sljit's
position-independent *LIR* and regenerate native code per-process at load. The
n64 `LiveRecomp` `LiveGenerator` does the opposite: `live_generator.cpp:150`
calls `sljit_generate_code` immediately and produces **final, position-dependent
machine code** with this-process pointers baked in — function entries
(`sljit_get_label_addr`), jump-table targets, reference/import symbol jumps
resolved by `sljit_set_jump_addr` to live host functions, string-literal heap
addresses, and the `executable_offset`. Persisting those bytes and reloading them
in a new process is a stale-pointer crash — exactly the relocation/symbol-rebind
problem §3/§5.3 warns about. A verbatim psx byte-blob port is therefore *not
viable* on n64; it is the broken option, not a shortcut.

**What was built** (`librecomp/src/overlays.cpp`, gated `PSR_FRAG_JIT`, sub-switch
`PSR_FRAG_CACHE=0` to disable just persistence):

- `persist_fragment_manifest_locked()` rewrites `coverage/fragment_manifest.json`
  from the in-memory `g_frag_cands` whenever a new candidate is keyed (cheap,
  rewrite-on-new — same model as `write_runtime_captures_locked`). Each record is
  arch-independent: `{addr, content_hash, code_lo, code_len}`. Stamped with
  `format_ver` / `codegen_ver` / `arch_abi`.
- `load_fragment_manifest()` (called from `init_overlays()` under its
  `FuncMapWriteLock`) reloads the manifest into `g_frag_cands` as `fn=null`
  candidates. A `format/codegen/arch` mismatch invalidates the whole file. The
  existing per-dispatch live-byte revalidation (`have_live_match`) re-keys each
  reloaded entry against live RAM before it can ever be used, so a reloaded
  candidate is exactly as safe as a freshly discovered one — and `fn` is always
  re-JIT'd, never trusted from disk.
- `ensure_jit_cache_reserved()` creates `jit/<arch-abi>/README.txt` documenting
  why the v2 blob slot is intentionally empty.
- Counters `frag_reloaded` / `frag_persisted` surface in the
  `runtime_captures.json` coverage block.

**Value before native exec (slice 3) lands:** the coverage currency accumulates
across sessions for Track C static fold-back, and warm-starts the validation
budget once native promotion exists. It is pure additive bookkeeping today —
execution still falls to the interpreter floor regardless.

**If a future v2 blob cache is ever wanted**, it must serialize the sljit LIR
(requires exposing the pre-`generate_code` compiler in `LiveGenerator` + a
deserialize-time rebind pass for the baked host pointers), keyed under
`jit/<arch-abi>/cg<N>/`. That is a large N64Recomp change with no clear win over
sub-ms re-JIT — deferred, possibly indefinitely.
