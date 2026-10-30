// pitch.h — MIDI note → frequency (stage 1: osc1 base frequency path).
//
// Spec: spec_osc.md §4.1 (base frequencies — FUN_18003bb00, osc1).
// Ghidra: FUN_18003bb00 (osc1 base freq), DAT_1801a2c70 (NOTEFREQ, 129
//         floats, NOTEFREQ[n] = 8.17579891564 · 2^(n/12), [69] = 440 Hz).
//
// STAGE 1: only the osc1 base frequency (spec_osc.md §4.1). The osc2 path
// (FUN_18003bba0, kbd-track/220·2^oct variants), the key-shift clamp,
// pitch-modulation sum (FUN_18003b940, 2^(sum/128) with ±1280 clamp and the
// 22000 Hz clamp) and unison pitch are stage 2 (spec_osc.md §4.1–§4.3,
// §6.8). The 2^x evaluation via DAT_1801a32a8[j]·DAT_1801a3070[i] is
// reproduced with double-precision pow() (spec_osc.md §4.1 states the power of
// two is evaluated via those two .rdata tables; the result is bit-equivalent
// for our purposes since the final multiply is float).

#pragma once

#include <cstdint>
#include <cmath>

namespace synth1::osc {

// m4-mastertune-land: device mastertune NOTEFREQ table (defined below).
inline const float* master_tune_table();

// spec_osc.md §4.1:  n = clamp(CVco.note + CVco.keyShift, 0, 128)
//                    osc1Hz = 2^((unisonPitch + fine12)/1200) · NOTEFREQ[n]
//                    NOTEFREQ[n] = 8.17579891564 · 2^(n/12)
// Stage 1 defaults: keyShift = 0, unisonPitch = 0, fine12 = 0 →
//                    osc1Hz = NOTEFREQ[n]
inline float midi_to_hz(std::uint8_t midi_note) {
  const int n = (int)midi_note;  // stage 1: keyShift = 0 (stage 2)
  // NOTEFREQ = DAT_1801a2c70: 8.17579891564 · 2^(n/12)
  return (float)(8.17579891564 * std::pow(2.0, (double)n / 12.0));
}

// NOTEFREQ[n] for the full 129-entry table (DAT_1801a2c70, n = 0..128 — the
// key-shifted note is clamped to that range by FUN_18003bb00).
// m4-mastertune-land: n = 0..127 come from the device mastertune table
// (FUN_10035fb0 rebuild, see MasterTune below); n = 128 keeps the closed-form
// value (device reads the halved master-freq global at table[128] —
// pre-existing engine divergence, OPEN item, kept to avoid mt=0 movers).
inline float note_freq(int n) {
  if (n < 0) n = 0;
  if (n > 128) n = 128;
  if (n == 128) return (float)(8.17579891564 * std::pow(2.0, (double)n / 12.0));
  return master_tune_table()[n];
}

// ---------------------------------------------------------------------------
// m4-mastertune-land — Master Tuning (ini key "mastertune", -100..+100 cents).
// Device law (32-bit authority, objdump-verified install/Synth1 VST.dll):
//   FUN_10035fb0 @ 0x10035fb0 (setter; called at DLL init FUN_10035680
//   line 38715 and live from the Options dialog FUN_1001ee60 line 25187):
//     guard: skip rebuild if cents == cached DAT_100800d8;
//     p2   = exp2_msvc((double)cents * 0.0008333333333333334)   [1/1200]
//            (CRT SSE2 log2(2.0)=1.0 * arg, then exp2; isnan-guarded)
//     mf   = (float)(p2 * 440.0)                    -> DAT_1015ac50
//     NOTEFREQ[k] = (float)((double)mf *
//                  exp2_msvc((double)(k-69) * 0.08333333333333333))  k=0..127
//                                                   -> DAT_1015aa50
//     DAT_1015ac50 = mf * 0.5f                     (osc2 non-kbd base,
//                                                    FUN_100343b0 -> +0xf0)
//   64-bit mirror FUN_18003d9a0: pow(2.0, cents*(1/1200)) * 440.0.
// At cents = 0 the table is bit-identical to the closed-form 8.17579891564·
// 2^(n/12) for n = 0..127 (verified), so mastertune = 0 renders are unchanged.
// ---------------------------------------------------------------------------

// Rebuild the device NOTEFREQ table + osc2 base for `cents` (clamped by the
// caller/settings layer to -100..+100; the device setter itself does not
// clamp — the Options trackbar range and the INI loader do).
inline void set_master_tune_cents(int cents);

inline int master_tune_cents();

// DAT_1015ac50 after the setter: (float)(mf * 0.5f) — the osc2 non-kbd base
// (device 220.0 at cents = 0, DAT_1801a2c68).
inline float master_tune_base220();

// The 128-entry mastertuned NOTEFREQ table (DAT_1015aa50).
inline const float* master_tune_table();

namespace detail {
struct MasterTuneState {
  float table[128];
  float base220;
  int cents;
  MasterTuneState() { rebuild(0); }
  void rebuild(int c) {
    cents = c;
    const double p2 = std::pow(2.0, (double)c * (1.0 / 1200.0));
    const float mf = (float)(p2 * 440.0);
    for (int k = 0; k < 128; ++k) {
      table[k] = (float)((double)mf *
                         std::pow(2.0, (double)(k - 69) * (1.0 / 12.0)));
    }
    base220 = (float)(mf * 0.5f);
  }
};
inline MasterTuneState& master_tune_state() {
  static MasterTuneState s;
  return s;
}
}  // namespace detail

inline void set_master_tune_cents(int cents) {
  detail::master_tune_state().rebuild(cents);
}
inline int master_tune_cents() { return detail::master_tune_state().cents; }
inline float master_tune_base220() { return detail::master_tune_state().base220; }
inline const float* master_tune_table() { return detail::master_tune_state().table; }

}  // namespace synth1::osc
