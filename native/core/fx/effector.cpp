// effector.cpp — CEffector multi-FX router + sub-effect implementations.
//
// Spec: out/spec_lfo_fx.md §5. See effector.h for the verified constants and
// the documented port deviations (compressor / phaser-sweep reconstructions;
// curve-0 cascade landed m3-u1-0b, tone biquad cascade landed m3-u1-0c).

#include "fx/effector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace synth1::fx {

namespace {
inline constexpr double PI = 3.14159265358979323846;
inline constexpr float DIST_EPS = 1e-4f;   // DAT_180072fe0
inline constexpr float DIST_A1 = 0.4f;     // DAT_180072fec (a.d.1 shape)
inline constexpr float DIST_TWO = 2.0f;    // DAT_180073000
inline constexpr float PH_FB_SPAN = 0.99f; // DAT_180073390
inline constexpr float PH_DRYWET = 0.5f;   // +0x24, ctor-only writer (#104)
inline const int kPhaserPairs[4] = {1, 2, 4, 6};

// one-sided cubic saturator f0(v) (curve 0), v >= 0.
inline float f0(float v) {
  return 1.0f - 1.0f / (1.0f + DIST_TWO * v * v * (v + DIST_A1));
}
// curve-0 stage gate, byte op order (FUN_100070d0:4950-4953):
// DAT_10063778 / ((v + DAT_1006389c) * v * v * DAT_100638b0 + DAT_10063524)
//   + DAT_10063524 ; .rdata: 0x10063778 = -1.0f, 0x10063524 = 1.0f.
inline float gate0(float v) {
  return -1.0f / (((v + DIST_A1) * v * v) * DIST_TWO + 1.0f) + 1.0f;
}
// symmetric cubic saturator f1(t) (curve-2 legacy path, NOT the device
// curve-2 kernel - #146 open), t >= 0.
inline float f1(float t) {
  return 1.0f - 1.0f / (1.0f + t * t * (t + 1.0f));
}

// Compressor .rdata pins (install/Synth1 VST.dll 9be31edeb5a5, all float32;
// file offsets = VA - 0x10001a00):
//   0x10063730 = 0x3debc8e3 =  0.115129254758358f = ln(10)/20 (DAT_10063730)
//   0x10063748 = 0x40135d8e =  2.3025851249694824f = ln(10)    (DAT_10063748)
//   0x10063758 = 0x41f00000 =  30.0f                          (DAT_10063758)
//   0x1006375c = 0x42200000 =  40.0f                          (DAT_1006375c)
//   0x10063760 = 0xbdebc8e3 = -0.115129254758358f             (DAT_10063760)
//   0x10063778 = 0xbf800000 = -1.0f                           (DAT_10063778)
//   0x10063790 = 0xc1a00000 = -20.0f                          (DAT_10063790)
//   0x10063520 = 0x3c010204 =  1/127f                         (DAT_10063520)
//   0x10063620 = 0x7fffffff                                   (DAT_10063620)
//   0x10063888 = 0x38d1b717 =  1e-4f  (DAT_10063888, curve EPS)
//   0x10063524 = 0x3f800000 =  1.0f                           (DAT_10063524)
// pow/exp step: FUN_1004ccb3 (:55360-55389) is the MSVC CRT float exp
// special-case tail (range bounds 0x42aeac4f/0x42b17217 = ln(FLT_MAX) class);
// the sites fold ln(10)/20 into the exponent and call expf, i.e. 10^p ==
// expf(p * ln10/20f). Double std::exp of the float32 exponent reproduces the
// float32 result (phaser precedent: do NOT use GCC float expf, 1 ulp off).
inline constexpr float COMP_LN10_20 = 0.115129254758358f;   // DAT_10063730
inline constexpr float COMP_LN10 = 2.3025851249694824f;     // DAT_10063748
inline constexpr float COMP_THR_SPAN = 40.0f;               // DAT_1006375c
inline constexpr float COMP_THR_BASE = -20.0f;              // DAT_10063790
}  // namespace

// curve-1 saturator f1d(t) = 1 - 1/(1 + t*(t+1)^2), byte op order
// (FUN_10007300 *this==1, decomp_all_32_full.c:5034-5043):
//   fVar9 = t + DAT_10063524(1);  fVar9*fVar9*fVar8(t) + 1;
//   DAT_10063778(-1)/(...) + 1.
float distortion_f1d(float t) {
  return -1.0f / ((t + 1.0f) * (t + 1.0f) * t + 1.0f) + 1.0f;
}

