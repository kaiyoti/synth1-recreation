// voice.h — one voice (CVoice): CVco osc + mod env → CVcf filter + filter
// env → CVca amp + amp env, with velocity, pitch/cutoff/amp modulation
// routing (Stage 5).
//
// Spec: dsp_flow.md §2 (per-voice chain CVco → CVcf → CVca, linked by
//       FUN_18001b5c0; CVca end-of-release hook FUN_180037f00 resets CVcf
//       and the CVco mod env); spec_osc.md §2/§4.1–§4.3 (pitch path,
//       FUN_18003b940 pitch modulation 2^(sum/128) clamped ±1280 with the
//       22000 Hz Nyquist guard), §6.1 (canonical kernel); spec_filter.md
//       §5 (cutoff index composition i0 + i_key + i_env + edx, 50 ms slew);
//       spec_env.md §0 (CEnvelopedObject scales: CVca 2048, CVcf 1024,
//       CVco 640/512/128 by param 71), §2 (amp env params 25–28, gain 29,
//       vel sens 30), §3.2 (final amplitude formula), §4.2–§4.3 (amp +
//       filter velocity), §5 (mod env params 10–13, dest 71: 0 pitch
//       ±5 oct / 1 FM linear-decay / 2 pulse width; no note-off — reset by
//       the CVca end-of-release hook), §6.5 (pitch bend in 1/128-octave
//       units), §7 (lifecycle: +0x480 active, +0x49c gate held, note-off
//       releases CVca AND CVcf, never CVco).
// Ghidra: CVco (+0x50), CVcf (+0x220), CVca (+0x3f0) inside voice stride
//         0x4a8; FUN_1800526e0/52740/525f0/52580 (voice set-velocity /
//         trigger / release / state), FUN_180037bc0 (amp velocity),
//         FUN_18003a780 (filter velocity + env note-on), FUN_18003db00
//         (mod env note-on), FUN_180038140 (filter env amount select,
//         velocity scaling via CVcf+0xc0), FUN_18003b940 (pitch mod),
//         FUN_180028fe0 cases 0x0a–0x0d (mod env), 0x0f–0x12 (filter env
//         times — same pow curves as amp; sustain is LINEAR band/127 via
//         FUN_18003b780), 0x15 (filter amount bipolar (band−63)/64 via
//         FUN_18003b670), 0x18 (filter velocity switch).
//
// Parameter ids follow core/params/params.gen.h (the generated table, which
// matches the dispatcher case numbers): filter env times = 15–18, filter
// amount = 21, filter kbd track = 22, filter velocity = 24. (The stage-5
// task text numbers these 14–17/18 — off by one vs the generated table.)
//
// Modulation routing (per-sample portable form of FUN_18003dbe0 /
// FUN_18003ad50 / FUN_180037c30):
//   pitch sum (1/128-octave units) = pitch_bend + porta_offset + lfo_pitch
//                                    + (mod_dest==0 ? ±mod_env_level : 0),
//                                    clamped ±1280, freq *= 2^(sum/128),
//                                    min(freq, 22000) (spec_osc.md §4.2).
//   cutoff index = i0 + i_key + i_vel + i_spread + extra_idx, where
//                  i_spread is the #198 unison filter-spread term (CVcf+0xa0)
//                  and extra_idx carries
//                  the ±filter_env level (scaled by the bipolar param-21
//                  amount, velocity-scaled when param 24 is on) plus the
//                  LFO-cutoff value — this resolves spec_filter.md UNKNOWN-2
//                  (the edx argument at the CVcf::process call site is the
//                  per-chunk modulation sum; the 50 ms slew smooths it).
//   amp gain = (envLevel/2048)·ampGain·velFactor·(2048−lfo_amp)/2048
//              (spec_env.md §3.2 modA/modB term; lfo_amp ∈ [−2048, 0]).
//
// The mod env → cutoff routing (mod_env_cutoff_amount) is a portable
// extension: the original param 71 has no cutoff destination (spec_env.md
// §5.4); it defaults to 0 (inactive).
//
// NO heap allocations in note_on/note_off/process_sample/process_block:
// all state is POD; wavetables are referenced through the static global
// bank (tables::global_bank(), built once at init).
//
// STAGE 1 compatibility (core/engine/engine.h): the legacy fields (phase,
// phase_inc, osc1_hz, vel_factor, amp_gain, midi_note, active, gate_held),
// the free functions note_on/note_off/is_finished and
// process_sample(const float* wavetable) keep their exact stage-1
// semantics (osc1 sine direct to the amp stage, no filter/mod path).

