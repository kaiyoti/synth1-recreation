// osc_dispatch.h — CVco oscillator engine: kernel dispatch + dual-osc render.
//
// Spec: kernel_map.md §1–§6 (72-kernel decision tree, leaf-index bijection,
//       amplitude compensation, arg-frame semantics), spec_osc.md §6.0–§6.5
//       (canonical kernel, pulse width, noise LFSR, sub oscillator, hard
//       sync) and §6.6 (ring mod).
// Ghidra: FUN_18003bf50 (dispatcher CVco::Process), the 72 kernels
//         0x18003e8f0…0x1800520b0 (canonical FUN_1800520b0, pulse
//         FUN_180051840, noise FUN_180051b30, sync FUN_1800486c0, ring
//         FUN_18004f0e0); DAT_180072b60 = 0.5f, DAT_180072b5c = 0.3f,
//         DAT_180077210 = 0.65f, DAT_180077390 = 1024.0 (duty scale).
//
// Phase convention (spec_osc.md §0, osc_phase.h): 32-bit unsigned
// accumulator, full cycle 2^27, table index (phase>>16)&0x7ff, linear
// interpolation fraction (phase&0xffff)·2⁻¹⁶. Per-sample DSP is float.

#pragma once

#include <cstdint>

#include "tables/wavetable_gen.h"