// ctl1 -> drive = 10^((70*ctl1/127 - 10)*0.05) = 0.3162 .. 1000.
// Bytes: 32-bit FUN_10007e30 (decomp_all_32_full.c:5436-5460) =
// pow(10, (ctl1*DAT_100638a0*_DAT_100638d0 - DAT_100638c0)*_DAT_100638a8);
// .rdata pinned in install/Synth1 VST.dll (9be31edeb5a5):
// 0x100638a0=1/127, 0x100638a8=0.05, 0x100638c0=10.0, 0x100638d0=70.0.
// 64-bit twin FUN_180008b40 (decomp_all.c:5518-5533):
// 180072ff0=1/127, 180073028=70.0, 180073010=10.0, 180072ff8=0.05.
// Runtime-pinned: device drive 18.356062 at ctl1=64 (m3-u1-effector F6).
// Kernel reads the FLOAT at CDistortion+0xc (FUN_100070d0:4944) - no int
// quantization.
float distortion_drive(int ctl1raw) {
  const double u = (double)ctl1raw * (1.0 / 127.0);
  return (float)std::pow(10.0, (70.0 * u - 10.0) * 0.05);
}

// ---------------------------------------------------------------------------
// CDistortion
// ---------------------------------------------------------------------------
void Distortion::setSampleRate(int sr) {
  sampleRate = sr;
  setToneCut(toneCut);  // re-derive the tone coefficient at the new sr
}

void Distortion::setCurve(int c) {
  curve = (c < 0) ? 0 : (c > 2 ? 2 : c);
  recomputeNorm();
}

void Distortion::setDrive(int ctl1raw) {
  drive = distortion_drive(ctl1raw);
  recomputeNorm();
}

void Distortion::setLevel(int raw) {
  level = ((float)raw + 2.0f) / 129.0f;  // 0.0155 .. 1.0
}

// ctl2 -> tone fc = (float)(400.0 * exp(log(40.0)*ctl2/127)) Hz.
// Bytes FUN_10006c00 (decomp_all_32_full.c:4739-4802; raw disasm
// 0x10006c00-0x10006de7): u = +0x14c * DAT_100638a0 (1/127 double);
// d = FUN_1004beeb(40.0=0x100638c8) then mulsd u == exp(log(40)*u)
// (finiteness check FUN_1004ca6a, pow fallback FUN_1004ce0a);
// fc = (float)(d * _DAT_100638e0=400.0). Runtime-pinned: fc(64) =
// 2566.83 Hz (F6 coeffs reproduce exactly, m3-u1-0c-tone F2).
float distortion_tone_fc(int ctl2raw) {
  const double u = (double)ctl2raw * (1.0 / 127.0);
  return (float)(TONE_FC_BASE * std::exp(std::log(TONE_FC_SPAN) * u));
}

// Tone biquad coefficients, byte op order (FUN_10006c00 disasm):
// section 0 (lowpass, fc from ctl2): c=cosf(w), s=sinf(w),
//   w = (float)(2pi*(double)fc/sr) [0x100638b8=2pi]; den = 1+s*(1/6f)
//   [0x10063894]; inv = 1/den;
//   b0(+0x2c)=b2(+0x4c)=(0.5f*(1-c))*inv [0x100637f8=0.5f];
//   b1(+0x3c)=inv*(1-c); a1(+0x6c)=(-2f*c)*inv [0x10063924=-2.0f];
//   a2(+0x7c)=(1-s*(1/6))*inv.
// section 1 (fixed 100 Hz highpass): w2 = (float)(628.3185307179587/sr)
//   [0x100638f8]; c2=cosf(w2), s2=sinf(w2), den2 = 1+s2*0.21739131f
//   [0x10063898]; inv2 = 1/den2;
//   b0(+0x30)=b2(+0x50)=(0.5f*(1+c2))*inv2; b1(+0x40)=-(1+c2)*inv2
//   (sign flip via xor 0x80000000 at 0x10063850); a1(+0x70)=(-2f*c2)*inv2;
//   a2(+0x80)=(1-s2*0.21739131f)*inv2.
// cosf/sinf: FUN_1004d8b2 / FUN_1004e0b2 (MSVC polynomial sin/cos).
// F6 runtime pins reproduced at ctl2=64 (sr 44100): +0x2c 0.031205,
// +0x30 0.996862, +0x40 -1.993724, +0x6c -1.762679, +0x7c 0.887501,
// +0x80 0.993825.
void Distortion::setToneCut(int raw) {
  toneCut = raw;
  const double sr = (double)sampleRate;
  const float fc = distortion_tone_fc(raw);
  const float w = (float)(2.0 * PI * (double)fc / sr);
  const float c = std::cos(w), s = std::sin(w);
  const float den = 1.0f + s * TONE_S0_K;
  const float inv = 1.0f / den;
  toneS0.b0 = toneS0.b2 = (0.5f * (1.0f - c)) * inv;
  toneS0.b1 = inv * (1.0f - c);
  toneS0.a1 = (-2.0f * c) * inv;
  toneS0.a2 = (1.0f - s * TONE_S0_K) * inv;
  const float w2 = (float)(TONE_HP_W / sr);
  const float c2 = std::cos(w2), s2 = std::sin(w2);
  const float den2 = 1.0f + s2 * TONE_S1_K;
  const float inv2 = 1.0f / den2;
  toneS1.b0 = toneS1.b2 = (0.5f * (1.0f + c2)) * inv2;
  toneS1.b1 = -(1.0f + c2) * inv2;
  toneS1.a1 = (-2.0f * c2) * inv2;
  toneS1.a2 = (1.0f - s2 * TONE_S1_K) * inv2;
}

