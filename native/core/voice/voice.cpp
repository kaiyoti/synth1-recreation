// voice.cpp — Stage 5 CVoice: the full per-voice chain
// CVco (osc + mod env) → CVcf (filter + filter env) → CVca (amp + amp env).
//
// Spec: dsp_flow.md §2, spec_osc.md §4.2/§6.1, spec_filter.md §5,
//       spec_env.md §0/§2–§7. Ghidra: FUN_1800526e0/52740/525f0 (voice
//       note-on/trigger/release), FUN_18003dbe0 (mod routing sums),
//       FUN_18003ad50 (cutoff index compose + slew), FUN_180037c30 (amp
//       gain), FUN_180037f00 (end-of-release hook).
//
// The legacy stage-1 per-sample path (process_sample) stays inline in
// voice.h for engine.h; the full chain lives here. NO heap allocations in
// any audio path: all state is POD, wavetables are referenced through the
// static global bank.

#include "voice.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>

#include "lfo/lfo.h"

namespace synth1::voice {

// m3r-111 G11 J1=7 (LANDED, unconditional): FUN_1000b3a0 :8168-8222 two-level
// CVco driver budget (outer L1-cut held per iteration at caller 0xb444, inner
// L2-cut re-read per step head at caller 0xb496) + driver cuts for routed
// dests 1..6 (device D012/D103: L1 dest 3 read at 0xb444, L2 dest 4 at
// 0xb496; PAN 7 excluded — D005: L1 dest 7 read at 0x13fa4, not by the
// driver) + mod-walk cap = cm_call_rem_ + call budget = the device render
// piece (min(piece,0x100) chained from piece_anchor_).

// ---------------------------------------------------------------------------
// note-on phase init (FUN_18003e600, param 91 "osc phase shift")
//
// Branch law (spec_phase.md §4):
//   A: param 91 >= 1 — every note-on uses the fixed T-table phase offsets.
//   B: param 91 == 0 and reseed flag cleared — phases are NOT touched.
//   C: param 91 == 0 and reseed flag set — first note-on per voice consumes
//      10 draws from the process-global MSVC rand() seed (DAT_1800938d4;
//      CRT-initialized to 1, s = s·0x343fd + 0x269ec3): osc1 (+0x98),
//      osc2 (+0x10c, masked 0x7ffffff), then the 8 unison detune slots
//      (+0xe4[0..7]).
//   phase = (int)((float)((double)((s>>16)&0x7fff) · 3.05185095e-05) ·
//                 134217728.0f)
// band p ≥ 1: osc1 phase = 0, osc2 phase = (int)((p−1)/126 · 2^26).
// ---------------------------------------------------------------------------
namespace {
std::uint32_t g_phase_lcg = 1;  // DAT_1800938d4 (MSVC rand seed)

std::uint32_t next_random_phase() {
  g_phase_lcg = g_phase_lcg * 0x343fd + 0x269ec3;
  return (std::uint32_t)((float)((double)((g_phase_lcg >> 16) & 0x7fff) *
                                 3.05185095e-05) * 134217728.0f);
}
}  // namespace

void prime_phase_init(int note_ons) {
  for (int n = 0; n < note_ons; ++n)
    for (int i = 0; i < 10; ++i) next_random_phase();
}

void reset_phase_lcg() { g_phase_lcg = 1u; }

std::uint32_t phase_lcg_state() { return g_phase_lcg; }
std::uint32_t phase_lcg_advance() { return next_random_phase(); }

// ---------------------------------------------------------------------------
// control plane
// ---------------------------------------------------------------------------
void Voice::prepare(float sample_rate, const tables::WavetableBank* bank) {
  this->sample_rate = sample_rate;
  osc_params.sample_rate = (int)sample_rate;  // cv+0x10 → FM ramp cap sr/1000
  filt.prepare(sample_rate);
  // CVcf-domain harness (env FILTER_DOMAIN, default 1.0 = no-op / current binary).
  if (const char* e = std::getenv("FILTER_DOMAIN")) filt.domain = std::strtof(e, nullptr);
  if (const char* e = std::getenv("FILTER_LEVELER")) filt.leveler_target = std::strtof(e, nullptr);
  amp_env.reset();
  filter_env.reset();
  mod_env.reset();
  amp_env.half_chunks = true;   // CVca+0x34 = 1 (spec_env.md §2.3)
  filter_env.half_chunks = false;
  mod_env.half_chunks = false;
  flt_seg_len_ = (int)(sample_rate / 20.0f);  // cvcf[0x9c] = fs/20
  flt_seg_rem_ = 0;
  flt_seg_from_ = 0;
  flt_seg_to_ = 0;
  active = false;
  gate_held = false;
  phase_reseed = true;
  osc_state = osc::OscState{};
  osc::init_phase(phase);
  cur_pitch_mod = 0x7fffffff;
  cur_pitch_mod2 = 0x7fffffff;
  last_fm_gate_ = 0x7fffffff;
  if (bank) refresh_tables(*bank);
}

void Voice::refresh_tables(const tables::WavetableBank& bank) {
  // Band selection per block from the MODULATED frequencies (spec_osc.md
  // §5.6) — osc2 from its own (octave/fine-shifted) frequency, so a +4-octave
  // osc2 gets a table with correspondingly fewer harmonics. Before the first
  // sample the modulated values equal the base frequencies.
  const float f1 = (cur_f1 > 0.0f) ? cur_f1 : osc1_hz;
  const float f2 = (cur_f2 > 0.0f) ? cur_f2 : osc2_hz;
  osc1_table = bank.table(static_cast<tables::Waveform>(osc_params.osc1_wave),
                          f1);
  osc2_table = bank.table(static_cast<tables::Waveform>(osc_params.osc2_wave),
                          f2);
  // Sub oscillator (spec_osc.md §6.4): shape via DAT_1800772c8 = {0,3,1,5}
  // (sine, triangle, saw, square); band from f1 · (−1 octave ? 0.5 : 1).
  static constexpr tables::Waveform kSubShape[4] = {
      tables::Waveform::kSine, tables::Waveform::kTriangle,
      tables::Waveform::kSaw, tables::Waveform::kSquare};
  const int sw = (sub_wave < 0) ? 0 : (sub_wave > 3 ? 3 : sub_wave);
  sub_table = bank.table(kSubShape[sw], sub_octave ? f1 * 0.5f : f1);
}

// m3-166-land (tasks/consults/m3-166-slotpitch-2.answer.md): device
// cents→ratio two-table law FUN_10035290 :38215-38237 (ratio =
// float(oct[p>>7]·fine[p&0x7f]), tables built FUN_10035680 :38692-38713)
// plus the FUN_10034740 :37757-37840 increment roundings. Gate-1 verified
// bit-exact against device taps (tasks/m3-166-slotpitch-2/RESULT.md F1).
namespace spdev {
inline const float* fine() {  // DAT_1015ac58[0..256] = float(2^(x/128)), x=-128..128
  static const std::array<float, 257> t = [] {
    std::array<float, 257> a{};
    for (int i = 0; i < 257; ++i)
      a[(size_t)i] = (float)std::pow(2.0, (double)((float)(i - 128) * 0.0078125f));
    return a;
  }();
  return t.data();
}
inline const float* oct() {  // DAT_1015b088[k] = float(2^k), k=-10..10
  static const std::array<float, 21> t = [] {
    std::array<float, 21> a{};
    for (int j = 0; j < 21; ++j) a[(size_t)j] = (float)std::pow(2.0, (double)(j - 10));
    return a;
  }();
  return t.data();
}
inline float ratio(int p) {  // FUN_10035290 :38226-38233 exact index arithmetic
  const int o = (p + ((p >> 31) & 0x7f)) >> 7;   // oct index (ceil for p<0)
  const int fi = p - 128 * o;                    // fine index in [-127,127]
  return oct()[o + 10] * fine()[fi + 128];
}
inline float det_scaled(int p, float det) {  // det loop :37826-37831: x87
  // oct*fine*det with a SINGLE float rounding (no float ratio intermediate)
  const int o = (p + ((p >> 31) & 0x7f)) >> 7;
  const int fi = p - 128 * o;
  return (float)((long double)oct()[o + 10] * (long double)fine()[fi + 128] *
                 (long double)det);
}
inline bool guard(float f) {  // 0x46abdfff < (int)bits(f)  (FUN_10034740 :37764)
  unsigned u; std::memcpy(&u, &f, 4);
  return (int)u > 0x46abdfff;
}
inline std::int32_t x87round(float p) {  // ROUND: ties-to-even (97101_rounding)
  const float fl = ::floorf(p);
  const float fr = p - fl;
  return (std::int32_t)fl + (fr > 0.5f || (fr == 0.5f && (((std::int32_t)fl) & 1)));
}
inline std::int32_t inc_osc(float f, float sr) {  // local_a4 :37795
  return x87round((float)((double)(f * 134217728.0f) * (1.0 / (double)sr)));
}
inline std::int32_t inc_det(float f, float sr) {  // afStack_30 :37838
  return x87round((float)((double)f * (1.0 / (double)sr) * 134217728.0));
}
}  // namespace spdev

// FUN_18003bb00 (osc1) + FUN_18003bba0 (osc2), spec_osc.md §4.1:
//   n      = clamp(note + keyShift, 0, 128)
//   osc1Hz = 2^(fine12/1200) · NOTEFREQ[n]
//   osc2Hz = kbd ? osc1Hz · 2^(osc2Fine/1200 + osc2Oct)
//                : 220 · 2^((fine12 + osc2Fine)/1200 + osc2Oct)
// (unisonPitch = 0: unison is not implemented.) The power is evaluated in
// double, the exponent in float (DAT_1800771f0 = 1/1200f).
void Voice::update_base_pitch() {
  const int n = (int)midi_note + key_shift;
  const float pitch_cents = fine12 + unison_pitch_cents;
  // m3-166-land: device FUN_100342d0 :37503-37519 — exponent double(cents)·
  // (1/1200.0), pow in double (gate-1: +0x7c bits match device taps).
  osc1_hz = (float)(std::pow(2.0, (double)pitch_cents * (1.0 / 1200.0)) *
                    (double)osc::note_freq(n));
  if (osc2_kbd) {
    osc2_hz = (float)(std::pow(2.0, (double)(osc2_fine * (1.0f / 1200.0f) +
                                             osc2_oct)) *
                      (double)osc1_hz);
  } else {
    osc2_hz = (float)(std::pow(2.0, (double)((pitch_cents + osc2_fine) *
                                                 (1.0f / 1200.0f) +
                                             osc2_oct)) *
                      (double)osc::master_tune_base220());  // DAT_1801a2c68
  }
  cur_pitch_mod = cur_pitch_mod2 = 0x7fffffff;  // force increment recompute
  regen_detune();
}

// Law K (#57/#165): CVco detune-Hz regeneration (FUN_10034500 :37591-37617).
// Base-2 law — FUN_1004e4c1(2.0) = ln2 feeds FUN_1004ccb3 = expf, net
// f1·2^(±k·det/1200), k = 2j+1 (constants _DAT_1006604c = +1/1200,
// _DAT_100661c0 = −1/1200, DAT_100638b0 = 2.0). Called from the pitch
// recompute and the param-76 setter iff the new detune > 0 (device gate);
// at detune == 0 the slots keep their previous values (boot default 100.0,
// CELLS correction 3) — inert because the D=0 kernels never read them.
void Voice::regen_detune() {
  const float det = osc_params.detune;  // cv+0x104 (0..20)
  if (!(0.0f < det)) return;
  // m3-166-land: device FUN_10034500 :37604-37614 float chain —
  // a=(float)k*det; e=(a*(1/1200f))*ln2f (DAT_100638b0 after
  // FUN_1004e4c1(2.0)); det_hz = float(exp(e))·+0x7c float mults
  // (gate-1: det_hz bits match device +0xec taps).
  static const float ln2f = (float)std::log(2.0);
  for (int j = 0; j < 4; ++j) {
    const int k = 2 * j + 1;
    const float a = (float)k * det;
    const float ep = (a * (1.0f / 1200.0f)) * ln2f;
    const float em = (a * (-1.0f / 1200.0f)) * ln2f;
    det_hz[2 * j] = (float)std::exp((double)ep) * osc1_hz;
    det_hz[2 * j + 1] = (float)std::exp((double)em) * osc1_hz;
  }
}

// FUN_18003b8c0 (spec_osc.md §7): the osc mix law.
//   denom = (subAmp + 1)(1 − mix) + mix
//   g1 = (1 − mix)/denom,  g2 = mix/denom,  gs = g1 · subAmp
void Voice::recompute_mix() {
  const float u1 = 1.0f - osc_mix;
  const float denom = (sub_amp + 1.0f) * u1 + osc_mix;
  const float norm = 1.0f / denom;  // cv+0x14c masterNorm
  osc_params.osc1_gain = norm * u1;
  osc_params.osc2_gain = norm * osc_mix;
  osc_params.sub_gain = osc_params.osc1_gain * sub_amp;
  // kernel sub mode (§6.1): 0 off, 1 unison, 2 −1 octave
  osc_params.sub_oct = (osc_params.sub_gain == 0.0f) ? 0 : sub_octave + 1;
}

void Voice::apply_filter_env_amount() {
  // FUN_180038140 (spec_env.md §4.3): the env depth CVcf+0xb4 (param 21,
  // bipolar) — scaled by CVcf+0xc0/127 when the velocity switch (param 24)
  // is on — is clamped to the cutoff headroom so a full-scale envelope never
  // drives the index past 0..1024:
  //   f1     = (1024 − i_spread − i_kbd − i0)/1024
  //   amount = clamp(depth, f1 − 1, f1)
  // then FUN_18000d2c0: level = (int)(1024·|amount|), sign in CVcf+0x24.
  // (#46: the +0xb8 term is the #198 unison filter-spread term, i_spread —
  // now included; it is 0 whenever unison is off.)
  float amt = filter_env_amount;
  if (filter_vel_on) {
    amt = (float)filter_vel127 * filter_env_amount * (1.0f / 127.0f);
  }
  // FUN_100321e0 bytes @0x100321e0: mov $0x400,%eax; sub 0xa0(%ecx)
  // (i_spread, #198 unison filter-spread term); sub 0x94(%ecx) (i_key);
  // sub 0x78(%ecx) (i0); cvtdq2ps; mulss [0x10066050] = 1/1024.
  const float f1 =
      (float)(1024 - filt.i_key - filt.i0 - filt.i_spread) * (1.0f / 1024.0f);
  if (amt > f1) amt = f1;
  if (amt < f1 - 1.0f) amt = f1 - 1.0f;
  filter_env_sign = (amt < 0.0f) ? -1 : 1;
  const float aamt = std::fabs(amt);
  filter_env.target_level = (int)((float)FILTER_ENV_SCALE * aamt);
  filter_env.sustain_level =
      (int)((float)FILTER_ENV_SCALE * aamt * filter_env.sustain_ratio);
}

void Voice::apply_mod_env_scale() {
  // spec_env.md §5.4 (FUN_18003e320, decomp_all.c:38544-38568): scale +
  // decay shape by param 71's OWN destination enum, stored at CVco+0x184:
  //   dest 0 → scale 0x280 (640), decay flag 0 (FUN_180005750(...,0))
  //   dest 1 → scale 0x200 (512), decay flag 1 = LINEAR decay (38556-38557)
  //   dest 2 → scale 0x80 (128), decay flag 0
  //   dest > 2 → scale left untouched (38560-38561: early return)
  // Law #74: the old decode used the LFO destination enum (pitch at 1/2,
  // PW at 5, FM at 6), contradicting voice.h:38/84-86/195.
  const int scale = (mod_dest == 0) ? MOD_ENV_SCALE_PITCH
                    : (mod_dest == 1) ? MOD_ENV_SCALE_FM
                    : (mod_dest == 2) ? MOD_ENV_SCALE_PW
                                      : 0;  // dest > 2: FUN_18003e320 sets no scale
  mod_env_sign = (mod_env_amount < 0.0f) ? -1 : 1;
  const float a = std::fabs(mod_env_amount);
  mod_env.target_level = (int)((float)scale * a);
  // The mod env is an AD envelope: decay returns to ZERO. CEnvelopedObject
  // ctor sets sustainRatio(+0x54)=1.0 but the CVco mod env has no sustain
  // target — VST renders prove the level decays back to 0 after the attack
  // (single ~74 ms blip on 033 at attack 48 ms / decay 10 ms), i.e. the
  // decay glides toward sustain_level = 0, not the attack target.
  mod_env.sustain_ratio = 0.0f;
  mod_env.sustain_level = 0;
  mod_env.decay_mode = (mod_dest == 1) ? 1 : 0;  // param 71 dest 1 FM = LINEAR decay (FUN_180005750 flag, 38557)
}

void Voice::note_on(int note, int velocity, float sample_rate,
                    const tables::WavetableBank& bank) {
  this->sample_rate = sample_rate;
  osc_params.sample_rate = (int)sample_rate;  // cv+0x10 → FM ramp cap sr/1000
  active = true;
  gate_held = true;
  midi_note = (std::uint8_t)note;

  // pitch (spec_osc.md §4.1: FUN_18003bb00 / FUN_18003bba0)
  update_base_pitch();
  cur_f1 = osc1_hz;
  cur_f2 = osc2_hz;

  // FUN_18003db00 (CVco note-on): fmLastTarget = −1, fmBlockCur = 0 — the
  // first target check of the note runs the state machine with zero hold
  // time ⇒ depth snaps to the note-on target.
  osc::fm_note_on_reset(osc_state.fm);

  // FUN_18003e600: param 91 == 0 leaves the accumulators free-running after
  // the first note-on; only the first note after init/patch-load consumes the
  // global LCG.  The noise LFSR is not re-seeded here.
  if (osc_phase_band > 0) {
    osc::init_phase(phase);
    osc_state.osc1_phase = 0;
    osc_state.osc2_phase =
        (std::uint32_t)((float)((float)(osc_phase_band - 1) * (1.0f / 126.0f)) *
                        67108864.0f) &
        0x7ffffffu;
    phase_reseed = false;
  } else if (phase_reseed) {
    static int skip = []{ const char* e = getenv("SYNTH1_PHASE_SKIP"); return e ? atoi(e) : 0; }();
    for (int i = 0; i < skip; ++i) next_random_phase();
    phase.phase = next_random_phase();
    osc_state.osc1_phase = phase.phase;
    osc_state.osc2_phase = next_random_phase() & 0x7ffffffu;
    for (int i = 0; i < 8; ++i)
      osc_state.det_phase[i] = next_random_phase();  // law K #166b: branch-C
    // draws seed the 8 detune slots UNCONDITIONALLY (FUN_10036d60
    // :39556-39566, no detune gate); the D=0 kernels just never advance
    // them (76=0 cells: draws frozen blk1==blk20).
    phase_reseed = false;
  } else {
    phase.phase = osc_state.osc1_phase;
  }
  phase_inc = osc::phase_increment(osc1_hz, sample_rate);
  phase_inc2 = osc::phase_increment(osc2_hz, sample_rate);

  // AMP: velocity factor + envelope note-on (FUN_180037bc0, spec_env §4.2)
  vel_factor = env::velocity_to_gain((std::uint8_t)velocity, amp_vel_sens_band);
  amp_env.note_on();
  amp_ch_rem_ = 0;    // law #90: the stage change cuts the CVca chunk walk
  amp_call_rem_ = 0; walk_start_rel_ = 0; walk_len_rel_ = 256; sc_pos_ = 0;

  // FILTER (FUN_18003a780, spec_env §4.3): keytrack from the key number,
  // velocity scalar CVcf+0xc0 = (int)(scale·127), env amount with the
  // headroom clamp, envelope note-on.
  filt.set_note(note);
  filter_vel127 = (int)(filter_velocity_scale(velocity) * 127.0f);
  apply_filter_env_amount();
  filt.extra_idx = 0;
  // FUN_18003a780: cvcf[0x9c] = fs/20 → snap; with the velocity switch on
  // the +0xc0 write restarts the slew (see filter.h arm_note_on).
  filt.arm_note_on(filter_vel_on);
  filter_env.note_on();
  flt_ch_rem_ = 0;    // law #95b: the stage change cuts the CVcf chunk walk
  flt_call_rem_ = 0;
  flt_seg_rem_ = 0;  // FUN_18003a780: cvcf[0x9c] = fs/20 re-armed on note-on

  // MOD ENV: fresh trigger (FUN_18003db00). The original never releases it
  // — it is reset by the CVca end-of-release hook (FUN_10037f00). NO reset
  // at note-on: FUN_1000b230 resumes the attack from the carried level
  // (progress = (attack_samples·cur_level)/target_level); the adsr note_on
  // resume branch reads cur_level, so the carried value MUST survive.
  mod_ch_rem_ = 0;    // law #121: the trigger cuts the CVco chunk walk
  mod_call_rem_ = 0;
  cm_call_rem_ = 0;   // m3-272: the trigger re-arms the CVco driver budget
  if (mod_env_on) {
    apply_mod_env_scale();
    mod_env.note_on();
  }

  refresh_tables(bank);
}

void Voice::retarget_note(int note, int velocity, float sample_rate,
                          const tables::WavetableBank& bank) {
  this->sample_rate = sample_rate;
  osc_params.sample_rate = (int)sample_rate;  // cv+0x10 → FM ramp cap sr/1000
  active = true;
  gate_held = true;
  midi_note = (std::uint8_t)note;

  // FUN_1800526e0 updates the CVco key and recomputes the target pitch; the
  // portamento offset is supplied separately by the allocator.  The envelope
  // trigger path (FUN_180052740) is intentionally not run for legato.
  update_base_pitch();
  cur_pitch_mod = 0x7fffffff;
  cur_pitch_mod2 = 0x7fffffff;
  phase_inc = osc::phase_increment(osc1_hz, sample_rate);
  phase_inc2 = osc::phase_increment(osc2_hz, sample_rate);
  refresh_tables(bank);
}

void Voice::note_off() {
  // FUN_1800525f0: gate off; CVca AND CVcf → release. CVco is NOT released.
  gate_held = false;
  amp_env.note_off();
  amp_ch_rem_ = 0;    // law #90: the stage change cuts the CVca chunk walk
  amp_call_rem_ = 0; walk_start_rel_ = 0; walk_len_rel_ = 256; sc_pos_ = 0;
  filter_env.note_off();  // off_level = held CVcf chunk level (law #95b grid)
  flt_ch_rem_ = 0;    // law #95b: the stage change cuts the CVcf chunk walk
  flt_call_rem_ = 0;
}

// ---------------------------------------------------------------------------
// parameter setters (band = raw 0..127; ids per params.gen.h / FUN_180028fe0)
// ---------------------------------------------------------------------------
void Voice::set_amp_env_bands(int b25, int b26, int b27, int b28) {
  amp_env.set_attack_seconds(env::amp_attack_seconds(b25), sample_rate);
  amp_env.set_decay_seconds(env::amp_decay_seconds(b26), sample_rate);
  amp_env.set_sustain_ratio(env::amp_sustain_ratio(b27));
  amp_env.set_release_seconds(env::amp_release_seconds(b28), sample_rate);
}

void Voice::set_amp_gain_band(int b29) {
  amp_gain = env::amp_gain_curve(b29);
}

void Voice::set_amp_vel_sens_band(int b30) {
  amp_vel_sens_band = (std::uint8_t)b30;
}

void Voice::set_filter_env_bands(int b15, int b16, int b17, int b18) {
  // Same time curves as the amp env (FUN_180028fe0 cases 0x0f/0x10/0x12 use
  // FUN_18000d210/0d260/0d3a0 with the same pow constants). Filter sustain
  // (case 0x11 → FUN_18003b780) is LINEAR: ratio = band/127.
  filter_env.set_attack_seconds(env::amp_attack_seconds(b15), sample_rate);
  filter_env.set_decay_seconds(env::amp_decay_seconds(b16), sample_rate);
  filter_env.sustain_ratio = (float)b17 * (1.0f / 127.0f);
  filter_env.set_release_seconds(env::amp_release_seconds(b18), sample_rate);
  // refresh target/sustain levels
  apply_filter_env_amount();
}

void Voice::set_filter_env_amount_band(int b21) {
  // FUN_18003b670 → FUN_180038140
  filter_env_amount = filter_amount_from_band(b21);
  apply_filter_env_amount();
}

void Voice::set_filter_cutoff_band(int b19) {
  // FUN_18003b6c0: i0 = (int)(band · DAT_180077288 / 127), DAT_180077288 =
  // 1023, then FUN_180038140 (the env headroom depends on i0)
  filt.set_cutoff_index((int)((float)b19 * (1023.0f / 127.0f)));
  apply_filter_env_amount();
}

void Voice::set_filter_resonance_band(int b20) {
  filt.set_resonance((float)b20 * (1.0f / 127.0f));
}

void Voice::set_filter_keytrack_band(int b22) {
  // FUN_18003b720: amount = band/127 → FUN_180038070 + FUN_180038140
  filt.set_keytrack_amount((float)b22 * (1.0f / 127.0f));
  apply_filter_env_amount();
}

void Voice::set_filter_saturation_band(int b23) {
  filt.set_saturation_param(b23);
}

void Voice::set_mod_env_amount_band(int b11) {
  mod_env_amount = mod_env_amount_from_band(b11);
  apply_mod_env_scale();
}

void Voice::set_mod_env_bands(int b12, int b13) {
  // spec_env.md §5.3: identical curves to the amp env (cases 0x0c/0x0d).
  mod_env.set_attack_seconds(env::amp_attack_seconds(b12), sample_rate);
  mod_env.set_decay_seconds(env::amp_decay_seconds(b13), sample_rate);
}

void Voice::set_mod_dest(int dest) {
  // param 71's own enum (params.gen.h positions = 3; FUN_18003e320 handles
  // 0/1/2): 0 = osc2 pitch, 1 = FM, 2 = pulse width. Not the LFO dest enum.
  mod_dest = (dest < 0) ? 0 : (dest > 2 ? 2 : dest);
  apply_mod_env_scale();
}

// ---- oscillator pitch / mix (FUN_180028fe0 cases 2/3/4/5/8/9/0x48/0x5f-0x61)
void Voice::set_osc2_pitch_band(int b2) {
  osc2_oct = osc2_octaves_from_band(b2);
  update_base_pitch();
}

void Voice::set_osc2_fine_band(int b3) {
  osc2_fine = fine_cents_from_band(b3);
  update_base_pitch();
}

void Voice::set_osc2_kbd_track(bool on) {
  osc2_kbd = on;
  update_base_pitch();
}

void Voice::set_key_shift(int semitones) {
  key_shift = semitones;
  update_base_pitch();
}

void Voice::set_fine_tune_band(int b72) {
  fine12 = fine_cents_from_band(b72);
  update_base_pitch();
}

void Voice::set_osc_mix_band(int b5) {
  osc_mix = (float)b5 * (1.0f / 127.0f);
  recompute_mix();
}

void Voice::set_sub_gain_band(int b95) {
  sub_amp = (float)(b95 * 4) * (1.0f / 127.0f);
  recompute_mix();
}

void Voice::set_sub_shape(int b96) { sub_wave = b96; }

void Voice::set_sub_octave(int b97) {
  sub_octave = (b97 != 0) ? 1 : 0;
  // M2b #22 (m2b-1922-pins 2026-10-11): the device's sub-octave selector is
  // LIVE — the setter FUN_18003e530 (decomp_all.c:38691-38695) is a pure
  // store `*(undefined4 *)(param_1 + 0x148) = param_2;` (no mix recompute:
  // the gains don't depend on subOct, contrast FUN_18003e540 @38702 which
  // does recompute masterNorm for subAmp; param dispatch calls them at
  // decomp_all.c:29424/29406), and every kernel derives the sub mode per
  // render call from the packed pair (CVco+0x140 gsub | CVco+0x148
  // subOct): `fStack_e8 = (float)param_1[0xf]; if (fStack_e8 == 0.0)
  // iStack_e4 = 0; else iStack_e4 = (int)(param_1[0xf] >> 0x20) + 1;`
  // (FUN_180047760 @44269-44275; spec_osc.md:591 "three-way branch inside
  // every kernel"). The engine caches that selector in osc_params.sub_oct
  // (osc_dispatch.cpp:253), so the setter must refresh it exactly as
  // recompute_mix() derives it — gains untouched (pure store, per bytes).
  osc_params.sub_oct = (osc_params.sub_gain == 0.0f) ? 0 : sub_octave + 1;
}

void Voice::set_fm_amount_band(int b45) {
  // FUN_18003e050: CVco+0xa0 = raw band. Per-block target law (FUN_18003bf50):
  // target = ((1000^(amt/127) − 1)/999)·2^26 phase units.
  fm_amt_band = b45;
  fm_update_target();
}

// FUN_18003bf50: gate-scaled FM amount.  fmGate is the groupD sum from
// FUN_18003dbe0: mod-env dest 1 (already scaled to 512) plus LFO dest 6
// (depth_to_range gives 0..512).  The binary clamps the gate to ±512 and
// interpolates the raw amount toward 0 or 127 before the cosine law.
void Voice::fm_update_target() {
  int n = fm_gate;
  if (n > 512) n = 512;
  if (n < -512) n = -512;
  // U5-c′ (m3-u5c-prime): FUN_10034740 @decomp_all_32_full.c:37859 —
  // gate==0 && cv+0x9c==0 ⇒ the target is NOT recomputed (depth holds).
  if (n == 0 && osc_state.fm.last_target == 0) return;
  float amt2;
  if (n > 0) {
    amt2 = (float)fm_amt_band +
           (float)n * (127.0f - (float)fm_amt_band) * (1.0f / 512.0f);
  } else {
    amt2 = (float)fm_amt_band +
           (float)n * (float)fm_amt_band * (1.0f / 512.0f);
  }
  // FUN_18003bf50 @0x18003c33a: log(1000)·x → exp (pow fallback
  // FUN_180057cc0), then (· − 1.0)·DAT_180077368(1/999)·DAT_180077398(2^26):
  //   target = (int)((1000^(amt'/127) − 1)/999 · 2^26)   0 … 2^26
  // (spec_osc.md §6.7's "cos" reading was wrong — DAT_1800733a8 = 1000.0.
  //  Re-tested 2026-10-07: cos gives 038 corr 0.84 but 127/128/126 stft
  //  0.7/0.4 → 2.0/2.2 — the reference FM width matches the 1000^ law.)
  // U5-b2: the 32-bit target (FUN_10034740 asm 0x10034c67-0x10034c85:
  // subsd 1.0; mulsd DAT_10066170; mulsd DAT_100661a0; cvttsd2si) uses the
  // full double 1/999 (.rdata DAT_10066170 = 0.001001001001001001) and
  // 2^26 (DAT_100661a0 = 67108864.0); the old literal 0.001001001 was
  // short by 1e-9 relative (d64 blk1 gate 498: device 55546698).
  const float a = amt2 * (1.0f / 127.0f);
  // #218 (m3-218-fm-idiom): the device writes the idiom exp(log(1000)·x),
  // NOT pow — FUN_10034740 asm 0x10034c67-0x10034c89: FUN_1004beeb (CRT log,
  // 2^52 denormal rescale) ; mulsd ; FUN_1004ca9d (CRT exp, 64·log2e +
  // 1.5·2^52 ni trick); pow (FUN_1004ce0a) is reached only via the isnan
  // fallback. Device-faithful at the x=1.0 boundary: exp(log(1000)) lands
  // 2 ulp BELOW 1000 → tail truncates to 67108863, not 67108864 (7/7
  // discriminating device gates).
  osc_params.fm_depth = (int)((std::exp(std::log(1000.0) * (double)a) - 1.0) *
                              (1.0 / 999.0) * 67108864.0);
  osc_params.fm_active = osc_params.fm_depth != 0;
}

void Voice::set_pulse_width_band(int b8) {
  osc_params.pulse_width = (float)b8 * (1.0f / 127.0f);
}

// ---------------------------------------------------------------------------
// audio — full chain, per sample (dsp_flow.md §2; zero allocations)
// ---------------------------------------------------------------------------
// m3-292-land-2 (ruling tasks/consults/m3-292-land-f7-gate-fail.answer.md
// decision (a); FUN_100345b0 :37627-37637 arms the CVco +0x6c/+0x70
// segment-cut flags iff the LFO is on AND dest ∈ {1 OSC2, 2 OSC12, 5 PW,
// 6 FM}). SCOPE: the DEST_PW (dest 5) consumer ONLY — the dest {1,2,6} legs
// are already lfo_wired_any (engine tick cuts s1/s2) and their mid-tick
// re-read cadence with an ACTIVE mod-env (FUN_1000b3a0 budget walk per
// FUN_10034740 call, FUN_10012be0 reads +0x64/+0x68) is NOT device-certified:
// extending the arm min() to them moved every mod-env-dest-1 (FM) F7 cell
// AWAY (tasks/m4-292-scratch-2 RESULT.md F5; varC attribution
// tasks/m3-292-land). The dest-5 leg is device-certified (m4-292-locate
// fmtap P512/P64 grids; stage join bit-exact). PROVISIONAL gate (M4 item):
// certify the FUN_1000b3a0 budget-walk cadence for mod-env-dest-1 consumers
// with an fmtap locate, then remove the gate or keep it with a citation.
// Local copy: core/voice must not include core/engine.
namespace {
bool cm_cvco_wired(const lfo::LfoState& l) {
  return l.onOff != 0 && l.destination == lfo::DEST_PW;
}
// m3r-111 G11 J1=7/J2=1 (LANDED): FUN_100345b0 :37627-37637 arms the CVco
// +0x6c/+0x70 driver legs; the driver node cuts cover every routed dest
// except PAN (device D012/D103: L1 dest 3 read at 0xb444, L2 dest 4 at
// 0xb496; D005: L1 dest 7 read at 0x13fa4, not by the driver).
bool cm_cvco_routed(const lfo::LfoState& l) {
  return l.onOff != 0 && l.destination >= 1 && l.destination <= 6;
}
}  // namespace

void Voice::begin_block(int n, int amp_cap, int env_cap, int flt_cap) {
  // spec_env.md §2.3 (FUN_18000ca70 / FUN_18000ccc0): every enveloped device
  // (CVca, CVcf, CVco) walks the render block on its OWN chunk schedule —
  // chunks never span blocks (each is capped by the block remainder) and
  // each chunk's level is evaluated at the chunk end. CVca ramps its gain
  // linearly across the chunk from the previous accumulated gain (+0x90,
  // FUN_180037c30); CVcf/CVco hold their chunk level (stair-step).
  //
  // law #90: the CVca chunk is NOT cut by the engine's 32-frame control tick.
  // The device cuts the gain-apply call (FUN_18000ccc0 span = min(+0x6c,
  // call-rem, stage-rem)) only at the envelope's own ladder boundary, the
  // render-chunk remainder (block edge + modulation-event distances,
  // FUN_18000ca70) and the stage end — the 0x20 cap of FUN_1800285a0 is dead
  // (+0xac90 statically 0, no setter; reports/90_ac90_settle.md). amp_cap_ =
  // frames from this call's first sample to the next such device cut; the
  // in-progress CVca chunk CARRIES across control ticks (amp_ch_rem_ is not
  // reset here). law #95b: CVcf carries too — the device cuts the CVcf env
  // walk (FUN_1000b580 span = min(+0x60 chunk, call-rem, stage-rem),
  // decomp_all_32_full.c:8296-8305) only at its ladder boundary (+0x60,
  // FUN_1000b090 — the SAME adaptive ladder as CVca: 017 CVcf device dump
  // chunks 166/249/374 = rate/9,/6,/4, marks 8542/17085/42713 = dur/20,/10,
  // /4; release 14/22 = rate/9,/6, marks 297/595 = dur/20,/10) and the
  // render-call remainder — NOT the 32-frame control tick (the 0x20 cap is
  // dead, +0xac90==0, reports/90_ac90_settle.md). law #121: CVco/mod env
  // carries too — the CVco env walk shares the SAME law-90/95b grid
  // (FUN_1000b3a0 driver :8131 budget = min(param_1, 0x100) call, walk
  // FUN_1000b580 span = min(+0x60 chunk, call-rem, stage-rem), re-rounds at
  // every call cut :8390; the CVco ladder is the shared FUN_1000b090 —
  // 062 dumpvoice chunks 20/80/160 = rate/4,rate,rate*2, marks 803/1071/
  // 1607 = D/4,/3,/2; reports/121_cvco_settle.md F2/F5). mod_ch_rem_ is NOT
  // reset here; the 32-tick cut is removed.
  blk_len_ = n;
  blk_i_ = 0;
  // law #82: amp_cap_ is the CVca GAIN-call grid — block-anchored
  // min(host-block-rem, 0x100) (FUN_1800155d0 :14113-14116) + range end +
  // segment cuts of AMP-WIRED LFOs only (FUN_18000ca70 param_1[0xe]/[0xf]);
  // NO wave-wrap cuts. env_cap_ is the CVcf/CVco env-walk grid (law-123:
  // 256 + wave-wrap + segment cuts kept, reports/82_sinedemod.md §4(ii)).
  amp_cap_ = (amp_cap > 0) ? amp_cap : n;
  env_cap_ = (env_cap > 0) ? env_cap : n;
  // law #172: the CVcf env-walk CALL grid (destination-gated, flt_call_cap172);
  // standalone callers (flt_cap <= 0) keep the env_cap_ grid.
  flt_cap_ = (flt_cap > 0) ? flt_cap : env_cap_;
}

// Start the next chunk of each envelope whose current chunk is exhausted.
void Voice::step_env_chunks() {
   // law #90 (FUN_18000ccc0 LAB_18000ced3 + FUN_180037c30): the CVca chunk
   // length comes from the adaptive ladder (min(chunk_len, stage-rem)) and is
   // NOT capped by the control tick; the gain-apply CALL span is
   // min(chunk-remaining, call-remaining) where call-remaining = distance
   // to the next device amp cut (amp_cap_). Law #82: that grid is the
   // BLOCK-ANCHORED min(host-block-rem, 0x100) (FUN_1800155d0
   // :14113-14116) + range end + segment cuts of AMP-WIRED LFOs only
   // (FUN_18000ca70 param_1[0xe]/[0xf] + flags +0x80/+0x84) — the
   // wave-wrap/foreign-LFO cuts of the law-90-era single cap are REMOVED
   // from this leg (reports/82_sinedemod.md §3/§4); the 0x100 cap itself
   // stands (law #123; device bs512==bs256 62-64 Hz floors, gaingrid-2
    // F3). m3-amp-restart (ruling
    // tasks/consults/m3-amp-restart-scratch.answer.md): a chunk truncated by
    // a call cut does NOT carry its remainder — the walk loop head
    // (FUN_1000b580 LAB_1000b7ad, 0x1000b815-0x1000b82c: mov 0x60(%ebx) =
    // FULL ladder chunk, cmovl min with the call budget, cmovl min with
    // stage rem) RE-ARMS the FULL ladder chunk on entry to every walk-call
    // iteration (m3-258 law, the same bytes the mod-env per_call_restart
    // mirrors); the lerp re-targets to the level at each call's end
    // (step = (+0x94 − +0x90)/span, +0x90 carried).
  if (amp_ch_rem_ == 0 || amp_call_rem_ == 0) {
    // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A;
    // FUN_10031c70 ramp lanes): device CVca walk-call grid — voice-anchored
    // 0x100 cap, re-anchored at block_start+1 (m3-sse-lane-scratch F1b,
    // device AT2 buf law).
    if (block_size_ > 0) {
      while ((std::int64_t)walk_start_rel_ + (std::int64_t)walk_len_rel_ <=
             (std::int64_t)rendered_) {
        walk_start_rel_ += walk_len_rel_;
      }
      const std::int64_t wa = base_abs_ + (std::int64_t)walk_start_rel_;
      const std::int64_t bend1 =
          (wa & ~((std::int64_t)block_size_ - 1)) + (std::int64_t)block_size_ + 1;
      std::int64_t wl = bend1 - wa;
      if (wl > 256) wl = 256;
      if (wl < 1) wl = 1;
      walk_len_rel_ = (int)wl;
    }
    int cap = amp_cap_ - blk_i_;
    if (cap < 1) cap = 1;
    int span;
    if (amp_env.entry_guard()) {
      // #171: the device loop-head entry guard (FUN_1000b580 :8352) breaks
      // the chunk loop BEFORE any step; the walk then consumes the FULL
      // call remainder in one terminal-level gain-apply (walk tail
      // :8470-8485 `if (local_3c < 1) return;` → apply(level, local_3c)).
      // No chunk boundary exists after the break, so the end-of-release
      // hook lands on the call cut (off+256 on the event-anchored L1 grid).
      span = cap;
      amp_ch_rem_ = span;
    } else {
      // m3-amp-restart: re-arm the FULL ladder chunk at every walk-call
      // boundary (FUN_1000b580 0x1000b815-0x1000b82c; ruling
      // tasks/consults/m3-amp-restart-scratch.answer.md) — no carried
      // remainder across the amp_cap_ call cut.
      const bool restart =
          amp_call_rem_ == 0 && amp_env.stage != env::STAGE_IDLE;
      if (amp_ch_rem_ == 0 || restart) {
        int cl = amp_env.next_chunk_len(0x7fffffff);  // ladder chunk, stage-rem
        if (cl < 1) cl = 1;
        amp_ch_rem_ = cl;
        amp_trace[amp_trace_n % AMP_TRACE_N] = cl;
        ++amp_trace_n;
      }
      span = (amp_ch_rem_ < cap) ? amp_ch_rem_ : cap;
    }
    amp_span_trace[amp_span_n % AMP_TRACE_N] = span;
    ++amp_span_n;
    const int amp = amp_env.advance_chunk(span);
    const float amp_mod = (2048.0f - (float)lfo_amp) * (1.0f / 2048.0f);
    const float g_target = ((float)amp * (1.0f / 2048.0f)) *
                           amp_gain * vel_factor * amp_mod;
    // FUN_10031c70 entry guard (m3-release-timing-locate F2/F3, ruling
    // m3-release-timing-locate.answer.md A1): inputs unchanged from the last
    // gain-apply call AND that call's cached target == 0.0f => the device
    // returns 1 and the walker zero-fills the whole call (FUN_1004bcd0):
    // exact-0 output from the call head, no post-zero ramp (the +-e-10
    // residue of the level-zero call's lerp is overwritten).
    amp_silent_call_ = (amp == amp_cache_lvl_ && lfo_amp == amp_cache_mod_ &&
                        amp_cache_target_ == 0.0f);
    amp_guard_hit_ = (amp == amp_cache_lvl_ && lfo_amp == amp_cache_mod_);
    amp_cache_lvl_ = amp;
    amp_cache_mod_ = lfo_amp;
    amp_cache_target_ = g_target;
    g_step_ = (g_target - g_cur_) / (float)span;
    amp_call_rem_ = span;
    // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A;
    // FUN_10031c70): per-call per-sample gains with the device accumulation
    // law (m3-sse-lane-scratch F1: prologue addss to the 16B line,
    // L1/L2/L3/L0 lanes + s4, carry lane3, sequential tail groups, scalar
    // tail; CONST = const target carry frozen, RET1 = silent carry frozen).
    if (span <= 256) {
      int n = 0;
      if (amp_silent_call_) {
      } else if (amp_guard_hit_) {
        for (int j = 0; j < span; ++j) sse_gain_[n++] = g_target;
      } else {
        float gg = g_cur_;
        const float s = g_step_;
        int i = 0;
        const int i0 = static_cast<int>((4 - ((static_cast<int>(rendered_) - walk_start_rel_) & 3)) & 3);
        if (span > 6) {
          while (i < i0) { gg += s; sse_gain_[n++] = gg; ++i; }
          float L3 = gg - s, L2 = L3 - s, L1 = L2 - s, L0 = gg;
          const float s4 = s * 4.0f;
          for (; i < span - 3; i += 4) {
            L1 += s4; L2 += s4; L3 += s4; L0 += s4;
            sse_gain_[n++] = L1; sse_gain_[n++] = L2;
            sse_gain_[n++] = L3; sse_gain_[n++] = L0;
          }
          gg = L0;
        }
        if (span - i > 3) {
          int n4 = (((span - i) - 4) >> 2) + 1;
          i += n4 * 4;
          do {
            const float g1 = gg + s, g2 = g1 + s, g3 = g2 + s;
            gg = g3 + s;
            sse_gain_[n++] = g1; sse_gain_[n++] = g2;
            sse_gain_[n++] = g3; sse_gain_[n++] = gg;
          } while (--n4);
        }
        for (; i < span; ++i) { gg += s; sse_gain_[n++] = gg; }
        sse_gain_last_ = gg;
      }
      sse_gain_n_ = n;
      sse_gain_i_ = 0;
    }
  }
  // law #172 (tasks/consults/m3-172-cvcf-level-law.answer.md +
  // m3-172-cvcf-level-gate4-stop.answer.md §2; FUN_1000b580 walk :8301/8366-
  // 8390, driver FUN_1000b3a0 :8160-8193): per env-walk CALL the walk loops
  // { span = min(FULL chunk re-armed this iteration, call_rem, stage_rem);
  // level evaluated at the span end; call_rem -= span } until call_rem == 0.
  // NO in-chunk remainder is carried across calls (overrides the law-95b-era
  // flt_ch_rem_ carry), NO 1-frame re-evaluation (cap < 1 = loop-head break,
  // FUN_1000b580 :8301 `local_3c < 1`). The CALL grid is destination-gated
  // (flt_cap_ = flt_call_cap172): block-rem + 256 + range end + segment cuts
  // of FILTER-WIRED LFOs only (+0x6c/+0x70 set by FUN_10032280 iff dest == 3;
  // device -envtap: 083/062 CVcf calls {0,256} per block, 121's filter-wired
  // LFO2 cuts at its segment distances). The CVco leg below keeps the full
  // law-123 env_cap_ grid (law-121 062 wrap cuts were CVco-wired).
  if (flt_ch_rem_ == 0 || flt_call_rem_ == 0) {
    if (flt_call_rem_ == 0) {
      const int cap = flt_cap_ - blk_i_;
      if (cap < 1) {
        // device loop-head break (FUN_1000b580:8301 `local_3c < 1`)
        static const bool envtap = std::getenv("SYNTH1_ENVTAP") != nullptr;  // #235
        if (envtap) {
          std::fprintf(stderr, "[envtap] blk %lld soff %d BREAK cap %d prog %d cur %d\n",
                       (long long)(rendered_ / 512), (int)(rendered_ % 512),
                       cap, filter_env.progress, filter_env.cur_level);
        }
      } else {
        static const bool envtap = std::getenv("SYNTH1_ENVTAP") != nullptr;  // #235
        if (envtap) {
          std::fprintf(stderr, "[envtap] blk %lld soff %d p1 %d prog %d cur %d\n",
                       (long long)(rendered_ / 512), (int)(rendered_ % 512),
                       cap, filter_env.progress, filter_env.cur_level);
        }
        flt_call_rem_ = cap;
        // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md
        // A; FUN_10033740 soft-clip group forms): CVcf walk-call grid —
        // call length + sample position for the filter's per-call lane form.
        sc_len_ = cap;
        sc_pos_ = 0;
        // m3-272 (m3-272-scratch.answer.md): CVcf driver-segment head —
        // re-read the FILTER-routed LFO sum ONLY at the new call
        // (FUN_1000b3a0 :8129-8228 reads the filter-wired FUN_10012be0
        // values once per budget segment; FUN_100338f0 param_4+param_5
        // constant across the call's chunk steps). A CVco-only LFO
        // contributes 0 -> the filter inputs are never re-sampled on the
        // CVco consumer's grid.
        if (cm_lfo1_ != nullptr || cm_lfo2_ != nullptr) cm_reread(2);
      }
    }
    if (flt_call_rem_ > 0) {
      const int call_rem = flt_call_rem_;
      int cl = filter_env.next_chunk_len(0x7fffffff);  // FULL chunk re-armed
      if (cl < 1) cl = 1;                             // every walk step
      const int span = (cl < call_rem) ? cl : call_rem;
      blk_flt_lvl_ = filter_env.advance_chunk(span);
      flt_trace_span[flt_trace_n % FLT_TRACE_N] = span;
      flt_trace_lvl[flt_trace_n % FLT_TRACE_N] = blk_flt_lvl_;
      ++flt_trace_n;
      flt_ch_rem_ = span;
      // flt_call_rem_ = CALL remainder: counts down per sample, never
      // re-set at a chunk boundary.
    }
  }
  // law #121 (reports/121_cvco_settle.md F5) as corrected by m3-258
  // (tasks/m3-258-cutset): the CVco mod-env walk shares the law-90/95b
  // grid — the env-walk CALL span is min(chunk-rem, call-rem) with the
  // same device call cuts as CVca/CVcf (block remainder + 256
  // voice-processor cap + wave-3/4 wrap + CVco-ROUTED LFO segment cuts +
  // range end = env_cap_), and the rounded level is re-evaluated at EVERY
  // cut (FUN_1000b580 :8390). The in-progress chunk RE-ARMS at every call
  // (0x1000b815-2c, mode-independent — the law-#121 "carry" reading came
  // from the broken cut set, see the restart note below). 062 dumpvoice
  // block edges: lvl 10@prog 490, 2@1002, 0@done; ladder chunks
  // 20/80/160, marks 803/1071/1607.
  // m3-bendslew-lfo-land (ruling tasks/consults/m3-bendslew-lfo-scratch.
  // answer.md): the CVco driver budget ALSO ends at the head of every device
  // render-piece, so cm_reread fires at every piece head as FUN_1000b3a0
  // does — the device re-enters the driver per render-loop piece:
  // FUN_10024ea0 :28511-28515 caps the piece at 0x20 while +0xac90 is armed
  // (FUN_10024c20 @28374 wheel step), FUN_100135b0 :14539-14542 budget =
  // min(piece, 0x100), driver head re-read FUN_1000b3a0 :8168-8193. The
  // piece grid is engine-side (engine.cpp piece_anchor_ chain, piece-capped
  // arm below). Inside ONE piece the value stays held across the consumer's
  // own segment (#272 law: the walk FUN_1000b580 passes the SAME modA/modB
  // per chunk step) — the segment-edge arms below are unchanged.
  const bool cm_piece_head = blk_i_ == 0 && cm_piece_head_;
  // m3r-111 G11 J1=7 (LANDED): the CVco DRIVER step (cm_call_rem_,
  // FUN_1000b3a0 two-level budget) is the master grid; the mod-env walk
  // chunks follow it (FUN_1000b580 call budget = iStack_20) and re-arm at
  // the ladder-chunk head OR call head (device D103 re-arms the walk per
  // ladder chunk, FM soff 0,1,2,...).
  // The FUN_1000b3a0 call budget is the DEVICE render piece
  // (min(piece,0x100) chained from piece_anchor_, cm_chunk_end_), NOT the
  // engine control-tick remainder (tl): device D041 block 0 walks
  // 256,244,12 (piece+L2 cuts) with the tick on the 32 unwired read grid;
  // D050 walks 178,78,129,127 (single outer LFO re-read per iteration).
  // GATE: while the mod-env is active the walk FUN_1000b580 takes the
  // attack/stage branch (:8280-8295) and steps per-sample (D103 block 0:
  // FM p6=1, p4=16,33,49,...) — the driver budget does NOT gate the walk;
  // the eng111g arm+cap law stands there.
  const bool modenv_live = mod_env_on && mod_env.stage != env::STAGE_IDLE;
  // m3r-111-porta (LANDED): while a LEGATO-RETARGET glide is active the
  // device CVco walk span is the porta bucket edge (FUN_10036730
  // :39086-39137 step=max(0x20,..); FUN_10036250 :38972-38977 sub-chunk =
  // min(param_3,(bucket+1)*step-pos)) — the two-level FUN_1000b3a0 budget
  // does NOT drive the walk; the live arm+cap law stands (fam077/078
  // mel12g0: J1=7 alone -86.74 -> -23.73). Fresh-attack glides keep J1.
  // m3r-062 (LANDED): the mod-env gate on the J1 driver budget applies
  // ONLY when no CVco LFO has a DEST_PW leg (device D062: with a PW leg
  // wired the two-level FUN_1000b3a0 budget still drives the walk while
  // the mod-env walks; the piece-head reset is KEPT for the PW leg —
  // device D062 fresh L1 reads at the 256 head and the note-off piece
  // head 17640).
  const bool pw_leg_top =
      (cm_lfo1_ != nullptr && cm_cvco_wired(*cm_lfo1_)) ||
      (cm_lfo2_ != nullptr && cm_cvco_wired(*cm_lfo2_));
  const bool j1on = !(modenv_live && !pw_leg_top) && !glide_active_;
  const bool anyrouted_top =
      (cm_lfo1_ != nullptr && cm_cvco_routed(*cm_lfo1_)) ||
      (cm_lfo2_ != nullptr && cm_cvco_routed(*cm_lfo2_));
  if ((mod_ch_rem_ == 0 || mod_call_rem_ == 0) || cm_piece_head) {
    // m3-272 (m3-272-scratch.answer.md): CVco driver-segment head.
    // FUN_1000b3a0 :8129-8228 reads the two FUN_10012be0 values ONCE per
    // budget segment at this+0x64/+0x68 (budget cut at the CVco-ROUTED
    // LFO segment edges, minSegment-floored by FUN_10012c00, + the
    // 256/block call cuts = the env_cap_ grid) and the walk FUN_1000b580
    // passes the SAME modA/modB to the consumer for every chunk step
    // inside the segment. mod_call_rem_ is the STEP remainder
    // (min(chunk, cap)) — the true call remainder is cm_call_rem_.
    {
      // m3-292-land-2 (ruling tasks/consults/m3-292-land-f7-gate-fail.answer.md
      // decision (a)): the CVco consumer re-arms on its OWN sub-chunk grid —
      // the FUN_1000b3a0 :8129-8228 budget walk cuts at the CVco-ROUTED LFO's
      // segment edges, floored at minSegment by FUN_10012c00 LAB_10012e2f
      // :13998-14005 (*param_1 = uVar7 - param_2; if (< +0x28) *param_1 =
      // +0x28), ∩ the tick remainder (block remainder + the 0x100
      // voice-processor cap + range end, FUN_100135b0 :14539-14542 /
      // FUN_10024ea0 :28536-28545). The pre-land arm is env_cap_ - blk_i_,
      // which is exactly 0 at a mid-tick CVco step head (e.g. 090 abs 141 at
      // bs512) → arm 1 → cm_reread(1) EVERY sample to the tick end. Seed at
      // the CALL remainder (tick tail, the voice-process call this head runs
      // inside), NOT at ccap: at a head that sits ON the env-cap cut (090 abs
      // 141; a chunk-cap w1/w2 cut mid-tick) ccap is ≤ 0 and a ccap-seeded
      // min() collapses to 1 = the per-sample re-read the ruling forbids.
      // The next budget-segment head is the next grid cut: the DEST_PW
      // consumer's floored segment edge (consumer_cut, minSegment inside per
      // FUN_10012c00 LAB_10012e2f :13998-14005) ∩ the call tail. PROVISIONAL
      // gate (M4): the re-arm applies ONLY when a DEST_PW leg is wired;
      // otherwise the live ccap arm verbatim (the mod-env-dest-1 mid-tick
      // cadence is uncertified — varC is the sole FM regressor without it).
      const int ccap = env_cap_ - blk_i_;
      // m3-bendslew-lfo-land: the device walker's budget ends at the
      // voice-process call (FUN_1000b3a0 re-entered per FUN_10024ea0 piece)
      // — end the cm budget at the piece head too, so the head re-read
      // fires every piece.
      const int chrem = (cm_chunk_end_ > 0) ? (int)(cm_chunk_end_ - (cm_abs_base_ + blk_i_)) : 0x7fffffff;
      if (cm_piece_head) { cm_call_rem_ = 0; cm_outer_rem_ = 0; }
      if (cm_call_rem_ == 0) {
        if (cm_lfo1_ != nullptr || cm_lfo2_ != nullptr) cm_reread(1);
        const bool pw_leg =
            (cm_lfo1_ != nullptr && cm_cvco_wired(*cm_lfo1_)) ||
            (cm_lfo2_ != nullptr && cm_cvco_wired(*cm_lfo2_));
        // Two-level budget only when a driver-cut LFO exists (device
        // D127-class LFO-off cells keep the eng111g env_cap_ arm).
        if (j1on && anyrouted_top) {
          // m3r-111 G11 J1=7 (LANDED):
          // FUN_1000b3a0 :8168-8222 is a TWO-LEVEL budget for the
          // CVco-ROUTED LFOs (FUN_100345b0 :37627-37637 arms +0x6c/+0x70
          // iff dest ∈ {1,2,5,6}). The LFO1 cut iStack_1c is read ONCE per
          // outer iteration at the iteration head (caller 0xb444), pinned
          // to min(piece-rem local_8, L1cut) for the whole inner loop; the
          // LFO2 cut iStack_20 is re-read at every inner step head (caller
          // 0xb496); the walk step = min(outer-rem, L2cut). The
          // FUN_10012c00 minSegment floor applies only to the freshly-read
          // segLen — a held outer residual is NOT re-floored (device
          // lfocut D111s: L1 @4352 cut=66, inner 64+2, L1 value re-read
          // at 4418; block 0: outer min(256,373)=256, inner 73,73,73,37).
          const int prem = (chrem > 0) ? chrem : 0x7fffffff;
          int nrem = (prem > 0) ? prem : 1;
          if (prem < nrem) nrem = prem;
          const std::int32_t coff =
              (std::int32_t)(cm_abs_base_ + blk_i_ - cm_chunk_start_);
          // m3r-111-g11 (121 fix): mirror the law-#123 wave-3/4 period-wrap
          // caps w1/w2 = lfo::chunk_cap - off (engine.cpp:735-736, 767;
          // reports/123_lfo_grid_settle.md §4(ii) "keep wrap cuts for
          // CVco/CVcf") into the J1=7 step base. J1=7's piece-rem base
          // dropped them, moving 121's law-#123 release-tail 256-cut grid
          // (device tail 114432 = 447*256).
          if (cm_lfo1_ != nullptr) {
            const std::int32_t w = lfo::chunk_cap(*cm_lfo1_) - coff;
            if (w < nrem) nrem = w;
          }
          if (cm_lfo2_ != nullptr) {
            const std::int32_t w = lfo::chunk_cap(*cm_lfo2_) - coff;
            if (w < nrem) nrem = w;
          }
          if (cm_outer_rem_ <= 0) {
            int c1 = 0x7fffffff;
            if (cm_lfo1_ != nullptr && cm_cvco_routed(*cm_lfo1_))
              c1 = lfo::consumer_cut(*cm_lfo1_, coff);
            cm_outer_rem_ = (c1 < prem) ? c1 : prem;
            if (cm_outer_rem_ < 1) cm_outer_rem_ = 1;
          }
          if (cm_outer_rem_ < nrem) nrem = cm_outer_rem_;
          if (cm_lfo2_ != nullptr && cm_cvco_routed(*cm_lfo2_)) {
            const int c = lfo::consumer_cut(*cm_lfo2_, coff);
            if (c < nrem) nrem = c;
          }
          cm_call_rem_ = (nrem > 0) ? nrem : 1;
          cm_outer_rem_ -= cm_call_rem_;
          mod_call_rem_ = 0;  // walk chunk follows the driver step head
        } else if (!pw_leg) {
          cm_call_rem_ = (ccap > 0) ? ccap : 1;
          if (chrem < cm_call_rem_) cm_call_rem_ = chrem > 0 ? chrem : 1;
        } else {
          const int tl = blk_len_ - blk_i_;  // tick remainder (the call tail)
          int nrem = (tl > 0) ? tl : 1;
          if (ccap > 0 && ccap < nrem) nrem = ccap;
          if (chrem < nrem) nrem = chrem;
          const std::int32_t coff =
              (std::int32_t)(cm_abs_base_ + blk_i_ - cm_chunk_start_);
          if (cm_lfo1_ != nullptr && cm_cvco_wired(*cm_lfo1_)) {
            const int c = lfo::consumer_cut(*cm_lfo1_, coff);
            if (c < nrem) nrem = c;
          }
          if (cm_lfo2_ != nullptr && cm_cvco_wired(*cm_lfo2_)) {
            const int c = lfo::consumer_cut(*cm_lfo2_, coff);
            if (c < nrem) nrem = c;
          }
          cm_call_rem_ = (nrem > 0) ? nrem : 1;
        }
      }
    }
  }
  if (mod_ch_rem_ == 0 || mod_call_rem_ == 0) {
    // m3-fm-walk-land rule A, scope (b) + m3-258 (#258 CLOSED,
    // tasks/m3-258-cutset): the device bytes recompute the chunk on ENTRY
    // to EVERY walk call — chunk = min(FULL +0x60, call budget, stage rem),
    // no carried remainder (0x1000b815-0x1000b82c: +0x60 read once at
    // 0x1000b815, min with the call budget, min with stage rem; +0x60 is
    // written ONLY by FUN_1000b090 at stage transitions :8355 and
    // ladder-mark crossings :8418 / cmp 0x5c(%ebx) @0x1000b963 — mark
    // progression and value formulas untouched). The chunk select is
    // MODE-INDEPENDENT (0x1000b815-2c reads no decay/release mode), so the
    // per-call restart applies in EVERY stage. The #258 "exponential-decay
    // carry" exception is RESOLVED: it was a CUT-SET defect (the CVco env
    // grid missed the CVco-routed LFO1 segment cuts and extra-cut at
    // absolute-256 edges), harmless under carry and fatal under restart —
    // with the grid fixed (engine.cpp env cap, m3-258) the unconditional
    // restart holds the 062 dumpvoice block edges (prog 490/1002, lvl
    // 10/2, chunks 20/80/160, marks 803/1071/1607, done 1514) and the 062
    // audio null at −35.74 (bar −35.24).
    // m3r-111 G11 J1=7 (LANDED): FUN_1000b580 call budget = the driver
    // step remainder (cm_call_rem_), not the tick env_cap_ grid.
    const int mcap = (j1on && anyrouted_top)
                         ? ((cm_call_rem_ > 0) ? cm_call_rem_ : 1)
                         : (env_cap_ - blk_i_);
    const bool per_call_restart =
        mod_env_on && mod_env.stage != env::STAGE_IDLE;
    if (mod_ch_rem_ == 0 || per_call_restart) {
      if (mod_env_on && mod_env.stage != env::STAGE_IDLE) {
        int cl = mod_env.next_chunk_len(0x7fffffff);  // ladder chunk, stage-rem
        if (cl < 1) cl = 1;
        mod_ch_rem_ = cl;
      } else {
        int cap = mcap;  // law #82: law-123 grid (256 cut kept)
        mod_ch_rem_ = (cap > 0) ? cap : 1;  // idle/off: level 0 to next cut
      }
    }
    int cap = mcap;  // law #82: law-123 grid (256 cut kept)
    const int span = (mod_ch_rem_ < cap) ? mod_ch_rem_ : cap;
    if (mod_env_on && mod_env.stage != env::STAGE_IDLE) {
      blk_mod_lvl_ = mod_env.advance_chunk(span);
      mod_trace_span[mod_trace_n % MOD_TRACE_N] = span;
      mod_trace_lvl[mod_trace_n % MOD_TRACE_N] = blk_mod_lvl_;
      ++mod_trace_n;
    } else {
      blk_mod_lvl_ = 0;
    }
    mod_call_rem_ = span;
    osc_state.fm.call_start = span;  // rule B: one device FUN_10034740 call per CVco span
  }
}

// m3-272 (tasks/consults/m3-272-scratch.answer.md): re-read the
// consumer-routed LFO sums at THIS consumer's own env-walk steps. The
// device supplies the routed LFO values to each consumer at the
// consumer's walk cuts: FUN_1000b3a0 :8129-8228 (CVco/CVcf env driver,
// decomp_all_32_full.c) cuts the walk budget at the segment edges of the
// CONSUMER-ROUTED LFOs via the local phase copy FUN_10012be0 (the
// FUN_10012c00 minSegment floor applies to the cut: at bs64 the device
// p4 changes at 1664 = the 64-floored cut, NOT the raw LFO2 edge 1657),
// and the consumer (CVco FM sum FUN_10036250 :38890-38900 per env chunk,
// CVco call FUN_10034740 :37858-37888 per sub-chunk; CVcf FUN_100338f0
// param_4+param_5 per call) receives the value read there. The engine's
// voice walk (step_env_chunks, law #121/#172/#258 grids) already
// reproduces the device's per-consumer step grid, so the re-read belongs
// at the walk step heads — NOT at the shared control-tick starts (cut by
// ANY-wired LFO segments) and NOT at raw unfloored LFO edges. An LFO
// routed only to another consumer contributes 0 to these sums.
void Voice::cm_reread(int which) {  // bit0 CVco dests, bit1 CVcf dest
  const std::int32_t off =
      (std::int32_t)(cm_abs_base_ + blk_i_ - cm_chunk_start_);
  int pitch = 0, pitch2 = 0, pw = 0, fm = 0, cutoff = 0;
  const lfo::LfoState* ls[2] = {cm_lfo1_, cm_lfo2_};
  for (const lfo::LfoState* l : ls) {
    if (l == nullptr || l->onOff == 0) continue;
    int seg = 0;
    const int v = lfo::read_at(*l, off, &seg);
    switch (l->destination) {
      case lfo::DEST_OSC2:   if (which & 1) pitch2 += v; break;
      case lfo::DEST_OSC12:  if (which & 1) pitch  += v; break;
      case lfo::DEST_FILTER: if (which & 2) cutoff += v; break;
      case lfo::DEST_PW:     if (which & 1) pw     += v; break;
      case lfo::DEST_FM:     if (which & 1) fm     += v; break;
      default: break;  // dest 4 amp: CVca grid (tick already cut by
                       // tick_amp_cap_); dest 7 pan: not per-voice.
    }
  }
  if (which & 1) {
    lfo_pitch = pitch; lfo_pitch2 = pitch2; lfo_pw = pw; lfo_fm = fm;
  }
  if (which & 2) lfo_cutoff = cutoff;
}

float Voice::process_full_sample() {
  if (blk_len_ == 0) begin_block(1, 1, 1, 1);
  step_env_chunks();
  const int flt_lvl = blk_flt_lvl_;
  const int mod_lvl = blk_mod_lvl_;

  // ---- FM gate (FUN_18003dbe0 groupD → FUN_18003bf50 target) ----
  // U5-b (m3-u4u5-law-scope §U5-b): the 32-bit consumer FUN_10036250
  // (decomp_all_32_full.c:38835) is called by the env walker PER ENV CHUNK
  // and builds the FM sum (env dest 1 + LFO A/B dest 6, :38890-38900); it
  // hands it per sub-chunk (env chunk ∩ LFO seg ∩ porta seg ∩ call cut,
  // FUN_10034610) to FUN_10034740, which recomputes the FM target +0x90 on
  // EVERY call (:37858-37888) and runs the ramp machine (+0x8c/+0x94/
  // +0x98/+0x9c/+0xa4/+0xa8, :37889-37925). The sums are constant inside a
  // sub-chunk, so re-evaluating here every sample and acting on a change is
  // the same cadence. (Replaces the once-per-block LEGION hunk-3 latch.)
  {
    int gate = lfo_fm;
    // param 71 dest 1 = FM gate sum (FUN_18003dbe0:38254-38255:
    // CVco+0x184 == 1 → param_2 into the groupD gate sum).
    if (mod_env_on && mod_dest == 1) gate += mod_env_sign * mod_lvl;
    if (gate != last_fm_gate_) {
      last_fm_gate_ = gate;
      fm_gate = gate;
      fm_update_target();
    }
  }

  // ---- pitch modulation (FUN_18003dbe0 routing, FUN_18003b940 apply) ----
  // osc1 sum = bend (CVco+0x90 = voice+0xa8) + porta + LFO dest 2
  // osc2 sum = (kbd ? bend + porta : 0) + LFO dest 1/2 + mod env dest 0
  // (spec_osc.md §4.5, spec_env.md §5.5: the mod env pitch is osc2-only).
  int m1 = pitch_bend + porta_offset + lfo_pitch;
  int m2 = (osc2_kbd ? pitch_bend + porta_offset : 0) + lfo_pitch + lfo_pitch2;
  // param 71 dest 0 = osc2 pitch sum (FUN_18003dbe0:38235-38238:
  // CVco+0x184 == 0 → param_2 into the osc2 pitch sum uVar12; the osc1 sum
  // uVar10 at 38221 never receives param_2 → mod-env is osc2-only).
  if (mod_env_on && mod_dest == 0) m2 += mod_env_sign * mod_lvl;  // scale 640 = ±5 oct max
  m1 = std::clamp(m1, -PITCH_MOD_CLAMP, PITCH_MOD_CLAMP);
  m2 = std::clamp(m2, -PITCH_MOD_CLAMP, PITCH_MOD_CLAMP);
  if (m1 != cur_pitch_mod) {
    cur_pitch_mod = m1;
    // m3-166-land: device FUN_10034740 :37757-37800 — f = float(ratio·+0x7c)
    // (ratio per FUN_10035290 two-table law), guard DAT_100661b0 = 22000.0
    // via :37764 int-bits test, inc = ROUND(float(f·2^27f)·(1/sr double)).
    float f1 = spdev::ratio(m1) * osc1_hz;
    if (spdev::guard(f1)) f1 = 22000.0f;
    cur_f1 = f1;
    phase_inc = spdev::inc_osc(cur_f1, sample_rate);
    // Law K: the dispatcher recomputes the 8 detune frame incs from the
    // same pitch delta (FUN_10034740 :37818-37848: octave+frac transpose by
    // param_1, clamp DAT_100661b0 = 22000.0, inc = (int)ROUND(hz·2^27·invSR);
    // m3-166-land: x87 single-round oct·fine·det per :37826-37831).
    if (0.0f < osc_params.detune) {
      for (int j = 0; j < 8; ++j) {
        float dh = spdev::det_scaled(m1, det_hz[j]);
        if (spdev::guard(dh)) dh = 22000.0f;
        det_inc[j] = spdev::inc_det(dh, sample_rate);
      }
    }
  }
  if (m2 != cur_pitch_mod2) {
    cur_pitch_mod2 = m2;
    float f2 = spdev::ratio(m2) * osc2_hz;
    if (spdev::guard(f2)) f2 = 22000.0f;
    cur_f2 = f2;
    phase_inc2 = spdev::inc_osc(cur_f2, sample_rate);
  }

  // ---- pulse-width modulation sum (FUN_18003dbe0 groupC): mod env dest 2
  // (scale 128) + LFO dest 5, in 1/128 units (spec_env.md §5.5) ----
  int pw = lfo_pw;
  // param 71 dest 2 = PW sum (FUN_18003dbe0:38241-38242: CVco+0x184 == 2 →
  // param_2 into the groupC pulse-width sum; scale 0x80 = 128).
  if (mod_env_on && mod_dest == 2) pw += mod_env_sign * mod_lvl;
  osc_params.pw_mod = pw;

  // ---- cutoff modulation (FUN_18003ad50) ----
  // The filter env level is the dispatcher's edx extra index; the LFO
  // cutoff sum is the modA/modB pair, mirror-folded across the remaining
  // headroom: i += ((m > 0 ? 1024 − i : i)·m) >> 10.
  int extra = filter_env_sign * flt_lvl;  // level already scaled by |amount|
  if (mod_env_on && mod_env_cutoff_amount != 0.0f) {
    extra += (int)(mod_env_cutoff_amount * (float)(mod_env_sign * mod_lvl));
  }
  filt.extra_idx = extra;
  filt.mirror_a = lfo_cutoff;
  filt.mirror_b = 0;

  // ---- oscillator (CVco → CVcf link) ----
  // m3-wavetable-band-land: the device rebuilds the kernel arg frame on
  // EVERY call (FUN_10034740 frame build, decomp_all_32_full.c:37784-37792):
  // the osc1/osc2/sub table pointers are re-selected per sample from the
  // MODULATED frequencies via FUN_100352f0 — not once per block. osc2's one
  // engine table feeds both the FM v2 read and the audible osc2 term, which
  // is the device's arg[15] (both read from arg[15] in FUN_100424b0).
  {
    const tables::WavetableBank& bank = tables::global_bank();
    const float f1 = (cur_f1 > 0.0f) ? cur_f1 : osc1_hz;
    const float f2 = (cur_f2 > 0.0f) ? cur_f2 : osc2_hz;
    osc1_table = bank.table(static_cast<tables::Waveform>(osc_params.osc1_wave), f1);
    osc2_table = bank.table(static_cast<tables::Waveform>(osc_params.osc2_wave), f2);
    static constexpr tables::Waveform kSubShape[4] = {
        tables::Waveform::kSine, tables::Waveform::kTriangle,
        tables::Waveform::kSaw, tables::Waveform::kSquare};
    const int sw = (sub_wave < 0) ? 0 : (sub_wave > 3 ? 3 : sub_wave);
    sub_table = bank.table(kSubShape[sw], sub_octave ? f1 * 0.5f : f1);
  }
  float osc_out = 0.0f;
  osc::render_oscillators(osc_params, osc1_table, osc2_table, sub_table,
                          &osc_out, 1, phase_inc, phase_inc2, osc_state,
                          0.0f < osc_params.detune ? det_inc : nullptr);
  phase.phase = osc_state.osc1_phase;  // keep the legacy field in sync

  // ---- filter (CVcf → CVca link) ----
  // CVcf-domain harness: write the CVco→CVcf link in the 2048 int-domain, read
  // the CVca output normalized back (/domain). Linear path net-neutral; only the
  // domain-scaled leveler/clamp change the result. domain=1.0 → current binary.
  const float dom = filt.domain;
  // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A;
  // FUN_10033740): CVcf walk-call grid for the filter's per-call soft-clip
  // group forms. Device CF-call buffers independently 16B-aligned
  // (m3-sse-lane-scratch F3) => start phase 0.
  filt.sse_sc_pos = sc_pos_;
  filt.sse_sc_len = sc_len_;
  filt.sse_sc_start_mod4 = 0;
  const float filt_out = filt.process_sample(osc_out * dom) / dom;

  // ---- amp (spec_env.md §3.2, FUN_180037c30) ----
  // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A;
  // FUN_10031c70 ramp lanes): per-sample gains from the device-form
  // per-call array (built at each gain-call head), not per-sample
  // accumulation. g_cur_ carries the device +0x78 store at the call end.
  float g;
  if (amp_silent_call_) {
    g = 0.0f;
  } else {
    g = sse_gain_[sse_gain_i_ < sse_gain_n_ ? sse_gain_i_++ : sse_gain_n_ - 1];
    if (amp_call_rem_ == 1 && !amp_guard_hit_) g_cur_ = sse_gain_last_;
  }
  ++sc_pos_;
  static const bool stage_rms = std::getenv("SYNTH1_STAGE_RMS") != nullptr;  // #235
  if (stage_rms) {
    static thread_local double so = 0, sf = 0, sa = 0;
    static thread_local long long n = 0, sidx = 0;
    static thread_local bool done = false;
    const long long lo = (long long)(0.3 * sample_rate), hi = (long long)(0.4 * sample_rate);
    if (sidx >= lo && sidx < hi) {
      const float a = filt_out * g;
      so += (double)osc_out * osc_out; sf += (double)filt_out * filt_out; sa += (double)a * a; ++n;
    }
    ++sidx;
    if (sidx == hi && !done && n > 0) {
      done = true;
      std::fprintf(stderr, "[stageRMS] osc %.4f filt %.4f amp %.4f amp/filt %.4f n %lld\n",
                   std::sqrt(so / n), std::sqrt(sf / n), std::sqrt(sa / n), std::sqrt(sa / sf), n);
    }
  }
  --amp_ch_rem_;
  --amp_call_rem_;
  // law #172: the CVcf counters never go negative — a loop-head BREAK
  // (cap < 1, FUN_1000b580 :8301) EXITS the call; the next call re-arms
  // (device FUN_1000b3a0 re-invokes FUN_1000b580 with a fresh budget).
  if (flt_ch_rem_ > 0) --flt_ch_rem_;
  if (flt_call_rem_ > 0) --flt_call_rem_;
  --mod_ch_rem_;
  --mod_call_rem_;
  if (cm_call_rem_ > 0) --cm_call_rem_;  // m3-272 CVco driver budget
  if (++blk_i_ >= blk_len_) blk_len_ = 0;
  ++rendered_;  // diagnostic counter (law #159-L1 hook pins)
  if (amp_silent_call_) return 0.0f;  // FUN_10031c70 guard-silent call (zero-fill)
  return filt_out * g;
}

void Voice::process_block(float* out, int num_samples,
                          const tables::WavetableBank& bank, int amp_cap,
                          int env_cap, int flt_cap) {
  if (!active) {
    for (int i = 0; i < num_samples; ++i) out[i] = 0.0f;
    return;
  }
  refresh_tables(bank);
  begin_block(num_samples, amp_cap, env_cap, flt_cap);
  for (int i = 0; i < num_samples; ++i) {
    if (!active) {  // finished mid-block (end-of-release)
      out[i] = 0.0f;
      continue;
    }
    out[i] = process_full_sample();
    // CVca end-of-release hook (FUN_180037f00): reset CVcf + CVco/mod env.
    // law #90: the chunk may span calls, so the hook also fires at a gain-
    // apply call boundary (the finest device amp cut).
    if ((amp_ch_rem_ == 0 || amp_call_rem_ == 0) && is_finished(*this)) {
      hook_pos_ = rendered_;  // diagnostic (law #159-L1 pins)
      active = false;
      g_cur_ = 0.0f;   // CVca+0x90 = 0 (FUN_180037f00)
      g_step_ = 0.0f;
      mod_env.reset();
      // law #187 row-33: FUN_10033d90 calls FUN_1000b250 FIRST — the CVcf
      // filter-env hard reset (stage/cur/prog = +0x2c/+0x50/+0x54 = 0)
      // before the CVcf filter state is zeroed.
      filter_env.reset();  // adsr.h reset() == FUN_18000c950/FUN_1000b250
      filt.reset();
      // FUN_180037f00 → FUN_18003dfd0: CVco+0xb8 = 1 — the first target
      // change of the next note jumps (snap) instead of ramping.
      osc::fm_release_reset(osc_state.fm);
    }
  }
}

}  // namespace synth1::voice
