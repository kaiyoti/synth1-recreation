// delay.h — CDelay "Tempo Delay" (post-voice chain device, engine +0x9538).
//
// Spec: out/spec_lfo_fx.md §2 (CDelay), §2.1 layout, §2.2 taps,
//       §2.3 per-sample process (normal/crossed + ping-pong),
//       §2.3.3 dry/wet, §2.4 delay-time parameters (TB[20] tempo grid),
//       §12 the shared tone buffer + feedback-insert wiring.
// Ghidra (out/decomp_all.c, image base 0x180000000):
//   CDelay ctor            FUN_180005780 (vftable 0x180072e48, size 0xb0)
//   tap computation        FUN_1800059c0
//   ping-pong process      FUN_180005b20
//   normal/crossed process FUN_180006090
//   device process         FUN_180006810
//   dry/wet (param 37)     FUN_1800069e0
//   setTime (seconds)      FUN_180006a40 / FUN_180006790
//   on/off (param 65)      FUN_180006a80  (rising edge clears the lines)
//   feedback (param 36)    FUN_180006b10  (clamp [0, DAT_180072e90])
//   tempo-sync (param 35)  FUN_180028ac0  (TB[20] @0x180093a90)
//   tone insert (param 98) FUN_180028b10  (CEq type 1/2 at engine+0x95e8)
//
// Verified .rdata constants (read directly from bin/Synth1_vst64_113b3.dll):
//   DAT_180072e90 = 0.999f  (feedback max — the spec's original 0.9 was wrong)
//   DAT_180072e80 = 1/63f   (dry/wet law denominator — NOT 1/127; the
//                            continuous law is wet = (127−x)/63 for x > 64,
//                            dry = (max(x,1)−1)/63 for x ≤ 64, both 1.0 at
//                            x = 64; spec §2.3.3's /127 is a transcription slip)
//   DAT_180072e98 = 0.5, DAT_180072ea0 = 1.0, DAT_180072e84 = 0.15f
//                            → tapSlew K = 1 − 0.5^(1/(sr·0.15)) (τ≈150 ms)
//   DAT_180072e88 = 1e-4    (min tap 0.1 ms), DAT_180072eac = 131072.0f
//   DAT_180072d90 = 0.1     (tap-slew convergence epsilon in the process loop)
//   tone low shelf:  f = pow(110.0, x/63) · 200     (200 … 22000 Hz)
//   tone high shelf: f = pow(800.0, (x−65)/62) · 10  (10 … 8000 Hz)
//
// Channel-tap convention (from the decompiled read/write loops + tap setter
// FUN_100051a0:3656-3662 / FUN_1800059c0, NOT the spec's §2.3 prose): the L
// channel/line is read at rightTap (+0x44/+0x60) and the R channel/line at
// leftTap (+0x48/+0x64). Positive spread (+0x54, seconds) moves rightTap →
// delays the L line; negative spread delays the R line (D1, m3-d1-pingpong-2).
//
// Documented port deviations (the binary's device-chain machinery):
//  * process() runs the delay IN PLACE (the engine chain's prev->process and
//    the *ioSilent latency flags are the engine's job; the standalone device
//    receives the block already carrying the dry input).
//  * The dry path: the binary leaves the (dry) input in the output buffer
//    (scaled by wet) and the tone-filtered taps in the shared tone buffer
//    (scaled by dry, consumed by the chorus insert send). The standalone
//    device forms the audible output itself:
//      out = wet·dryInput + dry·tone(taps)   (all modes)
//    with line[n] = dryInput[n] + feedback·tone(line[n−tap]) — the exact
//    recurrence of spec §2.3.1 (ping-pong per §2.3.2 / U4).
//  * The feedback "inserts" array (4 pointers) collapses to one integrated
//    one-pole shelf tone filter (the slot-0 tone CEq; slots 1..3 are null in
//    the engine ctor).
//  * While the taps are slewing the sub-block loop runs one sample per
//    iteration (the binary breaks its 4×-unrolled loop at the tap
//    discontinuity; the arithmetic is identical).
//
// No realtime allocations: the lines are fixed std::array storage
// (131072 frames per line, mask 0x1ffff = the binary's +0x38 = 0x20000).