// One sample of the 2-section cascade (FUN_10006ff0 :4883-4917), float32,
// byte op order: y = ((b1*x1 + b0*x + b2*x2) - a1*y1) - a2*y2; state
// x2=x1, x1=x, y2=y1, y1=y; section 0 output feeds section 1.
float Distortion::toneStep(ToneState& s0, ToneState& s1, float x) const {
  float y = ((toneS0.b1 * s0.x1 + toneS0.b0 * x + toneS0.b2 * s0.x2) -
             toneS0.a1 * s0.y1) - toneS0.a2 * s0.y2;
  s0.x2 = s0.x1; s0.x1 = x; s0.y2 = s0.y1; s0.y1 = y;
  float z = ((toneS1.b1 * s1.x1 + toneS1.b0 * y + toneS1.b2 * s1.x2) -
             toneS1.a1 * s1.y1) - toneS1.a2 * s1.y2;
  s1.x2 = s1.x1; s1.x1 = y; s1.y2 = s1.y1; s1.y1 = z;
  return z;
}

void Distortion::recomputeNorm() {
  if (curve == 0) {
    const float f = f0(drive);
    norm = (f > 0.0f) ? (2.0f / f) : 2.0f;
  } else {
    // norm = 1/f1d(drive) (FUN_10007e30 :5481-5484, FUN_10008030 :5551-5556;
    // curve-independent within curves 1/2 - the device setter is one law).
    const float f = distortion_f1d(drive);
    norm = (f > 0.0f) ? (1.0f / f) : 1.0f;
  }
}

// curve-0 chain coefficients from sample rate (FUN_10006a00,
// decomp_all_32_full.c:4697-4731; double math, float store).
void Distortion::deriveCoeffs0() {
  const double sr = (double)sampleRate;
  k18 = (float)(540.3539364174444 / sr);
  k28 = (float)(1.0 / (194.77874452256717 / sr + 1.0));
  k40 = (float)(41035.483241189875 / sr);
  k50 = (float)(829.3804605477054 / sr);
  k60 = k28;
  k78 = k40;
  k88 = (float)(1218.9379495928397 / sr);
  k98 = k28;
}

// One sample of the 3-stage gate/servo cascade (FUN_100070d0,
// decomp_all_32_full.c:4925-5011), byte op order. Returns d3 (the final
// leaky-differentiator output; caller scales by norm*level).
//   stage: off += ((1-g_prev)*5 - off)*k18 ; v = x*drive + off ;
//          g = v>=0 ? gate0(v) : 0 ; d = (g - g_hist + d)*k28 ;
//          y = (d - y)*k40 + y ; next stage fed by (1 - g_next) servo.
float Distortion::stepChain0(Chain0& c, float x, float d) const {
  c.off1 = ((1.0f - c.g1s) * 5.0f - c.off1) * k18 + c.off1;
  const float v1 = x * d + c.off1;
  const float g1 = (v1 >= 0.0f) ? gate0(v1) : 0.0f;
  c.g1s = g1;
  c.d1 = ((g1 - c.g1d) + c.d1) * k28;
  c.g1d = g1;
  c.y1 = (c.d1 - c.y1) * k40 + c.y1;

  c.s2 = ((1.0f - c.g2s) - c.s2) * k50 + c.s2;
  const float v2 = c.y1 + c.s2;
  const float g2 = (v2 >= 0.0f) ? gate0(v2) : 0.0f;
  c.g2s = g2;
  c.d2 = ((g2 - c.g2d) + c.d2) * k60;
  c.g2d = g2;
  c.y2 = (c.d2 - c.y2) * k78 + c.y2;

  c.s3 = ((1.0f - c.g3s) - c.s3) * k88 + c.s3;
  const float v3 = c.y2 + c.s3;
  const float g3 = (v3 >= 0.0f) ? gate0(v3) : 0.0f;
  c.g3s = g3;
  c.d3 = ((g3 - c.g3d) + c.d3) * k98;
  c.g3d = g3;
  return c.d3;
}

