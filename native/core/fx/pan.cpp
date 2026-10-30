// pan.cpp — see pan.h.
#include "fx/pan.h"

#include <cmath>

namespace synth1::fx {

namespace {
// _DAT_10063f28 / DAT_180073e68 = 0.002f (PE-pinned, m3-u4b-consumer-2 F2).
inline constexpr float CPAN_SLEW_TAU = 0.002f;
}  // namespace

void CPan::set_pan_param(int x) {
  // 1/64 left of centre (0..64 -> -1..0), 1/63 right of centre (64..127 -> 0..+1).
  const float p = (x <= 64) ? (static_cast<float>(x) - 64.0f) / 64.0f
                             : (static_cast<float>(x) - 64.0f) / 63.0f;
  set_pan(p);
}

void CPan::set_pan(float p) {
  if (p < -1.0f) p = -1.0f;
  if (p > 1.0f) p = 1.0f;
  p_ = p;
  gl_ = (p > 0.0f) ? (1.0f - p) : 1.0f;
  gr_ = (p < 0.0f) ? (1.0f + p) : 1.0f;
}

void CPan::set_sample_rate(float sr) {
  // FUN_10013ba0: fVar3 = (float)sr * 0.002f (float mul), exponent
  // 1.0/(double)fVar3, pow(0.5, e) in double, NaN -> 0.5, k = (float)(1.0 - d).
  const float t = static_cast<float>(static_cast<int>(sr)) * CPAN_SLEW_TAU;
  double d = std::pow(0.5, 1.0 / static_cast<double>(t));
  if (std::isnan(d)) d = 0.5;
  k_ = static_cast<float>(1.0 - d);
}

namespace {
inline bool pan_active(const synth1::lfo::LfoState* l) {
  return l != nullptr && l->onOff != 0 &&
         l->destination == synth1::lfo::DEST_PAN;
}
}  // namespace

void CPan::process(float* left, float* right, int num_frames, int silent) {
  // FUN_10013d20:14957 — the whole CPan body (static AND modulated) runs
  // iff the chain silent flag is clear; slew state and the walk are frozen.
  if (silent != 0) return;

  // Device chain cadence: one segment per core chunk, LFO state as of the
  // chunk start (FUN_10024ea0 advances the LFOs after each chunk's
  // voice-chain call). Standalone use falls back to the single tap pair.
  LfoSeg single{0, lfo_a_, lfo_b_};
  const LfoSeg* segs = (segs_ != nullptr && n_segs_ > 0) ? segs_ : &single;
  const int nsegs = (segs_ != nullptr && n_segs_ > 0) ? n_segs_ : 1;

  bool any_act = false;
  for (int i = 0; i < nsegs; ++i) {
    if (pan_active(segs[i].a) || pan_active(segs[i].b)) { any_act = true; break; }
  }
  if (!any_act) {
    // Static path; the multiply is skipped iff gL == gR (FUN_10013d20:14969).
    if (gl_ != gr_) {
      for (int i = 0; i < num_frames; ++i) {
        left[i] *= gl_;
        right[i] *= gr_;
      }
    }
    return;
  }

  int pos = 0;
  for (int si = 0; si < nsegs && pos < num_frames; ++si) {
    int seg_start = segs[si].start - seg_base_;
    if (seg_start < 0) seg_start = 0;
    if (seg_start > num_frames) seg_start = num_frames;
    int seg_end = (si + 1 < nsegs) ? segs[si + 1].start - seg_base_ : num_frames;
    if (seg_end < seg_start) seg_end = seg_start;
    if (seg_end > num_frames) seg_end = num_frames;
    if (seg_start > pos) {  // un-covered gap: static gains
      for (int j = pos; j < seg_start; ++j) {
        left[j] *= gl_;
        right[j] *= gr_;
      }
      pos = seg_start;
    }
    const synth1::lfo::LfoState* la = segs[si].a;
    const synth1::lfo::LfoState* lb = segs[si].b;
    const bool act_a = pan_active(la);
    const bool act_b = pan_active(lb);
    if (!act_a && !act_b) {
      if (gl_ != gr_) {
        for (int j = pos; j < seg_end; ++j) {
          left[j] *= gl_;
          right[j] *= gr_;
        }
      }
      pos = seg_end;
      continue;
    }

    // Modulated path: union-grid walk (FUN_10013d20:15018-15086), getter
    // position chunk-relative (m3-u4b-land: chunk-start anchor pins the
    // 41 v7 per-channel nulls to floor).
    int consumed = pos;
    while (consumed < seg_end) {
      int seglen = seg_end - consumed;
      int v = 0;
      if (act_a) {
        int s = 0;
        v += synth1::lfo::read_at(*la, consumed - seg_start, &s);
        if (s > 0) {
          // FUN_10012c00 epilogue: relative boundary clamped UP to minSegment.
          if (s < la->minSegment) s = la->minSegment;
          if (s < seglen) seglen = s;
        }
      }
      if (act_b) {
        int s = 0;
        v += synth1::lfo::read_at(*lb, consumed - seg_start, &s);
        if (s > 0) {
          if (s < lb->minSegment) s = lb->minSegment;
          if (s < seglen) seglen = s;
        }
      }

      // Cubic width law (amendment: float cube -> double affine -> float target).
      const float p = static_cast<float>(v) * (1.0f / 64.0f);
      double dL;
      if (p >= 0.0f) {
        const float c = p - 1.0f;
        dL = static_cast<double>(c * c * c) * 0.5 + 1.5;
      } else {
        const float c = p + 1.0f;
        dL = static_cast<double>(c * c * c);
      }
      // Right-channel law (m3-pan-sign-land, #163): DAT_10063850 = 0x80000000,
      // so 0x1001407d xorps is NEGATION (not fabs). comiss/jbe 0x10014084-91:
      // q = -p >= 0 -> 0x100140ab: 0.5(q-1)^3+1.5 (mulsd 0.5 0x100140be);
      // else p > 0 -> 0x10014093-a5: (1-p)^3 (xmm6 = 1.0).
      const float q = -p;
      double dR;
      if (q >= 0.0f) {
        const float cR = q - 1.0f;
        dR = static_cast<double>(cR * cR * cR) * 0.5 + 1.5;
      } else {
        const float cR = 1.0f - p;
        dR = static_cast<double>(cR * cR * cR);
      }

      const float tL = static_cast<float>(dL) * gl_;
      const float tR = static_cast<float>(dR) * gr_;
      for (int i = 0; i < seglen; ++i) {
        const int j = consumed + i;
        slew_l_ += (tL - slew_l_) * k_;
        slew_r_ += (tR - slew_r_) * k_;
        left[j] *= slew_l_;
        right[j] *= slew_r_;
      }
      consumed += seglen;
    }
    pos = seg_end;
  }
}

}  // namespace synth1::fx