#pragma once

#include <array>
#include <cstdint>

namespace synth1::fx {

inline constexpr int DELAY_LINE_SIZE = 131072;   // +0x38 (0x20000)
inline constexpr int DELAY_LINE_MASK = 0x1FFFF;  // & 0x1ffff
inline constexpr int DELAY_MAX_FRAMES = 2048;    // per-process() frame cap (tone scratch)
inline constexpr float DELAY_FB_MAX = 0.999f;    // DAT_180072e90 (spec-corrected)
inline constexpr float DELAY_TAP_EPS = 0.1;      // DAT_180072d90 convergence epsilon
// FUN_180005ab0 ring-out threshold: the budget is the number of feedback
// periods needed to decay to 2^-15 (DAT_180072b68 = 3.0517578125e-05), +2.
inline constexpr double DELAY_TAIL_EPS = 3.0517578125e-05;  // DAT_180072b68 = 2^-15

// delay mode (+0x44)
inline constexpr int DELAY_MODE_NORMAL = 0;
inline constexpr int DELAY_MODE_CROSSED = 1;
inline constexpr int DELAY_MODE_PINGPONG = 2;

// tone filter (the param-98 feedback-insert shelf)
inline constexpr int DELAY_TONE_OFF = 0;
inline constexpr int DELAY_TONE_LOW_SHELF = 1;
inline constexpr int DELAY_TONE_HIGH_SHELF = 2;

// tempo-sync beat table (param 35, TB[20] @0x180093a90): beats = TB[i]/24.
inline constexpr int kDelayBeatTable[20] = {
  0, 1, 2, 3, 4, 6, 8, 9, 12, 16, 18, 21, 24, 32, 36, 42, 48, 72, 84, 96
};

// tempo-sync delay time in seconds: (TB[clamp(idx,0,19)]·60) / (tempoBpm·24)
float delay_sync_seconds(int idx, float tempoBpm);

// dry/wet (param 37 law, FUN_1800069e0): x = 0..127.
float delay_wet_level(int x);
float delay_dry_level(int x);

struct Delay {
  // ---- state (binary byte offsets in comments, §2.1) ----
  int sampleRate = 44100;              // +0x10
  int writePos = 0;                    // +0x3c write cursor (frames)
  int onOff = 0;                       // +0x40 (boot set by init-sound replay)
  int mode = 0;                        // +0x44
  float feedback = 0.7f;               // +0x48 (ctor 0x3f333333)
  float dry = 1.0f;                    // +0x4c
  float wet = 1.0f;                    // +0x50
  float spread = 0.1f;                 // +0x54 (ctor 0x3dcccccd)
  // Chain mode (the FxChain wiring): the binary's device process leaves only
  // wet·input in the chain buffer and scales the tone buffer by dry
  // (FUN_180006810 tail); the chorus consumes the tone (added directly when
  // it is off, fed into its ring when on). Standalone mode (tests) mixes
  // out = wet·in + dry·tone itself instead.
  bool chainMode = false;
  float timeSec = 100.0f;              // +0x58 (ctor 0x42c80000)
  int timeSamp = 0;                    // +0x5c = (int)(sr·timeSec)
  int rightTap = 0;                    // +0x60 (L line/channel reads this tap)
  int leftTap = 0;                     // +0x64 (R line/channel reads this tap)
  int slewing = 0;                     // +0x70 fractional-tap flag
  double rightLive = 0.0;              // +0x78
  double leftLive = 0.0;               // +0x80
  float tapSlew = 0.0f;                // +0x88 = 1 − 0.5^(1/(sr·0.15))

  // Ring-out tail budget (FUN_180005ab0): frames of silent input the device
  // keeps ringing before it reports its output silent. silentFrames is the
  // accumulated silent-input frame count (+0x6c); the ctor seeds it at
  // 99999999 (0x5f5e0ff) so a fresh device is "exhausted" until real input
  // arrives and resets it. outActive mirrors the +0x20 output-silent flag.
  int tailBudget = 1;                  // +0x68 (ctor 1)
  int silentFrames = 99999999;         // +0x6c (ctor 0x5f5e0ff)
  int outActive = 0;                   // +0x20

