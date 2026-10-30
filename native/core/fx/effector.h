// effector.h — CEffector "Effect" multi-FX router (post-voice chain device,
// engine +0x9718) and its five sub-effect objects.
//
// Spec: out/spec_lfo_fx.md §5 (CEffector container, §5.1 type router,
//       §5.2 parameter application, §5.3 kernels), out/dsp_flow.md §3.
// Ghidra (out/decomp_all.c, image base 0x180000000):
//   CEffector ctor          FUN_18000c140 (0x380 B)
//   type router             FUN_18000c610(type)
//   ctl1 broadcast          FUN_18000c400(x)
//   ctl2 broadcast          FUN_18000c4e0(x)
//   level broadcast         FUN_18000c580(x)
//   CDistortion curves      FUN_180007f60 (curve 0), FUN_1800083d0 (curves 1/2)
//   CDistortion normalise   FUN_180008bc0(blk, drive)
//   CDecimator kernel       FUN_180005630
//   CRingMod kernel         FUN_180016ee0
//   CPhaser kernel          FUN_180016960 -> FUN_180016a20
//   CCompressor kernel      FUN_180005560 (body merged with the decimator in
//                           the dump — see U6; reconstructed here)
//
// Container layout (§5):
//   +0x10 sampleRate   +0x1c effectType 0..9   +0x20 ctl1   +0x24 ctl2
//   +0x28 level        +0x30 activeEffect -> one of the five objects
// Sub-effects embedded at +0x38 CDistortion, +0x1b0 CCompressor, +0x270
// CRingMod, +0x2a0 CDecimator, +0x2d0 CPhaser.
//
// Control parameters (engine dispatcher):
//   77 on/off (0 bypass / 1 on)   78 type 0..9   79 ctl1   80 ctl2   81 level
// Type map (§5.1): 0/1/2 = CDistortion curves a.d.1/a.d.2/d.d.; 3 = CDecimator;
//   4 = CRingMod; 5 = CCompressor; 6/7/8/9 = CPhaser with {1,2,4,6} all-pass
//   pairs.
//
// Verified .rdata constants (out/spec_lfo_fx.md §10):
//   1/127f raw->unit; 1e-4f distortion eps; 0.4f a.d.1 shape; 2.0f shape;
//   5.0f gate slew; 8000.0 ring-mod max freq; 1000.0f ring-mod default;
//   0.99f phaser feedback span; 0.02 phaser sweep scale; 1000.0 sweep base;
//   2000.0 comp attack/release base; -20/-10 comp threshold; 40.0 comp span;
//   65536.0f decimator quantise scale.
//
// Documented port deviations (spec UNKNOWNs / transcription slips):
//  * ctl1 drive law is the spec's 10^((70·x/127 − 10)·0.05) = 0.3162 .. 1000.
//    The earlier "transcription slip / range 10^±0.5" note was FALSE:
//    _DAT_100638d0 = 70.0 pinned in install/Synth1 VST.dll .rdata 0x100638d0
//    (FUN_10007e30, decomp_all_32_full.c:5436-5460; 64-bit twin
//    FUN_180008b40, decomp_all.c:5518-5531, _DAT_180073028 = 70.0);
//    runtime dump drive = 18.356062 at ctl1 = 64 (m3-u1-effector F6).
//  * CDistortion curve 0 is the device's 3-stage gate/servo cascade with
//    leaky differentiators (FUN_100070d0, decomp_all_32_full.c:4925-5011;
//    64-bit stereo twin FUN_180007f60, decomp_all.c:5125-5271), sr-derived
//    chain coefficients (FUN_10006a00, decomp_all_32_full.c:4697-4731) and
//    the 600-iteration ctor warmup (curve-block ctor FUN_10006240,
//    decomp_all_32_full.c:4424-4590, local_10=600). The earlier "half-wave
//    saturator, servo omitted" port was the m3-u1-effector F2b defect
//    (+35 dB / corr 0.17 on b_effect); landed by m3-u1-0b-cascade.
//  * CDistortion post tone filter (ctl2) is the device's 2-section biquad
//    cascade: coefficients FUN_10006c00 (decomp_all_32_full.c:4739-4802, raw
//    disasm 0x10006c00-0x10006de7), kernel FUN_10006ff0 (:4883-4917), applied
//    after BOTH distortion kernels by FUN_10007cd0 (:5383-5391). Section 0 =
//    lowpass fc = 400·40^(ctl2/127) Hz; section 1 = fixed 100 Hz highpass.
//    ctl2 = 127 is NOT transparent (16 kHz LP + 100 Hz HP). The earlier
//    one-pole "documented guess" was the m3-u1-effector F4 defect; landed by
//    m3-u1-0c-tone (6/6 F6 runtime coefficient pins at ctl2=64).
//  * CCompressor (U6) LANDED by m3-u1-dist-comp-land (#211) to the 32-bit
//    ref-path law (image authority; the 64-bit level->rand re-seed
//    FUN_18000c580->FUN_1800053b0 is documented, NOT used): over-threshold
//    envelope env = (env - max(0,|x|-thrLin))*k + over with a subnormal-env
//    flush to 0, soft-knee gain thrLin/(env+thrLin), makeup =
//    makeup1(thrDb)*makeup2(level) = 10^((-20-thrDb)/20) * 10^(1.5*level/127-1).
//    Cites: mono kernel FUN_100047c0 (decomp_all_32_full.c:3102-3141), stereo
//    FUN_10004880 (:3147-3233), threshold+makeup1 FUN_10004ac0 (:3246-3274),
//    makeup2 FUN_10004b60 (:3282-3291), thrLin FUN_10004cb0 (:3367-3377),
//    time law FUN_100049d0/4bd0/4c60 (:3196-3365). The earlier 4:1 hard-knee
//    reconstruction with linear makeup 2*level/127 was the m3-u1-dist-comp-2
//    F2/F3 defect (engine silent at level=0 where the device is not).
//  * CPhaser sweep (U9) LANDED by m3-u1-phaser-land (#103/#104/#105/#107/
//    #200): raw g-domain geometric walk, ctl1 bounds law, ctor g=200 seed,
//    ping-pong reciprocate-step per host block, 4/sr all-pass map, 0.5
//    dry/wet, comb feedback AROUND the all-pass chain (accumulator is the
//    chain working variable in-place). See CPhaser below for cites.
//
// No realtime allocations: every sub-effect is POD state with fixed std::array
// storage (the phaser's all-pass state is a fixed 6-pair ladder). process()
// performs zero heap allocations.