#pragma once

#include <cstdint>
#include <cmath>
#include "osc/osc_phase.h"
#include "osc/pitch.h"
#include "osc/osc_dispatch.h"
#include "filter/filter.h"
#include "env/adsr.h"
#include "env/velocity_curve.h"
#include "tables/wavetable_gen.h"

namespace synth1::lfo { struct LfoState; }

namespace synth1::voice {

// ---------------------------------------------------------------------------
// Envelope scales (spec_env.md §0: CEnvelopedObject +0x28) and modulation
// clamps (spec_osc.md §4.2).
// ---------------------------------------------------------------------------
inline constexpr int AMP_ENV_SCALE = env::AMP_SCALE;      // CVca = 2048
inline constexpr int FILTER_ENV_SCALE = 1024;             // CVcf = 0x400
inline constexpr int MOD_ENV_SCALE_PITCH = 640;           // param 71 dest 0
inline constexpr int MOD_ENV_SCALE_FM = 512;              // dest 1 (linear decay)
inline constexpr int MOD_ENV_SCALE_PW = 128;              // dest 2
inline constexpr int PITCH_MOD_CLAMP = 1280;              // FUN_18003b940 ±10 oct
inline constexpr float NYQUIST_GUARD = 22000.0f;          // DAT_1800773a8

// Param 11 — mod env amount, bipolar (spec_env.md §5.2, FUN_18003e260),
// piecewise: band 64 → 0; band < 64 → band/64 − 1 (DAT_18007338c);
// band > 64 → (band − 64)/63 (DAT_180072e80). Range [−1, +1].
inline float mod_env_amount_from_band(int band) {
  if (band == 0x40) return 0.0f;
  if (band < 0x40) return (float)band * (1.0f / 64.0f) - 1.0f;
  return (float)(band - 0x40) * (1.0f / 63.0f);
}

// Param 2 — osc2 pitch (FUN_180028fe0 case 2): semitones
// (band·120)/127 − 60 (integer), stored in octaves (× 5 × 1/60) at CVco+0x11c.
inline float osc2_octaves_from_band(int band) {
  return (float)((band * 0x78) / 0x7f - 0x3c) * 5.0f * 0.0166666675f;
}

// Params 3 / 72 — osc2 fine / osc1,2 fine (cases 3 / 0x48): cents with a
// dead zone: band < 62 → band − 62; 62..66 → 0; > 66 → band − 66.
inline float fine_cents_from_band(int band) {
  if (band < 0x3e) return (float)(band - 0x3e);
  if (band < 0x43) return 0.0f;
  return (float)(band - 0x42);
}

// Param 21 — filter amount, bipolar (FUN_180028fe0 case 0x15 →
// FUN_18003b670: (band − 0x3f) · 1/64 → CVcf+0xb4 env depth).
inline float filter_amount_from_band(int band) {
  return ((float)band - 63.0f) * (1.0f / 64.0f);
}

// Param 24 — filter velocity scale (spec_env.md §4.3, FUN_18003a780):
// scale = (1 − exp(−1.1·vel/127)) / (1 − exp(−1.3)); 0 at vel 0,
// 0.917 at vel 127. Scales the filter-env amount when the switch is on.
inline float filter_velocity_scale(int velocity) {
  const double num = 1.0 - std::exp(-1.1 * (double)velocity / 127.0);
  const double den = 1.0 - std::exp(-1.3);
  return (float)(num / den);
}

// Engine-supplied per-block modulation sums (the LFOs live in the engine;
// the voice consumes their routed values — spec_lfo_fx.md §1.8 units).
struct ModInputs {
  int pitch = 0;   // 1/128-octave units, LFO dest 2 "osc1,2" (both oscs)
  int pitch2 = 0;  // 1/128-octave units, LFO dest 1 "osc2" (osc2 only)
  int cutoff = 0;  // cutoff-index units (LFO dest 3, 0..1024; mirror-folded)
  int amp = 0;     // amp mod (LFO dest 4, −2048..0)
  int pw = 0;      // pulse-width mod, 1/128 units (LFO dest 5, ±128)
  int fm = 0;      // FM gate, 512 units (LFO dest 6, 0..512)
};

// ---------------------------------------------------------------------------
// Voice — the full CVoice chain.
// ---------------------------------------------------------------------------
struct Voice {
  // ---- oscillator (CVco) ----
  osc::OscParams osc_params;   // waveform / mix / FM / sync / PW / sub
  osc::OscState osc_state;     // osc1/osc2 phase accumulators + noise LFSR
  // legacy stage-1 fields (engine.h): osc1 direct path
  osc::PhaseState phase;
  std::int32_t phase_inc = 0;   // osc1 increment (spec_osc.md §4.3)
  std::int32_t phase_inc2 = 0;  // osc2 increment
  float osc1_hz = 0.0f;         // cv+0x94 (pre-modulation)
  float osc2_hz = 0.0f;         // cv+0x108 (pre-modulation)
  // Law K (#57/#165): detune slot frequencies, CVco +0xac..+0xc8 (32-bit).
  // det_hz[2j] = osc1_hz·2^(+(2j+1)·det/1200), det_hz[2j+1] = osc1_hz·
  // 2^(−(2j+1)·det/1200) — base-2 law, FUN_10034500 :37591-37617
  // (FUN_1004e4c1(2.0)=ln2 → FUN_1004ccb3=expf; #165 resolved).
  float det_hz[8] = {};
  // Per-render-call detune phase increments (dispatcher FUN_10034740
  // :37818-37848: transpose det_hz by the pitch delta, clamp 22000 Hz
  // (DAT_100661b0), inc = (int)ROUND(hz·2^27·invSR)).
  std::int32_t det_inc[8] = {};
  float cur_f1 = 0.0f;          // modulated osc1 Hz (band-table selection)
  float cur_f2 = 0.0f;          // modulated osc2 Hz

