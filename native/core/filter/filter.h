// filter.h — the CVcf filter stage (Stage 3).
//
// Spec: spec_filter.md §1.1 (LP12/HP12/HP24 two-integrator loop),
//       §1.2 (LP24 4-stage Tustin ladder with per-sample coefficient slew),
//       §1.3 + spec_lpdlosc.md (LPDL diode ladder, tanh loop saturation,
//       2x oversampling when FS <= 48000),
//       §2 (constant table), §4 (post-filter soft-clip + leveler, ±20
//       resonance-safety clamp), §5 (cutoff index composition + slew),
//       fc_lockin.md §1 (1 octave = 101.80 table indices).
// Ghidra: CVcf vftable @0x1800771a0; FUN_18003ad50 (dispatcher: index
//         compose + mirror-fold + clamp + 50 ms slew),
//         FUN_180038300 (LP12/HP12/HP24), FUN_1800390b0 (LP24),
//         FUN_1800398b0 (LPDL), FUN_18003a920 (soft-clip),
//         FUN_18003af70 (leveler), FUN_1800381f0 (cutoff table builder).
//
// Cutoff table: T[i] = 15.0 * 1066.66667^(i/1024), i = 0..1023
// (15.0 Hz .. 15891.43 Hz, per-index ratio 1.006832114 ~= 1.006832;
// spec_filter.md §2, verified by wine peek; fc_lockin.md §5 confirms the
// table values to 0.0002 Hz).
//
// Portability notes (documented deviations, spec UNKNOWNs):
//  * The env→cutoff mapping (spec_filter.md §5) takes the envelope level as
//    fs_kHz = 48 + 12*level (level 0..1 → exponent (fs_kHz-48)/12 in [0,1]).
//  * Keytrack (FUN_180038070, spec_env.md §4.3 — spec_filter.md §5 mislabels
//    +0xa8/+0xac as env fields): for amt = band/127 > 0,
//      x     = pow(amt + 1, (key − 48)/12)          (centre C3 = 48)
//      i_key = clamp((int)(1024·logf(T[i0]·x/15)/logf(1066.66663)), 0, 1024) − i0
//    ≈ 101.8·log2(amt+1)·(key−48)/12: full tracking = 1 octave/octave.
//  * The velocity contribution (the dispatcher's edx producer is UNKNOWN) is
//    mapped the same way: i_vel = amt * vel * 101.80 (one octave at full).
//  * The leveler retention +0xe0 = expf(−1/(FS·0.005)) (5 ms, FUN_18003a850
//    @0x18003a8b0); the step used here is 1 − that.
//    The leveler always runs (FUN_18003a920 calls FUN_18003af70 first,
//    unconditionally; it engages once |x| > 1.1) and the soft-clip follows
//    it only when saturation is on.
//
// No realtime allocations: process_sample() is pure arithmetic on POD state.

#pragma once

#include <cstdint>
#include "../spec_constants.h"

