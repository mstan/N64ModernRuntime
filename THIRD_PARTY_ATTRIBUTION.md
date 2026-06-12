# Third-Party Attribution

Portions of this project are ported from other open-source projects,
used with permission and under their licenses. Facts (struct offsets,
byte signatures, documented hardware/driver behavior) and, where noted,
ported logic are credited below.

## JRickey/gba-recomp

- **Upstream:** https://github.com/JRickey/gba-recomp
- **Author:** Jrickey
- **License:** MIT OR Apache-2.0 (used with the author's permission)

| Our file | Upstream source | What was ported |
|---|---|---|
| `librecomp/include/librecomp/audio_shadow.hpp`, `librecomp/src/audio_shadow.cpp` | `crates/gba-core/src/shadow.rs` (via the gbarecomp C++ port `src/gba/audio_shadow.*`) | Engine-agnostic verified-enhancement shadow verifier: envelope-correlation self-check vs the canon stream, probation auto-gain, prove/strike/pause-and-reprobe. Re-implemented in C++; namespace and enum names adapted to this runtime. Permitted under `recomp-template/PRINCIPLES.md` "Verified-Enhancement HLE Is Allowed; Load-Bearing HLE Is Not". |

The ported file carries an attribution header pointing here. Ported
code remains under the upstream's MIT OR Apache-2.0 terms; this notice
and that header satisfy the attribution requirement.

Note: this attribution file is added on the `feat/shadow-enhancements`
branch alongside the shadow-verifier foundation. See
`docs/SHADOW_ENHANCEMENTS.md` for the honest N64 fit assessment.