  // ---- pitch parameters (CVco, spec_osc.md §4.1) ----
  int key_shift = 0;            // cv+0x174, param 9 (semitones)
  float fine12 = 0.0f;          // cv+0x170, param 72 (cents)
  float osc2_oct = 0.0f;        // cv+0x114, param 2 (octaves)
  float osc2_fine = 0.0f;       // cv+0x12c, param 3 (cents)
  bool osc2_kbd = true;         // cv+0x130, param 4

  // ---- mix (CVco, spec_osc.md §7, FUN_18003b8c0) ----
  float osc_mix = 0.0f;         // cv+0x164, param 5 band/127
  float sub_amp = 0.0f;         // cv+0x140, param 95 band·4/127
  int sub_wave = 1;             // cv+0x144, param 96 (0 sine 1 tri 2 saw 3 sq)
  int sub_octave = 1;           // cv+0x148, param 97 (0 unison, 1 −1 oct)
  int fm_amt_band = 0;          // cv+0xa0, param 45 "osc1 FM" raw 0..127
  int fm_gate = 0;              // FUN_18003dbe0 groupD: mod-env dest 1 (×512,
                                // linear decay) + LFO1/2 dest 6 (0..512)

  // cached band tables for the full path (refreshed per block, no alloc)
  const float* osc1_table = nullptr;
  const float* osc2_table = nullptr;
  const float* sub_table = nullptr;

  // ---- filter (CVcf) ----
  filter::Filter filt;

  // ---- envelopes (spec_env.md §0: one CEnvelopedObject per stage) ----
  env::ADSR amp_env;      // CVca: params 25–28, scale 2048, amount 1.0
  env::ADSR filter_env;   // CVcf: params 15–18, scale 1024, amount param 21
  env::ADSR mod_env;      // CVco: params 10–13, scale by dest 71

