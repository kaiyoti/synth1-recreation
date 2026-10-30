// adsr.cpp — ADSR envelope (stub for now).
//
// Spec: spec_env.md; Ghidra: env state machine.
// SUBAGENT B: implement the envelope state machine.

#include "adsr.h"

#include <cmath>

namespace synth1::env {

// ---------------------------------------------------------------------------
// Time-constant curves (spec_env.md §2.1–§2.2, §3.1 — params 25–29).
//
// These are the band → physical-unit conversions from FUN_180028fe0 (the
// 99-case parameter→DSP dispatcher). Band quantisation itself is spec_env.md
// §1 (band = clamp((int)(value·128), 0, 127)); the raw 0..127 event value IS
// the band. The curves are evaluated in double and stored as float
// (spec_env.md §9).
// ---------------------------------------------------------------------------

// U5-a′ (d-opus-u5a-scope.answer.md): the 32-bit dispatcher builds the
// exponent in FLOAT32 — x = (float)band · 1/127.0f (mulss DAT_10063520),
// widened to double — then FUN_10004700 = exp(log(base)·x) (pow fallback),
// ·K in double, cvtpd2ps → float t (CVca 0x10025fac-0x10025ff9, CVcf
// 0x1002635c-0x10026385, CVco 0x10025c7f-0x10025cc5; same path for decay/
// release with base 6000 / K 0.01). exp∘log ≡ pow on every float t
// (tasks/d-opus-u5/probe/explog_vs_pow.cpp: 0/256 differ).
static inline double band_exponent(int band) {
  return (double)((float)band * (1.0f / 127.0f));
}

// Param 25 — amp attack (case 0x19, 0x1800299a0):
//   t = pow(56000.0, band/127) · 0.0005   [0.5 ms .. 28 s]
float amp_attack_seconds(int band) {
  return (float)(std::pow(56000.0, band_exponent(band)) * 0.0005);
}

// Param 26 — amp decay (case 0x1a, 0x180029a00):
//   t = pow(6000.0, band/127) · 0.01      [10 ms .. 60 s]
float amp_decay_seconds(int band) {
  return (float)(std::pow(6000.0, band_exponent(band)) * 0.01);
}

// Param 27 — amp sustain (FUN_1000bcd0, amp_env_level_law.md): PURE exponential
//   ratio = 2048^((band/127)^0.3 − 1)   [0 .. 1]; 0 at band 0.
//   Runtime authority (legion LIVE DLL dump): the 32-bit runtime ampGain field =
//   0.090623 = 2048^((36/127)^0.3)/2048 EXACT. The census grepped setter call
//   sites and missed the runtime amp path — the live measurement is authoritative.
//   Equal to the linear curve at band 127, so 051's sustain (27=97) is unaffected.
float amp_sustain_ratio(int band) {
  if (band == 0) return 0.0f;
  const double x = std::pow((double)band / 127.0, 0.3);
  return (float)(std::pow(2048.0, x) * (1.0 / 2048.0));
}

// Param 28 — amp release (case 0x1c, 0x180029b00): identical curve to decay.
float amp_release_seconds(int band) {
  return (float)(std::pow(6000.0, band_exponent(band)) * 0.01);
}

// Param 29 — amp gain (runtime authority, legion LIVE DLL dump): the runtime
// ampGain field = 2048^((band/127)^0.3)/2048 (band36 → 0.090623 EXACT). The
// census's exp-1.0 setter grep missed the runtime amp path; live measurement wins.
float amp_gain_curve(int band) {
  if (band == 0) return 0.0f;
  const double x = std::pow((double)band / 127.0, 0.3);
  return (float)(std::pow(2048.0, x) * (1.0 / 2048.0));
}

// Param 30 — amp velocity sensitivity (case 0x1e, 0x180029bd3): band/127.
float amp_velocity_sens(int band) {
  return (float)band * (1.0f / 127.0f);
}

}  // namespace synth1::env