// Curve-block ctor warmup (FUN_10006240, decomp_all_32_full.c:4495-4588,
// local_10=600): zero state, then 600 iterations of the chain with x = 0
// (stage-1 gate input = off1 alone; the x*drive term is exactly 0).
void Distortion::warmChain0(Chain0& c) {
  for (int i = 0; i < 600; i++) stepChain0(c, 0.0f, 0.0f);
}

void Distortion::reset() {
  toneL0 = toneL1 = toneR0 = toneR1 = ToneState{};  // FUN_10006df0 zero
  c0L = Chain0{};
  c0R = Chain0{};
  warmChain0(c0L);
  warmChain0(c0R);
}

// Effector on-edge: the device never re-warms or clears the curve-0 chain
// here (the 600-iter warm is the curve-block ctor only, FUN_10006240;
// while bypassed the kernel is not called, so chain state simply
// persists). Zeroing/warming again at the on-edge double-warmed the
// engine vs the device's single ctor warm (m3-u1-0b-cascade).
void Distortion::resetOnEnable() {
  toneL0 = toneL1 = toneR0 = toneR1 = ToneState{};
}

void Distortion::process(float* left, float* right, int n) {
  const float d = drive, nm = norm, lv = level;
  if (curve == 0) {
    for (int i = 0; i < n; i++) {
      const float ol = stepChain0(c0L, left[i], d) * nm * lv;
      const float orr = stepChain0(c0R, right[i], d) * nm * lv;
      left[i] = toneStep(toneL0, toneL1, ol);
      right[i] = toneStep(toneR0, toneR1, orr);
    }
  } else if (curve == 1) {
    // Curve-1 kernel (FUN_10007300 *this==1, decomp_all_32_full.c:5034-5043),
    // byte op order: v = (x + DAT_10063888(1e-4f))*drive; t = v & 0x7fffffff;
    // mag = -1/((t+1)*(t+1)*t + 1) + 1; out = signbit(v)<<32 | mag*(norm*level)
    // (fVar11 = +8*param_3 precomputed) == copysign(mag*norm*level, v).
    const float nl = nm * lv;
    for (int i = 0; i < n; i++) {
      const float vl = (left[i] + DIST_EPS) * d;
      const float vr = (right[i] + DIST_EPS) * d;
      const float ol = std::copysign(distortion_f1d(std::fabs(vl)) * nl, vl);
      const float orr = std::copysign(distortion_f1d(std::fabs(vr)) * nl, vr);
      left[i] = toneStep(toneL0, toneL1, ol);
      right[i] = toneStep(toneR0, toneR1, orr);
    }
  } else {
    // curve 2 (m3-fx2-land, #146): stateless fmodf triangle fold.
    // Mono kernel FUN_10007300 else-branch, bytes 0x100073d0-0x10007468;
    // stereo kernel FUN_10007a50 curve-2 branch 0x10007b5f-0x10007cc0
    // (same law per channel, interleaved stride-8, in-place replace).
    // Bytes: x = in*drive (mulss +4); fold iff thr<x || x<-thr (comiss,
    // xorps sign 0x10063850, comiss); r = fmodf(x-thr, thr*4.0f) — float32
    // subss, thr*4.0f float32 (mulss 0x100638e8), flds/fprem (CRT fmodf
    // FUN_1004d89a), fstps to float32; x = abs(abs(r) - thr*2.0f) - thr
    // (andps 0x10063620, mulss 0x100638b0, subss, andps, subss +0xc);
    // out = x*(norm*level) (gain precomputed). THR=0.7f from ctor
    // (FUN_10006140 +0x58=0x3f333333); drive setter never touches +0xc.
    // No RNG, no state ("rand-fold" #146 note = Ghidra mis-model of the
    // cdecl fmodf stack args).
    const float nl = nm * lv;
    const float thr = 0.7f;
    const float per = thr * 4.0f;  // float32 mulss, mirrors 0x100073f9
    const float d2 = thr * 2.0f;   // float32 mulss, mirrors 0x10007433
    for (int i = 0; i < n; i++) {
      float vl = left[i] * d;
      if (thr < vl || vl < -thr) {
        const float r = (float)std::fmod((long double)(vl - thr), (long double)per);
        vl = std::fabs(std::fabs(r) - d2) - thr;
      }
      float vr = right[i] * d;
      if (thr < vr || vr < -thr) {
        const float r = (float)std::fmod((long double)(vr - thr), (long double)per);
        vr = std::fabs(std::fabs(r) - d2) - thr;
      }
      left[i] = toneStep(toneL0, toneL1, vl * nl);
      right[i] = toneStep(toneR0, toneR1, vr * nl);
    }
  }
}

