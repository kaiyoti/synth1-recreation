// master_volume.cpp — Synth1 master volume law + anti-zipper smoothing.
//
// Spec: out/spec_mastervol.md. Ghidra cites in master_volume.h.

#include "master_volume.h"

#include <cmath>

namespace synth1::engine {

// ---------------------------------------------------------------------------
// Gain law (spec §3, FUN_1800288e0 0xfe branch, bit-exact constants).
//   raw == 0 → 0.0f (hard zero; pow not called — spec §3 closed form).
//   raw  > 0 → 10 ^ ( 0.05 * ( 20u − 1/u − 10 ) ),  u = (float)raw*(1/127) + 0.01
// .rdata: 1/127f, 0.01, 20.0, 1.0, 10.0, 0.05.
// ---------------------------------------------------------------------------
float master_gain_from_raw(int raw) {
  if (raw <= 0) return 0.0f;
  if (raw > MASTER_VOL_MAX_RAW) raw = MASTER_VOL_MAX_RAW;
  const double u = (double)((float)raw * (1.0f / 127.0f)) + 0.01;
  const double exponent = ((20.0 * u - 1.0 / u - 10.0) * 0.05);
  return (float)std::pow(10.0, exponent);
}

// ---------------------------------------------------------------------------
// One-pole coefficient (spec §5, FUN_180028150 @28043–47):
//   alpha = 1 − 0.5^(1/(sampleRate · 0.002))   (τ = 2 ms, corner ≈ 55 Hz).
// ---------------------------------------------------------------------------
float master_alpha_for_rate(float sample_rate) {
  const double sr = (double)sample_rate;
  if (sr <= 0.0) return 1.0f;  // degenerate: no smoothing
  return 1.0f - (float)std::pow(0.5, 1.0 / (sr * 0.002));
}

// ---------------------------------------------------------------------------
// Construction (spec §2/§7, law 76): default raw 64 → target 0.825370, but
// the smoothed current gain starts from 0 (ctor 26115 stores 0 into
// engine+0xbb4c). The one-pole therefore ramps up from 0 once per plugin
// instance/render, like the device.
// ---------------------------------------------------------------------------
MasterVolume::MasterVolume() {
  raw = MASTER_VOL_DEFAULT_RAW;
  target_gain = master_gain_from_raw(raw);
  current_gain = 0.0f;
  alpha = master_alpha_for_rate(sample_rate);
}

void MasterVolume::prepare(float sample_rate) {
  this->sample_rate = sample_rate;
  alpha = master_alpha_for_rate(sample_rate);
}

void MasterVolume::set_raw(int r) {
  if (r < 0) r = 0;
  if (r > MASTER_VOL_MAX_RAW) r = MASTER_VOL_MAX_RAW;
  raw = r;
  target_gain = master_gain_from_raw(raw);
}

void MasterVolume::set_norm(float norm01) {
  // spec §1 int→raw quantization: f = norm*128; if (127<=f) f=127; raw=(int)f.
  float f = norm01 * 128.0f;
  if (f >= (float)MASTER_VOL_MAX_RAW) f = (float)MASTER_VOL_MAX_RAW;
  if (f < 0.0f) f = 0.0f;
  set_raw((int)f);
}

// ---------------------------------------------------------------------------
// Apply at the plugin output (spec §4/§5, FUN_1800285a0 tail).
// Fast path when target == current: hard in-place scale (FUN_18001b5d0).
// Ramp path (FUN_180028480): the one-pole advances once per INTERLEAVED
// sample (2 per stereo frame); each sample is scaled by the gain after its
// own advance, with the 2^-16 snap checked after every advance.
// ---------------------------------------------------------------------------
void MasterVolume::process_stereo(float* interleaved, int num_frames) {
  if (interleaved == nullptr || num_frames <= 0) return;
  if (target_gain == current_gain) {
    const float g = current_gain;
    const int n = num_frames * 2;
    for (int i = 0; i < n; ++i) interleaved[i] *= g;
    return;
  }
  const int n = num_frames * 2;
  for (int i = 0; i < n; ++i) interleaved[i] *= advance_one_sample();
}

void MasterVolume::process(float* left, float* right, int num_frames) {
  if (num_frames <= 0) return;
  if (target_gain == current_gain) {
    const float g = current_gain;
    for (int f = 0; f < num_frames; ++f) {
      if (left)  left[f]  *= g;
      if (right) right[f] *= g;
    }
    return;
  }
  for (int f = 0; f < num_frames; ++f) {
    if (left)  left[f]  *= advance_one_sample();
    if (right) right[f] *= advance_one_sample();
  }
}

}  // namespace synth1::engine
