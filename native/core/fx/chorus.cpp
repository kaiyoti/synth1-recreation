// chorus.cpp — CChorus "Chorus/Flanger" implementation.
//
// Spec: out/spec_lfo_fx.md §3. Ghidra: FUN_1800034c0 (ctor), FUN_1800036d0
// (process), FUN_180003f00/FUN_180003f90/FUN_180004110 (setters),
// FUN_1800040b0/FUN_1800040e0/FUN_180004100/FUN_180004060.
// See chorus.h for the mode-3 blend weights (read from the decompiled loop)
// and the documented port deviations.

#include "fx/chorus.h"

#include <cmath>

namespace synth1::fx {

namespace {

constexpr float INV65536 = 1.0f / 65536.0f;  // DAT_180072b50

// 16.16 tap-offset formation (the shared preamble of every mode):
// u = ROUND(D·s) + readPos<<16  →  idx = (u & 0x1fffffff) >> 16,
// frac16 = (u & 0xffff).
// m3-176-land law (2): the device forms the per-sample 16.16 product with
// an x87 fistp — ROUND, ties-to-even (FUN_10003170, out/decomp_all_32_full.c
// :2100 `(int)ROUND(s · fVar15)`; objdump 1000350a/10003514: mulss →
// movss → flds → fistpl). The live core truncated the product toward zero.
struct Tap {
  int idx;
  std::uint32_t frac16;
};

inline Tap tap16(float dqn16, float s, unsigned rp) {
  const std::int32_t u =
      (std::int32_t)std::nearbyint((double)(dqn16 * s)) +
      (std::int32_t)(rp << 16);
  Tap t;
  t.idx = (int)(((std::uint32_t)u & 0x1FFFFFFFu) >> 16);
  const std::uint32_t f = (std::uint32_t)u & 0xFFFFu;
  t.frac16 = f;
  return t;
}

// m3-176-land law (3): 2-tap linear interpolation in the device's
// byte-verified SSE f32 form (FUN_10003170 mode-2 loop 10003545-10003578:
// subss, mulss frac16, mulss 2^-16, addss — one f32 rounding each on d, p,
// sum; the ·2^-16 = DAT_100635d8 scaling is exact):
//   tap = f32(f32(f32(b−a)·f32(frac16)·2⁻¹⁶) + a)
// The live core formed frac = (float)(frac16·2⁻¹⁶) first (a 4-op form with
// a different rounding pattern).
inline float lerp16(float a, float b, std::uint32_t f16) {
  const float d = b - a;
  const float p = d * (float)f16;
  const float t = p * 0.0000152587890625f;
  return t + a;
}

}  // namespace

void Chorus::setSampleRate(int sr) {
  sampleRate = sr;
  setBaseDelaySeconds(baseDelaySec);  // re-derives baseDelay/depth samples + writePos
  setRate(rateHz);
}

// FUN_180003f00: baseDelaySec → baseDelaySamples; writePos = readPos + base;
// re-derives depth; recomputes the depth normaliser.
void Chorus::setBaseDelaySeconds(float s) {
  baseDelaySec = s;
  baseDelaySamples = (int)((float)sampleRate * s);
  setDepth(depthParam);
  writePos = (readPos + (unsigned)baseDelaySamples) & CHORUS_LINE_MASK;
  recomputeTailBudget();  // FUN_180003f00 → FUN_180003610
}

// FUN_180003f90: depthSamples = (int)(baseDelaySamples·depth);
// depthStep = clamp((lfoPeriod/depthSamples)/100, ·, 6 floor);
// norm = min(1, lfoPeriod/(2π·depthSamples)).
void Chorus::setDepth(float depth) {
  depthParam = depth;
  depthSamples = (int)((float)baseDelaySamples * depth);
  const int ds = (depthSamples < 1) ? 1 : depthSamples;
  int step = (lfoPeriod / ds) / 100;
  depthStep = (step < 6) ? 6 : step;
  const double span = (double)depthSamples * 2.0 * 3.141592653589793;
  depthNorm = (span <= (double)lfoPeriod) ? 1.0f : (float)((double)lfoPeriod / span);
}

// FUN_180004110: lfoPeriod = (int)(sr/rate); k = 2·sin(π·rate/sr).
void Chorus::setRate(float hz) {
  rateHz = hz;
  lfoPeriod = (int)((float)sampleRate / hz);
  const int ds = (depthSamples < 1) ? 1 : depthSamples;
  int step = (lfoPeriod / ds) / 100;
  depthStep = (step < 6) ? 6 : step;
  phaseStep = (float)(2.0 * std::sin((double)hz * 3.141592653589793 / (double)sampleRate));
  const double span = (double)depthSamples * 2.0 * 3.141592653589793;
  depthNorm = (span <= (double)lfoPeriod) ? 1.0f : (float)((double)lfoPeriod / span);
}

// engine param 54 (dispatcher case 0x36, FUN_180025a60): the PIECEWISE law
//   rate = (x == 0) ? 0.01 Hz : 0.02 · pow(20000, x/127) Hz
// (DAT_180075780 = 20000, DAT_1800733a0 = 0.02; FUN_180057cc0 = pow).
// x = 0 is NOT on the curve: the 32-bit dispatcher case 54 seeds
// dVar7 = DAT_100639e8 = 0.01 and overwrites it only when x != 0
// (decomp_all_32_full.c:29579-29586; 64-bit twin DAT_180073398 = 0.01,
// decomp_all.c:29201-29207). x = 127 → 400 Hz;
// the plugin default 50 → 0.99 Hz.
void Chorus::setRateParam(int x) {
  if (x == 0) {
    setRate(0.01f);
  } else {
    // m3-176-land law (4): the device computes the pow EXPONENT in f32 —
    // (int)param · DAT_10063520, DAT_10063520 = f32(1/127) = 0.007874016
    // (dispatcher case 54, decomp_all_32_full.c:29579-29586). The engine
    // used a double exponent, landing 1 f32-ULP low on some params
    // (x=122: rate 0x43876cbe vs device 0x43876cbf → phaseStep 1 ULP low).
    const double exp32 = (double)((float)x * (float)(1.0 / 127.0));
    setRate((float)(0.02 * std::pow(20000.0, exp32)));
  }
}

// FUN_1800040b0
void Chorus::setFeedback(float fb) {
  feedback = fb;
  feedbackOn = (fb == 0.0f) ? 0 : 1;
  recomputeTailBudget();  // FUN_1800040b0 tail-calls FUN_180003610
}

// FUN_180004100 (+0x64)
void Chorus::setLevel(float l) {
  level = l;
}

// Continuous dry/wet crossfade (law = the delay's FUN_1800069e0, 1.0 at
// x = 64): wet = level (the added modulated taps), dry = dryGain.
void Chorus::setMix(int x) {
  dryGain = (x > 64) ? 1.0f : ((x == 0) ? 0.0f : (float)(x - 1) * (1.0f / 63.0f));
  level = (x > 64) ? (float)(127 - x) * (1.0f / 63.0f) : 1.0f;
}

// FUN_1800040e0: (m - 1) & ~3 == 0 keeps m (1, 2, 4); everything else
// (including 3 and >= 5) falls back to 2.
void Chorus::setMode(int m) {
  // FUN_1800040e0: mode = m; if ((m-1) & ~3) mode = 2; else if (m == 3) mode = 2.
  // Accepted: 1, 2, 4. 3 and everything >= 5 fall back to 2.
  mode = ((((m - 1) & ~3) == 0 && m != 3) ? m : CHORUS_MODE_STEREO);
}

// FUN_180004060: rising edge zeroes the whole 8192-frame line.
void Chorus::setOn(int on) {
  if (on != 0 && onOff == 0) lineL.fill(0.0f), lineR.fill(0.0f);
  onOff = on;
}

void Chorus::setToneInput(const float* toneL, const float* toneR) {
  toneInL = toneL;
  toneInR = toneR;
}

// FUN_180003610: periods = trunc(log(2^-15)/log|feedback|) + 2 (log(0) = -inf
// → 0 periods → the +2 floor), budget = periods · baseDelaySamples. If the
// silent counter already ran past the OLD budget it is pushed to newBudget+1,
// otherwise it is reset to 0 (the delay's version leaves it alone).
void Chorus::recomputeTailBudget() {
  const int old = tailBudget;
  const double num = std::log(CHORUS_TAIL_EPS);
  const double den = std::log((double)std::fabs(feedback));
  double ratio = num / den;
  if (!(ratio > 0.0)) ratio = 0.0;
  const int periods = (int)ratio + 2;  // cvttsd2si: truncate toward zero
  tailBudget = periods * baseDelaySamples;
  if (silentFrames > old) silentFrames = tailBudget + 1;
  else silentFrames = 0;
}

// FUN_1800036d0 (device-chain machinery omitted; see chorus.h deviations).
//
// Block-size invariance: the binary writes the whole host block into the ring
// before the tap loop reads it, so a tap that reaches into the freshly written
// region sees the pre-feedback value — a span (and therefore a result) that
// depends on the host block size. The port runs the same write→read→feedback
// sequence in fixed CHORUS_SUB_BLOCK_FRAMES sub-blocks counted from the start
// of the caller's window, so whenever every caller window is a multiple of the
// sub-block size (bs 64/128/256/512) the absolute partition — and with it the
// write-ahead span of every frame — is identical: the engine is bit-identical
// across those block sizes while keeping the binary's write-ahead semantics.
int Chorus::process(float* left, float* right, int numFrames, int* ioSilent) {
  if (onOff == 0) {
    // FUN_1800036d0 off path: with the delay tone insert wired and active
    // (**(int**)(+0x70) != 0) the tone buffer is ADDED straight to the
    // output (FUN_18001acd0) — the delay's wet·input already sits in the
    // chain buffer, so the delay stays audible with the chorus off.
    if (toneInL != nullptr && toneInR != nullptr) {
      for (int i = 0; i < numFrames; i++) {
        left[i] += toneInL[i];
        right[i] += toneInR[i];
      }
    }
    return 0;  // bypass: the block otherwise passes through untouched
  }

  // FUN_1800036d0 silent-input gate. The delay upstream clears its own
  // ioSilent while it is still ringing, so the chorus only starts counting
  // once the delay has given up (flag set and the delay's output inactive).
  if (ioSilent != nullptr) {
    if (*ioSilent == 0 || delayActive) {
      silentFrames = 0;
    } else {
      if (tailBudget < silentFrames) return numFrames;  // ring-out exhausted
      silentFrames += numFrames;
      // FUN_10003170 silent-window fill (m3-177-land): objdump 10003220
      // `mov $0x3089705f,%eax` + 10003227 `rep stos` over 2·frames dwords —
      // the window is filled with f32 0x3089705f (= 1.0e-9f, −180 dBFS),
      // NOT 0.0f; the fill is what the ring write then sees.
      for (int i = 0; i < numFrames; i++) left[i] = right[i] = CHORUS_SILENT_FILL;
    }
  }

  // 2. quadrature-oscillator energy clamp — ONCE PER CHAIN CALL, 256-FRAME
  // GRID (law #93, m3-177-land re-check): FUN_10003170 checks it at the
  // head of every chain call, after the silent gate and before the tap
  // loops. The device chain pulls the FX devices in ≤256-frame calls
  // (mixer FUN_100135b0 caps the chunk at 0x100, decomp_all_32_full.c:
  // 14545-14548; runtime: silentFrames steps by 256 at the flag edge, and
  // the clamp fires at a 256-frame boundary, bit-exact at blk174 of
  // p55_v0). Mirror the check at every 256-frame boundary inside the
  // window; the per-sample phasor rotation inside the sub-blocks is
  // unchanged, and 256 tiles the 512-frame host block so the write→read→
  // feedback span invariance above still holds.
  if (oscS1 > CHORUS_OSC_CLAMP) {
    oscS0 = 0.0f;
    oscS1 = CHORUS_OSC_CLAMP;
  }

  int done = 0;
  while (done < numFrames) {
    if (done > 0 && oscS1 > CHORUS_OSC_CLAMP) {
      oscS0 = 0.0f;
      oscS1 = CHORUS_OSC_CLAMP;
    }
    int n = numFrames - done;
    if (n > CHORUS_CHAIN_CHUNK_FRAMES) n = CHORUS_CHAIN_CHUNK_FRAMES;
    int sub = 0;
    while (sub < n) {
      int m = n - sub;
      if (m > CHORUS_SUB_BLOCK_FRAMES) m = CHORUS_SUB_BLOCK_FRAMES;
      processSubBlock(left + done + sub, right + done + sub,
                      toneInL ? toneInL + done + sub : nullptr,
                      toneInR ? toneInR + done + sub : nullptr, m);
      sub += m;
    }
    done += n;
  }
  return numFrames;
}

int Chorus::processSubBlock(float* left, float* right, const float* toneL,
                            const float* toneR, int numFrames) {
  // 1. write the dry input (plus the shared delay tone when wired) into the
  //    ring at the write cursor, in two chunks to handle the 8192 wrap.
  {
    int first = CHORUS_LINE_SIZE - (int)writePos;
    if (numFrames < first) first = numFrames;
    for (int i = 0; i < first; i++) {
      const float tl = toneL ? toneL[i] : 0.0f;
      const float tr = toneR ? toneR[i] : 0.0f;
      lineL[writePos + i] = left[i] * level + tl;
      lineR[writePos + i] = right[i] * level + tr;
    }
    for (int i = first; i < numFrames; i++) {
      const int j = i - first;
      const float tl = toneL ? toneL[i] : 0.0f;
      const float tr = toneR ? toneR[i] : 0.0f;
      lineL[j] = left[i] * level + tl;
      lineR[j] = right[i] * level + tr;
    }
  }

  // 3. per-sample tap generation
  // m3-176-land law (1): the device forms the integer depth dq with
  // `(int)ROUND(fVar15) << 0x10` (x87 fistp, ties-to-even) on the f32
  // product fVar15 = (float)depthInt · depthNorm (FUN_10003170,
  // out/decomp_all_32_full.c:2046 + :2089/:2127). The live core truncated:
  // at p54=127 61·0.287000716 = 17.507 → device 18, engine 17 (the −23 dB
  // high-rate cells; at p54=0 depthNorm = 1 → 61 both, bit-identical).
  const std::int32_t dq =
      (std::int32_t)std::nearbyint((double)((float)depthSamples * depthNorm));
  const float dqn16 = (float)(std::uint32_t)(dq << 16);  // depth in 16.16
  const float k = phaseStep;
  const float fb = feedback;
  unsigned rp = readPos;
  unsigned wp = writePos;

  if (mode == CHORUS_MODE_MONO) {
    // mode 1: one shared tap (s0 only), stereo line.
    for (int i = 0; i < numFrames; i++) {
      const float c = oscS0 - k * oscS1;
      oscS0 = c;
      oscS1 = c * k + oscS1;
      const Tap t = tap16(dqn16, oscS0, rp);
      const float tl = lerp16(lineL[t.idx], lineL[(t.idx + 1) & CHORUS_LINE_MASK], t.frac16);
      const float tr = lerp16(lineR[t.idx], lineR[(t.idx + 1) & CHORUS_LINE_MASK], t.frac16);
      left[i] = left[i] * dryGain + tl;
      right[i] = right[i] * dryGain + tr;
      lineL[wp] += tl * fb;
      lineR[wp] += tr * fb;
      rp = (rp + 1) & CHORUS_LINE_MASK;
      wp = (wp + 1) & CHORUS_LINE_MASK;
    }
  } else if (mode == CHORUS_MODE_STEREO) {
    // mode 2 (ctor default): L tapped from s0, R from s1 (quadrature).
    for (int i = 0; i < numFrames; i++) {
      const float c = oscS0 - k * oscS1;
      oscS0 = c;
      oscS1 = c * k + oscS1;
      const Tap tL = tap16(dqn16, oscS0, rp);
      const Tap tR = tap16(dqn16, oscS1, rp);
      const float tl = lerp16(lineL[tL.idx], lineL[(tL.idx + 1) & CHORUS_LINE_MASK], tL.frac16);
      const float tr = lerp16(lineR[tR.idx], lineR[(tR.idx + 1) & CHORUS_LINE_MASK], tR.frac16);
      left[i] += tl;
      right[i] += tr;
      lineL[wp] += tl * fb;
      lineR[wp] += tr * fb;
      rp = (rp + 1) & CHORUS_LINE_MASK;
      wp = (wp + 1) & CHORUS_LINE_MASK;
    }
  } else {
    // mode 3: four taps per channel — the direct taps (T1, T2) and their
    // mirrors about the read cursor (T0, T3), cross-blended (weights from
    // the decompiled loop; only the direct taps are fed back).
    for (int i = 0; i < numFrames; i++) {
      const float c = oscS0 - k * oscS1;
      oscS0 = c;
      oscS1 = c * k + oscS1;
      const Tap tA = tap16(dqn16, oscS0, rp);
      const Tap tB = tap16(dqn16, oscS1, rp);
      const unsigned mA = (rp * 2u - (unsigned)tA.idx) & CHORUS_LINE_MASK;
      const unsigned mB = (rp * 2u - (unsigned)tB.idx) & CHORUS_LINE_MASK;
      const float T0 = lerp16(lineL[(mA - 1) & CHORUS_LINE_MASK], lineL[mA], 0x10000u - tA.frac16);
      const float T1 = lerp16(lineL[tA.idx], lineL[(tA.idx + 1) & CHORUS_LINE_MASK], tA.frac16);
      const float T2 = lerp16(lineR[tB.idx], lineR[(tB.idx + 1) & CHORUS_LINE_MASK], tB.frac16);
      const float T3 = lerp16(lineR[(mB - 1) & CHORUS_LINE_MASK], lineR[mB], 0x10000u - tB.frac16);
      const float oL = left[i];
      const float oR = right[i];
      left[i] = T3 * 0.3f + T1 * 0.9f + T0 * 0.1f + T2 * 0.7f + oL;
      right[i] = T2 * 0.3f + T1 * 0.1f + T0 * 0.9f + T3 * 0.7f + oR;
      lineL[wp] += T1 * fb;
      lineR[wp] += T2 * fb;
      rp = (rp + 1) & CHORUS_LINE_MASK;
      wp = (wp + 1) & CHORUS_LINE_MASK;
    }
  }

  // 4. cursors
  readPos = (readPos + (unsigned)numFrames) & CHORUS_LINE_MASK;
  writePos = (writePos + (unsigned)numFrames) & CHORUS_LINE_MASK;
  return numFrames;
}

}  // namespace synth1::fx
