// delay.cpp — CDelay "Tempo Delay" implementation.
//
// Spec: out/spec_lfo_fx.md §2. Ghidra: FUN_180005780 (ctor), FUN_1800059c0
// (taps), FUN_180006090 (normal/crossed), FUN_180005b20 (ping-pong),
// FUN_180006810 (process), FUN_1800069e0 (dry/wet), FUN_180028ac0 (tempo).
// See delay.h for the verified constants and the documented port deviations.

#include "fx/delay.h"

#include <algorithm>
#include <cmath>

namespace synth1::fx {

namespace {
// #97: tapBase = v − (v & 3) — FLOOR to a multiple of 4. Both device
// binaries: 32-bit FUN_100051a0 (decomp_all_32_full.c:3646-3650)
// `uVar3 = *(uint*)(this+0x40) & 0x80000003; … iVar5 = *(uint*)(this+0x40) − uVar3`;
// 64-bit twin FUN_1800059c0 (decomp_all.c:3810-3813) identical. The
// binaries' negative branch (& 0x80000003 + sign fix) is dead code —
// timeSamp ≥ 0 always (setter trunc of sr·sec). The engine's former
// (v+2)&~3 rode the 64-bit SPEC DOC, not the binary (reports/97101_rounding.md).
inline int floor4(int v) { return v - (v & 3); }
}  // namespace

// FUN_180028ac0: seconds = TB[clamp(idx,0,19)]·60 / (tempo·24)
float delay_sync_seconds(int idx, float tempoBpm) {
  if (idx > 19) idx = 19;
  if (idx < 0) idx = 0;
  return (float)((double)(kDelayBeatTable[idx] * 60) / ((double)tempoBpm * 24.0));
}

// FUN_1800069e0 (DAT_180072e80 = 1/63f, verified in .rdata):
//   x ≤ 64: wet = 1, dry = (max(x,1)−1)/63   (dry 0 at x=0, 1 at x=64)
//   x > 64: wet = (127−x)/63 (1 at 64→0 at 127), dry = 1
float delay_wet_level(int x) {
  return (x > 64) ? (float)(127 - x) * (1.0f / 63.0f) : 1.0f;
}
float delay_dry_level(int x) {
  if (x > 64) return 1.0f;
  if (x == 0) x = 1;
  return (float)(x - 1) * (1.0f / 63.0f);
}

void Delay::setSampleRate(int sr) {
  sampleRate = sr;
  timeSamp = (int)((float)sr * timeSec);
  tapSlew = (float)(1.0 - std::pow(0.5, 1.0 / ((double)sr * 0.15)));
  recomputeTaps();
  if (toneFreq > 0.0f) setToneFilter(toneType, toneFreq);
}

void Delay::setTimeSeconds(float seconds) {
  timeSec = seconds;
  timeSamp = (int)((float)sampleRate * seconds);
  recomputeTaps();
}

void Delay::setTempoSyncBeatIndex(int idx, float tempoBpm) {
  setTimeSeconds(delay_sync_seconds(idx, tempoBpm));
}

void Delay::setFeedback(float fb) {
  // FUN_180006b10: clamp to [0, DAT_180072e90 = 0.999].
  feedback = (fb < 0.0f) ? 0.0f : (fb > DELAY_FB_MAX ? DELAY_FB_MAX : fb);
  recomputeTailBudget();  // FUN_180006b10 tail-calls FUN_180005ab0
}

void Delay::setDryWet(int x) {
  wet = delay_wet_level(x);
  dry = delay_dry_level(x);
}

void Delay::setSpread(float seconds) {
  spread = seconds;
  recomputeTaps();
}

void Delay::setMode(int m) {
  mode = m;
  recomputeTailBudget();  // FUN_180006ba0 tail-calls FUN_180005ab0
}

void Delay::setOn(int on) {
  // FUN_180006a80: rising edge clears both lines and the write cursor.
  if (on != 0 && onOff == 0) {
    writePos = 0;
    slewing = 0;
    rightLive = (double)rightTap;
    leftLive = (double)leftTap;
    lineL.fill(0.0f);
    lineR.fill(0.0f);
  }
  onOff = on;
}

void Delay::setToneParam(int x) {
  // FUN_180028b10: x = 64 → insert off; x < 64 → low shelf,
  // f = pow(110, x/63)·200; x > 64 → high shelf, f = pow(800, (x−65)/62)·10.
  if (x == 64) {
    setToneFilter(DELAY_TONE_OFF, 0.0f);
    return;
  }
  if (x < 64) {
    const float f = (float)(std::pow(110.0, (double)x * (1.0 / 63.0)) * 200.0);
    setToneFilter(DELAY_TONE_LOW_SHELF, f);
  } else {
    const float f = (float)(std::pow(800.0, (double)(x - 65) * (1.0 / 62.0)) * 10.0);
    setToneFilter(DELAY_TONE_HIGH_SHELF, f);
  }
}

void Delay::setToneFilter(int type, float freqHz) {
  toneType = type;
  toneFreq = freqHz;
  if (type == DELAY_TONE_OFF) return;
  // CEq (spec_lfo_fx.md §4.1/§4.2): setFreq clamps to ≥ 1 Hz; the shelf
  // kernels solve in double, store float.
  const double f = (freqHz > 1.0f) ? (double)freqHz : 1.0;
  const double w = f * 2.0 * 3.141592653589793;
  const double twoSr = (double)sampleRate * 2.0;
  const double k = 1.0 / (w + twoSr);
  toneA0 = (float)(w * k);          // type 1: a0 = a1 = 2πf/(2sr+2πf)
  toneA2 = (float)((twoSr - w) * k);
  if (type == DELAY_TONE_HIGH_SHELF) {
    toneA0 = (float)(twoSr * k);    // type 2: a0 = 2sr/(2sr+2πf), a1 = −a0
  }
}

// FUN_1800059c0 / FUN_100051a0 (decomp_all_32_full.c:3656-3662). Binary
// convention: +0x44/+0x60 ("rightTap") drives the L line, +0x48/+0x64
// ("leftTap") the R line; POSITIVE spread lands on +0x44/+0x60 (the L-line
// tap), negative on +0x48/+0x64 (the R-line tap). D1 (m3-d1-pingpong-2).
void Delay::recomputeTaps() {
  const int t = floor4(timeSamp);
  int right = t;
  int left = t;
  if (spread < 0.0f) {
    left = (int)(-spread * (float)sampleRate) + t;
  } else {
    right = (int)(spread * (float)sampleRate) + t;
  }
  const int lo = (int)((double)sampleRate * 0.0001);  // 0.1 ms
  const int hi = DELAY_LINE_SIZE - 256;
  right = std::clamp(right, lo, hi);
  left = std::clamp(left, lo, hi);
  rightTap = right;
  leftTap = left;
  recomputeTailBudget();  // FUN_1800059c0 → FUN_180005ab0 (ctor/setTime/setSpread)
  // FUN_1800059c0 tail: (re)snap the live taps; the process loop re-enables
  // slewing when the block starts lagging the latency guard (engine-only).
  rightLive = (double)rightTap;
  leftLive = (double)leftTap;
}

// FUN_180005ab0: how many frames of silent input the device keeps ringing
// before it gives up. periods = trunc(log(2^-15)/log(feedback)) + 2 (log(0)
// = -inf → 0 periods → the +2 floor), period = max(leftTap, rightTap),
// doubled in ping-pong mode (the round trip). If the silent counter has
// already run past the OLD budget it is pushed to newBudget+1 so the device
// stays alive one more frame across the parameter change.
void Delay::recomputeTailBudget() {
  const int old = tailBudget;
  const double num = std::log(DELAY_TAIL_EPS);
  const double den = std::log((double)feedback);
  double ratio = num / den;
  if (!(ratio > 0.0)) ratio = 0.0;  // feedback 0 or 1 → the +2 floor only
  int periods = (int)ratio + 2;     // cvttsd2si: truncate toward zero
  int period = (leftTap > rightTap) ? leftTap : rightTap;
  if (mode == DELAY_MODE_PINGPONG) period += period;
  tailBudget = periods * period;
  if (silentFrames > old) silentFrames = tailBudget + 1;
}

// one-pole shelf kernel over the tone scratch slice [off, off+count).
static inline void toneFilter(Delay& d, int off, int count) {
  if (d.toneType == DELAY_TONE_OFF) return;
  const float a0 = d.toneA0, a2 = d.toneA2;
  float x1l = d.toneX1L, x1r = d.toneX1R, y1l = d.toneY1L, y1r = d.toneY1R;
  if (d.toneType == DELAY_TONE_LOW_SHELF) {
    for (int i = off; i < off + count; i++) {
      const float xl = d.toneL[i], xr = d.toneR[i];
      y1l = a0 * (xl + x1l) + a2 * y1l;
      y1r = a0 * (xr + x1r) + a2 * y1r;
      x1l = xl; x1r = xr;
      d.toneL[i] = y1l;
      d.toneR[i] = y1r;
    }
  } else {
    for (int i = off; i < off + count; i++) {
      const float xl = d.toneL[i], xr = d.toneR[i];
      y1l = a0 * (xl - x1l) + a2 * y1l;
      y1r = a0 * (xr - x1r) + a2 * y1r;
      x1l = xl; x1r = xr;
      d.toneL[i] = y1l;
      d.toneR[i] = y1r;
    }
  }
  d.toneX1L = x1l; d.toneX1R = x1r;
  d.toneY1L = y1l; d.toneY1R = y1r;
}

// FUN_180006090 (normal/crossed), one sub-block of `count` frames starting at
// global frame offset `off`. The read stage writes the taps into the tone
// scratch (the decompiled param_4, NOT the out buffer — the dry input stays
// in out and re-fills the line at the write cursor).
int Delay::processNormal(float* left, float* right, int off, int count) {
  const int w = writePos;
  if (slewing == 0) {
    // integer-tap path: reads never reach the write cursor (count ≤ min tap).
    for (int i = 0; i < count; i++) {
      toneL[off + i] = lineL[(w - rightTap + i) & DELAY_LINE_MASK];
      toneR[off + i] = lineR[(w - leftTap + i) & DELAY_LINE_MASK];
    }
  } else {
    // fractional path: taps slewed per sample, 2-point linear interpolation.
    double pr = rightLive, pl = leftLive;
    const float k = tapSlew;
    for (int i = 0; i < count; i++) {
      pr += ((double)rightTap - pr) * (double)k;
      pl += ((double)leftTap - pl) * (double)k;
      float fr = (float)((double)(unsigned)(i + w) - pr);
      if (fr < 0.0f) fr += 131072.0f;
      float fl = (float)((double)(unsigned)(i + w) - pl);
      if (fl < 0.0f) fl += 131072.0f;
      const unsigned ir = (unsigned)fr, il = (unsigned)fl;
      const float aL = lineL[il & DELAY_LINE_MASK];
      const float bL = lineL[(il + 1) & DELAY_LINE_MASK];
      const float aR = lineR[ir & DELAY_LINE_MASK];
      const float bR = lineR[(ir + 1) & DELAY_LINE_MASK];
      toneL[off + i] = aL + (bL - aL) * (fl - (float)il);
      toneR[off + i] = aR + (bR - aR) * (fr - (float)ir);
    }
    rightLive = pr;
    leftLive = pl;
  }
  // tone inserts over the tone buffer (the taps).
  toneFilter(*this, off, count);
  // write-back / feedback: line[n] = out[n] + feedback·tone[n−tap] (+tone EQ);
  // crossed mode swaps L↔R in BOTH terms.
  const float fb = feedback;
  if (mode == DELAY_MODE_NORMAL) {
    for (int i = 0; i < count; i++) {
      const unsigned p = (unsigned)(w + i) & DELAY_LINE_MASK;
      lineL[p] = fb * toneL[off + i] + left[off + i];
      lineR[p] = fb * toneR[off + i] + right[off + i];
    }
  } else {
    for (int i = 0; i < count; i++) {
      const unsigned p = (unsigned)(w + i) & DELAY_LINE_MASK;
      lineL[p] = fb * toneR[off + i] + right[off + i];
      lineR[p] = fb * toneL[off + i] + left[off + i];
    }
  }
  writePos = (int)((unsigned)w + (unsigned)count) & DELAY_LINE_MASK;
  return count;
}

// FUN_180005b20 / FUN_10005280 (ping-pong, decomp_all_32_full.c:3836-3846):
// taps go to the tone scratch (L line at rightTap, R line at leftTap), the
// inserts run over them, then the write-back is CROSSED (D1, m3-d1-pingpong-2):
//   lineL[n] = (outL[n] + outR[n])·0.5 + feedback·toneR[n]
//   lineR[n] = toneL[n]
// Loop closure: lineL[n] = mix[n] + fb·lineL[n − rightTap − leftTap]
// (round trip), both channels live; R = L delayed by the round trip.
int Delay::processPingPong(float* left, float* right, int off, int count) {
  const int w = writePos;
  if (slewing == 0) {
    for (int i = 0; i < count; i++) {
      toneL[off + i] = lineL[(w - rightTap + i) & DELAY_LINE_MASK];
      toneR[off + i] = lineR[(w - leftTap + i) & DELAY_LINE_MASK];
    }
  } else {
    double pr = rightLive, pl = leftLive;
    const float k = tapSlew;
    for (int i = 0; i < count; i++) {
      pr += ((double)rightTap - pr) * (double)k;
      pl += ((double)leftTap - pl) * (double)k;
      float fr = (float)((double)(unsigned)(i + w) - pr);
      if (fr < 0.0f) fr += 131072.0f;
      float fl = (float)((double)(unsigned)(i + w) - pl);
      if (fl < 0.0f) fl += 131072.0f;
      const unsigned ir = (unsigned)fr, il = (unsigned)fl;
      const float aL = lineL[ir & DELAY_LINE_MASK];
      const float bL = lineL[(ir + 1) & DELAY_LINE_MASK];
      const float aR = lineR[il & DELAY_LINE_MASK];
      const float bR = lineR[(il + 1) & DELAY_LINE_MASK];
      toneL[off + i] = aL + (bL - aL) * (fr - (float)ir);
      toneR[off + i] = aR + (bR - aR) * (fl - (float)il);
    }
    rightLive = pr;
    leftLive = pl;
  }
  toneFilter(*this, off, count);
  const float fb = feedback;
  for (int i = 0; i < count; i++) {
    const unsigned p = (unsigned)(w + i) & DELAY_LINE_MASK;
    lineL[p] = (left[off + i] + right[off + i]) * 0.5f + fb * toneR[off + i];
    lineR[p] = toneL[off + i];
  }
  writePos = (int)((unsigned)w + (unsigned)count) & DELAY_LINE_MASK;
  return count;
}

// FUN_180006810: silent-input gate, sub-block loop (split at tap
// discontinuities), tap convergence, wet/dry mix. Returns the frames processed.
int Delay::process(float* left, float* right, int numFrames, int* ioSilent) {
  if (onOff == 0) {  // bypass: the block passes through untouched
    outActive = 0;   // FUN_180006810: +0x40 == 0 → +0x20 = 0
    return 0;
  }
  if (ioSilent != nullptr) {
    if (*ioSilent == 0) {
      silentFrames = 0;  // real input present: the tail counter restarts
    } else {
      if (tailBudget < silentFrames) {
        if (slewing == 0) {  // ring-out exhausted: report the output silent
          outActive = 0;
          return 0;
        }
      } else {
        silentFrames += numFrames;
      }
      *ioSilent = 0;  // keep the rest of the chain alive through the tail
      for (int i = 0; i < numFrames; i++) left[i] = 0.0f, right[i] = 0.0f;
    }
  }
  int total = 0;
  while (total < numFrames) {
    int remaining = numFrames - total;
    if (remaining > DELAY_MAX_FRAMES) remaining = DELAY_MAX_FRAMES;
    int count = remaining;
    if (slewing == 0) {
      // reads start at (w − tap); keep every sub-block within min(tap) so a
      // read never touches data written by this block.
      const int m = std::min(leftTap, rightTap);
      if (count > m) count = m;
    } else {
      count = 1;  // per-sample tap slewing (see delay.h deviations)
    }
    if (mode == DELAY_MODE_PINGPONG) {
      count = processPingPong(left, right, total, count);
    } else {
      count = processNormal(left, right, total, count);
    }
    total += count;
    if (slewing != 0) {
      if (std::fabs(rightLive - (double)rightTap) < DELAY_TAP_EPS) rightLive = (double)rightTap;
      if (std::fabs(leftLive - (double)leftTap) < DELAY_TAP_EPS) leftLive = (double)leftTap;
      if (rightLive == (double)rightTap && leftLive == (double)leftTap) slewing = 0;
    }
  }
  // dry/wet (FUN_180006810 tail): the out buffer carries the dry signal
  // (normal/crossed: line[n] = dry[n] + fb·tone(n−tap); ping-pong likewise
  // per §2.3.2) and the tone scratch carries tone(taps).
  if (chainMode) {
    // Binary chain semantics: FUN_18001b5d0(out, wet) and
    // FUN_18001b5d0(toneBuf, dry) — the chain buffer keeps ONLY wet·input;
    // the dry·tone taps stay in the tone buffer for the chorus insert
    // (added directly when the chorus is off, ring-fed when on).
    for (int i = 0; i < numFrames; i++) {
      left[i] *= wet;
      right[i] *= wet;
      toneL[i] *= dry;
      toneR[i] *= dry;
    }
  } else {
    // standalone device: out = wet·dryInput + dry·tone(taps).
    for (int i = 0; i < numFrames; i++) {
      left[i] = wet * left[i] + dry * toneL[i];
      right[i] = wet * right[i] + dry * toneR[i];
    }
  }
  outActive = 1;  // FUN_180006810 tail: the output is live again
  return numFrames;
}

}  // namespace synth1::fx