  // integrated tone filter (the slot-0 feedback insert, the param-98 shelf):
  int toneType = DELAY_TONE_OFF;       // engine gate +0x9604
  float toneFreq = 0.0f;               // Hz
  float toneA0 = 0.0f;                 // kernel coefficient (type-dependent)
  float toneA2 = 0.0f;
  float toneX1L = 0.0f, toneX1R = 0.0f;
  float toneY1L = 0.0f, toneY1R = 0.0f;

  // ping-pong mono lines / normal-mode stereo pair (planar, mask-indexed):
  std::array<float, DELAY_LINE_SIZE> lineL{};
  std::array<float, DELAY_LINE_SIZE> lineR{};
  // tone scratch (the binary's shared engine tone buffer, 131072 stereo
  // frames; the port indexes it by global frame offset, so it must span a
  // whole process() call — same size/mask as the lines):
  std::array<float, DELAY_LINE_SIZE> toneL{};
  std::array<float, DELAY_LINE_SIZE> toneR{};

  void setSampleRate(int sr);
  // free time (FUN_180006a40): seconds; re-derives timeSamp + taps.
  void setTimeSeconds(float seconds);
  // tempo sync (param 35, FUN_180028ac0): TB[clamp(idx)]/24 beats at tempo.
  void setTempoSyncBeatIndex(int idx, float tempoBpm);
  // feedback (param 36, FUN_180006b10): clamped to [0, 0.999].
  void setFeedback(float fb);
  // dry/wet (param 37, FUN_1800069e0): raw 0..127.
  void setDryWet(int x);
  // spread (param 83): signed seconds.
  void setSpread(float seconds);
  // mode (param 82): 0 normal / 1 crossed / 2 ping-pong.
  void setMode(int m);
  // on/off (param 65, FUN_180006a80): rising edge zeroes both lines + cursor.
  void setOn(int on);
  // tone param 98 (FUN_180028b10): 0..63 low shelf 200→22000 Hz,
  // 64 = off, 65..127 high shelf 10→8000 Hz.
  void setToneParam(int x);
  // direct tone control (type, freq Hz ≥ 1).
  void setToneFilter(int type, float freqHz);

  // FUN_1800059c0: t = timeSamp − (timeSamp & 3) (floor-to-4, #97), spread offsets, clamp [0.1 ms, size−256],
  // snap the live taps when not slewing.
  void recomputeTaps();

  // FUN_180005ab0: tailBudget = (trunc(log(2^-15)/log(feedback)) + 2) ·
  // max(leftTap, rightTap), doubled in ping-pong mode. Called by the same
  // setters the binary tail-calls it from (setFeedback FUN_180006b10,
  // setSpread FUN_180006b80, setMode FUN_180006ba0, setTime FUN_180006790,
  // ctor FUN_180005780).
  void recomputeTailBudget();

  // Runs the delay IN PLACE over numFrames stereo frames (numFrames ≤
  // DELAY_MAX_FRAMES). Returns the number of frames processed.
  //
  // ioSilent (FUN_180006810's *param_4, optional): the upstream "input is
  // silent" flag. While it is set the device zeroes the incoming block,
  // accumulates silentFrames, clears the flag so the rest of the chain keeps
  // running, and keeps ringing the line — the wet tail. Once silentFrames
  // passes tailBudget (and the taps are not slewing) the device stops and
  // reports outActive = 0. Any non-silent input resets silentFrames to 0.
  int process(float* left, float* right, int numFrames, int* ioSilent = nullptr);

 private:
  // sub-block kernels (offset = frame offset into the caller's buffers, the
  // tone scratch is indexed by the same offset).
  int processNormal(float* left, float* right, int off, int count);
  int processPingPong(float* left, float* right, int off, int count);
};

}  // namespace synth1::fx