#pragma once

#include <array>
#include <cstdint>

namespace synth1::fx {

// raw -> unit (DAT_180072ff0 / DAT_180072a00)
inline constexpr float FX_RAW_UNIT = 1.0f / 127.0f;

// F9 effector silent-budget cap factor (_DAT_180072fe8 = 0.025f, PE-pinned
// in bin/Synth1_vst64_113b3.dll .rdata 0x715e8; 32-bit DAT_10063890
// file off 0x61e90): cap = (int)((float)sr * 0.025f) = 1102 @44100.
inline constexpr float EFF_SILENT_BUDGET_SEC = 0.025f;

// tone filter .rdata pins (install/Synth1 VST.dll 9be31edeb5a5, FUN_10006c00):
// 0x100638c8=40.0, 0x100638e0=400.0, 0x100638b8=2pi, 0x100638f8=628.3185307179587
// (2pi*100) doubles; 0x10063894=1/6f, 0x10063898=0.21739131f, 0x100637f8=0.5f,
// 0x10063924=-2.0f.
inline constexpr double TONE_FC_BASE = 400.0;   // Hz at ctl2 = 0
inline constexpr double TONE_FC_SPAN = 40.0;    // fc = 400 * 40^(ctl2/127)
inline constexpr double TONE_HP_W = 628.3185307179587;  // 2*pi*100 rad/s
inline constexpr float TONE_S0_K = 0.16666667f;  // 1/6 f32 (0x10063894)
inline constexpr float TONE_S1_K = 0.21739131f;  // 0x10063898 (0x3e5e9bd4)

// effect types (§5.1)
inline constexpr int FX_TYPE_AD1 = 0;      // CDistortion curve 0 (a.d.1)
inline constexpr int FX_TYPE_AD2 = 1;      // CDistortion curve 1 (a.d.2)
inline constexpr int FX_TYPE_DD  = 2;      // CDistortion curve 2 (d.d.)
inline constexpr int FX_TYPE_DECIMATOR = 3;
inline constexpr int FX_TYPE_RINGMOD   = 4;
inline constexpr int FX_TYPE_COMPRESSOR = 5;
inline constexpr int FX_TYPE_PHASER1 = 6;  // CPhaser 1 pair
inline constexpr int FX_TYPE_PHASER2 = 7;  // 2 pairs
inline constexpr int FX_TYPE_PHASER3 = 8;  // 4 pairs
inline constexpr int FX_TYPE_PHASER4 = 9;  // 6 pairs

// control parameter ids (engine dispatcher)
inline constexpr int FX_PARAM_ONOFF = 77;
inline constexpr int FX_PARAM_TYPE  = 78;
inline constexpr int FX_PARAM_CTL1  = 79;
inline constexpr int FX_PARAM_CTL2  = 80;
inline constexpr int FX_PARAM_LEVEL = 81;

// phaser all-pass pair ladder (types 6..9 -> {1,2,4,6} pairs)
inline constexpr int FX_PHASER_MAX_PAIRS = 6;

// ---------------------------------------------------------------------------
// ctl1 -> CDistortion drive (range-corrected, see header deviation).
// ---------------------------------------------------------------------------
float distortion_drive(int ctl1raw);

// ctl2 -> tone lowpass cutoff fc = 400 * 40^(ctl2/127) Hz (FUN_10006c00:
// d = exp(log(40)*ctl2/127), fc = (float)(d * 400.0); 400 .. 16000 Hz).
float distortion_tone_fc(int ctl2raw);

// curve-1 saturator f1d(t) = 1 - 1/(1 + t*(t+1)^2), t >= 0, byte op order
// -1/((t+1)*(t+1)*t + 1) + 1 (FUN_10007300 *this==1 kernel,
// decomp_all_32_full.c:5034-5043; norm law 1/f1d(drive) FUN_10007e30
// :5481-5484 + FUN_10008030 :5551-5556). DAT_10063778 = -1.0f,
// DAT_10063524 = 1.0f (install/Synth1 VST.dll .rdata 0x10063778/0x10063524).
float distortion_f1d(float t);

// ---------------------------------------------------------------------------
// CDistortion — curves 0/1/2 (a.d.1 / a.d.2 / d.d.).
// ---------------------------------------------------------------------------
struct Distortion {
  int sampleRate = 44100;
  int curve = 0;              // 0/1/2
  float drive = 1.0f;         // from ctl1
  float level = 0.0f;         // (raw + 2)/129  -> 0.0155 .. 1.0
  float norm = 1.0f;          // 2/f0(drive) (curve 0) or 1/f1d(drive) (1/2)

