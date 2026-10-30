// lfo.cpp — the 2× CLFO (LFO1 / LFO2).
//
// Spec: out/spec_lfo_fx.md §1 (LFOs — 2 of them):
//   §1.3 speed→Hz        FUN_180015210  (free-run powf(1500, s/127)/12;
//                                        tempo-sync tempo·48/DIV[i]/120)
//   §1.4 freq slew       FUN_180014ff0  (1000 Hz/s glide + phase rescale)
//   §1.5 phase advance   FUN_180014f60  (phase += n, wrap, S&H re-seed)
//   §1.7 depth→range     FUN_180015080  (exp pitch law, per-dest lo/hi)
//   §1.9 waveforms       FUN_180014d10  (exact integer waveform generator)
//   S&H RNG              FUN_180055b98  (MSVC rand(): LCG *0x343fd + 0x269ec3,
//                                        (state >> 16) & 0x7fff)
//   noise table          FUN_18003cad0(0, 0.0) → DAT_1801a4298 (2048 floats);
//                                        a stable deterministic stand-in table
//                                        is generated here (stage-4 ambiguity).
//
// Precision: all waveform arithmetic is INTEGER, exactly as the binary
// (divisions truncate toward zero; the (int) casts on the depth→range path
// truncate toward zero). The wave-5 (table-sine) value rounds to nearest-even
// (32-bit fistpl, VST.dll 0x10012d55) before its truncating /2. The only
// float is the speed→Hz, the freq slew and the wave-5 table read.

#include "lfo/lfo.h"
#include "rng/crt_rand.h"

#include <array>
#include <cmath>