  // ---- velocity (spec_env.md §4) ----
  float vel_factor = 1.0f;             // amp velFactor (param 30 curve)
  std::uint8_t amp_vel_sens_band = 0;  // param 30
  bool filter_vel_on = false;          // param 24 switch (CVcf+0xbc)
  float filter_env_amount = 0.0f;      // bipolar amount (param 21)
  int filter_env_sign = 1;             // CVcf+0x24 sign flag

  // ---- mod env routing (CVco, params 10/11/71) ----
  bool mod_env_on = true;              // param 10 (CVco+0x20 envOn)
  float mod_env_amount = 0.0f;         // bipolar amount (param 11)
  int mod_env_sign = 1;                // CVco+0x24 sign flag
  int mod_dest = 0;                    // param 71: 0 pitch, 1 FM, 2 PW
  float mod_env_cutoff_amount = 0.0f;  // portable routing (default off)

  // ---- pitch modulation sums, 1/128-octave units ----
  int pitch_bend = 0;     // voice+0xa8 (spec_env.md §6.5)
  int porta_offset = 0;   // CVco+0x19c portamento remaining (spec_env §6.2)
  int lfo_pitch = 0;      // engine-routed LFO pitch sum, dest 2 (osc1+osc2)
  int lfo_pitch2 = 0;     // engine-routed LFO pitch sum, dest 1 (osc2 only)
  int lfo_pw = 0;         // engine-routed LFO pulse-width sum (dest 5)
  int lfo_fm = 0;         // engine-routed LFO FM gate sum (dest 6, 0..512)

  // ---- cutoff modulation sums (index units) ----
  int lfo_cutoff = 0;     // engine-routed LFO cutoff sum (dest 3)

  // ---- amp modulation (LFO dest 4, −2048..0) ----
  int lfo_amp = 0;

  // ---- lifecycle (spec_env.md §7: +0x480 / +0x49c / +0x4a0) ----
  float amp_gain = 0.5f;          // CVca+0xa4 (ctor default 0.5, param 29)
  std::uint8_t midi_note = 0;     // voice+0x4a0
  bool active = false;            // voice+0x480
  bool gate_held = false;         // voice+0x49c
  float sample_rate = 44100.0f;
  int osc_phase_band = 0;         // CVco+0x1b4/0x1b8 (param 91; 0 = random LCG)
  bool phase_reseed = true;       // CVco+0x1bc: first note-on seeds from LCG
  float unison_pitch_cents = 0.0f;  // CVco+0x178 (FUN_18003e020, unison slot)
  int unison_slot = 0;              // slot index within the unison group

  // last applied pitch-mod sums (increment recompute cache; osc1 / osc2)
  int cur_pitch_mod = 0x7fffffff;
  int cur_pitch_mod2 = 0x7fffffff;

  // filter velocity scalar CVcf+0xc0 = (int)(127·filter_velocity_scale(vel))
  int filter_vel127 = 0;

  // ---- control plane ----
  void prepare(float sample_rate, const tables::WavetableBank* bank = nullptr);

  // Full note-on (spec_env.md §7 FUN_1800526e0 + FUN_180052740): pitch,
  // phase, amp env + velFactor, filter env + velocity scaling, mod env.
  void note_on(int note, int velocity, float sample_rate,
               const tables::WavetableBank& bank);

  // Legato re-target (FUN_1800526e0 without FUN_180052740): update the CVco
  // key/target pitch and invalidate the pitch-mod cache, but do NOT retrigger
  // CVca/CVcf/CVco envelopes or oscillator phase.
  void retarget_note(int note, int velocity, float sample_rate,
                     const tables::WavetableBank& bank);

  // Full note-off (FUN_1800525f0): releases CVca AND CVcf; the mod env has
  // no independent release (reset at end-of-release, FUN_180037f00).
  void note_off();

