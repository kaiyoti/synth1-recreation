// fx_chain.h — the master post-voice FX chain (Milestone 4 / Stage 6).
//
// This is the portable form of the engine's CSignalDevice tail: the block that
// sits between the voice mixer and the plugin output.
//
// Spec: out/spec_lfo_fx.md §0 (the CSignalDevice chain + engine ctor wiring
//       FUN_18001b5c0), §9 (signal order), §12 (the delay tone buffer ⇄ chorus
//       insert wiring), §13 (per-sample evaluation order); §5 (CEffector),
//       §2 (CDelay), §3 (CChorus), §4 (CEq), §6 (CPan).
// Ghidra (out/decomp_all.c, image base 0x180000000):
//   engine ctor wiring   FUN_180025110 (effector←mix, eqA←effector,
//                        eqB←eqA, pan←eqB, delay←pan, chorus←delay)
//   chain driver         chorus vftable+0x18 pulls prev->vftable+0x18()
//   parameter dispatcher FUN_180028fe0 region (case 0x23..0x53 = params 35..83)
//
// Signal order (§0 / §9, authoritative — NOT the memory-layout order):
//
//   input stereo → CEffector → CEq(+0x9a98) → CEq(+0x9b18) → CPan(+0x9b98)
//                → CDelay(+0x9538)  [feedback insert = CEq(+0x95e8), param 98]
//                → CChorus(+0x9668) → output
//
// The chorus is wired to the delay's tone buffer (§12): the delay's
// tone-filtered taps are summed into the chorus ring input, so the delay's
// tone send is modulated by the chorus LFO. FxChain wires
// chorus.setToneInput(delay.toneL/toneR) whenever the delay is on.
//
// Parameter map (engine dispatcher, verified against the decompiled cases):
//   77 effect on/off      78 effect type   79 ctl1   80 ctl2   81 level   → effector
//   35 delay beat index   36 delay feedback   37 delay d/w
//   65 delay on/off       82 delay mode      83 delay spread    98 delay tone
//   52 chorus base delay  53 chorus depth    54 chorus rate
//   55 chorus feedback    56 chorus level    64 chorus type     66 chorus on/off
//   60 eq tone  61 eq freq  62 eq level  63 eq Q   (73 = eq on/off, both bands)
//   90 pan
// NOTE on the task's "64..66 → delay" grouping: the binary dispatches 64 to
// CChorus::setMode (case 0x40 → FUN_1800040e0) and 66 to CChorus::setOn
// (case 0x42 → FUN_180004060); only 65 is CDelay::setOn (case 0x41 →
// FUN_180006a80). FxChain follows the binary: 64/66 → chorus, 65 → delay.
//
// EQ / Pan run the REAL devices now (CEq / CPan from core/fx/eq.h and
// core/fx/pan.h). Param laws (all verified against the decompiled
// dispatcher, FUN_180028fe0 / FUN_18002a5c0):
//   61 → eqA freq    pow(320, x/127) · 50   (50 … 16 kHz)
//   62 → eqA gain    (x − 64)/127 · 50 dB   (±50)
//   63 → eqA Q       pow(10, x/127)         (1 … 10)
//   60 → eqB shelf   type: tone < 65 → low shelf, tone ≥ 65 → high shelf
//                    freq: tone < 65 → 200 · 110^(tone/64)   (200 … 22 kHz)
//                          tone ≥ 65 → 10 · 800^((tone−64)/64) (10 … 7206 Hz)
//   73 → enables both bands (engine writes the CEq enable from value == 1)
//   90 → pan         1/64 left half, 1/63 right half (spec §5/§6)
// PORT DEVIATION (eqB gain): the engine's shelf KERNELS are unity-gain
// one-pole LP/HP (DC/low gain 1, stopband 0) scaled by the +0x30 output gain
// (10^(dB·0.05)); the dispatcher never calls setGain on +0x9b18, so the
// binary's tone shelf is a unity-gain LP/HP. The port CEq implements RBJ
// shelves (0 dB ⇒ identity), so the chain gives eqB the same level law as
// eqA (param 62, centre 64 ⇒ 0 dB) and Q from param 63: at the engine's
// eqB gain of 0 dB the port band is transparent, and a moved level knob
// drives the shelf audibly. The tone ⇒ shelf-type/freq law is exact.
//
// STATUS (2026-10-06): the swap is APPLIED — core/fx/eq.h + eq.cpp (CEq) and
// core/fx/pan.h + pan.cpp (CPan) are members of the chain and run in
// process(); add core/fx/eq.cpp + core/fx/pan.cpp to the standalone link line
// (the suite in tests/CMakeLists already builds both).
//
// Documented port notes:
//  * Each sub-device runs IN PLACE (delay.h / chorus.h / effector.h deviations):
//    the chain's prev-pull, *ioSilent latency flags and the silence/NaN fill
//    are the engine's job, not the chain's.
//  * process() chunks the block at FX_CHAIN_MAX_FRAMES = 2048 frames, the
//    smaller of the delay's tone scratch (DELAY_MAX_FRAMES) and the chorus's
//    per-call cap (CHORUS_MAX_FRAMES). The tone buffer is indexed from 0 for
//    every chunk, which matches how chorus.process() reads it.
//  * Chain-mode mixing (resolved 2026-10-07, FUN_180006810 / FUN_1800036d0
//    tails): the delay runs with chainMode = true — the chain buffer keeps
//    only wet·input and the dry·tone taps stay in the tone buffer. The
//    chorus consumes the tone: added directly to the output when it is off
//    (FUN_18001acd0), fed into its ring write when on. No double-count.
//    Standalone delay tests keep the old out = wet·in + dry·tone contract.
//  * Param 83 (spread) and param 52 (chorus base delay) now use the
//    decompiled laws: spread = sign(x−64)·1e-4·pow(1000, min(|x−64|/63,1)) s
//    (FUN_180025ae0 → FUN_180006b80); base delay = 0.05·pow(300, pow(2u,0.6))
//    ms for u < 0.5 else 30·u ms, u = x/127 (case 0x34).
//
// No realtime allocations: every member is a POD state holder with fixed
// std::array storage (the chain is ~1.1 MB — declare it static / at init time).
// process() performs zero heap allocations.

