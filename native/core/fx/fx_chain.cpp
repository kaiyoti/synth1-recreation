#include <cstdio>
// fx_chain.cpp — master post-voice FX chain implementation.
//
// Spec: out/spec_lfo_fx.md §0/§9 (chain order), §12 (delay tone buffer ⇄
// chorus insert), §2.4/§2.5 (delay params), §3.2 (chorus params), §4.1 (EQ
// laws), §5.2 (effector params), §6 (pan law). See fx_chain.h for the
// parameter map, the verified dispatch cases and the documented port notes.

#include "fx/fx_chain.h"

#include <cmath>
#include <cstdlib>

namespace synth1::fx {

namespace {
inline constexpr double INV127 = 1.0 / 127.0;
inline constexpr double INV64 = 1.0 / 64.0;
inline constexpr double INV99 = 1.0 / 99.0;   // DAT_1800755f8 (chorus depth)
}  // namespace

// ---------------------------------------------------------------------------
// FxChain
// ---------------------------------------------------------------------------
void FxChain::set_sample_rate(float sr) {
  sampleRate = sr;
  const int isr = (int)sr;
  effector.set_sample_rate(sr);
  delay.setSampleRate(isr);
  chorus.setSampleRate(isr);
  eqA.set_sample_rate(sr);
  eqB.set_sample_rate(sr);
  pan.set_sample_rate(sr);  // FUN_10013c80: dest-7 slew k recompute on sr-set
  // the binary re-derives the tempo-synced delay time on every sample-rate
  // change (FUN_180028830 / FUN_180028ac0).
  delay.setTempoSyncBeatIndex(delayBeatIndex, tempoBpm);
}

void FxChain::set_tempo_bpm(float bpm) {
  tempoBpm = (bpm > 0.0f) ? bpm : FX_CHAIN_DEFAULT_TEMPO;
  delay.setTempoSyncBeatIndex(delayBeatIndex, tempoBpm);
}

void FxChain::set_param(int id, int value) {
  switch (id) {
    // ---- CEffector (params 77..81, §5.2) ----
    case FX_PARAM_ONOFF:
    case FX_PARAM_TYPE:
    case FX_PARAM_CTL1:
    case FX_PARAM_CTL2:
    case FX_PARAM_LEVEL:
      effector.set_param(id, value);
      break;

    // ---- CDelay (§2.4 / §2.5) ----
    case FXCHAIN_PARAM_DELAY_BEAT:
      // FUN_180028ac0 receives the raw knob value and clamps it to the 20-entry
      // tempo table.
      delayBeatIndex = std::clamp(value, 0, 19);
      delay.setTempoSyncBeatIndex(delayBeatIndex, tempoBpm);
      break;
    case FXCHAIN_PARAM_DELAY_FB:
      // FUN_180006b10: clamp(x/127, 0, 0.999) — the clamp lives in setFeedback.
      delay.setFeedback((float)value * (1.0f / 127.0f));
      break;
    case FXCHAIN_PARAM_DELAY_DW:
      delay.setDryWet(value);
      break;
    case FXCHAIN_PARAM_DELAY_ONOFF:
      delay.setOn(value != 0);
      break;
    case FXCHAIN_PARAM_DELAY_TONE:
      delay.setToneParam(value);
      break;
    case 82:  // delay mode (FUN_180006ba0)
      delay.setMode(value);
      break;
    case 83: {  // delay spread (case 0x53, FUN_180025ae0 → FUN_180006b80):
      // x == 64 → early return (spread unchanged); else
      //   sign(x−64) · 1e-4 · pow(1000, min(|x−64|/63, 1)) seconds
      // (DAT_180075610 = 1/63, DAT_180072e88 = 1e-4, DAT_1800733a8 = 1000).
      // x = 0 → −0.1 s, x = 127 → +0.1 s; the plugin default 66 → +0.1 ms.
      const int d = value - 64;
      if (d == 0) {
        delay.setSpread(0.0f);
      } else {
        double e = std::fabs((double)d) * (1.0 / 63.0);
        if (e > 1.0) e = 1.0;
        const double s = 1e-4 * std::pow(1000.0, e);
        delay.setSpread((float)((d < 0) ? -s : s));
      }
      break;
    }

    // ---- CChorus (§3.2 engine cases 0x34..0x38, 0x40, 0x42) ----
    case FXCHAIN_PARAM_CHORUS_DELAY: {
      // case 0x34 (FUN_180025a60 → FUN_180003f00): u = x/127,
      //   u < 0.5 → 0.05 · pow(300, pow(2u, 0.6)) ms
      //   u ≥ 0.5 → 30 · u ms
      // (DAT_180072b80 = 2, DAT_180075748 = 300, DAT_180072ff8 = 0.05,
      //  DAT_180072d88 = 0.001, DAT_180072da8 = 30; FUN_180057cc0 = pow).
      // x = 0 → 0.05 ms, x = 64 → 15.12 ms (the plugin default display),
      // x = 127 → 30 ms.
      const double u = (double)value * (1.0 / 127.0);
      const double ms = (u < 0.5)
          ? 0.05 * std::pow(300.0, std::pow(2.0 * u, 0.6))
          : 30.0 * u;
      chorus.setBaseDelaySeconds((float)(ms * 0.001));
      break;
    }
    case FXCHAIN_PARAM_CHORUS_DEPTH:
      // (pow(100, x/127) − 1) · 1/99  → 0 … 1
      chorus.setDepth((float)((std::pow(100.0, (double)value * INV127) - 1.0) * INV99));
      break;
    case FXCHAIN_PARAM_CHORUS_RATE:
      chorus.setRateParam(value);   // pow(20000, x/127) Hz
      break;
    case FXCHAIN_PARAM_CHORUS_FB:
      // (x − 64) · (1/64) · 0.99  → −0.99 … +0.99
      chorus.setFeedback((float)((double)(value - 64) * INV64 * 0.99));
      break;
    case FXCHAIN_PARAM_CHORUS_LEVEL:
      chorus.setLevel((float)value * (1.0f / 127.0f));
      break;
    case FXCHAIN_PARAM_CHORUS_MODE:
      chorus.setMode(value);
      break;
    case FXCHAIN_PARAM_CHORUS_ONOFF:
      chorus.setOn(value != 0);
      break;

    // ---- CEq (params 60..63, 73, spec §4.1) / CPan (param 90, §5/§6) ----
    // eqA (+0x9a98 / 32-bit eff+0x8e40): parametric peak — freq 61, gain
    // 62, Q 63 (cases 0x3d..0x3f, decomp_all_32_full.c:29612-29623).
    // eqB (+0x9b18 / eff+0x8eb4): tone shelf — type + freq from param 60
    // (FUN_100270a0 :29892):
    //   tone < 65 → low shelf  float(200.0 · pow(110.0, float(tone)/64f))
    //   tone ≥ 65 → high shelf float(10.0 · pow(800.0, float(tone−64)/64f))
    // The dispatcher NEVER writes eqB's gain or Q (62/63 are eqA-only —
    // D6 guard asserted in ctest; the shelf output scale +0x24 stays 1).
    case FXCHAIN_PARAM_EQ_TONE:
      eqB.set_tone_param(value);   // FUN_100270a0 float32 law (eq.cpp)
      break;
    case FXCHAIN_PARAM_EQ_FREQ:
      eqA.set_freq_param(value);   // 50 · 320^(x/127), float32 order
      break;
    case FXCHAIN_PARAM_EQ_LEVEL:
      eqA.set_gain_param(value);   // (x − 64)·(1/127f)·50 dB — eqA ONLY
      break;
    case FXCHAIN_PARAM_EQ_Q:
      eqA.set_q_param(value);      // 10^(x/127) — eqA ONLY (case 0x3f
                                   // targets +0x8db0 only; eqB's Q is
                                   // never written, D4 removed here —
                                   // shelf kernels ignore Q, bit-inert)
      break;
    case FXCHAIN_PARAM_EQ_ONOFF: {
      // engine case 0x49: FUN_18000d2b0 writes CEq+0x20 — NOT the process
      // gate. The audio gate is +0x1c (FUN_18000e7b0), set by the ctor and
      // never touched by any param: the EQ bands are always live.
      // (m3-u3a-eqgate 2026-10-12: the "+0x14 gate bypasses the EQ"
      // reading of FUN_1000ce60/FUN_100191c0 is REFUTED at the audio
      // level — the eqA +24.8 dB boost is present in device renders with
      // +0x14 pinned 0, and gate 0 vs 1 renders are bit-identical on
      // L==R cells; the gate switches chain mono/stereo processing, not
      // EQ on/off. See tasks/m3-u3a-eqgate/RESULT.md F4. Re-scope
      // consult: tasks/consults/m3-u3a-scope.md.)
      break;
    }
    case FXCHAIN_PARAM_PAN:
      pan.set_pan_param(value);
      break;

    default:
      break;
  }
}

void FxChain::reset() {
  effector.reset();
  delay.lineL.fill(0.0f);
  delay.lineR.fill(0.0f);
  delay.toneL.fill(0.0f);
  delay.toneR.fill(0.0f);
  delay.writePos = 0;
  delay.slewing = 0;
  delay.toneX1L = delay.toneX1R = 0.0f;
  delay.toneY1L = delay.toneY1R = 0.0f;
  chorus.lineL.fill(0.0f);
  chorus.lineR.fill(0.0f);
  // Law #96: the chorus quadrature phasor boots at the CChorus ctor seed
  // (FUN_1800034c0: +0x80 = 0, +0x84 = 0x3f7ff972; 32-bit ctor: +0x64 = 0,
  // +0x68 = 0x3f7ff972) — (0, 0.9999), 90.00 deg. The device has
  // no other phasor re-seed (setOn FUN_180004060 zeroes only the line),
  // so a fresh render starts at the ctor value; the old (1,0) seed was a
  // 90 deg boot-phase error that made the once-per-block energy clamp
  // (law #93) fire at 11264 where the device never clamps.
  chorus.oscS0 = 0.0f;
  chorus.oscS1 = CHORUS_OSC_CLAMP;
  chorus.setToneInput(nullptr, nullptr);
  eqA.reset();
  eqB.reset();
  // CPan: fresh-instance slew state (the device ctor leaves +0x20/+0x24 0).
  pan.reset();
}

void FxChain::process(float* left, float* right, int num_frames) {
  if (left == nullptr || right == nullptr || num_frames <= 0) return;

  // §12: the chorus insert send is the delay's tone buffer; the delay only
  // fills it while it is on, so the wiring follows the delay's on/off state.
  // Chain mode: the delay leaves wet·input in the chain buffer and the
  // dry·tone taps in the tone buffer (no double-count in the audible path).
  delay.chainMode = true;
  const bool toneSend = (delay.onOff != 0);
  const float* toneL = toneSend ? delay.toneL.data() : nullptr;
  const float* toneR = toneSend ? delay.toneR.data() : nullptr;

  // m3-fxchain-grid (#282): the device master pulls the WHOLE chain in
  // calls capped at 0x100 frames — mixer FUN_100135b0 :14545-14548
  // (local_10 = 0x100; if (param_1 < 0x100) local_10 = param_1; return
  // local_10) and the capped n propagates up the pull chain
  // (FUN_10024ea0 :28528 vtable+0xc root pull at synth+0x8a1c). The mixer
  // RE-DERIVES the silent flag per call: FUN_100139e0 :14738 (*param_3 = 1
  // at call head) ANDs the per-voice release-complete flags returned by
  // the voice render slot (vtable+4, :14747-14755); FUN_100135b0 sets
  // *param_3 = 0 for the first live voice (:14549) and = 1 when every
  // voice slot is zero (:14556-14558) or release-complete (:14697-14707).
  // So EVERY FX device sees per-256 calls: effector F9 counter (cap 1102 →
  // 5 calls = 1280, device freeze 15872+1280 = 17152), CEq gate counter
  // (budget 8820 → 35 calls = 8960, device cut 17152+8960 = 26112),
  // delay/chorus silentFrames stepping 256 (m3-chorus-177 F4), phaser
  // bounds test. The engine has no voice-chain object here, so the flag
  // proxy (all-zero chunk test) is RE-DERIVED per chunk — a once-per-block
  // flag consumed by the effector would leave chunk 2 at flag==0
  // (m3-214-ceq-scratch F3 c256 rejection).

  for (int off = 0; off < num_frames;) {
    int n = num_frames - off;
    if (n > FX_CHAIN_MAX_FRAMES) n = FX_CHAIN_MAX_FRAMES;
    float* l = left + off;
    float* r = right + off;

    int ioSilent = 1;
    for (int i = 0; i < n; i++) {
      if (l[i] != 0.0f || r[i] != 0.0f) { ioSilent = 0; break; }
    }

    effector.process(l, r, n, &ioSilent);  // CEffector (+0x9718) + F9 gate
    eqA.process(l, r, n, &ioSilent);  // CEq parametric peak band (+0x9a98)
                                      // + #214 gate leg (FUN_1000ce60)
    eqB.process(l, r, n, &ioSilent);  // CEq tone shelf band (+0x9b18)
                                      // + #214 gate leg (FUN_1000ce60)
    pan.set_lfo_segment_base(off);  // segment starts are block-relative
    pan.process(l, r, n, ioSilent);  // CPan (+0x9b98) + dest-7 LFO consumer
    delay.process(l, r, n, &ioSilent);  // CDelay (+0x9538), tone buffer here
    chorus.setToneInput(toneL, toneR);
    chorus.setDelayActive(delay.outActive);
    chorus.process(l, r, n, &ioSilent);  // CChorus (+0x9668) → chain output

    off += n;
  }
}

}  // namespace synth1::fx