  // Parameter setters (band = raw 0..127, params.gen.h ids).
  void set_amp_env_bands(int b25_attack, int b26_decay, int b27_sustain,
                         int b28_release);
  void set_amp_gain_band(int b29);
  void set_amp_vel_sens_band(int b30);
  void set_filter_env_bands(int b15_attack, int b16_decay, int b17_sustain,
                            int b18_release);
  void set_filter_env_amount_band(int b21);   // bipolar (band−63)/64
  void set_filter_vel_on(bool on) { filter_vel_on = on; }
  void set_filter_cutoff_band(int b19);       // i0 = band/127 · 1024
  void set_filter_resonance_band(int b20);    // res = band/127
  void set_filter_keytrack_band(int b22);     // keytrack amount = band/127
  void set_filter_saturation_band(int b23);
  void set_mod_env_on(bool on) { mod_env_on = on; }
  void set_mod_env_amount_band(int b11);      // bipolar band/63 − 1
  void set_mod_env_bands(int b12_attack, int b13_decay);
  void set_mod_dest(int dest);                // param 71 (0/1/2)
  // Oscillator pitch / mix (spec_osc.md §4.1 / §7).
  void set_osc2_pitch_band(int b2);           // osc2 octaves
  void set_osc2_fine_band(int b3);            // osc2 cents (dead zone)
  void set_osc2_kbd_track(bool on);           // param 4
  void set_key_shift(int semitones);          // param 9 (−24..+24)
  void set_fine_tune_band(int b72);           // osc1,2 cents (dead zone)
  void set_osc_mix_band(int b5);              // mix = band/127
  void set_sub_gain_band(int b95);            // subAmp = band·4/127
  void set_sub_shape(int b96);                // 0 sine 1 tri 2 saw 3 square
  void set_sub_octave(int b97);               // 0 unison, 1 −1 octave
  void set_pulse_width_band(int b8);          // pulseWidth = band/127
  void set_fm_amount_band(int b45);           // cv+0xa0 raw 0..127
  void regen_detune();                        // FUN_10034500 (law K #165):
  // det_hz[2j]=f1·2^(+(2j+1)·det/1200), det_hz[2j+1]=f1·2^(−(2j+1)·det/1200);
  // no-op unless detune > 0 (device setter gate).
  void set_osc_phase_band(int b91) {
    osc_phase_band = b91;
    phase_reseed = (b91 <= 0);  // FUN_18003e5e0 / FUN_18003cde0: CVco+0x1bc
  }
  void set_unison_slot(int slot, float cents, int filter_term) {
    unison_slot = slot;
    unison_pitch_cents = cents;
    filt.set_spread_term(filter_term);  // #198 CVcf+0xa0 (FUN_10034050)
    if (active) update_base_pitch();
  }
  void set_pitch_bend_units(int units) { pitch_bend = units; }
  void set_porta_offset(int units) { porta_offset = units; }
  // m3r-111-porta: this voice's porta/glide is in progress for this render
  // span (allocator glide_frames_to_edge != kNoGlideCut). Gates J1/J3.
  void set_glide_active(bool g) { glide_active_ = g; }
  void set_lfo_mod(const ModInputs& m) {
    lfo_pitch = m.pitch; lfo_pitch2 = m.pitch2; lfo_cutoff = m.cutoff;
    lfo_amp = m.amp; lfo_pw = m.pw; lfo_fm = m.fm;
  }
  void set_lfo_amp_only(int amp) { lfo_amp = amp; }  // m3-272 law

  // m3-272 (tasks/consults/m3-272-scratch.answer.md): per-consumer
  // modulation read grid. The CVco/CVcf consumers read their routed LFO
  // values at their OWN walk cuts (FUN_1000b3a0 :8129-8228 budget cut at
  // the consumer-routed LFO segment edges via FUN_10012be0, minSegment-
  // floored by FUN_10012c00; FUN_10036250 FM sum per env chunk), not at
  // the shared control-tick starts. The voice re-reads the routed sums
  // from the LFO states at its own sample position.
  void set_cm_context(const lfo::LfoState* a, const lfo::LfoState* b,
                      std::int64_t chunk_start_abs) {
    cm_lfo1_ = a; cm_lfo2_ = b; cm_chunk_start_ = chunk_start_abs;
  }
  void set_cm_abs_base(std::int64_t abs_base, std::int64_t piece_end = 0,
                       bool piece_head = false) {
    cm_abs_base_ = abs_base;
    cm_chunk_end_ = piece_end;  // m3-bendslew: device render-piece (FUN_1000b3a0 budget) end
    cm_piece_head_ = piece_head;
  }

