// master_volume.h — Synth1 master volume (param 0xfe) + anti-zipper smoothing.
//
// Spec: out/spec_mastervol.md. Ghidra cites:
//   FUN_1800288e0 @28347  gain law (0xfe branch) → engine+0x2c target gain
//   FUN_180028150 @28031  one-pole alpha → engine+0xbb50
//   FUN_180028480 @28150  ramped per-sample apply (engine+0xbb4c current gain)
//   FUN_18001b5d0 @18979  hard in-place scale (used when target == current)
//   FUN_1800285a0 @28197  engine process tail: master volume AFTER the chorus
//
// Master volume is a single engine-level float applied once per block to the
// whole stereo output buffer (spec §0: NOT the per-voice osc masterNorm).
// Parameter id 0xfe (254), raw 0..127, default 0x40 = 64 (spec §1/§2).

#pragma once

#include <cmath>
#include <cstdint>

namespace synth1::engine {

// Parameter identity (spec §1: common setting 0xfe = 254, not a dispatcher case).
inline constexpr int   MASTER_VOL_PARAM      = 0xfe;   // 254
inline constexpr int   MASTER_VOL_DEFAULT_RAW = 64;    // 0x40 (ctor 26084)
inline constexpr int   MASTER_VOL_MAX_RAW     = 127;

// Anti-zipper snap threshold: fabs(target - current) < 2^-16 (spec §5,
// DAT_180072b50 = 1/65536 = 1.5258789e-5).
inline constexpr float MASTER_VOL_SNAP = 1.0f / 65536.0f;

// Gain law (spec §3, FUN_1800288e0 0xfe branch). raw == 0 → 0.0f hard mute
// (pow not called). raw > 0 → 10 ^ ( 0.05 * ( 20u − 1/u − 10 ) ), u = raw/127 + 0.01.
float master_gain_from_raw(int raw);

// One-pole coefficient (spec §5, FUN_180028150): alpha = 1 − 0.5^(1/(fs·0.002)).
float master_alpha_for_rate(float sample_rate);

// The master-volume stage. State mirrors the engine fields (spec §2):
//   +0x28 raw int, +0x2c target gain, +0xbb4c current (smoothed) gain,
//   +0xbb50 alpha.
class MasterVolume {
 public:
  int   raw          = MASTER_VOL_DEFAULT_RAW;  // 0..127
  float target_gain  = 0.825370f;               // = master_gain_from_raw(64)
  float current_gain = 0.0f;                    // smoothed; ctor 26115 stores 0
                                                // into +0xbb4c (spec §7: the
                                                // one-pole starts from 0 once
                                                // per plugin instance/render)
  float alpha        = 0.007828f;               // recomputed in prepare()
  float sample_rate  = 44100.0f;

  MasterVolume();

  // Recompute alpha for the block rate (spec §5). Does not touch the gains.
  void prepare(float sample_rate);

  // Set raw 0..127 (clamped) and re-evaluate the target gain (spec §3/§7).
  void set_raw(int raw);

  // GUI normalizer path (spec §1 int→raw): f = norm*128; if (127<=f) f=127; raw=(int)f.
  void set_norm(float norm01);

  // Interleaved stereo [L0,R0,L1,R1,...] (spec §4: frames*2, applied at the
  // plugin output after the chorus). Law 76: the one-pole advances once per
  // INTERLEAVED sample (FUN_180028480 28170-28185), each sample scaled by
  // the gain after its own advance.
  void process_stereo(float* interleaved, int num_frames);

  // De-interleaved L/R convenience form: same per-interleaved-sample
  // one-pole (left gets 2f+1 advances, right 2f+2).
  void process(float* left, float* right, int num_frames);

 private:
  // Advance the one-pole by ONE interleaved sample and snap; returns the
  // gain to apply to that sample. The 2^-16 snap is checked after every
  // advance (the device checks once per block at 28187; the difference is
  // bounded by the snap threshold itself and keeps the engine block-size
  // invariant).
  inline float advance_one_sample() {
    current_gain += (target_gain - current_gain) * alpha;
    if (std::fabs(target_gain - current_gain) < MASTER_VOL_SNAP)
      current_gain = target_gain;
    return current_gain;
  }
};

}  // namespace synth1::engine