  // curve-0 chain coefficients, derived from sample rate (FUN_10006a00,
  // decomp_all_32_full.c:4697-4731; .rdata doubles pinned 0x100638f0=
  // 540.3539364174444, 0x100638d8=194.77874452256717, 0x10063918=
  // 41035.483241189875, 0x10063900=829.3804605477054, 0x10063908=
  // 1218.9379495928397, 0x10063818=1.0). Chain-block offsets in parens.
  float k18 = 0.0f;  // +0x18  540.3539/sr
  float k28 = 0.0f;  // +0x28  1/(194.7787/sr + 1)
  float k40 = 0.0f;  // +0x40  41035.483/sr
  float k50 = 0.0f;  // +0x50  829.3805/sr
  float k60 = 0.0f;  // +0x60  1/(194.7787/sr + 1)
  float k78 = 0.0f;  // +0x78  41035.483/sr
  float k88 = 0.0f;  // +0x88  1218.9379/sr
  float k98 = 0.0f;  // +0x98  1/(194.7787/sr + 1)

  // curve-0 3-stage gate/servo cascade state, per channel (FUN_100070d0
  // chain block; servo gate feeds +0xb0/+0xb8/+0xc0, differentiator
  // histories +0x30/+0x68/+0xa0).
  struct Chain0 {
    float off1 = 0.0f, d1 = 0.0f, g1d = 0.0f, y1 = 0.0f;   // +0x1c/2c/30/44
    float s2 = 0.0f, d2 = 0.0f, g2d = 0.0f, y2 = 0.0f;     // +0x54/64/68/7c
    float s3 = 0.0f, d3 = 0.0f, g3d = 0.0f;                // +0x8c/9c/a0
    float g1s = 0.0f, g2s = 0.0f, g3s = 0.0f;              // +0xb0/b8/c0
  };
  Chain0 c0L, c0R;