  // ---- m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md
  // A): voice absolute sample base + host block size for the device CVca
  // walk-call grid (FUN_10031c70 ramp lanes). ----
  void set_abs_base(std::int64_t abs) { base_abs_ = abs; }
  std::int64_t base_abs() const { return base_abs_; }
  void set_walk_start_rel(int rel) { walk_start_rel_ = rel; }
  void set_block_size(int bs) { block_size_ = bs; }

  // ---- audio ----
  // Legacy stage-1 path (engine.h): osc1 sine direct to the amp stage.
  // spec_env.md §3.2: out[i] = osc[i] · (envLevel/2048)·ampGain·velFactor
  // spec_osc.md §6.1: osc[i] = wavetable(phase); phase += inc (32-bit wrap)
  float process_sample(const float* wavetable) {
    if (!active) return 0.0f;
    const float osc_out = tables::read(wavetable, phase.phase);
    osc::advance(phase, phase_inc);
    const int env_level = amp_env.advance_chunk(1);
    const float g = ((float)env_level * (1.0f / 2048.0f)) * amp_gain * vel_factor;
    return osc_out * g;
  }

  // Full chain (dsp_flow.md §2): osc → filter → amp, per sample, using the
  // stored lfo_* modulation sums. Zero allocations.
  // amp_cap (law #90, anchor corrected by law #82): frames from the first
  // sample of this call to the next DEVICE CVca gain-apply cut — the
  // block-anchored min(host-block-rem, 0x100) + range end + segment cuts
  // of AMP-WIRED LFOs only (no wave-wrap cuts); the in-progress amp chunk
  // carries across calls up to that cap. env_cap: the CVcf/CVco env-walk
  // distance on the law-123 grid (256 + wave-wrap + segment cuts kept);
  // -1 = follow amp_cap.
  // -1 (default, standalone) = the whole call is the cap (device block).
  void process_block(float* out, int num_samples,
                     const tables::WavetableBank& bank, int amp_cap = -1,
                     int env_cap = -1, int flt_cap = -1);

  // Same, applying engine modulation sums first.
  void process_block(float* out, int num_samples,
                     const tables::WavetableBank& bank, const ModInputs& mod,
                     int amp_cap = -1, int env_cap = -1, int flt_cap = -1) {
    set_lfo_mod(mod);
    process_block(out, num_samples, bank, amp_cap, env_cap, flt_cap);
  }

 private:
  void cm_reread(int which);  // m3-272 re-read (bit0 CVco dests, bit1 CVcf)
  const lfo::LfoState* cm_lfo1_ = nullptr;
  const lfo::LfoState* cm_lfo2_ = nullptr;
  std::int64_t cm_chunk_start_ = 0;
  std::int64_t cm_abs_base_ = 0;
  std::int64_t cm_chunk_end_ = 0;  // m3-bendslew: device render-piece end (FUN_1000b3a0 budget)
  bool cm_piece_head_ = false;     // m3-bendslew: this call starts at the piece head
  bool glide_active_ = false;      // m3r-111-porta: porta/glide in progress this span
  int cm_outer_rem_ = 0;           // m3r-111-slewcut: FUN_1000b3a0 outer L1-cut countdown
  int cm_call_rem_ = 0;  // m3-272 CVco driver-budget remainder
  void refresh_tables(const tables::WavetableBank& bank);
  void update_base_pitch();                     // FUN_18003bb00 + FUN_18003bba0
  void recompute_mix();                         // FUN_18003b8c0
  void apply_filter_env_amount();               // FUN_180038140 (headroom clamp)
  void apply_mod_env_scale();                   // scale/decay-mode by dest 71
  void fm_update_target();                      // FUN_18003bf50 FM target law
  float process_full_sample();
  void begin_block(int n, int amp_cap, int env_cap,
                   int flt_cap = -1);  // spec_env.md §2.3/§3.2, laws #90/#82/#172

