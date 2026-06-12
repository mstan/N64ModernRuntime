# Shadow Audio + Screen Enhancements (N64 backport — HONEST FIT ASSESSMENT)

Backport assessment of the gbarecomp/snesrecomp "verified-enhancement" QoL
layer to the N64 recompiler (`N64ModernRuntime` + RT64 + game runners). All
work lives on the `feat/shadow-enhancements` branch / `_shadow_n64recomp`
worktree off `main`; it does not touch the in-flight
`work/pocket-monsters-stadium`, `dev/*`, or any other branch/worktree.

**Bottom line up front:** N64 is the weakest fit of the consoles surveyed so
far. The reusable `ShadowVerifier` foundation is ported and compiles, but
**no audio or video enhancement was implemented**, because neither subsystem
has the shape the shadow pattern needs (a lossy hardware synth/panel sitting
between a higher-fidelity source and the output). Forcing one would be a
marginal-to-wrong enhancement, which the rules forbid. This document is the
deliverable for the video and audio paths.

## Governing principle (the carve-out)

Faithfulness is the product; a shadow is an opt-in layer on top. The one
permitted form of HLE is a **verified-enhancement shadow**, allowed only when
ALL hold (`recomp-template/PRINCIPLES.md`, "Verified-Enhancement HLE Is
Allowed; Load-Bearing HLE Is Not"):

1. The faithful (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon
   stream and substitutes only after a proven window.
3. It reverts loudly (logs DEGRADED) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical.

Worst-case failure is "the user hears/sees the authentic output," and it
cannot mask a recompiler bug because the canon path it shadows is still the
thing being diffed.

## What ports verbatim vs what is N64-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `librecomp/include/librecomp/audio_shadow.hpp` + `librecomp/src/audio_shadow.cpp`, C++20, compiles clean (`-Wall -Wextra`), wired into `librecomp/CMakeLists.txt` | Engine-agnostic; algorithm byte-for-byte identical to gbarecomp/snesrecomp. Ported as the **reusable foundation** so the ecosystem shares one self-policing core, ready if a use ever emerges. |
| Color-science / panel LUT core | **NOT PORTED** | N64 video is already high-color and GPU-rendered; a GBA-style 15-bit panel LUT does not apply (see Video below). |
| Audio engine shadow | **NOT IMPLEMENTED** | N64 has no fixed hardware synth chip to re-render (see Audio below). |

## Why the shadow pattern fits GBA/SNES but not N64

The pattern needs a **lossy hardware stage** sitting between a recoverable
higher-fidelity source and the output, so the shadow can re-render that source
in float and beat the hardware's requantization:

- **GBA (MP2K):** a software driver mixes voices, but the hardware DAC path
  requantizes to 8-bit and FIFO-rate-limits. The shadow re-renders the *same
  driver's voices* in float. Clear win.
- **SNES (S-DSP):** a fixed hardware chip decodes BRR with Gaussian
  interpolation and truncates a 16-bit intermediate. The shadow re-renders the
  *same voices* with better interpolation/headroom. Clear win.
- **N64:** there is **no fixed synth chip and no fixed panel.** Both findings
  below.

## VIDEO — assessment: NOT a good fit (no enhancement implemented)

### Canon path (integration points found on `main`)

- VI thread queues a present each refresh: `ultramodern/src/events.cpp:250`
  (`enqueue(ScreenUpdateAction{...})`); `update_vi()` reads the framebuffer
  origin from `VI_ORIGIN_REG` at `events.cpp:85-128`.
- Graphics thread delegates to the renderer:
  `ultramodern/src/events.cpp:510-512` (`renderer_context->update_screen()`).
- Actual rendering is **RT64**, a hardware-accelerated GPU plugin that
  reinterprets the RDP display lists (not the recompiler's job; an external
  high-fidelity renderer). The VI framebuffer is uploaded to a GPU render
  target; there is **no CPU-side framebuffer the runtime composites**
  (`rt64/src/hle/rt64_present_queue.cpp:136-254`, `rt64_framebuffer.cpp`).
- Present/swap is GPU-timed: `rt64/src/hle/rt64_present_queue.cpp:400`
  (`swapChain->present(...)`) → D3D12 `rt64_d3d12.cpp:1275-1278`.
- The VI shader already does gamma correction and VI-accurate sampling:
  `rt64/src/shaders/VideoInterfacePS.hlsl:13-22`, gamma at
  `rt64/src/hle/rt64_vi.cpp:41-43`.

### Why a verified-enhancement shadow does NOT apply

1. **The output is already high-fidelity and high-color.** N64 framebuffers
   are RGBA5551 / RGBA8888, and RT64 already upscales, can use RGBA16F render
   targets, and renders far beyond native resolution. There is no 15-bit panel
   requantization to undo (the entire premise of the GBA/SNES color LUT). A
   color LUT here would be a *creative* filter, not a *fidelity-recovering*
   one — and the verifier has nothing to verify against, because there is no
   "more faithful" reference the shadow is restoring toward.
2. **The pixels live on the GPU.** The differential verifier is a CPU-side
   correlation over two sample streams; there is no canon CPU framebuffer to
   diff a shadow against without a full readback every frame, which the
   architecture deliberately avoids.
3. **A present-time CRT / deflicker / dither-cleanup model COULD exist** — but
   it would be a *display model*, not a verified shadow of a guest subsystem,
   and N64 hardware video output behavior (composite/S-video filtering, VI
   AA/dither, deflicker interlace) is **not something to guess at.** Per the
   hard rule "NEVER guess hardware behavior," this is documented as a possible
   future *renderer* feature (it belongs in RT64's post-process / render-hook
   path, see below), **not faked here.**

### If anyone ever does pursue a present-time display model

It would be a renderer post-process, not a shadow, and would plug into RT64's
existing hooks (documented for completeness, intentionally not implemented):

- Render-hook callback after VI render, before swap-chain present:
  `rt64/src/rhi/rt64_render_hooks.h:9-18`, invoked at
  `rt64/src/hle/rt64_present_queue.cpp:348-350`.
- Existing post-process shader pipeline:
  `rt64/src/shaders/PostProcessPS.hlsl`, `rt64/src/render/rt64_shader_library.*`.
- It must be opt-in, off by default, and modeled from measured hardware — not
  invented. Until that data exists, it stays unimplemented.

**Verdict: not worthwhile as a verified shadow.** The color LUT premise does
not transfer; any present-time CRT model is a renderer feature requiring
measured-hardware data we do not have, and faking it is forbidden.

## AUDIO — assessment: marginal, NOT worthwhile (no enhancement implemented)

### Canon path (integration points found on `main`)

- The guest's **RSP audio microcode mixes all voices into int16 PCM in RDRAM**
  — there is no fixed hardware synth chip; the "mixer" is per-game guest code
  the recompiler already runs faithfully.
- libultra DMAs that PCM out: `osAiSetNextBuffer_recomp` →
  `ultramodern::queue_audio_buffer` (`librecomp/src/ai.cpp:69-76`).
- `queue_audio_buffer` forwards the raw int16 PCM to the host callback:
  `ultramodern/src/audio.cpp:25-36` (`audio_callbacks.queue_samples(...)`).
- The host runner implements `queue_samples` (e.g.
  `PocketMonstersStadiumRecomp/src/main/main.cpp:187-243`): int16→float, a
  stereo channel-swap for libultra's R,L word order
  (`main.cpp:205-208`), then an **`SDL_AudioCVT` resample** from the
  guest rate (commonly 32000 Hz) to the host's 48000 Hz
  (`main.cpp:177-185, 218-221`), then `SDL_QueueAudio`. AI back-pressure is
  emulated via `AI_LEN_REG` (`librecomp/src/ai.cpp:23-57`).

### Why a verified-enhancement shadow is marginal

1. **There is no engine to re-render.** Unlike MP2K (a software driver whose
   voices can be re-mixed in float) or the S-DSP (a fixed chip whose BRR
   interpolation can be improved), N64 audio reaches the runtime as
   **already-mixed PCM**. The faithful path *is* the guest's own mixer output;
   there is no lossy hardware stage between a higher-fidelity source and the
   PCM for a shadow to beat. The only thing downstream of the PCM is the host
   resampler.
2. **The only candidate enhancement is the resample stage**, where
   `SDL_AudioCVT` does linear interpolation (32000→48000 with no explicit
   anti-alias low-pass; high-frequency content can fold). One *could* swap in a
   polyphase/band-limited resampler. But:
   - This is a **DSP-quality swap on the canon PCM, not a re-render of a guest
     subsystem.** It does not match the "shadow a guest engine, verify against
     the hardware path" shape. There is no meaningful "canon vs shadow"
     differential to police — both are just the same PCM resampled two ways;
     the verifier would near-always pass and add nothing.
   - It is **better located as a plain host-side resampler-quality option** in
     the runner's `queue_samples` (an ordinary audio-quality setting), not as a
     verified-enhancement HLE shadow. Dressing it as a shadow would be
     theater: the self-policing machinery guards against a divergence class
     that cannot meaningfully occur here.
   - The runner already has a sample-dropping decimation valve under queue
     pressure (`main.cpp:230-240`); resampler quality is secondary to that.
3. **The decimation valve, not interpolation, is the real artifact source.**
   Improving resampler taps while the valve still hard-drops samples under load
   would chase the smaller defect.

**Verdict: not worthwhile as a verified shadow.** A band-limited resampler is a
legitimate *audio-quality option* for the host runner, but it is not a guest-
engine shadow and should not be wrapped in the verifier. Recommended as a
separate, ordinary runner setting if desired — explicitly out of scope for the
verified-enhancement layer.

## What WAS implemented

- **`ShadowVerifier` foundation only** — the reusable, engine-agnostic
  differential verifier core, ported language-matched (C++20) into
  `librecomp`, compiled clean standalone, and wired into the build. It carries
  no N64-specific consumer because, per the assessments above, none is
  warranted. It exists so that *if* a future N64 subsystem ever presents the
  right shape (a lossy hardware stage over a recoverable source), the
  self-policing core is already in place and shared with the rest of the
  ecosystem.
- Standalone compile + behavioral smoke check (ran during development, not
  committed as a build target): identical canon/shadow streams → `proven`,
  zero pauses, correlation 1.00; a deliberately diverged shadow → loud revert
  (`pauses=1`, DEGRADED reason string). Confirms the gate works.

## Recommendation for review

1. **Keep the `ShadowVerifier` foundation** (it's small, self-contained,
   attributed, and ecosystem-shared) but **ship no N64 audio or video shadow.**
   N64 lacks the lossy-hardware-over-recoverable-source shape both prior ports
   relied on.
2. **Video:** do not pursue a color/CRT LUT as a *shadow*. If a present-time
   CRT/deflicker display model is ever wanted, build it as an **RT64
   post-process** from *measured* hardware data (hooks at
   `rt64_render_hooks.h:9-18` / `PostProcessPS.hlsl`), opt-in and off by
   default — never invented.
3. **Audio:** if resample quality matters, add a **plain band-limited
   resampler option** in the runner's `queue_samples`
   (`.../main.cpp:187-243`) as an ordinary audio setting — not wrapped in the
   verifier. Fix the decimation valve first if pop/click is the complaint.
4. **Default OFF / byte-identical** is trivially satisfied: nothing in the
   canon audio or video path was changed on this branch; the only added code
   is dormant foundation with no call site.

## Attribution

`ShadowVerifier` ported from JRickey/gba-recomp (`crates/gba-core/src/shadow.rs`)
via the gbarecomp C++ port, © Jrickey, MIT OR Apache-2.0, used with permission.
See `THIRD_PARTY_ATTRIBUTION.md`.