  // post tone filter (ctl2): 2-section biquad cascade (FUN_10006c00 coeffs,
  // FUN_10006ff0 kernel). Section 0: lowpass fc=400*40^(ctl2/127) Hz.
  // Section 1: fixed 100 Hz highpass (sr-derived only). Coeffs shared
  // across channels; state per channel (64-bit twin FUN_180007dc0 uses
  // per-channel state slots +0x84../+0x94.. rel section; the 32-bit mono
  // kernel FUN_10006ff0 has one state set - image difference, class #45).
  struct ToneCoeffs { float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
  struct ToneState { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
  int toneCut = 64;  // ctor default: CDistortion ctor sets +0x14c = 64
  ToneCoeffs toneS0, toneS1;
  ToneState toneL0, toneL1, toneR0, toneR1;

  Distortion() { deriveCoeffs0(); setToneCut(toneCut); }

  void setSampleRate(int sr);
  void setCurve(int c);
  void setDrive(int ctl1raw);   // drive = distortion_drive(ctl1raw)
  void setLevel(int raw);       // (raw + 2)/129
  void setToneCut(int raw);     // ctl2 -> 2-section biquad coeffs (FUN_10006c00)
  void reset();                 // zeroes tone state; WARM curve-0 chain
  void resetOnEnable();         // effector on-edge: tone only - the device
                                // chain block is ctor-warmed once and its
                                // state persists across on/off (kernel not
                                // called while bypassed; FUN_10006240 is
                                // the only warm site).
  void process(float* left, float* right, int n);

 private:
  void recomputeNorm();
  void deriveCoeffs0();
  float stepChain0(Chain0& c, float x, float d) const;
  void warmChain0(Chain0& c);
  float toneStep(ToneState& s0, ToneState& s1, float x) const;
};

// ---------------------------------------------------------------------------
// CDecimator — bit-crush + sample-and-hold, dry/wet mixed (FUN_180005630).
// ---------------------------------------------------------------------------
struct Decimator {
  int sampleRate = 44100;
  int period = 1;             // ctl1 + 1  (sample-and-hold period)
  unsigned mask = ~0u << 1;   // ctl2: ~0u << ((ctl2 >> 3) + 1)
  float mix = 0.0f;           // level / 127  (dry/wet)
  unsigned cnt = 0;
  float heldL = 0.0f, heldR = 0.0f;

  void setSampleRate(int sr) { sampleRate = sr; }
  void setPeriod(int ctl1raw) { period = ctl1raw + 1; }
  void setCrush(int ctl2raw);
  void setMix(int raw) { mix = (float)raw * FX_RAW_UNIT; }
  void reset() { cnt = 0; heldL = 0.0f; heldR = 0.0f; }
  void process(float* left, float* right, int n);
};

// ---------------------------------------------------------------------------
// CRingMod — quadrature oscillator x signal (FUN_180016ee0).
// Boot/reset quadrature seed (s0,s1) = (0.0, 1.0) — m3-106-ringmod law #106:
// ctor FUN_10014a20 (decomp_all_32_full.c:15550-15573) writes +0x18 s0 = 0,
// +0x1c s1 = 1.0 (x87 site Ghidra folds; advisor-verified); the CEffector
// ctor's ctl1=0/level=0 broadcasts preserve (0,1) — setFreq FUN_10014be0
// (:15659-15690) clamps s0 to [-1,1] and re-normalises
// s1 = sign(s1)*sqrt(1-s0^2), which fixes (0,±1) points.
// setFreq carries that clamp + re-normalisation (no setter re-seeds).
// ---------------------------------------------------------------------------
struct RingMod {
  int sampleRate = 44100;
  float freq = 1000.0f;       // pow(8000, ctl1/127) Hz
  float k = 0.0f;             // 2*sin(pi*freq/sr)
  float level = 0.0f;         // level/127 (0 bypass .. 1 full ring-mod)
  float s0 = 0.0f, s1 = 1.0f; // quadrature state (+0x24/+0x28), ctor seed

  void setSampleRate(int sr);
  void setFreq(int ctl1raw);  // freq = pow(8000, ctl1/127) + renorm
  void setLevel(int raw) { level = (float)raw * FX_RAW_UNIT; }
  void reset() { s0 = 0.0f; s1 = 1.0f; }  // ctor seed re-assert
  void process(float* left, float* right, int n);