namespace synth1::lfo {

namespace {

// The device's CRT rand() stream (FUN_180055b98), shared with the arp.
int msvc_rand() { return rng::crt_rand(); }

// The 2048-float wave-5 table (FUN_18003cad0(0, 0.0) → DAT_1801a4298), built
// at init (decomp_all.c:37850): T[i] = (float)sin(i · 2π · (1/2048)) ·
// DAT_180094f20 (1.0f, never written). It is a SINE table — LFO wave 5 is a
// table sine, not noise (spec_lfo_fx.md's "noise" label was wrong).
const float* noise_table() {
  static const std::array<float, 2048> table = [] {
    std::array<float, 2048> t{};
    for (std::size_t i = 0; i < t.size(); ++i)
      t[i] = (float)std::sin((double)(int)i * 6.283185307179586 * 0.00048828125) * 1.0f;
    return t;
  }();
  return table.data();
}

}  // namespace

// ---------------------------------------------------------------------------
// Speed → Hz (spec_lfo_fx.md §1.3, FUN_180015210).
// ---------------------------------------------------------------------------
void speed_to_hz(LfoState& lfo, float tempo_bpm) {
  float freq;
  if (lfo.tempoSync == 0) {
    // free-running: speed 0 → 0.0833 Hz, speed 127 → 125 Hz.
    freq = powf(1500.0f, (float)lfo.speed / 127.0f) * (1.0f / 12.0f);
  } else {
    int i = (lfo.speed * 30) >> 7;  // 0..29
    if (i > 29) i = 29;
    freq = tempo_bpm * 48.0f / (float)kDiv[i] * (1.0f / 120.0f);
  }
  lfo.freqTarget = freq;
  lfo.minSegment = (freq > 10.0f) ? 32 : 64;
  // FUN_180015210 tails FUN_180014ff0(lfo, 1) (:13827); the 32-bit speed setter
  // FUN_10013140 inlines the same n=1 slew+rescale after writing freqTarget
  // (:14207-14228, step DAT_10063524/(sr*0.001) = 1000/sr). Every speed /
  // tempoSync param write therefore advances freqCur by one 1000/sr step —
  // the LFO startup glide is already part-way through at the first process
  // call (law #85; the certified host writes these params in several passes).
  freq_slew(lfo, 1);
}

// ---------------------------------------------------------------------------
// Sample-rate setter (FUN_10013360, cited @10013360; called by the
// effSetSampleRate dispatcher FUN_10024ad0 on LFO1 +0x9d0c and LFO2
// +0x9e60, once per call). One n=1 slew step iff freqCur != freqTarget,
// then the period recompute + phase rescale run UNCONDITIONALLY
// (m3-u4c-glide-convergence F2: after_sr readback cur +1, pd recomputed).
// ---------------------------------------------------------------------------
void sr_setter(LfoState& lfo) {
  const float step = 1.0f / ((float)lfo.sampleRate * 0.001f);
  float cur = lfo.freqCur;
  const float tgt = lfo.freqTarget;
  if (cur != tgt) {
    if (tgt <= cur) {
      cur -= step;
      if (tgt > cur) cur = tgt;
    } else {
      cur += step;
      if (cur > tgt) cur = tgt;
    }
    lfo.freqCur = cur;
  }
  const std::int32_t oldPeriod = lfo.period;
  lfo.period = (int)((float)lfo.sampleRate / lfo.freqCur);
  if (oldPeriod > 0) {
    lfo.phase = (int)(((float)lfo.period * (float)lfo.phase) / (float)oldPeriod);
  }
}

// ---------------------------------------------------------------------------
// INI entry-load target set (m3-u4c-land F-c): the hostselectprogram load
// at DLL entry applies the program's speed VALUE (field + target) WITHOUT
// a slew step (after_entry readback: sp/tgt = file values, cur = boot-
// replay count). tempoSync is NOT applied here (c012 after_sr ts=0).
// ---------------------------------------------------------------------------
void set_speed_target(LfoState& lfo, float tempo_bpm) {
  if (lfo.tempoSync == 0) {
    lfo.freqTarget = powf(1500.0f, (float)lfo.speed / 127.0f) * (1.0f / 12.0f);
  } else {
    int i = (lfo.speed * 30) >> 7;
    if (i > 29) i = 29;
    lfo.freqTarget = tempo_bpm * 48.0f / (float)kDiv[i] * (1.0f / 120.0f);
  }
  lfo.minSegment = (lfo.freqTarget > 10.0f) ? 32 : 64;
}

// ---------------------------------------------------------------------------
// Depth → modulation range (spec_lfo_fx.md §1.7, FUN_180015080).
// Pitch destinations (1,2): exponential ((10^(d/127))−1)/9 × 5 semitones × 128.
// All others: linear d/127. (int) casts truncate toward zero.
// ---------------------------------------------------------------------------
void depth_to_range(LfoState& lfo) {
  double d;
  if ((std::uint32_t)(lfo.destination - 1) < 2u) {
    d = (pow(10.0, (double)lfo.depth * (1.0 / 127.0)) - 1.0) * (1.0 / 9.0);
  } else {
    d = (double)lfo.depth * 0.007874015748031496;  // = depth/127
  }
  const float f = (float)d;
  switch (lfo.destination) {
    case DEST_OSC2:
    case DEST_OSC12:
      lfo.hi = (int)(f * 5.0f * 128.0f);
      lfo.lo = (int)(f * -5.0f * 128.0f);  // = −hi (truncation is symmetric)
      break;
    case DEST_FILTER:
      lfo.hi = (int)(f * 1024.0f);
      lfo.lo = 0;
      break;
    case DEST_AMP:
      // Device: 32-bit FUN_10012f80 case 4, DAT_10063eb4 = +2048.0f
      // (64-bit twin FUN_180015080 case 4); v in [0,+2031] -> gain cut
      lfo.hi = 0;
      lfo.lo = (int)(f * 2048.0f);
      break;
    case DEST_PW:
      lfo.hi = (int)(f * 128.0f);
      lfo.lo = (int)(f * -128.0f);
      break;
    case DEST_FM:
      lfo.hi = (int)(f * 512.0f);
      lfo.lo = 0;
      break;
    case DEST_PAN:
      lfo.hi = (int)(f * 64.0f);
      lfo.lo = (int)(f * -64.0f);
      break;
    default:
      lfo.lo = 0;
      lfo.hi = 0;
      break;
  }
}

// ---------------------------------------------------------------------------
// Frequency slew + phase (spec_lfo_fx.md §1.4, FUN_180014ff0(lfo, n)).
// Glide-limited to 1000 Hz/second; the phase is rescaled by period/oldPeriod
// so the waveform does not jump when the period changes.
// ---------------------------------------------------------------------------
void freq_slew(LfoState& lfo, int n) {
  float cur = lfo.freqCur;
  const float tgt = lfo.freqTarget;
  if (cur == tgt) return;
  const float step = (float)n / ((float)lfo.sampleRate * 0.001f);  // = n·1000/sr
  if (tgt <= cur) {
    cur -= step;
    if (tgt > cur) cur = tgt;  // undershot → clamp to target
  } else {
    cur += step;
    if (cur > tgt) cur = tgt;  // overshot → clamp to target
  }
  const std::int32_t oldPeriod = lfo.period;
  lfo.freqCur = cur;
  lfo.period = (int)((float)lfo.sampleRate / cur);
  if (oldPeriod > 0) {
    lfo.phase = (int)(((float)lfo.period * (float)lfo.phase) / (float)oldPeriod);
  }
}

// ---------------------------------------------------------------------------
// The 6 waveforms (spec_lfo_fx.md §1.9, FUN_180014d10(lfo, &segLen, phase)).
// Exact integer reconstruction: P = period, p = phase (0..P−1),
// mx = max(hi,lo), mn = min(hi,lo), flip = (hi < lo).
// ---------------------------------------------------------------------------
int waveform(int wave, std::int32_t P, std::int32_t p,
             std::int32_t lo, std::int32_t hi,
             std::int32_t rndCur, std::int32_t rndNext,
             int minSegment, int* outSegLen) {
  if (hi == lo) {
    *outSegLen = -1;
    return hi;
  }
  std::int32_t mx = hi;
  std::int32_t mn = lo;
  bool flip = hi < lo;
  if (flip) {
    mx = lo;
    mn = hi;
  }

  std::int32_t v = mn;
  std::int32_t segEnd = P;

  switch (wave) {
    case WAVE_SAW: {
      // saw (down): mx → mn over one period.
      const std::int32_t span = (mx - mn) + 1;
      if (span < P) {
        v = mx - (span * p) / P;
        segEnd = (((mx - v) + 1) * P - mn + mx) / span;
      } else {
        v = mx - ((mx - mn) * p) / (P - 1);
        segEnd = p + 1;
      }
      break;
    }
    case WAVE_TRI: {
      // triangle: mn → mx over half, mx → mn over the rest.
      const std::int32_t half = P / 2;
      const std::int32_t span = mx - mn;
      if (p < half) {
        v = mn + (span * p) / half;
        segEnd = (span < P) ? ((((v - mn) + 1) * half - mn - 1 + mx) / span)
                            : (p + 1);
      } else {
        const std::int32_t rem = P - half;
        v = mx - (span * (p - half)) / rem;
        segEnd = (span < rem) ? ((((mx - v) + 1) * rem - mn - 1 + mx) / span + half)
                              : ((p - half) + 1 + half);
      }
      break;
    }
    case WAVE_SQUARE:
      v = (p < P / 2) ? mx : mn;
      segEnd = (p < P / 2) ? P / 2 : P;
      break;
    case WAVE_SH:
      v = rndCur;
      segEnd = P;
      break;
    case WAVE_RGLIDE: {
      // random glide: linear ramp between the two random levels (rndCur →
      // rndNext), using the wave-0 ramp from max(a,b) down to min(a,b).
      const std::int32_t a = rndNext;
      const std::int32_t b = rndCur;
      if (a == b) {
        v = b;
        segEnd = P;
      } else {
        if (a <= b) {
          flip = !flip;
          mx = b;
          mn = a;
        } else {
          mx = a;
          mn = b;
        }
        const std::int32_t span = (mx - mn) + 1;
        if (span < P) {
          v = mx - (span * p) / P;
          segEnd = (((mx - v) + 1) * P - mn + mx) / span;
        } else {
          v = mx - ((mx - mn) * p) / (P - 1);
          segEnd = p + 1;
        }
      }
      break;
    }
    case WAVE_NOISE: {
      const float* T = noise_table();
      const std::uint32_t idx =
          ((std::uint32_t)p << 11) / (std::uint32_t)(P > 0 ? P : 1);  // 0..2047
      // The 32-bit audio DLL (the render reference) converts the float sum with
      // flds+fistpl at VST.dll 0x10012d55 = x87 round-to-nearest-even (no
      // fldcw/fnstcw in FUN_10012c00, so the control word is the process
      // default); the /2 that follows truncates toward zero (cltd/sub/sar $1).
      // The 64-bit twin's cvttss2si (truncation) is a codegen difference — the
      // 32-bit DLL is the authority (m3-t3-rounding-authority ruling).
      // std::lrintf under the default FE_TONEAREST = round-to-nearest-even.
      v = (int)std::lrintf((float)(mx - mn) * T[idx] + (float)(mx + mn)) / 2;
      segEnd = p + (minSegment - p % minSegment);
      break;
    }
    default:
      break;
  }

  // common epilogue
  if (flip) v = mn + (mx - v);
  if (segEnd > P) segEnd = P;
  int segLen = segEnd - p;
  // lfo_full_law.md §1: only the S&H / random-glide (wave 3/4) segments are
  // quantized to minSegment (64, or 32 when freq > 10 Hz). Continuous shapes
  // (saw/tri/square) update at their natural value-change rate — there is NO
  // fixed 32-sample grid for them. The old blanket clamp quantized every wave
  // to the grid (the 19:58 tick-grid bug).
  if ((wave == WAVE_SH || wave == WAVE_RGLIDE) && segLen < minSegment)
    segLen = minSegment;
  if (segLen < 1) segLen = 1;
  *outSegLen = segLen;
  return v;
}

// m3-u4c-resume-land (m3-lfo-nodecut-law, FUN_10012c00 :13907-14013): the
// LFO-wired CONSUMER chunk bound = max(segEnd - phase, minChunk), capped at
// the render segment by the caller. The phase copy is LOCAL (FUN_10012be0
// does not persist phase; the persisting tick FUN_10012e60 stays per chunk).
// Waves 3/4: segEnd = period (wrap only, :13983-13994); the waveform
// epilogue already floors their segLen at minSegment.
int consumer_cut(const LfoState& lfo, std::int32_t offset) {
  if (lfo.onOff == 0 || lfo.period <= 0) return 0x7fffffff;
  std::int32_t p = lfo.phase + offset;
  if (lfo.period <= p) p %= lfo.period;
  int segLen = 0;
  waveform(lfo.wave, lfo.period, p, lfo.lo, lfo.hi,
           lfo.rndCur, lfo.rndNext, lfo.minSegment, &segLen);
  if (segLen <= 0) return 0x7fffffff;   // lo == hi: constant value, no boundary
  if (segLen < lfo.minSegment) segLen = lfo.minSegment;  // FUN_10012c00 floor
  return segLen;
}

int segment_at(const LfoState& lfo, std::int32_t offset) {
  if (lfo.onOff == 0 || lfo.period <= 0) return 0x7fffffff;
  std::int32_t p = lfo.phase + offset;
  if (lfo.period <= p) p %= lfo.period;
  int segLen = 0;
  waveform(lfo.wave, lfo.period, p, lfo.lo, lfo.hi,
           lfo.rndCur, lfo.rndNext, lfo.minSegment, &segLen);
  if (segLen <= 0) return 0x7fffffff;   // lo == hi: constant value, no boundary
  return segLen;
}

// ---------------------------------------------------------------------------
// lfo_tick — FUN_180014f60(lfo, n) (phase advance + S&H re-seed on wrap +
// freq slew), then read the waveform at the new phase (the FUN_180014ce0
// accessor with offset 0). Called once per render chunk (spec §1.5 / §13).
// ---------------------------------------------------------------------------
namespace {
// rand() % |hi − lo| + min(lo, hi), or hi when lo == hi (FUN_180014f60 /
// FUN_1800152e0 / FUN_180015450 share this law).
std::int32_t draw_sh(const LfoState& lfo) {
  const std::int32_t hi = lfo.hi, lo = lfo.lo;
  if (hi == lo) return hi;
  if (hi < lo) return msvc_rand() % (lo - hi) + hi;
  return msvc_rand() % (hi - lo) + lo;
}
}  // namespace

void set_wave(LfoState& lfo, int wave) {
  // FUN_180015450: +0xc = wave; waves 3/4: rndNext = (lo + hi)/2, rndCur drawn.
  lfo.wave = wave;
  if ((std::uint32_t)(wave - 3) < 2u) {
    lfo.rndNext = (lfo.lo + lfo.hi) / 2;
    lfo.rndCur = draw_sh(lfo);
  }
}

void key_sync(LfoState& lfo) {
  // FUN_1800152e0: phase = 0 (+0x58 = 0); waves 3/4: rndNext = rndCur, redraw.
  lfo.phase = 0;
  if ((std::uint32_t)(lfo.wave - 3) < 2u) {
    lfo.rndNext = lfo.rndCur;
    lfo.rndCur = draw_sh(lfo);
  }
}

int lfo_tick(LfoState& lfo, int n) {
  lfo.phase += n;
  if (lfo.period > 0 && lfo.phase >= lfo.period) {
    lfo.phase %= lfo.period;
    if ((std::uint32_t)(lfo.wave - 3) < 2u) {  // waves 3 (S&H) and 4 (glide)
      lfo.rndNext = lfo.rndCur;
      lfo.rndCur = draw_sh(lfo);
    }
  }
  freq_slew(lfo, n);

  int segLen = 0;
  return waveform(lfo.wave, lfo.period, lfo.phase, lfo.lo, lfo.hi,
                  lfo.rndCur, lfo.rndNext, lfo.minSegment, &segLen);
}

}  // namespace synth1::lfo
