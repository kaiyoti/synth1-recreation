// eq.cpp — see eq.h. Builder = FUN_1000bdb0 (32-bit render DLL),
// float32 op order; bit parity vs the m3-u3-eq F7 runtime dumps proven
// in tasks/m3-u3b-eqform F1.
#include "fx/eq.h"

#include <cmath>

namespace synth1::fx {

namespace {
// Constants PE-pinned from Synth1 VST.dll (9be31edeb5a5) .rdata
// (tools/pe_const.py method; values in tasks/m3-u3b-eqform/RESULT.md F0).
constexpr double kTwoPi = 6.2831853071795862;   // DAT_100638b8
constexpr double kPi = 3.1415926535897931;      // DAT_10063608
constexpr double kE = 2.7182817459106445;       // DAT_10063a78 (pow base, NOT true e)
constexpr double kTen = 10.0;                   // DAT_100638c0
constexpr double kOne = 1.0;                    // DAT_10063818
constexpr double kTwoD = 2.0;                   // DAT_10063600
constexpr float kHalf = 0.05f;                  // DAT_10063a70
constexpr float kOneF = 1.0f;                   // DAT_10063524
constexpr float kTwoF = 2.0f;                   // DAT_100638b0
constexpr float kNegTwoF = -2.0f;               // DAT_10063924
constexpr float kZeroF = 0.0f;                  // DAT_1006351c
constexpr float kInv127 = 1.0f / 127.0f;        // DAT_10063520 (0x3c010204)
constexpr float kInv64 = 1.0f / 64.0f;          // DAT_100639e0
constexpr float kFiftyF = 50.0f;                // DAT_10063f58
constexpr float kRingOut = 1.0e-6f;             // FUN_1000ce60 :9455 imm 0x358637bd (#214)
}  // namespace

void CEq::set_sample_rate(double sr) {
  sr_ = static_cast<float>(sr);
  // CEq ctor FUN_1000bd10 :8689: +0x1c = (int)(sr * _DAT_10063a74),
  // _DAT_10063a74 = 0.2f → 8820 @44100 (float product, then trunc).
  tailBudget_ = static_cast<int>(sr_ * 0.2f);
  dirty_ = true;
}

void CEq::set_type(int t) {
  type_ = (t == kLowShelf || t == kHighShelf) ? t : kPeak;
  stL_ = State();
  stR_ = State();
  dirty_ = true;
}

// FUN_1000d020: f = max(1.0f, f), eager rebuild (lazy here — D5).
void CEq::set_freq_hz(double f) {
  float fv = static_cast<float>(f);
  if (fv < kOneF) fv = kOneF;
  f_ = fv;
  dirty_ = true;
}

void CEq::set_gain_db(double g) {
  g_ = static_cast<float>(g);
  dirty_ = true;
}

void CEq::set_q(double q) {
  q_ = static_cast<float>(q);
  dirty_ = true;
}

// Dispatcher case 61 (decomp_all_32_full.c:29612-29616).
void CEq::set_freq_param(int x) {
  const double e = static_cast<double>(static_cast<float>(x) * kInv127);
  set_freq_hz(std::pow(320.0, e) * 50.0);
}

// Dispatcher case 62 (:29617-29620): float(float(x-64)*(1/127f)*50.0f).
void CEq::set_gain_param(int x) {
  const float g = (static_cast<float>(x - 64) * kInv127) * kFiftyF;
  set_gain_db(g);
}

// Dispatcher case 63 (:29621-29623): float(pow(10.0,(double)(float(x)*(1/127f)))).
void CEq::set_q_param(int x) {
  const double e = static_cast<double>(static_cast<float>(x) * kInv127);
  set_q(std::pow(kTen, e));
}

// FUN_100270a0 (param 60): tone<65 -> type 1, f = float(pow(110.0,
// (double)(float(tone)*(1/64f)))*200.0); else type 2,
// f = float(pow(800.0,(double)(float(tone-64)*(1/64f)))*10.0).
void CEq::set_tone_param(int x) {
  if (x < 65) {
    set_type(kLowShelf);
    const double e = static_cast<double>(static_cast<float>(x) * kInv64);
    set_freq_hz(std::pow(110.0, e) * 200.0);
  } else {
    set_type(kHighShelf);
    const double e = static_cast<double>(static_cast<float>(x - 64) * kInv64);
    set_freq_hz(std::pow(800.0, e) * kTen);
  }
}

void CEq::prepare() {
  dirty_ = false;

  if (type_ == kPeak) {
    // FUN_1000bdb0 peak branch, float32 op order (byte cites in eq.h).
    const float f6 = static_cast<float>(kTwoPi * static_cast<double>(f_));
    const float A = static_cast<float>(
        std::pow(kTen, static_cast<double>(g_ * kHalf)));
    const float invSr = kOneF / sr_;
    const float q2 = q_ * kTwoF;
    const float r = kOneF / q2;
    float r1, r2;
    if (A < kOneF) {
      r1 = r;
      r2 = r1 / A;
    } else {
      r2 = r;
      r1 = r2 * A;
    }
    // section (r_i): d_i, b1_i, b2_i
    const float wr1 = -((invSr * f6) * r1);
    const float d1 = static_cast<float>(std::pow(kE, static_cast<double>(wr1)));
    const float wr2 = -((f6 * invSr) * r2);
    const float d2 = static_cast<float>(std::pow(kE, static_cast<double>(wr2)));
    float b1z, b2z, b1p, b2p;
    {
      const float r2v = static_cast<float>(
          std::pow(static_cast<double>(r1), kTwoD));
      float C1;
      if (std::fabs(r1) <= kOneF) {
        const float th = (std::sqrt(kOneF - r2v) * invSr) * f6;
        C1 = static_cast<float>(std::cos(static_cast<double>(th)));
      } else {
        const float th = (std::sqrt(r2v - kOneF) * invSr) * f6;
        C1 = static_cast<float>(std::cosh(static_cast<double>(th)));
      }
      b1z = (C1 * d1) * kNegTwoF;
      b2z = static_cast<float>(std::pow(static_cast<double>(d1), kTwoD));
    }
    {
      const float r2v = static_cast<float>(
          std::pow(static_cast<double>(r2), kTwoD));
      float C2;
      if (std::fabs(r2) <= kOneF) {
        const float th = (std::sqrt(kOneF - r2v) * invSr) * f6;
        C2 = static_cast<float>(std::cos(static_cast<double>(th)));
      } else {
        const float th = (std::sqrt(r2v - kOneF) * invSr) * f6;
        C2 = static_cast<float>(std::cosh(static_cast<double>(th)));
      }
      b1p = (C2 * d2) * kNegTwoF;
      b2p = static_cast<float>(std::pow(static_cast<double>(d2), kTwoD));
    }
    const float w = f6 * invSr;
    const float c = static_cast<float>(std::cos(static_cast<double>(w)));
    const float s = static_cast<float>(std::sin(static_cast<double>(w)));
    const float magP =
        static_cast<float>(std::pow(static_cast<double>(c) * (static_cast<double>(b2p) + kOne) +
                                    static_cast<double>(b1p), kTwoD)) +
        static_cast<float>(std::pow(-(static_cast<double>(s) *
                                      (static_cast<double>(b2p) - kOne)), kTwoD));
    const float magZ =
        static_cast<float>(std::pow(static_cast<double>(c) * (static_cast<double>(b2z) + kOne) +
                                    static_cast<double>(b1z), kTwoD)) +
        static_cast<float>(std::pow(-(static_cast<double>(s) *
                                      (static_cast<double>(b2z) - kOne)), kTwoD));
    const float a0 = (std::sqrt(magP) * A) / std::sqrt(magZ);
    a0_ = a0;
    a1_ = a0 * b1z;
    a2_ = a0 * b2z;
    b1_ = b1p;
    b2_ = b2p;
    return;
  }

  // FUN_1000bdb0 shelf branch (type 1/2): w = 2*pi*f (Hz, not normalised),
  // f6 = 2*sr, k = 1/(w+f6), pole = (f6-w)*k.
  const float f6 = static_cast<float>(kTwoD * static_cast<double>(sr_));
  const float w = static_cast<float>(kPi * static_cast<double>(f_ * kTwoF));
  const float k = kOneF / (w + f6);
  const float pole = (f6 - w) * k;
  if (type_ == kLowShelf) {
    a0_ = k * w;
    a1_ = a0_;
  } else {
    a0_ = k * f6;
    a1_ = -a0_;
  }
  a2_ = 0.0f;
  b1_ = -pole;
  b2_ = 0.0f;
}

void CEq::process(float* left, float* right, int num_frames, int* ioSilent) {
  if (!enabled_ || num_frames <= 0) return;

  // Per-type bypass (FUN_1000ce60 :9436-9444): the kernel runs only if
  //   type 0: gain != 0.0f;  type 1: f < (float)(sr/2);  type 2: 1.0f < f.
  // Otherwise passthrough with state kept (no clamp anywhere — the device
  // has none; replaces the old engine clamps, m3-u3-eq F6/D3).
  if (dirty_) prepare();   // device computes coefficients eagerly at the
                           // setters, even while bypassed (F7 dumps)

  if (type_ == kPeak) {
    if (g_ == kZeroF) return;
  } else if (type_ == kLowShelf) {
    if (!(f_ < static_cast<float>(static_cast<int>(sr_) / 2))) return;
  } else {
    if (!(kOneF < f_)) return;
  }

  // #214 gate leg (FUN_1000ce60 :9444-9470), INSIDE the qualifier above:
  // a bypassed band returns at :9470 without touching flag, counter or
  // buffer (m3-214-qualifier Q1a: eqA leg iff gain != 0 dB, eqB leg
  // always; first ACTIVE gate in signal order consumes the flag).
  if (ioSilent != nullptr && *ioSilent != 0) {
    if (silentFrames_ <= tailBudget_) {
      // :9448-9462 ring-out: counter += n, fill BOTH channels with the
      // immediate 0x358637bd = 1.0e-6f (NOT exact zero — unlike CDelay
      // :4118 / CChorus :2012), clear the flag, then run the kernel on
      // the injected buffer (the plateau is the FILTERED ring-out:
      // type-2 eqB H(0)=0 cancels it → exact-0 after ~80-frame transient).
      silentFrames_ += num_frames;
      for (int i = 0; i < num_frames; ++i) {
        left[i] = kRingOut;
        right[i] = kRingOut;
      }
      *ioSilent = 0;
    } else {
      return;  // :9470 FROZEN: flag passes, buffer untouched
    }
  } else {
    silentFrames_ = 0;  // :9445 live input clears the counter
  }

  State& l = stL_;
  State& r = stR_;
  for (int i = 0; i < num_frames; ++i) {
    const double xl = left[i];
    const double xr = right[i];

    const double yl = a0_ * xl + a1_ * l.x1 + a2_ * l.x2 - b1_ * l.y1 - b2_ * l.y2;
    l.x2 = l.x1;
    l.x1 = xl;
    l.y2 = l.y1;
    l.y1 = yl;
    left[i] = static_cast<float>(yl);

    const double yr = a0_ * xr + a1_ * r.x1 + a2_ * r.x2 - b1_ * r.y1 - b2_ * r.y2;
    r.x2 = r.x1;
    r.x1 = xr;
    r.y2 = r.y1;
    r.y1 = yr;
    right[i] = static_cast<float>(yr);
  }
}

}  // namespace synth1::fx
