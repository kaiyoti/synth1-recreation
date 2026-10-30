// chorus.h — CChorus "Chorus/Flanger" (post-voice chain tail, engine +0x9668).
//
// Spec: out/spec_lfo_fx.md §3 (CChorus), §3.1 layout (size 0xb0), §3.2
//       setters, §3.3 the internal quadrature LFO, §3.4 process order,
//       §3.5 the per-mode tap generation.
// Ghidra (out/decomp_all.c, image base 0x180000000):
//   CChorus ctor        FUN_1800034c0 (vftable 0x180072b18; defaults:
//                       base delay 0.02 s (DAT_180072b54), depth 0.5,
//                       rate 1.0 Hz, mode 2, level 1, feedback 0)
//   setBaseDelay        FUN_180003f00  (+0xa8 seconds; writePos = readPos+base)
//   setDepth            FUN_180003f90  (+0x4c = (int)(baseDelay·depth))
//   setRate             FUN_180004110  (+0xac Hz; lfoPeriod = (int)(sr/rate),
//                       k = +0x7c = 2·sin(π·rate/sr))
//   setFeedback         FUN_1800040b0  (+0x5c)
//   setLevel            FUN_180004100  (+0x64 — the wet/dry mix, param 56)
//   setMode             FUN_1800040e0  (accepts 1,2; everything else → 2)
//   setOn               FUN_180004060  (rising edge zeroes the 8192-frame line)
//   process             FUN_1800036d0  (vftable +0x18)
//
// Internal LFO: a 2-pole magic-circle (quadrature) oscillator
//   c = s0 − k·s1; s0 = c; s1 = c·k + s1,  k = 2·sin(π·rate/sr)
// with the energy clamp s1 > 0.9999 ⇒ (s0,s1) = (0, 0.9999) applied once per
// process() block (FUN_1800036d0 — the spec's "per sample" is a slip).
//
// Rate is the CONTINUOUS exponential law pow(20000, x/127) (spec §3.2,
// engine case 0x36; the "band table" language in dsp_flow.md is wrong —
// RECONSTRUCTION_PLAN.md §spec audit).
//
// Mode-3 blend weights (read from the decompiled loop, the spec §3.5 table
// has T1/T3 swapped): with T0 = mirrored L tap, T1 = direct L tap,
// T2 = direct R tap, T3 = mirrored R tap:
//   outL += 0.9·T1 + 0.1·T0 + 0.7·T2 + 0.3·T3
//   outR += 0.9·T0 + 0.1·T3 + 0.7·T2 + 0.3·T1
//   line[wp].L += T1·feedback,  line[wp].R += T2·feedback   (direct taps only)
//
// Wet/dry: the binary's mix is the `level` control (+0x64, param 56) applied
// as line = in·level with the modulated tap ADDED to the dry block
// (additive wet-over-dry; spec §3.5). The port exposes setWet/setDry (the
// engine wires dry = 1, wet = level).
//
// Port deviations: process() runs in place (prev->process / *ioSilent /
// latency guards are the engine's job); the shared delay-tone insert
// (+0x68/+0x70) is an optional external buffer fed by setToneInput().
// No realtime allocations (8192-frame ring, std::array).

#pragma once

#include <array>
#include <cstdint>

namespace synth1::fx {

inline constexpr int CHORUS_LINE_SIZE = 8192;    // +0x20 (0x10008/8 frames)
inline constexpr int CHORUS_LINE_MASK = 0x1FFF;  // & 0x1fff
inline constexpr int CHORUS_MAX_FRAMES = 2048;   // per-process() frame cap
// Block-size invariance: the write→read→feedback span is fixed to this many
// frames, counted from the start of each caller window (see chorus.cpp), so
// the engine is bit-identical at host bs 64/128/256/512. Must divide
// CHORUS_LINE_SIZE and be <= the smallest host block size the engine runs at.
inline constexpr int CHORUS_SUB_BLOCK_FRAMES = 64;
// m3-177-land: the device FX chain pulls every FX device in ≤256-frame
// calls (mixer FUN_100135b0 caps the chunk at 0x100, decomp_all_32_full.c:
// 14545-14548), so the once-per-call laws (e.g. the #93 energy clamp at the
// FUN_10003170 head) run on this grid inside a larger host block.
inline constexpr int CHORUS_CHAIN_CHUNK_FRAMES = 256;
inline constexpr float CHORUS_OSC_CLAMP = 0.9999f;  // DAT_180072b78 / 0x3f7ff972
// FUN_10003170 silent-window fill: objdump 10003220 mov $0x3089705f,%eax;
// 10003227 rep stos — f32 0x3089705f (= 1.0e-9f), not 0.0f (m3-177-land).
inline constexpr float CHORUS_SILENT_FILL = 1.0e-9f;  // 0x3089705f
// FUN_180003610 ring-out threshold: budget = (trunc(log(2^-15)/log|feedback|)
// + 2) · baseDelaySamples (DAT_180072b68 = 3.0517578125e-05 = 2^-15).
inline constexpr double CHORUS_TAIL_EPS = 3.0517578125e-05;  // DAT_180072b68

// chorus mode (+0x34): 1 = mono modulation (shared tap), 2 = stereo
// quadrature (ctor default), 3 = wide 4-tap flanger (setMode maps 3 → 2,
// per FUN_1800040e0; the mode-3 kernel is reachable by setting the field).
inline constexpr int CHORUS_MODE_MONO = 1;
inline constexpr int CHORUS_MODE_STEREO = 2;
inline constexpr int CHORUS_MODE_WIDE = 3;

struct Chorus {
  // ---- state (binary byte offsets in comments, §3.1) ----
  int sampleRate = 44100;              // +0x10
  unsigned writePos = 882;             // +0x38 write cursor (ctor: readPos+base)
  unsigned readPos = 0;                // +0x3c base read cursor (frames)
  int onOff = 0;                       // +0x30 (boot set by init-sound replay)
  int mode = CHORUS_MODE_STEREO;       // +0x34 (ctor 2)
  int baseDelaySamples = 882;          // +0x48 = (int)(sr·baseDelaySec)
  int depthSamples = 441;              // +0x4c = (int)(baseDelay·depth)
  float depthNorm = 1.0f;              // +0x50 (≤ 1: 2π·depth ≤ lfoPeriod)
  float depthParam = 0.5f;             // +0x54 raw depth
  int depthStep = 100;                 // +0x58 block-splitting hint
  float feedback = 0.0f;               // +0x5c
  int feedbackOn = 0;                  // +0x60
  float level = 1.0f;                  // +0x64 wet/dry mix (param 56)
  float dryGain = 1.0f;                // port: dry path gain (setMix), 1 = pure add
  int lfoPeriod = 44100;               // +0x78 = (int)(sr/rate)
  // ctor seeds the quadrature oscillator at (s0,s1) = (0, 0.9999) with the
  // 1 Hz step (FUN_1800034c0: phaseStep = 2·sin(π·1/44100)).
  float phaseStep = 1.42475857e-4f;    // +0x7c = 2·sin(π·rate/sr) @ 1 Hz
  float oscS0 = 0.0f;                  // +0x80 quadrature cos state
  float oscS1 = 0.9999f;               // +0x84 quadrature sin state
  float baseDelaySec = 0.02f;          // +0xa8
  float rateHz = 1.0f;                 // +0xac