  // Per-block control quantization (spec_env.md §2.3 FUN_18000ca70: the
  // block is walked in chunks split at envelope stage boundaries; §3.2
  // FUN_180037c30: CVca gain ramps linearly across each chunk from the
  // previous chunk's final gain).
  void step_env_chunks();
  int blk_len_ = 0;
  int blk_i_ = 0;
  float g_cur_ = 0.0f;    // CVca+0x90: accumulated applied gain
  float g_step_ = 0.0f;   // per-sample gain step of the current CVca chunk
  int amp_ch_rem_ = 0;    // samples left in the current CVca chunk
  int amp_call_rem_ = 0;  // law #90: samples left in the current gain-apply call
  int amp_cache_lvl_ = -1;       // CVca+0x80: level of the last FUN_10031c70 normal call
  int amp_cache_mod_ = -1;       // CVca+0x84/+0x88: amp mods of that call
  float amp_cache_target_ = 0.0f;  // CVca+0x7c: gain target written by that call
  bool amp_silent_call_ = false;   // FUN_10031c70 guard hit with cached gain 0: silent
  // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A):
  // device FUN_10031c70 ramp-leg per-call gain array + CVca/CVcf walk grids.
  bool amp_guard_hit_ = false;     // guard hit (cached level+mods), any target
  float sse_gain_[256] = {};       // per-call device-form gains (span <= 256)
  int sse_gain_n_ = 0;             // valid entries in sse_gain_
  int sse_gain_i_ = 0;             // consumed entries
  float sse_gain_last_ = 0.0f;     // device +0x78 store value at call end
  int sc_pos_ = 0;                 // CVcf walk-call sample position
  int walk_start_rel_ = 0;         // current CVca walk-call start (voice-rel)
  int walk_len_rel_ = 256;         // current CVca walk-call len
  int block_size_ = 0;             // host block size
  int sc_len_ = 0;                 // current CVcf walk-call len
  std::int64_t base_abs_ = 0;      // abs sample of voice-rel 0
  int amp_cap_ = 0;       // law #82: frames to the next CVca GAIN-call cut
                          // (law #159-L1: min(range-rem,256) anchored at the
                          // render-range start + block rem + range end +
                          // segment cuts of AMP-WIRED LFOs only; no wrap cuts)
  int env_cap_ = 0;       // frames to the next CVcf/CVco env-walk cut (law-123 grid)

 public:
  // Diagnostic (law #90 unit pins): ring buffer of the CVca ladder chunk
  // lengths as drawn by the chunk walk (last AMP_TRACE_N chunks; amp_trace_n
  // = total drawn). Audio path unaffected; zero allocations.
  static constexpr int AMP_TRACE_N = 16;
  int amp_trace[AMP_TRACE_N] = {};
  int amp_trace_n = 0;
  // Diagnostic (law #159-L1 hook-boundary pins): the voice-local rendered
  // sample count at the last CVca end-of-release hook (== the absolute
  // render position of the first silent sample when the voice started at
  // render start; -1 = never). Audio path unaffected.
  std::int64_t hook_pos_ = -1;
  std::int64_t rendered_ = 0;  // samples processed by this voice
  // Diagnostic (law #82 unit pins): the CVca GAIN-APPLY call spans (last
  // AMP_TRACE_N calls; amp_span_n = total). Audio path unaffected.
  int amp_span_trace[AMP_TRACE_N] = {};
  int amp_span_n = 0;
  float amp_g_step() const { return g_step_; }  // law #90 steady-state pin

  // Diagnostic (law #121 unit pins): the CVco mod-env walk draws — span and
  // re-rounded level of each advance_chunk call (last MOD_TRACE_N draws;
  // mod_trace_n = total drawn). Pins the carry-across-cuts + re-round-at-
  // every-cut grid (reports/121_cvco_settle.md F5). Audio path unaffected.
  static constexpr int MOD_TRACE_N = 32;
  int mod_trace_span[MOD_TRACE_N] = {};
  int mod_trace_lvl[MOD_TRACE_N] = {};
  int mod_trace_n = 0;