// ---------------------------------------------------------------------------
// CDecimator
// ---------------------------------------------------------------------------
void Decimator::setCrush(int ctl2raw) {
  const int b = ((ctl2raw >> 3) + 1) & 0x1f;
  mask = ~0u << b;
}

void Decimator::process(float* left, float* right, int n) {
  const float mix = this->mix;
  const int m = (int)mask;
  const int period = this->period;
  unsigned cnt = this->cnt;
  float heldL = this->heldL, heldR = this->heldR;
  for (int i = 0; i < n; i++) {
    if ((int)(cnt % (unsigned)period) == 0) {
      const int il = (int)(left[i] * 65536.0f);
      const int ir = (int)(right[i] * 65536.0f);
      heldL = (float)(il & m) * (1.0f / 65536.0f);
      heldR = (float)(ir & m) * (1.0f / 65536.0f);
    }
    left[i] = heldL * mix + (1.0f - mix) * left[i];
    right[i] = heldR * mix + (1.0f - mix) * right[i];
    cnt++;
  }
  this->cnt = cnt;
  this->heldL = heldL;
  this->heldR = heldR;
}

// ---------------------------------------------------------------------------
// CRingMod
// ---------------------------------------------------------------------------
void RingMod::setSampleRate(int sr) {
  sampleRate = sr;
  recomputeK();
}

void RingMod::setFreq(int ctl1raw) {
  freq = (float)std::pow(8000.0, (double)ctl1raw * (1.0 / 127.0));
  recomputeK();
  // FUN_10014be0 (decomp_all_32_full.c:15659-15690): after k, clamp s0 to
  // [-1,1] (DAT_10063524 = 1.0f, DAT_10063778 = -1.0f) and re-normalise
  // s1 = sign(s1)*sqrt(1 - s0^2) — the sign is kept via the XOR-sign-bit
  // leg (:15685-15687, taken when the old s1 < 0.0). (0,±1) seeds are
  // fixed points, so the ctor seed survives every setFreq.
  if (s0 > 1.0f) {
    s0 = 1.0f;
  } else if (s0 < -1.0f) {
    s0 = -1.0f;
  }
  s1 = std::copysign(std::sqrt(1.0f - s0 * s0), s1);
}

void RingMod::recomputeK() {
  k = (float)(2.0 * std::sin(PI * (double)freq / (double)sampleRate));
}

void RingMod::process(float* left, float* right, int n) {
  const float kk = k, lv = level;
  float s0 = this->s0, s1 = this->s1;
  for (int i = 0; i < n; i++) {
    const float c = s0 - kk * s1;
    s0 = c;
    s1 = c * kk + s1;
    const float g = s0 * lv + (1.0f - lv);
    left[i] *= g;
    right[i] *= g;
  }
  this->s0 = s0;
  this->s1 = s1;
}

// ---------------------------------------------------------------------------
// CCompressor — 32-bit ref-path law (m3-u1-dist-comp-land, #211).
// Stereo kernel FUN_10004880 (decomp_all_32_full.c:3147-3233) is the render
// path (interleaved L/R); mono twin FUN_100047c0 (:3102-3141) is identical
// per channel. Bypass leg (*param_3 != 0 -> vtable+0x28, :3138/:3229-3230)
// is unreachable through the engine: the F9 gate (Effector::process, law
// #147, FUN_10007cd0/7d80 :5358-5438) zeroes the buffer and clears the flag
// before dispatch, so the kernel always enters with flag == 0.
// ---------------------------------------------------------------------------
void Compressor::setSampleRate(int sr) {
  sampleRate = sr;
  setTimes(ctl2raw);
}

// Threshold setter FUN_10004ac0 (decomp_all_32_full.c:3246-3274):
//   thrDb = DAT_10063790(-20.0f) - u*DAT_1006375c(40.0f)          (-20..-60)
//   makeup1(+0x74): if (DAT_10063790 < thrDb) 1.0f (0x3f800000) else
//     expf((DAT_10063790 - thrDb) * DAT_10063778(-1.0f)
//                       * DAT_10063760(-ln10/20))  == 10^((-20-thrDb)/20)
// thrLin per-channel (+0x14/+0x48) via sub-object setter FUN_10004cb0
// (:3367-3377): expf(thrDb * DAT_10063730(ln10/20)) == 10^(thrDb/20).
void Compressor::setThreshold(int ctl1raw) {
  const float u = (float)ctl1raw * FX_RAW_UNIT;
  thrDb = COMP_THR_BASE - COMP_THR_SPAN * u;
  thrLin = (float)std::exp((double)(thrDb * COMP_LN10_20));
  makeup1 = (COMP_THR_BASE < thrDb)
                ? 1.0f
                : (float)std::exp((double)((COMP_THR_BASE - thrDb) * COMP_LN10_20));
}