namespace synth1::osc {

// OscParams — the CVco configuration fields feeding the dispatch dimensions
// (kernel_map.md §4 field table).
struct OscParams {
  int osc1_wave = 0;       // cv+0x9c: 0 sine, 1 saw, 2 pulse, 3 triangle
  int osc2_wave = 1;       // cv+0x110: 1 saw, 2 pulse, 3 triangle, 4 noise
  bool sync = false;       // cv+0x118, param 6 "osc2 sync"
  bool ring = false;       // cv+0x168, param 7 "osc2 ring modula"
  bool fm_active = false;  // cv+0xa4 fmDepth != 0 || cv+0xa8 fmTarget != 0
  int fm_depth = 0;        // cv+0xa8 FM TARGET depth (ramp state lives in OscState)
  int sample_rate = 44100;  // cv+0x10: FM ramp cap = sr/1000 samples (spec_osc §6.7)
  float detune = 0.0f;     // cv+0x104 float, param 76 "osc1 detune":
                           // raw/127·20 (FUN_180028fe0 case 0x4c →
                           // FUN_18003e470); branch gate 0.0 < detune
                           // (spec_osc.md §6.8/§8, law B #62)
  float pulse_width = 1.0f;  // cv+0x16c, param 8 band/127 (ctor 1.0 = square)
  int pw_mod = 0;          // pulse-width modulation sum, 1/128 units (§4.5)
  int sub_oct = 0;         // 0 off, 1 unison, 2 −1 octave, 3 −2 octaves
  float sub_gain = 0.0f;   // gs (cv sub gain)
  float osc1_gain = 1.0f;  // g1
  float osc2_gain = 0.0f;  // g2
};

// FmRampState — CVco FM depth ramp state machine (spec_osc.md §6.7,
// FUN_18003bf50 field map + FUN_18003e8f0 per-sample ramp).
struct FmRampState {
  int depth = 0;          // cv+0xa4 current depth
  int start = 0;          // cv+0xac ramp start
  int rem = 0;            // cv+0xb0 ramp countdown
  int last_target = -1;   // cv+0xb4
  int flag = 0;           // cv+0xb8: set at end-of-release (FUN_180037f00 →
                          // FUN_18003dfd0); forces the next target change to snap
  int total = 0;          // cv+0xbc ramp duration
  int block_cur = 0;      // cv+0xc0 samples since last target change
  int call_start = 0;     // m3-fm-walk-land rule B: device FUN_10034740 call
                          // length, announced at each CVco span start
  int call_left = 0;      // samples left in the current device call
};

// CVco note-on (FUN_18003db00): force the next target change through the
// state machine with zero hold time ⇒ depth snaps to the note-on target.
inline void fm_note_on_reset(FmRampState& fm) {
  fm.last_target = -1;
  fm.block_cur = 0;
}

// CVco end-of-release (FUN_180037f00 → FUN_18003dfd0): arm the snap flag so
// the first target change of the next note jumps instead of ramping.
inline void fm_release_reset(FmRampState& fm) { fm.flag = 1; }

// Param-45 setter (FUN_18003e050): cv+0xb4 = -1 (force the state machine) and
// cv+0xc0 = sr/10 — a 100 ms hold seed, so a knob change mid-stream ramps the
// FM depth over up to 100 ms of COSRAMP instead of the ≤1 ms blockCur cap.
// (A following note-on, FUN_18003db00, re-seeds cv+0xc0 = 0 ⇒ snap.)
inline void fm_amount_set_seed(FmRampState& fm, int sample_rate) {
  fm.last_target = -1;
  fm.block_cur = sample_rate / 10;
}

// OscState — per-voice runtime accumulators (CVco phase / LFSR fields).
// Law K (#57/#65/#166): det_phase[0..7] are the 8 unison detune phase
// accumulators (32-bit CVco +0xcc[0..7], 64-bit +0xe4[0..7]). Seeded at
// note-on by the branch-C LCG draws (FUN_10036d60 :39556-39566, writes
// unconditional on detune — #166b resolved), advanced per sample by the
// detune increments iff D = 0.0 < detune (the D=0 kernels never touch
// them: m3-k-prep 76=0 cells show the draws frozen blk1==blk20).
struct OscState {
  std::uint32_t osc1_phase = 0;  // cv+0x98
  std::uint32_t osc2_phase = 0;  // cv+0x10c
  std::uint32_t det_phase[8] = {};  // cv+0xcc[0..7] (32-bit) detune slots
  std::uint32_t lfsr = 10u;      // cv+0x134, noise seed (spec_osc.md §6.3)
  FmRampState fm;                // FM depth ramp (spec_osc.md §6.7)
};

// Leaf-index bijection (kernel_map.md §2):
//   leaf = SYNC·36 + g·6 + (1 − P)·3 + N,  0 ≤ leaf ≤ 71
// over the 72 dispatched combinations (FM is not evaluated when ring is on).
int compute_kernel_leaf(const OscParams& p);

// Amplitude compensation folded into the dispatcher per selection
// (kernel_map.md §4): osc1 pulse & ring off ×0.5; detune (cv.detune > 0) &
// ring off ×0.3 on
// osc1 and sub; osc2 pulse & FM off ×0.5 (DAT_180072b60), osc2 noise & FM
// off ×0.65 (DAT_180077210) — the :37110 short-circuit makes 0.65
// noise-only (reports/osc2_pulse_gain.md, M2 law 3).
void apply_amplitude_compensation(OscParams& p);

// Pulse-width duty index (spec_osc.md §6.2, FUN_180051840):
//   d = pwMod < 1 ? pulseWidth·(1 + pwMod/128)
//                 : pulseWidth + (2 − pulseWidth)·pwMod/128
//   d = clamp(d, 0.01, 1.99); dutyIdx = (int)(d · 1024.0)
// pulseWidth = param 8 band/127 ∈ [0, 1] (FUN_180028fe0 case 8): band 127
// (d = 1) is the square, band 64 a 25 % pulse; dutyIdx/2048 is the
// positive-lobe fraction of the cycle.
int pulse_duty_index(float pulse_width, int pw_mod = 0);

// Dual-oscillator per-sample synthesis (the canonical kernel FUN_1800520b0
// plus the pulse/noise/sub/ring/sync variants, spec_osc.md §6.1–§6.6).
// A pulse on EITHER oscillator is the two-saw difference with the shared
// dutyIdx (osc1: FUN_180051840; osc2: FUN_180051db0; both: FUN_180051500),
// so osc1_table / osc2_table must be the saw tables for wave 2.
// osc2_table may be nullptr when osc2_wave == 4 (noise). Amplitude
// compensation is applied to a copy of params (dispatcher-side, §4).
// det_inc (law K, #57/#65): the 8 detune phase increments, computed by
// the caller per render call exactly like the device dispatcher computes
// the frame incs (FUN_10034740 :37818-37848: transpose by the porta/pitch
// delta, clamp 22000 Hz, inc = (int)ROUND(detHz·2^27·invSR)). Non-null
// iff D = 0.0 < params.detune: the kernel then runs the 9-tap detuned
// family (osc1 term = sum of 9 raw nearest-index taps of the SAME table
// at osc1+det[0..7] phases — no interpolation on the taps; pulse taps =
// saw-difference; sub term = 9 raw reads; osc2 lerp kept; FM increment
// added to ALL 9 accumulators, FUN_10040430 :45505-45513).
void render_oscillators(const OscParams& params,
                        const float* osc1_table, const float* osc2_table,
                        const float* sub_table,
                        float* output, int num_samples,
                        std::int32_t inc1, std::int32_t inc2,
                        OscState& state,
                        const std::int32_t* det_inc = nullptr);

}  // namespace synth1::osc
