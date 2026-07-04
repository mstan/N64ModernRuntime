# N64 Modern Runtime

> **Fork notice** — This is a fork maintained as part of **SS Anne**, a
> [Pokémon Stadium recompilation project](https://github.com/mstan/PokemonStadiumRecomp).
> The changes here exist to support that port; they may lag behind
> upstream and are not intended as a replacement for the canonical
> project. For canonical N64ModernRuntime, see
> [N64Recomp/N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).

## Changes in this fork

N64ModernRuntime stands in for the N64's operating system while the
recompiled game runs on PC. These changes make it host Pokémon Stadium
correctly:

- **Fixes the menu cursor and icons turning to garbage**, by correctly
  tracking where the game's load-on-demand code sits in memory as it loads
  and unloads.
- **Stops the music and sound effects clicking and ticking**, by letting
  the game set its own audio pace the way it does on the real console, and
  hardens the sound code against a class of crashes.
- **Makes saving work** — registered Pokémon are kept between runs (the
  game writes to emulated flash memory).
- **Fixes several freezes and softlocks**, including ones reaching certain
  menus and Game Boy Tower, by letting busy game loops step aside instead
  of locking up, and by handing the graphics/audio co-processor's work to
  the right place.
- **Stops out-of-range memory reads from crashing the game** — bad reads
  return zeroes and get logged instead.
- **Lets the game detect the Transfer Pak** accessory being plugged in.
- **Live, hot-reloadable English translation** (consumer-implemented on this
  runtime's trace + guest-memory surface). A Japanese-only game is rendered in
  English at runtime with no ROM edits or asset re-packing, and the translation
  table is reloaded **on the fly** — edit a string in the JSON and it updates in
  the running game within a frame, no rebuild or restart. Coverage spans menus,
  battle and overlay text (it hooks the game's universal text formatter, not
  just one draw routine), and each line auto-fits the original Japanese footprint
  so longer English never overflows the text boxes. See
  [Runtime text-translation hook](#runtime-text-translation-hook).
- **Always-on diagnostic rings + a guest-memory access surface.** The fork
  records libultra calls, message-queue and scheduler events, SP/graphics
  tasks and (with `trace_mode`) per-function entries into always-on ring
  buffers, and exposes the guest RDRAM base to host code. These are the
  diagnostic backbone used to bring the port up — and the same surface a
  consumer can build runtime features on. See
  [Runtime text-translation hook](#runtime-text-translation-hook) for a
  worked example (the Pocket Monsters Stadium English patch).
- **Smaller things:** a fix for a rare crash when two threads touch the
  function-lookup table at once; points its recompiler dependency at this
  project's fork; records which files were modified.

## Runtime text-translation hook

A consumer of this runtime —
[PocketMonstersStadiumRecomp](https://github.com/mstan/PocketMonstersStadiumRecomp)
— uses the infrastructure here to render a Japanese-only game in **English at
runtime**, with no ROM edits or asset re-packing. It is a useful reference for
what the diagnostic/trace surface enables, so the mechanism is documented here.

**The surface this relies on:**

- **`trace_mode` per-function entry hook.** When the recompiler
  ([N64Recomp fork](https://github.com/mstan/N64Recomp)) is built with
  `trace_mode = true`, it emits a `TRACE_ENTRY()` at the top of every
  recompiled function, where the function's `ctx` (registers) and `rdram`
  (guest memory) are in scope. The consumer defines that macro, so it is a
  zero-cost extension point into every guest function call.
- **`recomp_runtime_get_rdram()`** + the always-on rings (see
  `ultramodern/ultra_trace.hpp`) let host code read guest memory and observe
  execution without arming a one-shot trace — recording is continuous from
  process start, so probes query a window rather than capture-then-hope.

**How the translation works, end to end:**

1. **Find the text routines, empirically.** A per-function census (built on the
   trace hook) records every function entered with arguments that look like a
   string draw — small x/y plus a pointer to NUL-terminated bytes — so the
   game's text routines are identified from runtime behaviour, not guessed from
   addresses (recompiled code is laid out differently than any source oracle).
   Text is drawn through *many* routines, so coverage centres on the game's
   universal `_Printf`-style formatter chokepoint (every formatted string passes
   through it) plus the sibling draw routines — menus, battle and overlay text
   are all reached, not just one routine. A targeted watchlist (scanning arg
   registers and stack slots for known byte sequences) pins down routines whose
   calling convention differs.
2. **Key by content hash.** On each call to that routine the hook reads the
   source glyph bytes from guest RDRAM (the game's text is a 2-byte EUC-JP-like
   encoding; ASCII is single-byte) and hashes them (FNV-1a). The hash is the
   translation key — robust to the game reusing text buffers.
3. **Look up + replace, hot-reloaded on the fly.** Keys map to English strings
   in a small JSON table loaded next to the executable. The table is re-read
   whenever its modification time changes, so **editing a string updates the
   running game within a frame — no rebuild, no restart**: author translations
   live while the game is open. On a miss, the original text is drawn unchanged.
4. **Render English through the game's own font.** The font sheets already
   contain Latin glyphs, so a hit re-drives the game's glyph drawer over the
   English bytes — no glyph injection. Because the replacement never writes
   back into game memory, it is **not bounded by the original length**.
   Latin advance widths are measured from the resident font (or fall back to a
   tight uniform advance for fonts streamed per-draw) so English is spaced
   proportionally. Each line then **auto-fits to the original Japanese footprint**
   — if the English is wider, its inter-glyph advance is condensed so the text
   stays within the box the Japanese occupied, so longer translations don't spill
   over. Format strings (`%d`/`%s`) keep the game's own formatting path so dynamic
   values still work.

The translation logic itself lives in the consumer
([`src/main/diagnostics.cpp`](https://github.com/mstan/PocketMonstersStadiumRecomp/blob/main/src/main/diagnostics.cpp),
`include/trace.h`), not in this runtime — the runtime only provides the rings,
the guest-memory accessor, and the trace entry point. See the
[PocketMonstersStadiumRecomp README](https://github.com/mstan/PocketMonstersStadiumRecomp#english-translation-patch)
for how to capture strings and author translations.

A modern runtime for traditional ports and recompilations of N64 games. \
The runtime is consists of two libraries: [ultramodern](#ultramodern) and [librecomp](#librecomp).

## ultramodern

ultramodern is a reimplementation of much of the core functionality of libultra. It can be used with either statically recompiled projects that use N64Recomp or direct source ports. It implements the following libultra functionality:

* Threads
* Controllers
* Audio
* Message Queues
* Timers
* RSP Task Handling
* VI timing

Platform-specific I/O is handled via callbacks that are provided by the project using ultramodern. This includes reading from controllers and playing back audio samples.

ultramodern expects the user to provide and register a graphics renderer. The recommended one is [RT64](https://github.com/rt64/rt64).

## librecomp

librecomp is a library meant to be used to bridge the gap between code generated by N64Recomp and ultramodern. It provides wrappers to allow recompiled code to call ultramodern. Librecomp also provides some of the remaining libultra functionality that ultramodern doesn't provide, which includes:

* Overlay handling
* PI DMA (ROM reads)
* EEPROM, SRAM and Flashram saving (these may be partially moved to ultramodern in the future)

## Building

The recommended usage of these libraries is to include them in your project's CMakeLists.txt file via add_subdirectory. This project requires C++20 support and was developed using Clang 15, older versions of clang may not work. Recent enough versions of MSVC and GCC should work as well, but are not regularly tested.

These libraries can be built in a standalone environment (ie, developing new features for the libraries of this project) via the following:

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