  // Diagnostic (law #172 unit pins): the CVcf env-walk draws — span and
  // level-at-span-end of each advance_chunk call (last FLT_TRACE_N draws;
  // flt_trace_n = total drawn). Pins the full-chunk-re-armed call grid
  // (tasks/consults/m3-172-cvcf-level-law.answer.md). Audio path unaffected.
  static constexpr int FLT_TRACE_N = 32;
  int flt_trace_span[FLT_TRACE_N] = {};
  int flt_trace_lvl[FLT_TRACE_N] = {};
  int flt_trace_n = 0;

 private:
  int flt_cap_ = 0;       // law #172: frames to the next CVcf env-walk CALL cut
                          // (law-123 grid destination-gated: block-rem + 256 +
                          // range end + segment cuts of FILTER-WIRED LFOs only;
                          // no wrap cuts; -1 from standalone callers = env_cap_)
  int flt_ch_rem_ = 0;    // ... CVcf chunk (law #95b: carries across ticks)
  int flt_call_rem_ = 0;  // law #95b: samples left in the current CVcf env call
  int mod_ch_rem_ = 0;    // ... CVco (mod env) chunk (law #121: carries
  int mod_call_rem_ = 0;  // law #121: samples left in the current CVco env call
  int blk_flt_lvl_ = 0;
  int blk_mod_lvl_ = 0;
  int last_fm_gate_ = 0x7fffffff;
  // CVcf envelope slew: target-seeking, one step per fs/20 segment
  // (FUN_18003ad50: cvcf[0x9c] = fs/20), linear between segment endpoints.
  int flt_seg_len_ = 0;
  int flt_seg_rem_ = 0;
  int flt_seg_from_ = 0;
  int flt_seg_to_ = 0;
};

// The voice is finished when its amp env stage is idle and the gate is not
// held (spec_env.md §7, FUN_180052580: 3 = finished but still allocated).
// The engine then frees the slot; the CVca end-of-release hook
// (FUN_180037f00) resets CVcf and the CVco mod env.
inline bool is_finished(const Voice& v) {
  return !v.gate_held && (v.amp_env.stage == env::STAGE_IDLE);
}

// ---------------------------------------------------------------------------
// Legacy stage-1 API (core/engine/engine.h) — unchanged semantics:
// osc1 sine direct to the amp stage (no filter / mod env / LFO path).
// ---------------------------------------------------------------------------

// Note-on (spec_env.md §4.1, FUN_1800526e0 + FUN_180037bc0):
// set pitch from the midi note, phase = 0, phase increment, amp env attack,
// velocity factor.
inline void note_on(Voice& v, std::uint8_t midi_note, std::uint8_t velocity,
                    float sample_rate, std::uint8_t vel_sens_band) {
  v.active = true;
  v.gate_held = true;
  v.midi_note = midi_note;
  v.sample_rate = sample_rate;
  v.osc1_hz = osc::midi_to_hz(midi_note);
  osc::init_phase(v.phase);
  v.phase_inc = osc::phase_increment(v.osc1_hz, sample_rate);
  v.amp_env.note_on();  // FUN_18000c930
  v.vel_factor = env::velocity_to_gain(velocity, vel_sens_band);
}

// Note-off (spec_env.md §7, FUN_1800525f0 → FUN_180037bb0):
// amp env → release (captures off_level). The voice stays "active" until
// the release completes (stage → idle); the engine then frees the slot.
inline void note_off(Voice& v) {
  v.gate_held = false;
  v.amp_env.note_off();  // FUN_180037bb0
}

// Advance the global note-on phase-init LCG (DAT_1800938d4) by note_ons·10
// draws without sounding anything — replicates the reference host's silent
// pre-roll note-ons so the audible note lands on the same LCG state.
void prime_phase_init(int note_ons);

// Re-seed the phase-init LCG to 1 (DAT_1800938d4 ctor value). Called once per
// engine instance so renders are deterministic within a process.
void reset_phase_lcg();

// Test/inspection helpers for the global phase-init LCG.
std::uint32_t phase_lcg_state();
std::uint32_t phase_lcg_advance();

}  // namespace synth1::voice
