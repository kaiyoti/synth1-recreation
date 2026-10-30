// osc_phase.h — 32-bit phase accumulator + frequency → phase increment.
//
// Spec: spec_osc.md §0 (phase conventions: 32-bit unsigned accumulator, one
//       full cycle = 2^27 = 134 217 728 units, DAT_1800773b4 /
//       _DAT_1800773a0 = 134217728.0; wavetable 2048 float entries; index
//       (phase>>16)&0x7ff; fraction (phase&0xffff)·(1/65536); linear fused
//       blend; single-precision per-sample DSP, double for the
//       phase-increment derivation), §4.3 (phase increments — inc =
//       (int)ROUND((float)((double)f · 134217728.0 · invSR)), #83
//       ROUND law, reports/83_baseinc.md §4).
// Ghidra: DAT_1800773b4 (= 2^27), _DAT_1800773a0 (= 134217728.0 double),
//         DAT_180072b50 (= 1/65536), FUN_1800398b0 (one of the 72 kernel
//         fns; phase write-back blk[0x18]), FUN_18003cad0 (band selection —
//         stage 2).
//
// Precision (spec_osc.md §0, #83): the phase increment is derived with a
// double intermediate, stored as float, then x87-ROUNDed (nearest-even)
// to int: (int)ROUND((float)((double)freq · 134217728.0 · (1.0/sampleRate))).
// Per-sample advance is unsigned 32-bit (the original
// kernel does `ph1 += inc1` on a 32-bit accumulator; only the 16-bit windows
// (phase>>16)&0x7ff and (phase&0xffff) are consumed, so 32-bit wrap is
// behaviourally identical to 27-bit wrap).
//
// STAGE 2 (not here): the 8-voice unison detune accumulators detPhase[0..7]
// (spec_osc.md §6.8), the phase-init fields cv+0x1b4/0x1b8/0x1bc (param 91
// "osc phase shift", spec_phase.md, offset (p−1)/126 cycles — not traced in
// spec_osc.md §11.7, so stage 1 seeds phase = 0 at note-on), the sync
// restart (spec_osc.md §6.5), FM inside the accumulator (spec_osc.md §6.7).

#pragma once

#include <cstdint>

namespace synth1::osc {

// One full phase cycle: 2^27 = 134 217 728 units (spec_osc.md §0,
// DAT_1800773b4 / _DAT_1800773a0).
inline constexpr std::uint32_t PHASE_CYCLE = 1u << 27;  // 134217728

// 32-bit phase accumulator state.
// Ghidra: cv+0x98 (osc1Phase, uint) — spec_osc.md §2.
struct PhaseState {
  std::uint32_t phase = 0;  // 32-bit unsigned accumulator (spec_osc.md §0)
};

// Frequency (Hz) → phase increment (int), #83 (reports/83_baseinc.md §4):
//   inc = (int)ROUND((float)((double)f · 134217728.0 · invSR))
// 32-bit render DLL primary: FUN_10034740 (decomp_all_32_full.c:37795-
// 37798) `(int)ROUND((float)((double)(pitch · DAT_100661bc) ·
// *(double*)(this+0x140)))` — x87 fist, default mode = round-to-nearest,
// ties-to-even (97101_rounding.md §3); the 64-bit twin FUN_18003bf50
// (decomp_all.c:36967-36969) truncates (SSE cvttss2si) — 32-bit primary
// convention (95a precedent; the #101 non-transfer ruling does not apply:
// this is 1 unit/sample of a 27-bit accumulator, gate-measured,
// 83_baseinc.md §4). The product is stored as float before the convert
// (float-stored values 398126.875 / 1193032.5 / 1592507.5 at the gate
// notes, §2). Explicit ties-to-even, independent of the fesetround mode
// (adsr.h #95a/#122 idiom). The double intermediate is mandatory
// (spec_osc.md §0).
inline std::int32_t phase_increment(float freq, float sample_rate) {
  const float p =
      (float)((double)freq * 134217728.0 * (1.0 / (double)sample_rate));
  const float fl = ::floorf(p);
  const float fr = p - fl;
  return (std::int32_t)fl +
         (fr > 0.5f || (fr == 0.5f && (((std::int32_t)fl) & 1)));
}

// Advance the phase accumulator by one sample.
// spec_osc.md §6.1 canonical kernel: `ph1 += inc1;` (32-bit unsigned wrap).
inline std::uint32_t advance(PhaseState& state, std::int32_t inc) {
  state.phase += (std::uint32_t)inc;
  return state.phase;
}

// Note-on: seed the accumulator (stage 1: phase = 0; the param-91 phase-init
// offset (p−1)/126·2^27 is stage 2 — spec_osc.md §11.7 leaves the exact
// formula untraced).
inline void init_phase(PhaseState& state) {
  state.phase = 0;
}

}  // namespace synth1::osc