  // Ring-out tail budget (FUN_180003610): frames of silent input the chorus
  // keeps ringing after the delay upstream has given up. silentFrames
  // (+0x28) is seeded to 99999999 (0x5f5e0ff) by the ctor, so a fresh chorus
  // is exhausted until real input arrives.
  int tailBudget = 0;                  // +0x2c
  int silentFrames = 99999999;         // +0x28 (ctor 0x5f5e0ff)
  // Port: mirrors the upstream delay's +0x20 output-active flag, which
  // FUN_1800036d0 dereferences through +0x70 before it starts counting.
  int delayActive = 1;

  std::array<float, CHORUS_LINE_SIZE> lineL{};
  std::array<float, CHORUS_LINE_SIZE> lineR{};

  // optional shared delay-tone insert (+0x68 / +0x70): when set, the ring
  // write becomes line[i] = (in[i] + tone[i])·level. The engine wires the
  // delay's tone buffer here; nullptr (default) = off.
  const float* toneInL = nullptr;
  const float* toneInR = nullptr;

  void setSampleRate(int sr);
  void setRate(float hz);            // FUN_180004110 (continuous rate)
  void setRateParam(int x);          // engine law pow(20000, x/127), param 54
  void setDepth(float depth);        // FUN_180003f90 (0..1)
  void setBaseDelaySeconds(float s); // FUN_180003f00
  void setFeedback(float fb);        // FUN_1800040b0
  void setLevel(float l);            // FUN_180004100 (the wet/dry mix)
  // Param-56-style dry/wet crossfade (continuous, 1.0 at x = 64 like the
  // delay's FUN_1800069e0 law): x ≤ 64: level = 1, dry = (max(x,1)−1)/63;
  // x > 64: dry = 1, level = (127−x)/63. process() then returns
  // dry·input + wet·(modulated taps).
  void setMix(int x);
  void setMode(int m);               // FUN_1800040e0 ({1,2}, else → 2)
  void setOn(int on);                // FUN_180004060 (rising edge zeroes line)
  void setToneInput(const float* toneL, const float* toneR);
  // Port: the upstream delay's output-active flag (see delayActive).
  void setDelayActive(int active) { delayActive = active; }

  // FUN_180003610: tailBudget = (trunc(log(2^-15)/log|feedback|) + 2) ·
  // baseDelaySamples. Tail-called by setFeedback (FUN_1800040b0) and
  // setBaseDelaySeconds (FUN_180003f00). Unlike the delay, the counter is
  // reset to 0 whenever it has not yet passed the old budget.
  void recomputeTailBudget();

  // Runs the chorus IN PLACE over numFrames stereo frames (numFrames ≤
  // CHORUS_MAX_FRAMES) in CHORUS_SUB_BLOCK_FRAMES sub-blocks counted from the
  // start of the window. Each sub-block writes its input into the ring at the
  // write cursor, then per sample advances the quadrature LFO, reads the
  // modulated tap(s), adds them to the output (additive wet-over-dry) and
  // writes the feedback. Returns the number of frames processed.
  // ioSilent (FUN_1800036d0's *param_4, optional): the upstream silent-input
  // flag. While set, the chorus zeroes the incoming block and rings its own
  // line until silentFrames passes tailBudget, then skips processing.
  int process(float* left, float* right, int numFrames, int* ioSilent = nullptr);

  // One write→read→feedback span (see chorus.cpp for the invariance note).
  int processSubBlock(float* left, float* right, const float* toneL,
                      const float* toneR, int numFrames);
};

}  // namespace synth1::fx