// Time law FUN_100049d0 (:3196-3233): t = pow(2000.0, sqrt(u))*0.1 ms
// (DAT_10063750 = 2000.0 double, log/exp FUN_1004beeb/FUN_1004ca9d);
// k (+0x2c/+0x60) = exp(DAT_10063788(-1.0) / (t * DAT_10063738(0.001) * sr))
// (FUN_10004bd0/4c60 :3306-3365).
void Compressor::setTimes(int ctl2raw) {
  this->ctl2raw = ctl2raw;
  const float u = (float)ctl2raw * FX_RAW_UNIT;
  const double t = std::pow(2000.0, std::sqrt((double)u)) * 0.1;  // ms
  const double sec = (t > 0.0 ? t : 0.1) * 0.001;
  k = (float)std::exp(-1.0 / ((double)sampleRate * sec));
}

// makeup2 setter FUN_10004b60 (decomp_all_32_full.c:3282-3291):
//   expf(level*DAT_10063520(1/127f)*DAT_10063758(30.0f)*DAT_10063730(ln10/20)
//        - DAT_10063748(ln10))  == 10^(1.5*level/127 - 1) in [0.1, 3.1623]
// (float32 exponent, x87 expf site FUN_1004ccb3; NOT the 64-bit image's
// level->rand re-seed FUN_18000c580->FUN_1800053b0, documented not used).
void Compressor::setLevel(int raw) {
  const float e = (float)raw * FX_RAW_UNIT * 30.0f * COMP_LN10_20 - COMP_LN10;
  makeup2 = (float)std::exp((double)e);
}

void Compressor::process(float* left, float* right, int n) {
  const float thr = thrLin;             // +0x14 / +0x48 (equal, one setter)
  const float kk = k;                   // +0x2c / +0x60 (equal, one setter)
  const float mk = makeup2 * makeup1;   // fVar6 = +0x78 * +0x74 (precomputed)
  float envL = this->envL, envR = this->envR;
  for (int i = 0; i < n; i++) {
    const float xl = left[i];
    float over = std::fabs(xl) - thr;   // (x & DAT_10063620) - thr, clamped
    if (over < 0.0f) over = 0.0f;
    envL = (envL - over) * kk + over;
    {  // nonzero-subnormal flush (:3127-3129): (bits<<1) exponent field 0,
       // mantissa field nonzero -> env = 0 (+0.0f store).
      std::uint32_t b;
      std::memcpy(&b, &envL, 4);
      const std::uint32_t s = b << 1;
      if ((s & 0xff000000u) == 0 && (s & 0x00ffffffu) != 0) envL = 0.0f;
    }
    left[i] = ((thr * xl) / (envL + thr)) * mk;

    const float xr = right[i];
    over = std::fabs(xr) - thr;
    if (over < 0.0f) over = 0.0f;
    envR = (envR - over) * kk + over;
    {
      std::uint32_t b;
      std::memcpy(&b, &envR, 4);
      const std::uint32_t s = b << 1;
      if ((s & 0xff000000u) == 0 && (s & 0x00ffffffu) != 0) envR = 0.0f;
    }
    right[i] = ((thr * xr) / (envR + thr)) * mk;
  }
  this->envL = envL;
  this->envR = envR;
}

// ---------------------------------------------------------------------------
// CPhaser
// ---------------------------------------------------------------------------
void Phaser::setSampleRate(int sr) {
  sampleRate = sr;
  w4sr = 4.0f / (float)sr;  // FUN_10014490: +0x90 = DAT_100638e8(4.0f)/sr
  recomputeStep();          // FUN_10014490 recomputes +0x98 as well
}

void Phaser::setPairs(int p) {
  nPairs = (p < 1) ? 1 : (p > FX_PHASER_MAX_PAIRS ? FX_PHASER_MAX_PAIRS : p);
}

void Phaser::setFeedback(int levelRaw) {
  float fbv = ((float)levelRaw - 64.0f) * (1.0f / 64.0f) * PH_FB_SPAN;
  fb = (fbv < -PH_FB_SPAN) ? -PH_FB_SPAN : (fbv > PH_FB_SPAN ? PH_FB_SPAN : fbv);
}