namespace synth1::filter {

// 1024-entry geometric cutoff table, built once (lazy static):
// fc = 15.0 * pow(1066.66667, i/1024) — equivalent to 15.0 * ratio^i with
// ratio = 1066.66667^(1/1024) = 1.006832114 (spec_filter.md §2).
const float* fc_table();

// filter types (CVcf +0xa4; ctor default 1 = LP24)
inline constexpr int FTYPE_LP12 = 0;
inline constexpr int FTYPE_LP24 = 1;
inline constexpr int FTYPE_HP12 = 2;
inline constexpr int FTYPE_HP24 = 3;
inline constexpr int FTYPE_LPDL = 4;

// ±20 resonance-safety clamp (spec_filter.md §4; LP12/HP12/HP24 + LP24 only)
inline constexpr float CLAMP_HI = 20.0f;
inline constexpr float CLAMP_LO = -20.0f;
inline constexpr float CLAMP_RES_TRIGGER = 0.9f;  // DAT_1800756d0

// one octave of cutoff = 101.80 table indices (fc_lockin.md §1)
inline constexpr float OCTAVE_INDICES = 101.80f;

// LPDL tanh3 Padé clamp domain / coefficients (spec_filter.md §1.3)
inline constexpr double TANH3_LO = -3.0;
inline constexpr double TANH3_HI = 3.0;
inline constexpr double TANH3_NUM = 27.0;   // DAT_180077270
inline constexpr double TANH3_DEN = 9.0;    // DAT_180077260
inline constexpr double LADDER_GAIN_97 = 1.2857142857142858;  // 9/7, _DAT_180077230

// rational tanh Padé saturator (spec_filter.md §1.3)
inline double tanh3(double e) {
  if (e < TANH3_LO) return -1.0;
  if (e > TANH3_HI) return 1.0;
  const double e2 = e * e;
  return e * (e2 + TANH3_NUM) / (TANH3_DEN * e2 + TANH3_NUM);
}

// ---------------------------------------------------------------------------
// Filter state (CVcf fields used by the portable engine).
// ---------------------------------------------------------------------------
struct Filter {
  // ---- patch parameters ----
  int ftype = FTYPE_LP24;   // +0xa4 (ctor 1)
  float res = 0.1f;         // +0x94 (ctor 0.1), clamped to [0,1] on set
  int i0 = 512;             // +0x90 base cutoff index (ctor 512)
  float env_amt = 0.0f;     // +0xa8 filter-env amount (>0 enables env→cutoff)
  float env_level = 0.0f;   // envelope level 0..1 (portable mapping, header)
  int i_env = 0;            // +0xac env contribution
  float keytrack_amt = 0.0f;
  int i_key = 0;            // +0xb8 keytrack contribution
  float vel_amt = 0.0f;     // filter velocity amount (portable mapping)
  float vel01 = 0.0f;       // note velocity 0..1
  int i_vel = 0;            // velocity contribution to cutoff index
  int i_spread = 0;         // +0xa0 unison filter-spread term (#198, FUN_10034050)
  int extra_idx = 0;        // dispatcher edx extra index (portable hook)
  int mirror_a = 0;         // s5 mirror-fold flag (spec §dispatcher; default 0)
  int mirror_b = 0;         // s6 mirror-fold flag
  float sat = 0.0f;         // +0xc4 saturation amount
  // CVcf signal-domain harness (env FILTER_DOMAIN, default 1.0 = no-op). The
  // device writes the CVco->CVcf link in the 2048 int-domain; the leveler
  // (DAT_180073c1c=1.1) and the ±20 clamp (DAT_10064528) are thresholds in
  // that domain, so they must scale with it. Output is normalized /domain at
  // the CVca read, keeping the linear path net-neutral.
  float domain = 1.0f;
  float leveler_target = 1.1f;  // DAT_180073c1c; env FILTER_LEVELER probes lower device targets

  // ---- sample-rate derived (prepare) ----
  int fs = 44100;             // +0x10 (int Hz)
  int slew_interval = 2205;   // FS/20 (50 ms cutoff slew)
  int lpdl_sub = 2;           // LPDL sub-steps: (FS<=48000)?2:1
  float leveler_coef = 0.0f;  // 1 − (+0xe0 retention), see header

  // ---- cutoff slew state ----
  int i_cur = 512;    // +0x98
  int i_start = 512;  // +0xa0
  int slew_n = 2205;  // +0x9c (init FS/20)

  // ---- coefficient cache (+0xcc/+0xd0) ----
  int cache_idx = -1;
  float cache_res = -1.0f;

  // ---- LP12/HP12/HP24 (+0xe4..+0xf8) ----
  float g = 0.0f, A = 0.0f, rmin = 0.0f, K = 0.0f;
  float s1 = 0.0f, s3 = 0.0f;

  // ---- LP24 (+0xfc..+0x138) ----
  float L0t = 0.0f, L1t = 0.0f, L2t = 0.0f;  // targets
  float L0 = 0.0f, L1 = 0.0f, L2 = 0.0f;     // live (slewed)
  float slew_c = 0.0f;      // LP24 coeff slew step = 1 - ret, ret = 0.02^(1/(FS*0.001))
                            // (CVcf+0x120/+0x138 retention, 2% of gap in 1 ms;
                            // reports/123_selfosc.md §5, M2b law 1)
  float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
  float aP = 0.0f, bP = 0.0f, cP = 0.0f, dP = 0.0f;
  bool lp24_live_init = false;

  // ---- LPDL (+0x1a8..+0x1c8, doubles) ----
  double kk = 0.0;  // k = T/(2(1+T))
  double a1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0;
  double w2 = 0.0, w3 = 0.0, g4 = 0.0, g5 = 0.0;
  double c2 = 0.0, c3 = 0.0, c4 = 0.0;
  double u1 = 0.0, rprime = 0.0, m = 0.0, gq = 0.0, nout = 0.0;
  double m4 = 0.0, ls1 = 0.0, ls2 = 0.0, ls3 = 0.0, m2 = 0.0;

