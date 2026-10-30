// pan.h — CPan "Pan" (post-voice chain, engine +0x9b18 pair with CEq).
//
// Spec: out/spec_lfo_fx.md §5 pan law.
// Law (verified against the decompiled param handler; engine param 90,
// DAT_18007338c = 1/64, DAT_180072e80 = 1/63):
//   p  = x <= 64 ? (x - 64)/64 : (x - 64)/63      /* -1 … +1, 64 = centre */
//   gl = p > 0 ? 1 - p : 1                        /* left  gain */
//   gr = p < 0 ? 1 + p : 1                        /* right gain */
// Constant-power is NOT used — this is the original linear law: at hard
// left (x = 0) the right channel is fully muted, at centre both are unity.
//
// U4-b (#196): the dest-7 (pan) LFO consumer — 32-bit FUN_10013d20
// (decomp_all_32_full.c:14927-15090), routing setter FUN_10013c50
// (:14876), slew ctor FUN_10013ba0 (:14844), k recompute FUN_10013c80
// (:14894). Law (tasks/m3-u4b-pan/RESULT.md F4 + m3-u4b-consumer LAW
// AMENDMENT):
//   active[i] = (lfo[i].onOff != 0 && lfo[i].destination == 7)
//   walk the block on the union of the two active LFOs' segment grids
//   (getter FUN_10012be0: (phase + block-relative pos) % period; the
//   FUN_10012c00 boundary is relative and clamped UP to minSegment):
//     v  = val(lfo1) + val(lfo2)   (inactive LFO contributes 0)
//     p  = (float)v * (1/64f)
//     fL = p >= 0 ? 0.5*(p-1)^3 + 1.5 : (p+1)^3   /* float cube, double affine */
//     fR = 0.5*(|p|-1)^3 + 1.5                    /* always (else-branch dead) */
//     target = (float)affine * gl / gr; per-sample one-pole slew
//     sL += (tL - sL) * k; L *= sL  (state persists across blocks; boot 0)
//   k = 1 - 0.5^(1/(sr * 0.002))  (_DAT_10063f28 = 0.002f PE-pinned;
//   double pow, NaN -> k = 0.5, float store; recompute on sample-rate set).
//   Silent flag set (FUN_10013d20:14957): buffer, slew state and the
//   segment walk are ALL untouched (skip leg, not a gain bypass).
// The LFO phase seen here is the post-mixer value (the chain head
// advances the LFOs per core chunk before returning), i.e. block-relative
// pos rides the block-END phase anchor — a one-block lead vs the voice
// reads, reproduced by reading the engine's live LfoState at fx_chain
// time with block-relative offsets.
//
// Port notes: process() runs in place over interleaved-free L/R blocks.
// No realtime allocations. The block walk restarts per fx_chain chunk
// (<= FX_CHAIN_MAX_FRAMES); the device chain block is the host block,
// identical for blocks <= 2048 frames.

#pragma once

#include "lfo/lfo.h"

namespace synth1::fx {

class CPan {
 public:
  CPan() = default;

  // Raw knob value 0..127 (64 = centre).
  void set_pan_param(int x);

  // Direct pan position, clamped to [-1, +1].
  void set_pan(float p);

  float pan() const { return p_; }
  float gain_left() const { return gl_; }
  float gain_right() const { return gr_; }

  // CPan+0x10/+0x14: the two LFO objects (live state; the FUN_10013c50
  // active flags are the live predicate, evaluated per block).
  void set_lfo_taps(const synth1::lfo::LfoState* a,
                    const synth1::lfo::LfoState* b) {
    lfo_a_ = a;
    lfo_b_ = b;
    segs_ = nullptr;
    n_segs_ = 0;
  }

  // Device chain cadence: the chain stage runs per core chunk with the
  // LFO state as of the CHUNK start (FUN_10024ea0 advances the LFOs after
  // the voice-chain call of each chunk). One segment per chunk: `start` is
  // block-relative; a/b are the LFO states in force for that segment.
  struct LfoSeg {
    int start;
    const synth1::lfo::LfoState* a;
    const synth1::lfo::LfoState* b;
  };
  void set_lfo_segments(const LfoSeg* segs, int n) {
    segs_ = segs;
    n_segs_ = n;
  }

  // m3-fxchain-grid: segment starts are block-relative; when the chain
  // stage runs per 256-frame call, the call sees the segment grid shifted
  // by its block offset (FUN_10013d20 getter position stays segment-
  // relative). Default 0 = whole-block call (unchanged behavior).
  void set_lfo_segment_base(int base) { seg_base_ = base; }

  // FUN_10013ba0 / FUN_10013c80: k = 1 - 0.5^(1/(sr*0.002)), double pow,
  // NaN -> 0.5, float store (CPan+0x28). Recomputed on sample-rate set,
  // NOT per block.
  void set_sample_rate(float sr);

  // Fresh-instance state (the device ctor leaves +0x20/+0x24 zeroed).
  void reset() {
    slew_l_ = 0.0f;
    slew_r_ = 0.0f;
  }

  // In-place stereo pan. silent != 0 -> FUN_10013d20:14957 skip leg:
  // buffer, slew state and segment walk all untouched.
  void process(float* left, float* right, int num_frames, int silent);
  void process(float* left, float* right, int num_frames) {
    process(left, right, num_frames, 0);
  }

 private:
  float p_ = 0.0f;
  float gl_ = 1.0f;
  float gr_ = 1.0f;
  const synth1::lfo::LfoState* lfo_a_ = nullptr;  // CPan+0x10
  const synth1::lfo::LfoState* lfo_b_ = nullptr;  // CPan+0x14
  const LfoSeg* segs_ = nullptr;
  int n_segs_ = 0;
  int seg_base_ = 0;
  float slew_l_ = 0.0f;                           // CPan+0x20
  float slew_r_ = 0.0f;                           // CPan+0x24
  float k_ = 0.0f;                                // CPan+0x28
};

}  // namespace synth1::fx