#pragma once

#include "fx/chorus.h"
#include "fx/delay.h"
#include "fx/effector.h"
#include "fx/eq.h"
#include "fx/pan.h"

#include <algorithm>
#include <cstdint>

namespace synth1::fx {

// per-process() frame cap: min(DELAY_MAX_FRAMES, CHORUS_MAX_FRAMES).
// m3-fxchain-grid (#282): the device caps every chain call at 0x100 frames
// (mixer FUN_100135b0 :14545-14548 local_10 = min(param_1, 0x100), the
// capped n propagates up the pull chain FUN_10024ea0 :28528), so the
// engine grid is 256, not the scratch bound 2048.
inline constexpr int FX_CHAIN_MAX_FRAMES = 256;
inline constexpr float FX_CHAIN_DEFAULT_TEMPO = 120.0f;

// parameter ids (engine dispatcher)
inline constexpr int FXCHAIN_PARAM_DELAY_BEAT = 35;
inline constexpr int FXCHAIN_PARAM_DELAY_FB = 36;
inline constexpr int FXCHAIN_PARAM_DELAY_DW = 37;
inline constexpr int FXCHAIN_PARAM_CHORUS_DELAY = 52;
inline constexpr int FXCHAIN_PARAM_CHORUS_DEPTH = 53;
inline constexpr int FXCHAIN_PARAM_CHORUS_RATE = 54;
inline constexpr int FXCHAIN_PARAM_CHORUS_FB = 55;
inline constexpr int FXCHAIN_PARAM_CHORUS_LEVEL = 56;
inline constexpr int FXCHAIN_PARAM_EQ_TONE = 60;
inline constexpr int FXCHAIN_PARAM_EQ_FREQ = 61;
inline constexpr int FXCHAIN_PARAM_EQ_LEVEL = 62;
inline constexpr int FXCHAIN_PARAM_EQ_Q = 63;
inline constexpr int FXCHAIN_PARAM_CHORUS_MODE = 64;
inline constexpr int FXCHAIN_PARAM_DELAY_ONOFF = 65;
inline constexpr int FXCHAIN_PARAM_CHORUS_ONOFF = 66;
inline constexpr int FXCHAIN_PARAM_EQ_ONOFF = 73;
inline constexpr int FXCHAIN_PARAM_PAN = 90;
inline constexpr int FXCHAIN_PARAM_DELAY_TONE = 98;

// ---------------------------------------------------------------------------
// FxChain — effector → eqA → eqB → pan → delay → chorus.
// ---------------------------------------------------------------------------
struct FxChain {
  Effector effector;
  Delay delay;
  Chorus chorus;
  CEq eqA;       // engine +0x9a98 — parametric peak band (params 61..63)
  CEq eqB;       // engine +0x9b18 — tone shelf band (param 60, + level/Q)
  CPan pan;      // engine +0x9b98 (param 90)

  float sampleRate = 44100.0f;
  float tempoBpm = FX_CHAIN_DEFAULT_TEMPO;
  int delayBeatIndex = 0;   // param 35 latch, re-applied whenever tempo changes

  // Propagates the sample rate to every sub-device, then re-derives the
  // tempo-synced delay time (the binary re-runs FUN_180028ac0 on sr/tempo).
  void set_sample_rate(float sr);

  // Propagates tempo to the delay (param 35 grid recalculation, §2.4).
  void set_tempo_bpm(float bpm);

  // Engine parameter dispatch (see the map in the header comment).
  void set_param(int id, int value);

  void reset();

  // Runs the whole chain IN PLACE over num_frames stereo frames, chunked at
  // FX_CHAIN_MAX_FRAMES. Zero heap allocations.
  void process(float* left, float* right, int num_frames);
};

}  // namespace synth1::fx
