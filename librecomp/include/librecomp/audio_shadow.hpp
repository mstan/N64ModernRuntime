// audio_shadow.hpp — engine-agnostic verified-enhancement shadow verifier.
//
// This is the gate that makes a "verified-enhancement shadow" SAFE per
// recomp-template/PRINCIPLES.md ("Verified-Enhancement HLE Is Allowed;
// Load-Bearing HLE Is Not"). A shadow renders a guest audio/video subsystem
// at higher fidelity ALONGSIDE the faithful (canon) path and a canon-domain
// check copy; this class decides, from the running canon stream, whether the
// shadow is proven enough to substitute, and reacts the instant it diverges:
//
//   - envelope correlation + level-ratio self-check vs the canon stream
//   - probation auto-gain calibration (driver/mixer revisions scale output)
//   - prove-then-substitute, strike-then-pause, escalating re-prove
//
// The canon path stays the authoritative output AND the verify oracle; the
// shadow is NEVER ground truth and reverts loudly (the caller logs a
// DEGRADED-class signal) on divergence. If this verifier is wrong, the worst
// case is "we keep playing/showing the faithful output" — it cannot corrupt
// output and cannot mask a recompiler bug, because the path it shadows is
// still the thing being diffed.
//
// NOTE for N64: see docs/SHADOW_ENHANCEMENTS.md. On N64 there is no fixed
// hardware synth chip or panel to shadow (audio is RSP-microcode-mixed PCM;
// video is GPU-rendered high-color). This verifier is ported as the REUSABLE
// FOUNDATION so the ecosystem shares one self-policing core; whether any N64
// subsystem is a good fit for a shadow is assessed honestly in that doc.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs) via the
// gbarecomp C++ port (src/gba/audio_shadow.*), © Jrickey, MIT OR Apache-2.0,
// used with permission. See THIRD_PARTY_ATTRIBUTION.md. Engine-agnostic;
// reused across the recomp ecosystem.

#ifndef LIBRECOMP_AUDIO_SHADOW_HPP
#define LIBRECOMP_AUDIO_SHADOW_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

// One window's outcome, for subsystem-specific behavior search.
enum class ShadowJudgement {
    None,  // mid-window, or canon silent (no verdict)
    Pass,  // window passed all gates
    Fail,  // window failed (see last_fail_structural)
};

// Differential self-check: shadow stream vs the canon DAC stream. We police
// STRUCTURE (same content, same loudness, same time), not raw samples: both
// streams are rectified, lowpassed to envelopes, decimated, and Pearson-
// correlated over a coarse lag range covering the pipeline delay.
class ShadowSelfCheck {
public:
    // Feed one grid sample of (canon, shadow) stereo pairs. Returns true at
    // window ends that produced a verdict, filling best_r and level_ratio.
    bool push(float canon_l, float canon_r, float shadow_l, float shadow_r,
              float& best_r, float& level_ratio);

    void reset_strikes() { strikes_ = 0; }
    uint32_t strikes() const { return strikes_; }
    void add_strike() { ++strikes_; }
    void sub_strike() { if (strikes_) --strikes_; }

private:
    float lp_x_l_ = 0, lp_x_r_ = 0;  // canon envelope follower (per side)
    float lp_y_l_ = 0, lp_y_r_ = 0;  // shadow envelope follower
    std::vector<float> ex_l_, ex_r_; // decimated canon envelope
    std::vector<float> ey_l_, ey_r_; // decimated shadow envelope
    uint32_t phase_ = 0;
    uint32_t strikes_ = 0;
};

// The engine-agnostic verification state machine.
class ShadowVerifier {
public:
    // Calibrated output gain (1.0 = stock scale). The shadow multiplies its
    // output by this in BOTH the substituted output and the check copy.
    float gain() const { return gain_; }
    bool  proven() const { return proven_; }
    uint64_t pauses() const { return pauses_; }
    float last_r() const { return last_r_; }
    float last_ratio() const { return last_ratio_; }
    // True if the most recent verdict was a Fail whose correlation broke
    // while the level was in-band (the semantics-mismatch signature a
    // subsystem can search over). Only meaningful right after judge()==Fail.
    bool last_fail_structural() const { return fail_structural_; }
    // Set on each pause with the reason; caller surfaces it (DEGRADED) then
    // clears. Empty = none.
    std::string take_reverted() { std::string s = reverted_; reverted_.clear(); return s; }

    // Feed one grid sample of (canon, shadow-check-copy) pairs, both in the
    // canon's normalized domain. Drives calibration, proving, strikes, pauses.
    ShadowJudgement judge(float canon_l, float canon_r, float chk_l, float chk_r);

private:
    void pause(std::string reason);

    ShadowSelfCheck check_{};
    float gain_ = 1.0f;
    bool  calibrated_ = false;
    float ratio_hist_[3] = {0, 0, 0};
    uint32_t ratio_n_ = 0;
    bool  proven_ = false;
    uint32_t prove_need_ = 1;
    uint32_t consec_pass_ = 0;
    uint64_t pauses_ = 0;
    std::string reverted_;
    float last_r_ = 0;
    float last_ratio_ = 0;
    bool  fail_structural_ = false;
};

}  // namespace recomp

#endif  // LIBRECOMP_AUDIO_SHADOW_HPP
