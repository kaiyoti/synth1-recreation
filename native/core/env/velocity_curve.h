// velocity_curve.h — amp velocity → gain factor.
//
// Spec: spec_env.md §4.2 (amp velocity sensitivity — param 30 → CVca+0xac =
//       band/127; FUN_180037bc0 computes the velocity factor).
// Ghidra: FUN_180037bc0 (amp velocity factor + amp envelope note-on);
//         FUN_180037f60 (set velocity sensitivity, param 30, case 0x1e);
//         FUN_180004d80 (pow); DAT_180073010 = 10.0, DAT_180077158 = -30.0f,
//         DAT_180072ff0 = 1/127.0 (double), DAT_180072ff8 = 0.05.
//
// The exact law (spec_env.md §4.2):
//   velFactor = 10^( −1.5 · velSens · (127 − velocity) / 127 )
// The double intermediate: 10.0 ^ ( (cvca[0xac] · −30.0f) · (0x7f − velocity)
// · (1/127.0) · 0.05 )  — note (−30.0 · 0.05) = −1.5, so the two forms are
// identical; the double is used throughout (spec_env.md §9: time-constant
// curves are evaluated in double).
//
// velocity = 127 → 1.0 (0 dB) for any setting.
// velocity = 0   → 10^(−1.5·velSens); at velSens = 1 that is 0.03162 = −30 dB
// (hence the −30 constant).
// velSens = band/127 (param 30), so the GUI's 128 bands sweep 0 dB … −30 dB
// of attenuation at velocity 0.
//
// It is a MULTIPlicative gain factor, applied in FUN_180037c30 alongside
// ampGain (spec_env.md §3.2).

#pragma once

#include <cstdint>
#include <cmath>

namespace synth1::env {

// Map a MIDI velocity (0..127) + sensitivity band (0..127) to a gain factor.
// amp_env_level_law.md §velocity target (FUN_10031bc0), legion's exact numerics:
//   vel_target = 10^( −1.5 · sens · (127 − velocity)/127 ),  sens = param30/127
// (DAT_100638c0 = 10.0 is the pow base; the earlier 10^(10·e) shorthand is
// superseded. −30·0.05 = −1.5.)
inline float velocity_to_gain(std::uint8_t velocity, std::uint8_t sensitivity_band) {
  const double vel_sens = (double)sensitivity_band / 127.0;
  const double exponent = -1.5 * vel_sens * (127.0 - (double)velocity) / 127.0;
  return (float)std::pow(10.0, exponent);
}

}  // namespace synth1::env