 private:
  void recomputeK();
};

// ---------------------------------------------------------------------------
// CCompressor — 32-bit ref-path law (m3-u1-dist-comp-land, #211).
// Kernel FUN_100047c0 mono (decomp_all_32_full.c:3102-3141) / FUN_10004880
// stereo (:3147-3233), per channel env +0x10/+0x44, thrLin +0x14/+0x48,
// k +0x2c/+0x60, makeup2 +0x78, makeup1 +0x74:
//   over = max(0, (x & 0x7fffffff) - thrLin)          ; DAT_10063620 mask
//   env  = (env - over)*k + over
//   env  := 0 when env is a NONZERO SUBNORMAL         ; :3127-3129 flush
//   out  = (thrLin*x)/(env + thrLin) * makeup2 * makeup1
// Bypass leg (*param_3 != 0 -> vtable+0x28 on +0xc/+0x40, :3138/3229) is
// unreachable through the engine: the F9 gate (Effector::process, law #147)
// zeroes the buffer and clears the flag before dispatch, so the kernel
// always enters with flag == 0.
// Boot = device post-broadcast (CEffector ctor FUN_1000ac30(0)/ae40(0)):
// thrLin 0.1, makeup2 0.1 (level 0), makeup1 1.0 (boot threshold -20 dB),
// k = exp(-1/(44100*0.0001)) (ctl2 0), env 0.
// ---------------------------------------------------------------------------
struct Compressor {
  int sampleRate = 44100;
  float thrDb = -20.0f;       // -20 - 40*u  -> -20 .. -60 dB (FUN_10004ac0)
  float thrLin = 0.1f;        // expf(thrDb*ln10/20) (FUN_10004cb0)
  float k = 0.79711413f;      // exp(-1/(t*sr)), +0x2c/+0x60; ctl2=0 @44100
  float makeup1 = 1.0f;       // +0x74: 10^((-20-thrDb)/20) (FUN_10004ac0)
  float makeup2 = 0.1f;       // +0x78: 10^(1.5*level/127 - 1) (FUN_10004b60)
  int ctl2raw = 0;            // cached for sample-rate re-solve
  float envL = 0.0f, envR = 0.0f;   // +0x10 / +0x44

  void setSampleRate(int sr);
  void setThreshold(int ctl1raw);   // thrDb law + thrLin + makeup1
  void setTimes(int ctl2raw);       // t = 2000^sqrt(u)*0.1 ms -> k
  void setLevel(int raw);           // makeup2 (FUN_10004b60)
  void reset() { envL = 0.0f; envR = 0.0f; }
  void process(float* left, float* right, int n);
};

// ---------------------------------------------------------------------------
// CPhaser — feedback-around-allpass phaser. 32-bit render DLL cites
// (decomp_all_32_full.c): kernel FUN_10014770 :15410-15475, per-block
// wrapper FUN_100146c0 :15359-15408, bounds setter FUN_10014900
// :15487-15515, rate setter FUN_100149a0 :15517-15537, ctor FUN_100141a0
// :15106-15165, ctl1/ctl2/level broadcasts FUN_1000ac30/ad50/ae40,
// type->sections FUN_1000aed0. Live-pinned state at bank07_103:
// tasks/m3-u1-phaser/scan/scan103.csv (g=200, step 0x3f7fffff, w 0.5,
// lo 804.4736, hi 1243.0488, fb 0.8198438, 4/sr 9.0703e-05).
// Law (m3-u1-phaser F1/F2/F7 + m3-u1-phaser-land transfer ID):
//   g-domain raw scale: v = (1 - g*4/sr)/(1 + g*4/sr)  (DAT_100638e8=4.0f)
//   ctl1 (79): lo = 100*10^((127-x)/127), hi = 1000*10^(x/127); g re-seeds
//     to lo ONLY when g == 0 (DAT_1006351c = 0.0f); ctor seeds g = 200 and
//     the CEffector ctor ctl1=0 broadcast collapses lo=hi=1000 WITHOUT
//     re-seeding (g != 0) -> the ctor seed survives into the render.
//   step = exp(ln(hi/lo)/(0.5*sr/rate)) (DAT_100637f8=0.5f: one-way sweep
//     0.5/rate s, ping-pong period 1/rate), stored direction-preserving:
//     if 1.0 < step_old store step else store 1/step.
//   per host block: g < lo and step <= 1 and step != 1 -> step = 1/step;
//     g > hi and step >= 1 and step != 1 -> step = 1/step (bounce, never
//     wrap, never re-seed).
//   per sample: g *= step; the comb accumulator IS the all-pass chain
//     working variable (fVar9 rewritten in place): acc = fb*acc + x;
//     y = AP_chain_N(v)(acc); out = 0.5*x + 0.5*y — the feedback loop
//     closes AROUND the chain, and the 0.5 dry/wet (+0x24, ctor-only
//     writer, #104) halves the wet path.
// ---------------------------------------------------------------------------
struct Phaser {
  int sampleRate = 44100;
  int nPairs = 1;             // 1/2/4/6 (types 6..9)
  float fb = 0.0f;            // feedback -0.99 .. +0.99 (from level)