void Phaser::setCtl1(int ctl1raw) {
  // FUN_1000ac30 -> FUN_10014900: lo = 100*10^((127-x)/127),
  // hi = 1000*10^(x/127). float32 pow reproduces the device bits
  // (ctl1=12: lo 0x44491e4f, hi 0x449b6190 — scan103.csv).
  const float x = (float)ctl1raw;
  gLo = 100.0f * std::pow(10.0f, (127.0f - x) / 127.0f);
  gHi = 1000.0f * std::pow(10.0f, x / 127.0f);
  if (g == 0.0f) g = gLo;  // FUN_10014900: re-seed iff g == DAT_1006351c(0.0f)
  recomputeStep();
}

void Phaser::setSweepRate(int ctl2raw) {
  rate = (ctl2raw == 0)
             ? 0.01f
             : (float)(std::pow(1000.0, (double)ctl2raw * (1.0 / 127.0)) * 0.02);
  recomputeStep();
}

void Phaser::recomputeStep() {
  if (rate <= 0.0f) rate = 0.01f;
  // FUN_10014900/FUN_100149a0: step = exp(ln(hi/lo) / ((sr/rate) * 0.5f));
  // DAT_100637f8 = 0.5f -> one-way sweep 0.5/rate s (#107 half-period).
  // double intermediates reproduce the device float32 result bit-exact
  // (ctl1=12, ctl2=0 -> +0x98 = 0x3f7fffff, scan103.csv); GCC float
  // logf/expf is 1 ulp off, so do NOT use them here.
  const double f = std::exp(std::log((double)gHi / (double)gLo) /
                            (((double)sampleRate / (double)rate) * 0.5));
  const float fs = (float)f;
  gStep = (1.0f < gStep) ? fs : 1.0f / fs;  // direction-preserving store
}

void Phaser::reset() {
  state.fill(0.0f);
  accL = 0.0f;
  accR = 0.0f;
  frozen = false;
  // F2 writer audit: the device has no g/step writer outside ctor + setters;
  // g and gStep persist across enable edges.
}

void Phaser::process(float* left, float* right, int n) {
  if (nPairs <= 0) return;
  const float fbv = fb;
  float accL = this->accL, accR = this->accR;
  float g = this->g;
  float step = this->gStep;
  const float w4 = w4sr;
  const bool frozen = this->frozen;
  if (!frozen) {
    // per-host-block bounds test (FUN_100146c0): reciprocate the step at
    // the bounds, never wrap, never re-seed (#107 ping-pong).
    if (g <= gHi) {
      if (!(gLo <= g) && !(1.0f < step) && step != 1.0f) step = 1.0f / step;
    } else {
      if (!(step < 1.0f) && step != 1.0f) step = 1.0f / step;
    }
  }
  for (int i = 0; i < n; i++) {
    if (!frozen) g *= step;
    const float a = g * w4;  // FUN_10014770: +0x94 * +0x90
    const float v = (1.0f - a) / (a + 1.0f);
    const float xl0 = left[i], xr0 = right[i];
    accR = fbv * accR + xr0;
    accL = fbv * accL + xl0;
    float xl = accL, xr = accR;
    for (int st = 0; st < nPairs; st++) {
      const int b = st * 4;
      float h0 = state[b], h1 = state[b + 1], h2 = state[b + 2], h3 = state[b + 3];
      float a = h0 - xr * v;
      float bb = a * v;
      h0 = bb + xr;
      xr = h1 - bb;
      h1 = xr * v + a;
      a = h2 - xl * v;
      bb = a * v;
      h2 = bb + xl;
      xl = h3 - bb;
      h3 = xl * v + a;
      state[b] = h0; state[b + 1] = h1; state[b + 2] = h2; state[b + 3] = h3;
    }
    accL = xl;  // in-place: chain output feeds the next comb (#200)
    accR = xr;
    left[i] = (1.0f - PH_DRYWET) * xl0 + PH_DRYWET * xl;
    right[i] = (1.0f - PH_DRYWET) * xr0 + PH_DRYWET * xr;
  }
  this->accL = accL;
  this->accR = accR;
  this->g = g;
  this->gStep = step;
}

// ---------------------------------------------------------------------------
// CEffector
// ---------------------------------------------------------------------------
void Effector::set_param(int param_id, int value) {
  switch (param_id) {
    case FX_PARAM_ONOFF: {
      const int prev = onOff;
      onOff = value;
      // rising edge: sub-effect state clear WITHOUT re-warming the
      // curve-0 chain (device warms once at ctor; FUN_10006240).
      if (onOff != 0 && prev == 0) resetOnEnable();
      break;
    }
    case FX_PARAM_TYPE:
      effectType = (value < 0) ? 0 : (value > 9 ? 9 : value);
      applyType(effectType);
      break;
    case FX_PARAM_CTL1:
      ctl1 = value;
      broadcastCtl1();
      break;
    case FX_PARAM_CTL2:
      ctl2 = value;
      broadcastCtl2();
      break;
    case FX_PARAM_LEVEL:
      level = value;
      broadcastLevel();
      break;
    default:
      break;
  }
}

