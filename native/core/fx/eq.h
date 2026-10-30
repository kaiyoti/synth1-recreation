// eq.h — CEq "Equalizer" (post-voice chain, engine +0x9a98 / +0x9b18).
//
// Spec: out/spec_lfo_fx.md §4 (CEq: layout, ctor FUN_18000bd10/
// FUN_18000d420, vftable 0x10063a54 / 0x180073438).
//
// Param mapping (dispatcher cases, 32-bit decomp_all_32_full.c:
// 29612-29623 + FUN_100270a0 :29892; float32 op order per m3-u3b-eqform):
//   61 freq -> eqA: f = float(pow(320.0, (double)(float(x)*(1/127f)))*50.0)
//       (DAT_10064d40=320.0, DAT_10063520=1/127f, DAT_10064d18=50.0);
//       setter FUN_1000d020 clamps f = max(1.0f, f).
//   62 gain -> eqA ONLY (never eqB — D6 guard, asserted in ctest):
//       g = float(float(x-64)*(1/127f)*50.0f)  (DAT_10063f58=50.0f)
//   63 Q    -> eqA ONLY: Q = float(pow(10.0, (double)(float(x)*(1/127f))))
//       (DAT_100638c0=10.0). eqB's Q is never written (FUN_100270a0
//       writes type+freq only; shelf kernels ignore Q).
//   60 tone -> eqB: tone<65 type 1, f = float(pow(110.0,(double)(float(
//       tone)*(1/64f)))*200.0) (DAT_10064d28=110.0, DAT_100639e0=1/64f,
//       _DAT_10064d30=200.0); tone>=65 type 2, f = float(pow(800.0,
//       (double)(float(tone-64)*(1/64f)))*10.0) (DAT_10064d48=800.0).
// Param 73 writes the CEq +0x14 CHAIN-MODE flag (mono/stereo+duplicate),
// NOT an EQ bypass (m3-u3a-eqgate F4 refutation): the EQ kernel is live
// in both modes; case 73 stays a no-op here.
//
// Coefficient builder FUN_1000bdb0 (decomp_all_32_full.c:8699-9018;
// 64-bit mirror FUN_18000d4a0 decomp_all.c:8752-8895), float32, eager at
// the setters; object offsets +0x20 g, +0x24 A, +0x28 f, +0x2C Q,
// +0x30 invSr, +0x34 a0, +0x38 b1(r1), +0x3c b2(r1), +0x40 a1, +0x44 a2,
// +0x48 b1(r2), +0x4c b2(r2). type 0 (parametric peak) — matched
// pole/zero form, NOT RBJ (m3-u3-eq F4; bit parity vs runtime dumps proven
// in tasks/m3-u3b-eqform F1):
//   f6 = float(2pi_d * f);  A = float(pow(10.0, (double)(g*0.05f)));
//   invSr = 1.0f/(float)sr; q2 = Q*2.0f; r = 1.0f/q2;
//   A<1: r1=r, r2=r1/A   else: r2=r, r1=r2*A;
//   d_i = float(pow(e_const, (double)(-((invSr*f6)*r_i))))
//       (DAT_10063a78 e_const = 2.7182817459106445 — the DLL's pow base,
//        NOT true e);
//   |r_i|<=1: C_i = cos((double)((sqrtf(1.0f-(float)pow(r_i,2.0))*invSr)*f6))
//   |r_i|> 1: C_i = cosh((double)((sqrtf((float)pow(r_i,2.0)-1.0f)*invSr)*f6));
//   b1_i = (C_i*d_i)*-2.0f;  b2_i = float(pow(d_i,2.0));
//   w = f6*invSr; c = cos(w); s = sin(w);
//   mag_i = float(pow(c*(b2_i+1.0)+b1_i,2.0)) + float(pow(-s*(b2_i-1.0),2.0));
//   a0 = (sqrtf(mag_r2)*A)/sqrtf(mag_r1);  a1 = a0*b1_r1;  a2 = a0*b2_r1.
// Kernel (FUN_1000cb00 / FUN_18000e010): y[n] = a0·x[n] + a1·x[n-1]
//   + a2·x[n-2] - b1(r2)·y[n-1] - b2(r2)·y[n-2].  |H(fc)| = A exactly.
//
// type 1/2 shelves (FUN_1000bdb0 shelf branch; engine matches m3-u3-eq F5):
//   f6 = float(2.0*sr); w = float(pi_d*(double)(f*2.0f)); k = 1.0f/(w+f6);
//   pole = (f6-w)*k; type1: a0=a1=k*w; type2: a0=k*f6, a1=-a0.
//   The shelf output scale +0x24 (=pow(10,g*0.05f), FUN_1000cf50 tail) is
//   inert: eqB's gain is never written (D6) so A == 1 always.
//
// Per-type bypass (FUN_1000ce60 :9436-9444 — kernel runs ONLY if
// type!=0 || g!=0.0f;  type!=1 || f < (float)(sr/2);  type!=2 ||
// 1.0f < f; else passthrough, state kept). Replaces the old engine
// clamps (f [10,0.499sr], Q [0.05,1000]) — the device has no clamps.
//
// Port deviations: process() runs in place on separate L/R float buffers;
// coefficients are recomputed lazily on the first block after a setter
// (device: eagerly at the setters — D5, inert); the kernel accumulates in
// double (device: float) — pre-existing, outside the U3-b law; set_type
// clears state (device set_type FUN_1000d0f0 does not — D5, record-only).