  float rate = 0.01f;         // Hz (from ctl2; ctor 0.1 then broadcast 0.01)
  float gLo = 1000.0f, gHi = 1000.0f;  // ctl1 bounds post ctor-broadcast
  float g = 200.0f;           // raw g state (+0x94; ctor seed 200)
  float gStep = 1.0f;         // direction-carrying per-sample multiplier
  float w4sr = 4.0f / 44100.0f;  // +0x90 = DAT_100638e8(4.0f) / sr
  bool frozen = false;        // test hook: hold v constant

  // all-pass state: 4 floats per pair (h0,h1 = R; h2,h3 = L).
  std::array<float, 4 * FX_PHASER_MAX_PAIRS> state{};
  float accL = 0.0f, accR = 0.0f;   // feedback accumulators (+0x9c/+0xa0)

  void setSampleRate(int sr);
  void setPairs(int p);       // clamped to [1, FX_PHASER_MAX_PAIRS]
  void setFeedback(int levelRaw);   // (raw-64)*(1/64)*0.99
  void setCtl1(int ctl1raw);        // bounds law + g-reseed + step
  void setSweepRate(int ctl2raw);   // ctl2 -> Hz
  void reset();

  // test hook: freeze the all-pass coefficient v (g via the g->v map).
  void freezeAt(float v) { frozen = true; g = (1.0f - v) / ((1.0f + v) * w4sr); gStep = 1.0f; }

  void process(float* left, float* right, int n);

 private:
  void recomputeStep();
};

// ---------------------------------------------------------------------------
// CEffector — the container / router.
// ---------------------------------------------------------------------------
struct Effector {
  int sampleRate = 44100;
  int onOff = 0;              // +0x40-ish: 0 bypass / 1 on
  int effectType = 0;         // +0x1c 0..9
  int ctl1 = 0;               // +0x20 raw 0..127
  int ctl2 = 0;               // +0x24 raw 0..127
  int level = 0;              // +0x28 raw 0..127

  Distortion dist;
  Compressor comp;
  RingMod rm;
  Decimator dec;
  Phaser ph;

  // F9 silent-flag/budget gate (law #147, FUN_180008a50 decomp_all.c:
  // 5470-5495 stereo / FUN_1800089e0 :5433-5467 mono; 32-bit twins
  // FUN_10007cd0/FUN_10007d80 decomp_all_32_full.c:5358-5438):
  //   flag==0            -> counter=0, kernel runs            (live)
  //   counter<=cap       -> counter+=len, zero buffer, flag=0, kernel
  //                         runs on the zeroed buffer          (ring-out)
  //   cap<counter        -> return: kernel skipped, buffer/flag/
  //                         counter untouched                  (FROZEN)
  // cap = (int)((float)sr * 0.025f) (sr handler FUN_1800076f0
  // decomp_all.c:4875-4888, _DAT_180072fe8=0.025f; 1102 @44100).
  // Device fields: counter +0x2c, cap +0x30 (64-bit); +0x20/+0x24 (32).
  int silentCounter = 0;
  int silentBudget = 1102;  // (int)(44100.0f * 0.025f); re-set by set_sample_rate

  void set_param(int param_id, int value);
  void set_sample_rate(float sr);
  void reset();
  void process(float* left, float* right, int num_samples, int* silent);

 private:
  void resetOnEnable();       // on-edge state clear; curve-0 chain NOT re-warmed
  void applyType(int type);   // FUN_18000c610: route + broadcast curve/stages
  void broadcastCtl1();       // FUN_18000c400
  void broadcastCtl2();       // FUN_18000c4e0
  void broadcastLevel();      // FUN_18000c580
};

}  // namespace synth1::fx