  // ---- leveler (+0xdc) ----
  float leveler_env = 0.0f;

  // ---- stage debug peaks (SYNTH1_STAGE_DEBUG) ----
  bool dbg = false;
  float dbg_in = 0.0f, dbg_out = 0.0f, dbg_env = 0.0f, dbg_gain_min = 1.0f;
  float dbg_x = 0.0f;  // filter INPUT peak (osc-mix out)
  int dbg_clamp = 0;
  float dbg_L0 = 0.0f, dbg_L1 = 0.0f, dbg_L2 = 0.0f, dbg_fc = 0.0f;
  int dbg_i0 = 0, dbg_i_key = 0, dbg_i_env = 0, dbg_i_vel = 0, dbg_i_cur = 0;

  // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A):
  // device FUN_10033740 soft-clip group forms on the CVcf walk-call grid.
  int sse_sc_pos = 0;              // sample position in the current CF call
  int sse_sc_len = 0;              // current CF walk-call length
  int sse_sc_start_mod4 = 0;       // call-start phase mod 4 (device: 0)

  // ---- lifecycle ----
  void prepare(float sample_rate);
  ~Filter();
  void reset();

  // ---- parameter setters (each re-triggers the 50 ms cutoff slew, like
  //      FUN_18003b6c0: i_start = i_cur, slew_n = 0) ----
  // switching core type invalidates the shared coefficient cache (+0xcc/+0xd0):
  // the new core must rebuild its coefficients on the next sample.
  void set_filter_type(int t) { ftype = t; cache_idx = -1; }
  void set_resonance(float r) {
    res = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r);  // stability limit
  }
  void set_cutoff_index(int idx) {
    i0 = (idx < 0) ? 0 : (idx > 1023 ? 1023 : idx);
    retrigger_slew();
  }
  void set_env_amount(float amt) { env_amt = amt; recompute_env(); }
  void set_env_level(float level01);   // envelope 0..1 → i_env (spec §5)
  // FUN_18003b720 (param 22): keytrack recompute + slew restart.
  void set_keytrack_amount(float amt) {
    keytrack_amt = amt; recompute_key(); retrigger_slew();
  }
  // FUN_18003a780 (note-on): the velocity scalar CVcf+0xc0 is written
  // FIRST (it only scales the env amount via FUN_180038140), then
  // cvcf[0x9c] = FS/20 — the slew counter is set to the interval, so the
  // cutoff SNAPS to the new target at every note-on, param 24 or not.
  // (The earlier param-24 conditional retrigger was wrong: it slewed the
  // onset for param-24 patches and cost bank0 039 Synth Bass 1 13.6 dB of
  // null; the reference onset snap is unconditional.)
  void arm_note_on(bool velocity_on) {
    (void)velocity_on;
    slew_n = slew_interval;
  }
  void set_note(int midi) { note = midi; recompute_key(); }
  void set_velocity(float amount01, float v01) {
    vel_amt = amount01; vel01 = v01;
    i_vel = (int)(vel_amt * vel01 * OCTAVE_INDICES);
    retrigger_slew();
  }
  // #198 unison filter-spread term (CVcf+0xa0, writer FUN_10034050
  // decomp_all_32_full.c:37333-37361): the writer touches ONLY +0xa0 —
  // no slew retrigger (unlike the param setters that arm cvcf+0x9c).
  void set_spread_term(int term) { i_spread = term; }
  // param 23 curve (spec_filter.md §4): sat = (p/127)·exp(5.8·(p/127 − 1))
  void set_saturation_param(int p);

  int oversample_sub() const { return lpdl_sub; }

  // ---- audio ----
  float process_sample(float x);

private:
  int note = 60;
  void retrigger_slew() { i_start = i_cur; slew_n = 0; }
  void recompute_key();          // FUN_180038070 (see header)
  void recompute_env();
  void update_cutoff();          // compose + mirror-fold + clamp + slew
  void update_coeffs();          // cache check + per-core coefficient setup
  void setup_lp12();
  void setup_lp24();
  void setup_lpdl();
  float run_lp12hp(float x);     // ftype 0/2/3
  float run_lp24(float x);       // ftype 1
  float run_lpdl(float x);       // ftype 4
};

}  // namespace synth1::filter