void Effector::set_sample_rate(float sr) {
  sampleRate = (int)sr;
  // F9 gate cap (FUN_1800076f0, decomp_all.c:4875-4888):
  // +0x30 = (int)((float)sr * _DAT_180072fe8=0.025f); the handler does
  // NOT reset the counter. 1102 @44100 (float mul, truncate toward 0).
  silentBudget = (int)((float)sampleRate * EFF_SILENT_BUDGET_SEC);
  dist.setSampleRate(sampleRate);
  comp.setSampleRate(sampleRate);
  rm.setSampleRate(sampleRate);
  dec.setSampleRate(sampleRate);
  ph.setSampleRate(sampleRate);
}

void Effector::reset() {
  dist.reset();
  comp.reset();
  rm.reset();
  dec.reset();
  ph.reset();
}

void Effector::resetOnEnable() {
  dist.resetOnEnable();
  comp.reset();
  rm.reset();
  dec.reset();
  ph.reset();
}

void Effector::applyType(int type) {
  if (type <= 2) {
    dist.setCurve(type);
  } else if (type >= 6) {
    ph.setPairs(kPhaserPairs[type - 6]);
  }
}

void Effector::broadcastCtl1() {
  dist.setDrive(ctl1);
  comp.setThreshold(ctl1);
  rm.setFreq(ctl1);
  dec.setPeriod(ctl1);
  ph.setCtl1(ctl1);  // FUN_1000ac30 -> FUN_10014900 bounds law (#200)
}

void Effector::broadcastCtl2() {
  dist.setToneCut(ctl2);
  comp.setTimes(ctl2);
  dec.setCrush(ctl2);
  ph.setSweepRate(ctl2);
  // CRingMod has no ctl2 target.
}

void Effector::broadcastLevel() {
  dist.setLevel(level);
  comp.setLevel(level);  // FUN_10004b60 makeup2 (32-bit ref path, #211)
  rm.setLevel(level);
  dec.setMix(level);
  ph.setFeedback(level);
}

// F9 silent-flag/budget gate (law #147) — the CEffector process slot IS
// the gate wrapper (FUN_180008a50, decomp_all.c:5470-5495; mono twin
// FUN_1800089e0 :5433-5467; 32-bit twins FUN_10007cd0/FUN_10007d80,
// decomp_all_32_full.c:5358-5438):
//   *flag==0          counter=0, kernel runs on the block        (live)
//   counter<=cap      counter+=len, ZERO the buffer (len*2 floats),
//                     *flag=0, kernel runs on the zeroed buffer  (ring-out:
//                     the cascade state decays from its last value and the
//                     downstream units see the ring-out as LIVE)
//   cap<counter       return: kernel skipped, buffer/flag/counter all
//                     untouched                                   (FROZEN)
// The flag is the voice-chain mixer's silent flag (FUN_180015a20 :14332/
// :14353-14359, recomputed fresh every block); restart is immediate: any
// non-silent block takes the live branch and zeroes the counter.
// Effect OFF: the graph does not run the node at all (the wrapper has no
// +0x18 test, but the effect-off 14-set refs are byte-identical with the
// flag passing through untouched to the delay budget - if the wrapper ran
// while off its budget branch would clear the flag for 25 ms and reset
// the delay counter, lengthening the delay tails; it does not). So the
// bypass returns BEFORE the gate, buffer and flag untouched.
void Effector::process(float* left, float* right, int num_samples, int* silent) {
  if (onOff == 0) return;  // bypass: block passes through untouched
  if (*silent == 0) {
    silentCounter = 0;
  } else {
    if (silentBudget < silentCounter) return;  // FROZEN
    silentCounter += num_samples;
    for (int i = 0; i < num_samples; i++) {
      left[i] = 0.0f;
      right[i] = 0.0f;
    }
    *silent = 0;
  }
  switch (effectType) {
    case 0: case 1: case 2: dist.process(left, right, num_samples); break;
    case 3: dec.process(left, right, num_samples); break;
    case 4: rm.process(left, right, num_samples); break;
    case 5: comp.process(left, right, num_samples); break;
    case 6: case 7: case 8: case 9: ph.process(left, right, num_samples); break;
    default: break;
  }
}

}  // namespace synth1::fx