#pragma once

#include <cmath>

namespace synth1::fx {

class CEq {
 public:
  enum Type { kPeak = 0, kLowShelf = 1, kHighShelf = 2 };

  CEq() = default;

  void set_sample_rate(double sr);
  void set_enabled(bool on) { enabled_ = on; }
  double sample_rate() const { return sr_; }
  // Clears the per-channel filter state (used when the chain resets the band).
  void reset() { stL_ = State(); stR_ = State(); silentFrames_ = 0; }
  void set_type(int t);
  void set_freq_hz(double f);
  void set_gain_db(double g);
  void set_q(double q);

  // Raw knob setters using the verified engine laws (float32 op order).
  void set_freq_param(int x);
  void set_gain_param(int x);
  void set_q_param(int x);
  // eqB tone law (param 60, FUN_100270a0): sets type + freq.
  void set_tone_param(int x);

  double freq_hz() const { return f_; }
  double gain_db() const { return g_; }
  double q() const { return q_; }
  int type() const { return type_; }
  bool enabled() const { return enabled_; }

  // Kernel coefficients (float32 values), for test pins.
  float a0() const { return a0_; }
  float a1() const { return a1_; }
  float a2() const { return a2_; }
  float b1() const { return b1_; }
  float b2() const { return b2_; }

  // #214 CEq gate leg (FUN_1000ce60 :9438-9470). ioSilent is the chain
  // silent flag (device param_3, threaded by the pull chain):
  //   flag==0                    -> silentFrames_ = 0, kernel runs (:9444-9446)
  //   flag!=0, counter<=budget   -> counter += n, buffer := 1e-6f (0x358637bd,
  //                                 NOT exact zero), flag := 0, kernel runs
  //                                 (:9448-9462 — the ring-out plateau)
  //   flag!=0, counter>budget    -> return: flag PASSES, buffer untouched (:9470)
  // The leg runs only inside the per-type qualifier below (a bypassed band
  // passes the flag through untouched, same :9470 return). nullptr = flag==0.
  void process(float* left, float* right, int num_frames, int* ioSilent = nullptr);

 private:
  void prepare();

  float sr_ = 44100.0f;
  int type_ = 0;
  bool enabled_ = true;
  bool dirty_ = true;

  float f_ = 1000.0f;
  float g_ = 0.0f;
  float q_ = 1.0f;

  // type 0: a0,a1,a2 / b1,b2 (r2 section). Shelves: a0,a1 + b1=-pole.
  float a0_ = 1, a1_ = 0, a2_ = 0, b1_ = 0, b2_ = 0;

  struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
  State stL_, stR_;

  // #214 gate-leg state (CEq ctor FUN_1000bd10 :8669-8695): counter +0x18
  // boots 0 (NOT ctor-exhausted like CDelay/CChorus); budget +0x1c =
  // (int)(sr * 0.2f) (_DAT_10063a74 = 0.2f, .rdata file 0x062074) = 8820
  // @44100 — per instance, independent counters (m3-msa-32bit-chain F2/F3).
  int silentFrames_ = 0;
  int tailBudget_ = 8820;
};

}  // namespace synth1::fx
