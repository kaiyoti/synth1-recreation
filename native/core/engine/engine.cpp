// engine.cpp — the Synth1 engine: events → sample-accurate sub-blocks.
//
// Spec: dsp_flow.md §1/§2/§5, spec_env.md §7 (pool/lifecycle),
//       spec_lfo_fx.md §1/§7/§8/§13, spec_mastervol.md §4/§5.
// Ghidra: FUN_180028fe0 (99-case dispatcher — portable subset),
//         FUN_1800288e0 (0xfe master volume), FUN_180028300 (bend slew),
//         FUN_1800526e0/525f0 (note-on/off), FUN_180001b40 (arp step).
//
// NO heap allocations in process()/render_*: the wavetable bank is fetched
// once in prepare(), the mix scratch is a fixed member array, and every
// subsystem (voices, allocator, arp, LFOs, master volume) is fixed-size.

#include "engine.h"
#include "../params/dispatch_ranges.gen.h"
#include "rng/crt_rand.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace synth1::engine {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// #134 device dispatch clamp (law): the program store keeps the RAW value —
// FUN_180027c70 stores via FUN_18001e9c0 FIRST, then dispatches FUN_180028fe0,
// which looks up the per-id (min,max) pair (FUN_180037400 over DAT_1801a0e80,
// prefilled (-1,-1), 98 entries from DAT_180093f10) and clamps into iVar10,
// the value fed to EVERY consumer in the switch (decomp_all.c:28718-28750:
// lookup 28718, (-1,-1) pass-through guard 28726, clamp 28731-28749,
// iVar10 = (int)uVar11 28750, switch 28752). (-1,-1) (ids 86-89) or an absent
// entry (94) = no clamp. raw_params[]/norm_params[] and the -checkparams
// readback stay RAW; only the consumer value is clamped (clamping the store
// would break LOADCHECK on every out-of-range file — sweep p09_v1 readback
// 32 with voice field 24). Table: core/params/dispatch_ranges.gen.h.
inline int dispatch_clamp(int id, int v) {
  const params::DispatchRange& dr = params::kDispatchRanges[id];
  if (dr.min != -1 || dr.max != -1) {
    if (v < dr.min) v = dr.min;
    if (v > dr.max) v = dr.max;
  }
  return v;
}

// FUN_10030830 unison detune law (unison_law.md): spread =
// (exp(2·raw75/127)−1)·100/(e²−1) — NORMALIZED so raw75=127 gives exactly
// 100 cents total spread (#166a: the old form omitted the /(e²−1) = 6.389056
// divisor and ran 6.389× hot; device pin: 75=70 → f1 230.9724→235.2107 =
// 31.5 cents total, CELLS k75x70 rows).
// m3-166-land (tasks/consults/m3-166-slotpitch-2.answer.md): device-bit-exact
// float chain per FUN_10030830 :34779-34787 — runtime float(exp(2.0))-1
// divisor, single float roundings (gate-1: +0x160 bits match device taps).
inline float unison_detune_cents(int raw75) {
  const float x = ((float)raw75 * (1.0f / 127.0f)) * 2.0f;
  const float num = (float)std::exp((double)x) - 1.0f;
  const float den = (float)std::exp(2.0) - 1.0f;
  return (num / den) * 100.0f;
}

// m3-166-land: device scalar path FUN_10030830 :34858-34862 —
// cents[i] = (i·(1/(N−1))f·spread) − spread·0.5f in that float op order,
// odd-slot adder float((raw85−24)·100) + c (gate-1 verified per-slot).
inline float unison_slot_pitch_cents(int slot, int count, int raw75, int raw85) {
  if (count <= 1) return 0.0f;
  const float spread = unison_detune_cents(raw75);
  const float recip = 1.0f / (float)(count - 1);
  float cents = ((float)slot * recip) * spread - spread * 0.5f;
  if ((slot & 1) != 0) cents = (float)((raw85 - 24) * 100) + cents;
  return cents;
}

// #198 unison FILTER-spread term (CVcf+0xa0). Device: FUN_10030830 filter
// array afStack_84[i] = ((1.0f/N)·i − 0.5f)·spread (decomp_all_32_full.c:
// 34858-34859 — reciprocal-multiply, /N NOT the pitch /(N−1), param85 never);
// writer FUN_10034050 :37333-37361:
//   +0xa0 = (int)( float(2^(c·(1/1200)) − 1) · 1.0f/(log(1066.66663f)·(1/1024)) )
// (arg float-rounded, pow in double, pre-scale cast to float, trunc toward 0;
// constant = 146.86702). Unison off / N<2 / raw75=0 → 0 (:34779-34787).
inline int unison_slot_filter_term(int slot, int count, int raw75) {
  if (count <= 1) return 0;
  const float spread = unison_detune_cents(raw75);
  const float c = ((1.0f / (float)count) * (float)slot - 0.5f) * spread;
  const float arg = (float)((double)c * (1.0 / 1200.0));
  const double d = std::pow(2.0, (double)arg) - 1.0;
  return (int)((float)d * (1.0 / (std::log((double)(float)1066.66663f) * (1.0 / 1024.0))));
}

// Staged knob for the prog50 amp-attack decomposition: SYNTH1_AMP_ATTACK_SRC=15
// feeds the amp attack from the filter-env attack band (15) instead of the amp
// attack band (25). Default (unset) keeps the current 25 law. Lands the instant
// legion's amp-stage answer arrives.
inline int amp_attack_src_band(const std::int32_t* rp) {
  static const int src = [] {
    const char* e = std::getenv("SYNTH1_AMP_ATTACK_SRC");
    return (e && e[0] == '1' && e[1] == '5') ? 15 : 25;
  }();
  return rp[src];
}

}  // namespace

// ---------------------------------------------------------------------------
// prepare / reset
// ---------------------------------------------------------------------------
void Engine::prepare(float sample_rate, std::int32_t block_size) {
  this->sample_rate = sample_rate;
  this->block_size = block_size;
  this->absolute_sample = 0;
  this->tick_start_abs_ = 0;
  this->tick_len_ = this->tick_done_ = 0;

  // Build the global wavetable bank HERE (allocations at init only).
  this->bank_ = &tables::global_bank();

  this->arpeggiator = arp::Arpeggiator{(int)sample_rate};
  this->allocator = voice::VoiceAllocator{};
  this->allocator.set_sample_rate(sample_rate);
  this->pitch_bend = midi::PitchBend{};
  this->mod_wheel = midi::ModWheel{};
  this->master_vol = MasterVolume{};
  this->master_vol.prepare(sample_rate);

  this->lfo1 = lfo::LfoState{};
  this->lfo2 = lfo::LfoState{};
  this->lfo1.sampleRate = this->lfo2.sampleRate = (int)sample_rate;
  // The CLFO ctor (LAB_180014c40) leaves freqCur = freqTarget = 1.0f,
  // period = 44100, minSegment = 64 — it does NOT run the speed setter.
  // speed_to_hz now carries FUN_180015210's trailing FUN_180014ff0(lfo,1)
  // slew step, so calling it here would advance freqCur before any param
  // write (law #85: the startup glide starts at the first speed/tempoSync
  // write, not at init). depth_to_range(0) is idempotent (lo = hi = 0).
  lfo::depth_to_range(this->lfo1);
  lfo::depth_to_range(this->lfo2);

  // Post-voice FX chain (dsp_flow.md §3): sample rate + tempo sync grid.
  // The CEq ctor default is enabled; the binary's .sy1 loader never
  // dispatches param 73 when the preset omits it (no bank0 patch carries
  // 73), so the tone shelf stays live by default — verified against the
  // reference: 073's tone 113 high shelf attenuates its 261 Hz fundamental
  // ~7x (FUN_18002a5c0 → CEq+0x9b18).
  this->fx_chain.set_sample_rate(sample_rate);
  this->fx_chain.set_tempo_bpm(this->tempo_bpm);
  this->fx_chain.reset();
  // Plugin defaults for params absent from a .sy1 (install/base_params.txt):
  // 82 delay typ 0, 83 delay tim 66 (+0.1 ms spread), 98 delay ton 64 (off),
  // 84 unison pa 64 (spread 0), 93 unison vo 9 (N = 2), 73 unison mo 0
  // (EQ left at its ctor-enabled default — see above).
  this->fx_chain.set_param(82, 0);
  this->fx_chain.set_param(83, 66);
  this->fx_chain.set_param(98, 64);
  this->fx_chain.set_param(90, 64);  // pan centre (unity L/R)
  // U4-b (#196): CPan holds the two LFO pointers (FUN_10013c50 /
  // CPan+0x10/+0x14); the dest-7 consumer runs at the chain stage.
  this->fx_chain.pan.set_lfo_taps(&this->lfo1, &this->lfo2);
  // Segment scratch capacity for the whole render (no alloc in the audio
  // path): worst case one open+close commit per 32-frame chunk in a window
  // up to ~16k frames (covers every host block size in the gate protocol).
  this->pan_lfo_segs_.reserve(1024);
  this->pan_segs_scratch_.reserve(1024);
  this->raw_params[73] = 0;
  this->raw_params[75] = 22;
  this->raw_params[84] = 64;
  this->raw_params[85] = 24;
  this->raw_params[93] = 9;
  this->unison_on_ = false;
  // unison_count_ boot default N=2 (render-verified: hardcoding 1 left
  // 73=1 & 93-absent presets at N=1, -6.02 dB). The device unison-struct
  // ctor stamps count 1 (FUN_180036270, decomp_all.c:32938); the effective
  // boot N=2 stands until a load re-fires case 93 (upgrade injection
  // 93=9 -> dispatch clamp [2,8] -> count 8 per the #134 consumer law;
  // boot-vs-load count divergence on unison-on presets is an OPEN row).
  this->unison_count_ = 2 + (int)std::lround((double)this->raw_params[93] * (6.0 / 127.0));
  if (this->unison_count_ < 2) this->unison_count_ = 2;
  if (this->unison_count_ > 8) this->unison_count_ = 8;
  this->recompute_voice_pans();
  this->recompute_voice_detune();

  this->arp_note = -1;
  this->arp_gate_off_at = 0;
  this->arp_notes_emitted = 0;

  // Defaults matching the original ctors (spec_env.md §0: CVca ampGain 0.5
  // ≈ band 64 curve; param 30 default 0; CVco pulseWidth 1.0 = band 127,
  // spec_osc.md §3).
  this->raw_params[29] = 64;
  this->norm_params[29] = 64.0f * (1.0f / 127.0f);
  this->raw_params[8] = 127;
  this->norm_params[8] = 1.0f;
  // Master-volume dispatcher mirror: the original's ctor stamps raw 0x40 for
  // param 0xfe (spec_mastervol.md §1/§7); keep the mirror in sync with the
  // MasterVolume object's ctor default (no .sy1 carries 0xfe).
  this->raw_params[MASTER_VOL_PARAM] = MASTER_VOL_DEFAULT_RAW;
  this->norm_params[MASTER_VOL_PARAM] = 64.0f * (1.0f / 127.0f);
  // FX-chain ctor-default mirror for slots a ver<113 file omits (upgrade_v105
  // injects the init sound for 73..98, so these match kInitSound exactly).
  // NOTE: the v100 defaults_audit "7-fix" list was derived from the device
  // getParameter raw*127 column, which is a scaling artifact for 87/89/94/250/
  // 251 and is load-path-dependent (bank0 -prog vs banked -setsy1 differ, e.g.
  // 96/97 = 1 for bank0 but 0 for banked). Applying it regressed bank0 039/048.
  // Reverted to the original ctor values; defaults must be validated per-patch
  // against device DISPLAY + audio, not the raw column.
  this->raw_params[82] = 0;
  this->raw_params[83] = 66;
  this->raw_params[98] = 64;
  this->raw_params[79] = 64;
  this->raw_params[80] = 64;
  this->raw_params[81] = 64;
  this->raw_params[87] = 44;
  this->raw_params[89] = 43;
  // #206 (m3-206d): the device applies kInitSound through the assign setter at
  // plugin init (44/43 are FLAG1 ids -> stored verbatim), so the BOOT dest of
  // slot 0/1 is 44/43, not the ModSlot{} −1. A file row the setter later
  // REJECTS (flag-0 id, 94, or out of range) keeps this boot dest — the
  // sust_d0o p87=0 case (device cctap 44, F6).
  this->mod_wheel.slot[0].assign = 44;
  this->mod_wheel.slot[1].assign = 43;
  this->raw_params[90] = 64;
  this->raw_params[94] = 16;

  voice::reset_phase_lcg();  // DAT_1800938d4 = 1 (MSVC rand seed)
  rng::crt_srand(1u);        // CRT _holdrand = 1 (FUN_180055b98, never srand'd)
  for (auto& k : live_keys_) k = false;
  live_key_count_ = 0;
  for (auto& v : this->voices) {
    v.prepare(sample_rate, this->bank_);
    v.set_amp_gain_band(64);
    v.set_amp_vel_sens_band(0);
    v.set_pulse_width_band(127);
  }
  // Allocator steal-level envelope times (bands 0 defaults); mirrors the
  // CVca setter conversion (U5-a′ env::seconds_to_samples).
  this->allocator.set_env_times(
      env::seconds_to_samples(env::amp_attack_seconds(0), sample_rate),
      env::seconds_to_samples(env::amp_release_seconds(0), sample_rate));

  // m3-u4c-land: ctor boot-table replay (DAT_1007f1e0 through
  // FUN_10024640, assign slots -1) then the effSetSampleRate sr setter
  // on both LFOs. NAMED HOST DEPENDENCE (m3-u4c-law answer): the engine
  // models exactly ONE effSetSampleRate call = the frozen render host;
  // a host calling it k times adds k slew steps per LFO at load.
  this->boot_replay_ctor();
  lfo::sr_setter(this->lfo1);
  lfo::sr_setter(this->lfo2);
}

void Engine::reset() {
  this->absolute_sample = 0;
  this->tick_start_abs_ = 0;
  this->tick_len_ = this->tick_done_ = 0;
  this->chunk_start_abs_ = 0;
  this->chunk_len_ = this->chunk_done_ = 0;
  this->arp_note = -1;
  this->arp_gate_off_at = 0;
  this->arp_notes_emitted = 0;
  for (auto& v : this->voices) {
    v.prepare(this->sample_rate, this->bank_);
    v.set_amp_gain_band(this->raw_params[29]);
    v.set_amp_vel_sens_band(this->raw_params[30]);
  }
  this->allocator = voice::VoiceAllocator{};
  this->allocator.set_sample_rate(this->sample_rate);
  this->arpeggiator = arp::Arpeggiator{(int)this->sample_rate};
  this->lfo1 = lfo::LfoState{};
  this->lfo2 = lfo::LfoState{};
  this->lfo1.sampleRate = this->lfo2.sampleRate = (int)this->sample_rate;
  this->pitch_bend = midi::PitchBend{};
  this->mod_wheel = midi::ModWheel{};
  this->master_vol = MasterVolume{};
  this->master_vol.prepare(this->sample_rate);
  this->fx_chain.reset();
}

// ---------------------------------------------------------------------------
// block processing: split at event offsets, render sub-blocks, FX chain,
// master tail (dsp_flow.md §3/§5: voice pool → FxChain (L/R) → master → out)
// ---------------------------------------------------------------------------
// Stereo core for the block window [wb, we): `left`/`right` point AT
// position wb (all internal positions are window-relative), the voice
// pool sums mono into `left`, the block is duplicated into `right`,
// fx_chain runs IN PLACE, then the master-volume tail scales both
// channels. absolute_sample is advanced by the caller; the control-tick
// grid is anchored at render_abs_base_ = absolute_sample + wb.
void Engine::process_stereo_window(float* left, float* right,
                                   std::int32_t wb, std::int32_t we,
                                   const events::Event* events,
                                   std::size_t num_events) {
  render_abs_base_ = absolute_sample + (std::int64_t)wb;
  win_left_ = left;   // render_tick_audio derives the right channel from these
  win_right_ = right;
  // U4-b: CPan segment list — state in force from the window start until
  // the first chunk commit in this window.
  pan_lfo_segs_.clear();
  pan_lfo_segs_.push_back({render_abs_base_, lfo1, lfo2});
    std::int32_t cursor = wb;
    pos_notes_n_ = 0;
    for (std::size_t e = 0; e < num_events; ++e) {
      const events::Event& ev = events[e];
      const std::int32_t off = (std::int32_t)ev.sample_offset;
      if (off < wb || off >= we) continue;
      cur_event_abs_ = render_abs_base_ + (std::int64_t)off - (std::int64_t)wb;  // m3-sse-lanes-land
        // E-place LAW (m3-eplace-x87-recheck answer, 2026-10-16; replaces
        // the retracted #189 "event_sample−1 / 513−delta" wording — that
        // −1 was the dump host's double-rounded frame product, see
        // AGENTS.md x87 frame law): device note-on side effects (mono/
        // legato retrigger env restart, LFO key-sync reset) take effect
        // AT THE EVENT SAMPLE (deltaFrames) at every arm — initial and
        // retrigger, unison and non-unison, gate-held and gate-gap. The
        // chunk is CUT at the event sample; at delta 0 the event clamps
        // to the block start. There is NO −1.
        // Bytes (install/Synth1 VST.dll FUN_100173a0, objdump-verified):
        //   0x1001742d 8b 50 08        mov 0x8(%eax),%edx   (deltaFrames)
        //   0x10017430 2b 91 90 ad 00 00 sub 0xad90(%ecx),%edx (− rendered)
        //   0x1001743b-0x1001744a      min with frames, negative → 0
        //   (the delta-0 clamp); drain executes at rendered ==
        //   deltaFrames; no −1 anywhere in the dispatch.
        std::int32_t aoff = off;
        bool keysync_split = false;
        if (ev.type == events::EventType::kNoteOn) {
          // 68/70 key-sync armed: the key-sync reset lands AT the event
          // sample, with the core chunk CUT at the event sample so the
          // pre-event LFO advance commits BEFORE the reset (without the
          // cut the whole block advances post-reset: blk47 = 512, the
          // pre-#189 failure mode). Device counters: U4-T LFO2 phase
          // 322@blk47 = 512−190 (true x87 deltaFrames), doff sweep
          // 324/323/322/321/320 = 512−delta (m3-eplace-minus1 frozen
          // dumps, re-labeled by m3-eplace-x87-recheck F5/F6).
          if (lfo1.keySync != 0 || lfo2.keySync != 0) {
            keysync_split = true;
            aoff = off;
          }
        }
      if (aoff > cursor) {
        // m3-porta-land: the PREVIOUS position's batched note events take
        // effect for [cursor, aoff) — flush (held-key table + ONE evaluate +
        // drive voices) BEFORE rendering the range that follows the position.
        flush_note_position();
        // event offset cuts the tick (window-relative positions)
        render_range(left, cursor - wb, aoff - wb, true);
      }
      // An event landing on the current position (e.g. at a block start,
      // where no range precedes it) must also cut the running control tick.
      cut_control_tick(aoff - wb);
      if (ev.type == events::EventType::kNoteOn && keysync_split) {
        // The device drains the event AT the event sample (rendered ==
        // deltaFrames, FUN_100173a0 0x1001742d-0x1001744a): the LFO
        // advance (FUN_10012e60, once per core chunk in FUN_10024ea0)
        // must commit the pre-side-effect samples BEFORE the reset
        // lands, else the whole 256-capped chunk advances post-reset
        // (the U4-T blk47 512 failure mode). The cut sits AT the event
        // sample — no −1 (m3-eplace-x87-recheck LAW). Same close_chunk
        // pattern as the arp edge.
        close_chunk();
      }
      if (keysync_split) {
        const int note = ev.payload.note.note;
        const int vel = ev.payload.note.velocity;
        const bool first_key = note_on_keyboard(note);
        if (first_key) note_on_keysync_reset();
        if (off > aoff) {
          render_range(left, aoff - wb, off - wb, true);
          cut_control_tick(off - wb);
        }
        if (arpeggiator.enabled()) {
          arpeggiator.key_down(note, vel);
          if (first_key) arpeggiator.note_on_reset(vel);
        } else if (pos_notes_n_ < (int)(sizeof(pos_notes_) / sizeof(pos_notes_[0]))) {
          pos_notes_[pos_notes_n_] = {note, vel, true};
          ++pos_notes_n_;
        }
        if (off > cursor) cursor = off;
      } else {
        apply_event(ev);
        if (aoff > cursor) cursor = aoff;
      }
    }
    flush_note_position();
   // The window tail ends at the caller's block boundary: not a control cut.
   if (cursor < we) render_range(left, cursor - wb, we - wb, false);


  // Stereo voice sum (per-voice pan law), then the post-voice chain in place.
  // U4-b (#196): the CPan dest-7 getter reads the LFOs at their CHAIN-time
  // state — the device advances the LFOs after the chain stage reads, i.e.
  // the chunk-START anchor (m3-u4b-land: chunk-start pins 41 v7 per-channel
  // nulls to floor; block-end leaves a -25 dB L residual). The engine
  // commits its LFO advance inside the voice window, so hand CPan one
  // segment per chunk with the state in force for that chunk.
  const std::int32_t len = we - wb;
  pan_segs_scratch_.clear();
  for (const auto& s : pan_lfo_segs_) {
    fx::CPan::LfoSeg seg;
    seg.start = static_cast<int>(s.start_abs - render_abs_base_);
    seg.a = &s.a;
    seg.b = &s.b;
    pan_segs_scratch_.push_back(seg);
  }
  fx_chain.pan.set_lfo_segments(pan_segs_scratch_.data(),
                                static_cast<int>(pan_segs_scratch_.size()));
  fx_chain.process(left, right, len);
  apply_master_gain(left, right, len);
}

void Engine::process(float* left, float* right, std::int32_t num_frames,
                     const events::Event* events, std::size_t num_events) {
  if (num_frames <= 0) return;
  process_stereo_window(left, right, 0, num_frames, events, num_events);
  absolute_sample += num_frames;
}

void Engine::process(float* left, float* right, std::int32_t num_frames) {
  process(left, right, num_frames, nullptr, 0);
}

// Backward-compatible mono overload: the stereo render runs with `output`
// as the left channel and the fixed mono_right_ scratch as the right
// channel (the chain needs two buffers; the right result is discarded).
// Blocks longer than the scratch are tiled into windows — bit-identical
// (window boundaries are not control cuts; the tick grid and every FX
// delay line are absolute-sample anchored / streaming).
void Engine::process(float* output, std::int32_t num_frames,
                     const events::Event* events, std::size_t num_events) {
  if (num_frames <= 0) return;
  for (std::int32_t wb = 0; wb < num_frames;) {
    std::int32_t we = wb + ENGINE_FX_SCRATCH_FRAMES;
    if (we > num_frames) we = num_frames;
    process_stereo_window(output + wb, mono_right_, wb, we, events, num_events);
    wb = we;
  }
  absolute_sample += num_frames;
}

void Engine::process(float* output, std::int32_t num_frames) {
  process(output, num_frames, nullptr, 0);
}

// ---------------------------------------------------------------------------
// render_range: split at arpeggiator step / gate edges (spec_lfo_fx.md §13:
// the engine loop is "arp gate edge → LFO edge → chain → clock advance").
// end_is_edge says whether `end` is a control-cutting edge (an event / arp
// edge offset) or merely the end of the caller's block (a plain block
// boundary must NOT cut a control tick — see render_sub).
// ---------------------------------------------------------------------------
void Engine::render_range(float* output, std::int32_t begin, std::int32_t end,
                          bool end_is_edge) {
  std::int32_t pos = begin;
  // m3-freeze-anchor-land: this entry IS a FUN_100173a0 host-segment re-entry
  // (processReplacing splits the host block at MIDI-event deltas and renders
  // each segment in min(seg_rem, 0x100) pieces FROM THE SEGMENT START), so the
  // 0x100 voice-call grid anchors here, not at the arp sub-segment starts.
  host_range_anchor_abs_ = render_abs_base_ + (std::int64_t)begin;
  // m3-bendslew-lfo-land (ruling tasks/consults/m3-bendslew-lfo-scratch.
  // answer.md): the device render-piece chain restarts at every FUN_100173a0
  // host-segment re-entry (law #159-L1); FUN_100135b0 :14539-14542 gives the
  // FUN_1000b3a0 budget = min(piece, 0x100) chained from this anchor.
  piece_anchor_ = host_range_anchor_abs_;
  // m3r-055-land (m3r-055-tick F2, ACCEPTED): the device FUN_10024ea0 piece
  // chain restarts FROM THE SEGMENT START at every FUN_100173a0 host-segment
  // re-entry (law #159-L1 as amended). A chunk opened in the PREVIOUS host
  // segment keeps its stale cap100 chain (44032+256=44288 for bank0_055), a
  // phantom piece boundary that injects a phantom control-tick head.
  // Re-anchor the in-progress chunk's 0x100 chain at this re-entry's anchor
  // (extend only; wrap caps recomputed identically since the LFO phase does
  // not advance mid-chunk). Evidence: 055 -47.20 -> -129.02 dB, 005 ->
  // -101.92, 053 -> -134.48; 80-cell sweep 10 movers / 0 away.
  if (chunk_len_ > 0 && chunk_done_ < chunk_len_ &&
      chunk_start_abs_ < host_range_anchor_abs_ && !pitch_bend.slewing) {
    const std::int64_t cs = chunk_start_abs_;
    const std::int64_t bs = block_size > 0 ? block_size : 64;
    std::int32_t len = (std::int32_t)((cs / bs + 1) * bs - cs);
    const std::int64_t an = host_range_anchor_abs_;
    const std::int64_t rel = cs > an ? (cs - an) : 0;
    const std::int64_t nb =
        an + (rel / DEVICE_PROCESS_CHUNK_FRAMES + 1) *
                 (std::int64_t)DEVICE_PROCESS_CHUNK_FRAMES;
    const std::int32_t cap100 = (std::int32_t)(nb - cs);
    if (len > cap100) len = cap100;
    const std::int32_t c1 = lfo::chunk_cap(lfo1);
    const std::int32_t c2 = lfo::chunk_cap(lfo2);
    if (c1 < len) len = c1;
    if (c2 < len) len = c2;
    if (len < 1) len = 1;
    if (len > chunk_len_) chunk_len_ = len;
  }
  if (!arpeggiator.enabled()) {
    render_sub(output, pos, end, end_is_edge);
    return;
  }
  while (pos < end) {
    const int p = arpeggiator.position();
    const int step = arpeggiator.step_len();
    std::int32_t d;
    if (p == 0) {
      // Step boundary: release the previous arp note, emit the next. The
      // boundary is a control cut (the note state changes here).
      arp_edge_abs_ = render_abs_base_ + pos;  // m3r-111-j1cond J4=2
      cut_control_tick(pos);
      close_chunk();                 // FUN_1800285a0: an arp edge ends the chunk
      const int to_gate = arp_step_boundary();
      arp_gate_off_at = (to_gate > 0 && to_gate <= step) ? to_gate : step;
      d = arp_gate_off_at;
    } else {
      const int to_step = step - p;
      const int to_gate = arpeggiator.samples_to_gate_off_edge();
      if (to_gate == 0) {
        // FUN_1800018f0 says we are exactly on a gate-off edge.
        arp_edge_abs_ = render_abs_base_ + pos;  // m3r-111-j1cond J4=2
        cut_control_tick(pos);
        close_chunk();               // FUN_1800285a0: an arp edge ends the chunk
        if (arp_note != -1) arp_release_note();
        d = to_step;
      } else {
        d = (to_gate < to_step) ? to_gate : to_step;
      }
    }
    if (d > end - pos) d = end - pos;
    if (d <= 0) d = 1;  // degenerate gate: never stall the clock
    // An edge inside the range cuts the control tick; running to the range
    // end cuts only when that end is itself an edge.
    const bool cut = (pos + d < end) || end_is_edge;
    const std::int32_t seg = (d < 1) ? 1 : d;
    render_sub(output, pos, pos + seg, cut);
    arpeggiator.advance(seg);
    pos += seg;
  }
}

// ---------------------------------------------------------------------------
// render_sub: render [begin, end) on the control-tick grid inside the device
// render chunk.
//
// Two nested grids, exactly as FUN_1800285a0 / FUN_180014ce0 do it:
//   * the CHUNK (open_chunk) is the remaining frames of the caller block
//     (block_size, absolute-sample aligned), capped at (period - phase) for an
//     ON wave-3/4 LFO (FUN_180014cd0) and at CONTROL_TICK_FRAMES while the
//     pitch bend slews. The LFO phase / frequency slew advance (FUN_180014f60 +
//     FUN_180014ff0) is committed ONCE per chunk — the LFO free-runs per block
//     from prepare, across notes and gaps; note events do not cut it.
//   * the TICK runs from its start to the chunk end, cut further at the LFO
//     segment boundaries (FUN_180014d10 *outSegLen, clamped to minSegment) and
//     at absolute event / arp-edge offsets. Each tick reads the LFO at
//     (phase + offset) mod period (FUN_180014ce0) — the value the voice holds
//     for that segment.
// A tick may span a caller block boundary only when the chunk does; the
// in-progress tick / chunk carry across process() calls.
// ---------------------------------------------------------------------------
// m3r-111 G11 (LANDED, unconditional):
// J2=1: open_chunk 0x100 cap chains from host_range_anchor_abs_.
// J3=3: the tick is NOT cut at CVco-routed (dest {1,2,5,6}) LFO node cuts;
//       dests {3,4} keep the tick node cut (m3r-111-movers J3B).
// J4=2: the outer L1 rem resets at the piece_anchor_ 0x100-chain heads AND
//       at arp gate/step-edge piece heads (m3r-111-j1cond: device D125 re-
//       reads L1 at every 256-chain head but the chain is NOT re-anchored
//       at the host-block-capped chunk heads; D111s block 75 re-reads at
//       the arp gate edge 38586).

void Engine::open_chunk(std::int64_t abs) {
  chunk_start_abs_ = abs;
  const std::int64_t bs = block_size > 0 ? block_size : 64;
  std::int32_t len = (std::int32_t)((abs / bs + 1) * bs - abs);  // block remaining
  // m3r-111 G11 J2=1 (LANDED): the FUN_100135b0 :14539-14542
  // 0x100 voice-processor cap CHAINS from the host-range anchor, it does
  // not re-anchor at an arp-edge render_sub split (law #159-L1; device
  // D111e block 5: piece 2778->2816 = 38, NOT 2778->3034).
  const std::int64_t an = host_range_anchor_abs_;
  const std::int64_t nb =
      an + ((abs - an) / DEVICE_PROCESS_CHUNK_FRAMES + 1) *
               (std::int64_t)DEVICE_PROCESS_CHUNK_FRAMES;
  const std::int32_t cap100 = (std::int32_t)(nb - abs);
  if (len > cap100) len = cap100;
  // m3-engine-events CORRECTION to the law-#90 note: the 0x20 "while
  // slewing" chunk cap + FUN_10024c20 advance ARE live in v1.13b3.
  // reports/90_ac90_settle.md's displacement scan searched `90 ac 00 00`
  // and found no setter, but the bend setter FUN_10025900 (:29072) runs
  // with this = engine+0xc (the vftable subobject installed at
  // CSynthesizer1+0xc, ctor FUN_10021750 `param_1[3] = vftable`) and
  // stores `[this+0xac84] = 1` = engine+0xac90 (file bytes 84 ac 00 00
  // @0x24d0b). The flag is armed by every 0xe0 message; the render loop
  // FUN_10024ea0 (:28511-28515) then caps the sub-chunk at 0x20 and
  // advances the wheel ±1 once per chunk (FUN_10024c20 :28374). Inert
  // without bend events (flag 0) -> frozen-host renders untouched.
  if (pitch_bend.slewing) {
    static const bool dbg_delta = std::getenv("S1_DBG_DELTA") != nullptr;  // #235
    if (dbg_delta)
      std::fprintf(stderr, "SLEWCHUNK abs=%lld cur=%d tgt=%d\n",
                   (long long)abs, pitch_bend.current, pitch_bend.target);
    pitch_bend.slew_one_chunk();          // FUN_10024c20 at the sub-chunk head
    if (len > 0x20) len = 0x20;           // FUN_10024ea0 :28512-28514
    // m3-bendslew-lfo-land: while the wheel slews (+0xac90 armed,
    // FUN_10024c20 @28374 wheel step) FUN_10024ea0 :28511-28515 caps the
    // piece at 0x20 and the NEXT piece starts at this piece's end — the
    // 0x100 budget chain re-anchors here.
    piece_anchor_ = abs + len;
  }
  const std::int32_t c1 = lfo::chunk_cap(lfo1);
  const std::int32_t c2 = lfo::chunk_cap(lfo2);
  const std::int32_t len_pre_wrap = len;
  if (c1 < len) len = c1;
  if (c2 < len) len = c2;
  if (len < 1) len = 1;
  // m3r-124-land (m3r-124-locate F2 + advisor adv124 gate): a chunk cut at a
  // wave-3/4 period wrap ends a FUN_10024ea0 piece (:28517-28527); the LFO
  // re-seeds there and FUN_1000b3a0 is re-entered fresh (device D124: fresh
  // outer reads at the wrap 6157 and every 64 after it: 6221, 6285, 6349),
  // while the 0x100 chain keeps its anchor (piece still ends at 6400).
  if (len < len_pre_wrap) wrap_head_abs_ = abs + len;
  chunk_len_ = len;
  chunk_done_ = 0;
  // U4-b: the chain stage for this chunk sees the LFO state as of NOW
  // (preset param events at the window head are already applied — the
  // device's program-load setters ran before block 0).
  pan_lfo_segs_.push_back({abs, lfo1, lfo2});
}

void Engine::close_chunk() {
  if (chunk_done_ <= 0) { chunk_len_ = 0; chunk_done_ = 0; return; }
  lfo::lfo_tick(lfo1, chunk_done_);
  lfo::lfo_tick(lfo2, chunk_done_);
  // U4-b: the post-commit state is what the NEXT chunk's chain stage sees.
  pan_lfo_segs_.push_back({chunk_start_abs_ + chunk_done_, lfo1, lfo2});
  chunk_len_ = 0;
  chunk_done_ = 0;
}

void Engine::render_sub(float* output, std::int32_t begin, std::int32_t end,
                        bool end_is_edge) {
  std::int32_t pos = begin;
  while (pos < end) {
    if (tick_done_ >= tick_len_) {  // no tick in progress: start one at `pos`
      const std::int64_t abs = render_abs_base_ + (std::int64_t)pos;
      if (chunk_done_ >= chunk_len_ || abs >= chunk_start_abs_ + chunk_len_)
        open_chunk(abs);
      tick_start_abs_ = abs;
      const std::int32_t to_chunk = (std::int32_t)(chunk_start_abs_ + chunk_len_ - abs);
      const std::int32_t rem = end - pos;
      const std::int32_t off = (std::int32_t)(abs - chunk_start_abs_);
      // m3-u4c-resume-land (m3-lfo-nodecut-law LAW, FUN_1000b3a0 :8176-8193 +
      // FUN_10012c00): for an LFO-WIRED consumer the render segment splits at
      // min(segment, max(segEnd - phase, minSegment)) — the CVca chunk walk,
      // L1 outer bound / L2 inner bound, local non-persisting phase copy
      // (FUN_10012be0); the persisting tick (lfo_tick / FUN_10012e60) stays
      // once per chunk. Waves 3/4 wrap-only (consumer_cut). The
      // CONTROL_TICK_FRAMES base belongs to the UNWIRED modulation read grid
      // only: the device's wired-consumer walk is chunk-anchored at the
      // floored node cuts (envtap 942/942, p115 square nodes carry no 32
      // grid). Unwired: the validated absolute-32 read grid stands.
      const bool wired1 = lfo_wired_any(lfo1);
      const bool wired2 = lfo_wired_any(lfo2);
      const std::int32_t s1 = wired1 ? lfo::consumer_cut(lfo1, off) : 0x7fffffff;
      const std::int32_t s2 = wired2 ? lfo::consumer_cut(lfo2, off) : 0x7fffffff;
      std::int32_t len;
      if (wired1 || wired2) {
        len = to_chunk;                  // chunk-anchored walk (cap <= 256)
      } else {
        // The voice kernel holds routed modulation constant across a tick, so
        // the read grid keeps the CONTROL_TICK_FRAMES base (the device's voice
        // loop re-reads per sample inside the chunk); the chunk only bounds it.
        len = CONTROL_TICK_FRAMES -
              (std::int32_t)(abs % CONTROL_TICK_FRAMES);
        if (to_chunk < len) len = to_chunk;
      }
      if (rem < len) len = rem;          // stop at the range end
      // m3r-111 G11 J3=3 (LANDED): the L1/L2 node cuts are the
      // FUN_1000b3a0 driver budget (voice cm arm), not the call/tick
      // length (FUN_10024ea0 pieces end only at arp edges / 0x20 slew /
      // wave-3-4 nodes; D111s block 8: one call 4352->4608).
      // dests {3,4} (CVcf/amp) keep the tick node cut; only CVco-dest
      // {1,2,5,6} cuts move to the driver budget.
      // m3r-111-porta (LANDED): while a LEGATO-RETARGET glide is in
      // progress the device CVco walk span is the porta bucket edge
      // (FUN_10036730 :39086-39137 step=max(0x20,..); FUN_10036250
      // :38972-38977 sub-chunk = min(param_3,(bucket+1)*step-pos)) — the
      // tick keeps its cut; the tick-cut removal applies only with no
      // retarget glide in progress (fam077/078 mel12g0: J3=3 alone
      // -86.74 -> -2.18).
      bool any_glide = false;
      for (int vi = 0; vi < voice::MAX_VOICES; ++vi)
        if (voices[vi].active && allocator.glide_retarget_active(vi)) {
          any_glide = true;
          break;
        }
      if (any_glide || raw_params[10] != 0) {
        if (s1 < len) len = s1;          // L1 outer cut (the consumer re-reads)
        if (s2 < len) len = s2;          // L2 inner cut (:8188-8193)
      } else {
        if (wired1 && !lfo_wired_cvco(lfo1) && s1 < len) len = s1;
        if (wired2 && !lfo_wired_cvco(lfo2) && s2 < len) len = s2;
      }
      // m3-292-land-2 (#291 CLOSED; ruling
      // tasks/consults/m3-292-land-f7-gate-fail.answer.md decision (a)): a
      // DEST_PW (dest 5) LFO is CVco-wired (FUN_100345b0 :37627-37637) but
      // NOT lfo_wired_any (dests {1,2,3,4,6}), so a PW-only routing leaves
      // the tick on the absolute-32 read grid — not the device's CVco budget
      // grid, which FUN_1000b3a0 :8129-8228 cuts at the CONSUMER-routed
      // LFO's segment edges, minSegment-floored by FUN_10012c00 LAB_10012e2f
      // :13998-14005. Cut the tick at the CVco-ROUTED LFO's floored segment
      // edges as well. For dests 1/2/6 s1/s2 already carry the identical
      // consumer_cut(off) → no new cut, grid unchanged. Law-consequent:
      // dest 5 ∉ lfo_wired_any, FUN_1000b3a0 consumer cuts.
      // m3r-111 G11 J3=3 (LANDED): the CVco-routed LFO segment cuts belong
      // to the FUN_1000b3a0 DRIVER budget (voice cm arm), NOT to the
      // FUN_100135b0 call/tick length (device D111e: call 2816->3072 spans
      // the L2 64-cuts; D111s block 8: call 4352->4608 spans the L1 66-cut).
      if (any_glide || raw_params[10] != 0) {
        if (lfo_wired_cvco(lfo1)) {
          const std::int32_t c = lfo::consumer_cut(lfo1, off);
          if (c < len) len = c;
        }
        if (lfo_wired_cvco(lfo2)) {
          const std::int32_t c = lfo::consumer_cut(lfo2, off);
          if (c < len) len = c;
        }
      }
      if (len < 1) len = 1;
      if (std::getenv("S1_DBG_CUT") && (wired1 || wired2)) {
        const std::int32_t dbs = block_size > 0 ? block_size : 64;
        std::fprintf(stderr, "CUT abs=%lld block=%d soff=%d len=%d\n",
                     (long long)abs, (int)(abs / dbs), (int)(abs % dbs), len);
      }
      // law #90: the CVca gain-apply span = min(ladder chunk, stage-rem,
      // call-rem) where call-rem (FUN_1800285a0 sub-chunk + FUN_18000ca70
      // modulation-event cuts) = the CALLER-BLOCK remainder, the wave-3/4
      // period wrap and the LFO segment boundaries — NOT the
      // CONTROL_TICK_FRAMES base (the 0x20 cap is dead, +0xac90 == 0;
      // reports/90_ac90_settle.md). The span also stops at the range end
      // (note / arp-edge trigger law). The modulation read grid above is
      // unchanged for every other consumer.
      // law #123 (CORRECTS the law-90-era exclusion): the 0x100
      // voice-processor cap IS a call cut for the env span —
      // FUN_100135b0 :14540-14542 `local_10 = 0x100; if (param_1 < 0x100)
      // local_10 = param_1;` returns min(param_1, 0x100) (:14714) and the
      // caller loop FUN_10024ea0 :28536-28545 re-enters at that cut, so
      // FUN_1000b3a0 (:8131) walks with a budget ending at the next 256
      // edge and FUN_1000b580 re-rounds at EVERY call cut (:8390). The
      // cited 017 trace does not discriminate: a chunk starting 53 before
      // a 512 edge has no intervening 256 edge (reports/123_lfo_grid_settle.md
      // §2); 121's release tail stops one 256-cut earlier on the device
      // (114432 = 447*256 vs engine 114688 = 224*512, settle §3).
      // law #123 (unchanged): the 0x100 cap cuts the CVca gain-apply
      // span too — FUN_1800155d0 :14113-14116 `iVar27 = 0x100;
      // if (param_2 < 0x100) iVar27 = param_2;` bounds the voice process
      // call (and with it the CVca driver FUN_18000ca70 inside it) at
      // <=256 frames at EVERY host block size; the device's own 62-64 Hz
      // floors are identical at bs512 and bs256 (tasks/m2b-law82-gaingrid-2
      // RESULT F3), which a host-block grid cannot produce.
      const std::int32_t bs = block_size > 0 ? block_size : 64;
      const std::int32_t block_rem =
          (std::int32_t)((abs / bs + 1) * bs - abs);
      const std::int32_t w1 = lfo::chunk_cap(lfo1) - off;
      const std::int32_t w2 = lfo::chunk_cap(lfo2) - off;
      // law #82: the CVca GAIN-call span is the block-anchored grid +
      // segment cuts of AMP-WIRED LFOs only (FUN_18000ca70
      // param_1[0xe]/[0xf]); no wave-wrap cuts. The CVcf/CVco env-walk
      // span keeps the full law-123 grid (amp_call_cap123).
      const std::int32_t as1 = lfo_wired_to_amp(lfo1) ? s1 : 0x7fffffff;
      const std::int32_t as2 = lfo_wired_to_amp(lfo2) ? s2 : 0x7fffffff;
      // law #159-L1 (amended by m3-freeze-anchor-land): the 0x100 cap
      // re-anchors at the enclosing HOST-range start, not at the arp
      // sub-segment start. A host-event split is a FUN_100173a0 re-entry
      // (segments [pos, next-event-delta), pieces of min(seg_rem, 0x100)
      // from the SEGMENT START, event dispatched after the segment,
      // FUN_10012730 :18247) and DOES re-anchor; an arp edge is an
      // in-piece sub-cut inside FUN_10024ea0 (:28497-28545,
      // iVar3 -= iVar9 keeps the piece budget) and does NOT.
      const std::int64_t range_start_abs = host_range_anchor_abs_;
      tick_amp_cap_ = gain_call_cap82(abs, range_start_abs, block_rem, rem,
                                      as1, as2);
      // m3-258 (#258, tasks/m3-258-cutset F2): the CVco env-walk grid cuts
      // at the segment boundaries of the CVco-ROUTED LFOs only
      // (lfo_wired_cvco: FUN_100345b0 :37627-37637 arms the CVco +0x6c/
      // +0x70 flags iff dest ∈ {1,2,5,6}) — NOT the lfo_wired_any set
      // (dest 5 PW is CVco-wired: 062's LFO1 vertices 864/928/992/1088...
      // are device cuts the engine was missing) — and the 0x100 cap
      // anchors at the host-range start like CVca/CVcf (law #159-L1 as
      // amended by m3-freeze-anchor-land; the absolute anchor cut at
      // 22272 where the device cut at 22306 = 22050+256).
      const std::int32_t es1 =
          lfo_wired_cvco(lfo1) ? lfo::consumer_cut(lfo1, off) : 0x7fffffff;
      const std::int32_t es2 =
          lfo_wired_cvco(lfo2) ? lfo::consumer_cut(lfo2, off) : 0x7fffffff;
      tick_env_cap_ = amp_call_cap123(abs, range_start_abs, block_rem, rem,
                                      es1, es2, w1, w2);
      // law #172: the CVcf env-walk cap keeps the segment cuts of the LFOs
      // WIRED to the filter only (FUN_10032280 +0x6c/+0x70); no wrap cuts.
      // #302 amendment (m3r-121): the device's CVcf driver step is bounded by
      // the UNFLOORED render-chunk wave-3/4 period-wrap cut (FUN_10024ea0
      // :28517-28527 + FUN_10012bd0 = period-phase; re-seed FUN_10012e60
      // :14019-14033): a FILTER-WIRED wave-3/4 LFO cuts the CVcf call AT the
      // wrap sample even when the wrap distance is < 64 - the consumer_cut
      // minSegment floor (lfo.cpp:331) hides it. Mirror the CVco J1=7 w1/w2
      // caps (voice.cpp:976-983); law #172 "no wrap cuts" holds only for
      // non-wave-3/4 filter-wired LFOs (see reports/172_cvcf_callgrid.md).
      std::int32_t fs1 = lfo_wired_to_filter(lfo1) ? s1 : 0x7fffffff;
      std::int32_t fs2 = lfo_wired_to_filter(lfo2) ? s2 : 0x7fffffff;
      if (lfo_wired_to_filter(lfo1) && w1 < fs1) fs1 = w1;
      if (lfo_wired_to_filter(lfo2) && w2 < fs2) fs2 = w2;
      tick_flt_cap_ = flt_call_cap172(abs, range_start_abs, block_rem, rem, fs1, fs2);
      static const bool lfo_grid_dbg = std::getenv("SYNTH1_LFOGRIDDBG") != nullptr;  // #235
      if (lfo_grid_dbg) {
        std::fprintf(stderr, "[lfogrid] abs %lld off %d s1 %d s2 %d len %d flt %d env %d\n",
                     (long long)abs, off, s1, s2, len, tick_flt_cap_, tick_env_cap_);
      }
      // law #159-L1: the caller loop re-enters the voice processor at the
      // call cut (FUN_10024ea0 :28536-28545), so the control tick ENDS at
      // the next CVca call cut — the cap is consumed at a tick boundary,
      // never stale mid-tick (pre-fix absolute-256 cuts were always
      // 32-aligned; event-anchored cuts are not).
      if (tick_amp_cap_ < len) len = tick_amp_cap_;
      tick_len_ = len;
      tick_done_ = 0;
      begin_control_tick();
    }
    std::int32_t n = end - pos;
    const std::int32_t left = tick_len_ - tick_done_;
    if (n > left) n = left;  // a block boundary may split the tick's audio
    render_tick_audio(output + pos, n);
    tick_done_ += n;
    pos += n;
    // Retro-cut: an edge inside the running tick ends the tick here.
    const std::int64_t at = render_abs_base_ + (std::int64_t)pos;
    if (end_is_edge && at < tick_start_abs_ + (std::int64_t)tick_len_) {
      tick_len_ = (std::int32_t)(at - tick_start_abs_);
    }
    if (tick_done_ >= tick_len_) commit_control_tick(tick_len_);
  }
}

// ---------------------------------------------------------------------------
// begin_control_tick: evaluate the control plane at the tick's first sample
// and route it into the active voices (the LFO advance is committed at the end
// of the CHUNK, not of the tick).
// ---------------------------------------------------------------------------
void Engine::begin_control_tick() {
  const int bend = pitch_bend.bend_offset();
  const std::int32_t off = (std::int32_t)(tick_start_abs_ - chunk_start_abs_);

  // LFO values at (phase + offset) mod period — FUN_180014ce0.
  voice::ModInputs mod;
  lfo_route(lfo1, mod, off);
  lfo_route(lfo2, mod, off);
  // m3-272 (tasks/consults/m3-272-scratch.answer.md): the CVco/CVcf-routed
  // sums are owned by the consumers' own walk-step re-reads (voice.cpp
  // cm_reread; FUN_1000b3a0 :8129-8228 reads the FUN_10012be0 values once
  // per budget segment, cut at the consumer-routed LFO segment edges,
  // minSegment-floored by FUN_10012c00); the shared tick-start write must
  // not touch them at all (it would both zero the held value between
  // steps and overwrite mid-step past a floored LFO segment cut). Amp
  // (dest 4) stays on the tick grid: the tick is already cut at the CVca
  // call cap.

  for (int i = 0; i < voice::MAX_VOICES; ++i) {
    // m3-272 context: the consumer-routed LFO states + the chunk anchor
    // for the voice-side re-read (set for ALL voices so a mid-tick
    // allocation is covered).
    voices[i].set_cm_context(&lfo1, &lfo2, chunk_start_abs_);
    if (!voices[i].active) continue;
    voices[i].set_pitch_bend_units(bend);
    voices[i].set_porta_offset(allocator.voice(i).remaining);
    voices[i].set_lfo_amp_only(mod.amp);  // m3-272: consumer-owned sums
                                          // re-read on the consumer grid
  }
}

// ---------------------------------------------------------------------------
// commit_control_tick: advance the control plane by the `len` frames of the
// tick that was just rendered (pitch-bend slew ±1/sample, allocator envelopes /
// glide curve / slot release) and, when the tick closes the device chunk, the
// LFO phase + freq slew + S&H re-seed for the whole chunk (FUN_180014f60 +
// FUN_180014ff0).
// ---------------------------------------------------------------------------
void Engine::commit_control_tick(std::int32_t len) {
  if (len <= 0) return;

  // Pitch-bend slew: advanced once per CHUNK at open_chunk (FUN_10024c20
  // called from the FUN_10024ea0 sub-chunk head under the 0x20 cap) —
  // m3-engine-events; nothing to advance per tick here.

  // The LFOs advance once per device chunk (free-run per block from prepare).
  chunk_done_ += len;
  if (chunk_done_ >= chunk_len_) close_chunk();

  // Allocator: envelope chunks + slot release. The glide curve is NOT
  // advanced here — law #114 re-evaluates the porta offset only at glide
  // bucket edges, which render_tick_audio cuts per voice (advance_glide).
  allocator.advance_env(len);
}

// ---------------------------------------------------------------------------
// cut_control_tick: end the running control tick at the current render
// position (an event offset), so the control plane is re-evaluated there.
// No-op when no tick is in progress at that position.
// ---------------------------------------------------------------------------
void Engine::cut_control_tick(std::int32_t pos) {
  if (tick_done_ <= 0 || tick_done_ >= tick_len_) return;
  if (render_abs_base_ + (std::int64_t)pos !=
      tick_start_abs_ + (std::int64_t)tick_done_) {
    return;
  }
  tick_len_ = tick_done_;
  commit_control_tick(tick_len_);
}

// ---------------------------------------------------------------------------
// render_tick_audio: render and sum the voice pool for `len` frames of the
// current tick (per-voice modulations already routed by begin_control_tick).
// ---------------------------------------------------------------------------
void Engine::render_tick_audio(float* output, std::int32_t len) {
  if (len <= 0) return;
  float* right = win_right_ + (output - win_left_);

  bool first = true;

  for (int i = 0; i < voice::MAX_VOICES; ++i) {
    if (!voices[i].active) continue;
    // CMixDevice::process pan law (FUN_180015a20, voice+0x488 via
    // FUN_180027c00): pan > 0 → gL = max(1−pan, 1e-7), gR = 1;
    // pan < 0 → gL = 1, gR = max(1+pan, 1e-7); pan = 0 → unity both.
    const float pan = voice_pan_[i];
    float gl = 1.0f, gr = 1.0f;
    if (pan > 0.0f) {
      gl = (1.0f - pan < 1e-7f) ? 1e-7f : 1.0f - pan;
    } else if (pan < 0.0f) {
      gr = (1.0f + pan < 1e-7f) ? 1e-7f : 1.0f + pan;
    }
    // law #114 (glide_g4_trace.md §1, FUN_10036250): the porta offset is
    // re-evaluated ONLY at the edges of the glide-relative bucket grid,
    // and the device render chunk runs to the next edge
    // (n = min(rem_frames, (bucket+1)·step − pos)). Cut THIS voice's
    // render span at its next bucket edge and re-route the offset there
    // (same cap-consumption plumbing as the #123 amp-call cut below).
    // The CVca/CVcf env-call spans keep the #90/#95b/#123 caps — a bucket
    // cut segments only the CVco offset, never an env call (the env-call
    // remainder carries across the cut, voice.cpp step_env_chunks).
    // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A):
    // CVca walk grid anchor + host block size (FUN_10031c70 ramp lanes).
    voices[i].set_walk_start_rel((int)(tick_start_abs_ - voices[i].base_abs()));
    voices[i].set_block_size(block_size > 0 ? block_size : 64);
    std::int32_t off = 0;
    while (off < len) {
      std::int32_t c =
          (len - off < ENGINE_SCRATCH_FRAMES) ? len - off : ENGINE_SCRATCH_FRAMES;
      allocator.glide_sync(i);
      voices[i].set_porta_offset(allocator.voice(i).remaining);
      const std::int32_t edge = allocator.glide_frames_to_edge(i);
      if (edge > 0 && edge < c) c = edge;
      // m3r-111-porta: per-voice glide-active flag for the J1 gate.
      voices[i].set_glide_active(allocator.glide_retarget_active(i));
      // m3-272 (m3-272-scratch.answer.md): abs position of this call's
      // first sample, for the voice-side consumer-grid modulation re-read.
      // m3-bendslew-lfo-land (ruling tasks/consults/m3-bendslew-lfo-scratch.
      // answer.md): pass the device render-piece grid — while slewing the
      // piece IS the 0x20 chunk (FUN_10024ea0 :28511-28515, FUN_10024c20
      // @28374); otherwise min(piece, 0x100) chained from piece_anchor_
      // (FUN_100135b0 :14539-14542). FUN_1000b3a0 :8168-8193 re-reads the
      // routed LFO values at every piece head.
      {
        const std::int64_t a = tick_start_abs_ + tick_done_ + off;
        std::int64_t pend = 0; bool phead = false;
        if (pitch_bend.slewing) { pend = chunk_start_abs_ + chunk_len_; phead = (a == chunk_start_abs_); }
        else {
          // m3r-111 G11 J4=2 (LANDED): the outer L1 rem resets at the
          // piece_anchor_ 0x100-chain heads AND at arp gate/step-edge
          // render-piece heads ONLY (device D125 re-reads L1 at every
          // 256-chain head but the chain is NOT re-anchored at the
          // host-block-capped chunk heads; D111s block 75: piece
          // 38400->38586 arp edge, fresh L1 read at 38586 -> step 64 to
          // 38650, not the held 16 to 38602).
          const std::int64_t an = piece_anchor_;
          const std::int64_t k = (a - an) / DEVICE_PROCESS_CHUNK_FRAMES;
          const std::int64_t ps = an + k * (std::int64_t)DEVICE_PROCESS_CHUNK_FRAMES;
          pend = ps + DEVICE_PROCESS_CHUNK_FRAMES;
          phead = (a == ps) || (a == arp_edge_abs_) || (a == wrap_head_abs_);
          // m3r-062 (LANDED): the FUN_10024ea0 voice-process call ENDS
          // at the host block boundary, so the FUN_1000b3a0 budget
          // (param_1) truncates the 0x100-chain piece there (device
          // D062 note 2: piece 22306->22528 = 222, NOT 22306->22562;
          // FUN_1000b580 :8283 local_3c = call rem gives step 10 at
          // 22518, not 20).
          const std::int64_t bs = block_size > 0 ? block_size : 64;
          const std::int64_t bend = (a / bs + 1) * bs;
          if (pend > bend) pend = bend;
        }
        voices[i].set_cm_abs_base(a, pend, phead);
      }
      // law #90: distance from this chunk's first frame to the next
      // device CVca amp cut (set at tick start; see render_sub).
      voices[i].process_block(scratch_, c, *bank_,
                              tick_amp_cap_ - tick_done_ - off,
                              tick_env_cap_ - tick_done_ - off,
                              tick_flt_cap_ - tick_done_ - off);
      if (first) {
        for (std::int32_t j = 0; j < c; ++j) {
          output[off + j] = scratch_[j] * gl;
          right[off + j] = scratch_[j] * gr;
        }
      } else {
        for (std::int32_t j = 0; j < c; ++j) {
          output[off + j] += scratch_[j] * gl;
          right[off + j] += scratch_[j] * gr;
        }
      }
      allocator.advance_glide(i, c);
      off += c;
    }
    first = false;
  }
  if (first) {
    for (std::int32_t i = 0; i < len; ++i) {
      output[i] = 0.0f;
      right[i] = 0.0f;
    }
  }
}

// FUN_180037250 (param 84 handler): assign the per-voice pan spread across
// the pool. Unison off (engine+0x194 == 0) → every voice pan 0. On:
//   voice[i]+0x488 = spread · (2·((i mod N)/(N−1)) − 1)
// with spread = (param84 − 64)/64 and N = unison voice count (param 93).
void Engine::recompute_voice_pans() {
  if (!unison_on_ || unison_count_ < 2) {
    for (int i = 0; i < voice::MAX_VOICES; ++i) voice_pan_[i] = 0.0f;
    return;
  }
  // The device unison struct holds the DISPATCH-CLAMPED values (case 0x54
  // FUN_180037250(iVar10), decomp_all.c:29364) — read them clamped, the
  // program store stays raw (#134).
  const float spread =
      (float)(dispatch_clamp(84, raw_params[84]) - 64) * (1.0f / 64.0f);
  const int n = unison_count_;
  for (int i = 0; i < voice::MAX_VOICES; ++i) {
    const float pos = (float)(i % n) / (float)(n - 1);
    voice_pan_[i] = spread * (2.0f * pos - 1.0f);
  }
}

// FUN_180037130 / FUN_1800371a0: assign the per-voice unison detune cents
// across the pool. Unison off or N < 2 → every voice detune 0. On:
//   voice[i]+0x178 = detune_cents·((i mod N)/(N−1) − 1/2)
//                    + (odd slot ? (param85 − 24)·100 : 0)
// with detune_cents = 100·(exp(2·param75/127) − 1)  (unison_law.md FUN_10030830).
void Engine::recompute_voice_detune() {
  if (!unison_on_ || unison_count_ < 2) {
    for (int i = 0; i < voice::MAX_VOICES; ++i) {
      voices[i].set_unison_slot(i, 0.0f, 0);
    }
    return;
  }
  const int n = unison_count_;
  for (int i = 0; i < voice::MAX_VOICES; ++i) {
    const int slot = i % n;
    // unison struct holds dispatch-clamped 75/85 (cases 0x4b/0x55,
    // decomp_all.c:29313/29367) — store stays raw (#134).
    voices[i].set_unison_slot(
        slot, unison_slot_pitch_cents(slot, n, dispatch_clamp(75, raw_params[75]),
                                      dispatch_clamp(85, raw_params[85])),
        unison_slot_filter_term(slot, n, dispatch_clamp(75, raw_params[75])));
  }
}

// ---------------------------------------------------------------------------
// master volume tail (spec_mastervol.md §4/§5, FUN_180028480/1b5d0):
// law 76 — the one-pole starts from 0 once per plugin instance/render
// (ctor 26115 stores 0 into engine+0xbb4c) and advances ONCE PER
// INTERLEAVED SAMPLE (FUN_180028480 28170-28185: 2 advances per stereo
// frame; each sample is scaled by the gain after its own advance — left
// gets 2f+1 advances, right 2f+2). The 2^-16 snap is checked after every
// advance (the device checks once per block at 28187; the difference is
// bounded by the snap threshold itself and keeps the engine block-size
// invariant). Fast path when settled: hard scale (FUN_18001b5d0).
// ---------------------------------------------------------------------------
void Engine::apply_master_gain(float* left, float* right, std::int32_t num_frames) {
  if (master_vol.target_gain == master_vol.current_gain) {
    const float g = master_vol.current_gain;
    for (std::int32_t i = 0; i < num_frames; ++i) {
      left[i] *= g;
      right[i] *= g;
    }
    return;
  }
  for (std::int32_t i = 0; i < num_frames; ++i) {
    master_vol.current_gain +=
        (master_vol.target_gain - master_vol.current_gain) * master_vol.alpha;
    if (std::fabs(master_vol.target_gain - master_vol.current_gain) < MASTER_VOL_SNAP)
      master_vol.current_gain = master_vol.target_gain;
    left[i] *= master_vol.current_gain;
    master_vol.current_gain +=
        (master_vol.target_gain - master_vol.current_gain) * master_vol.alpha;
    if (std::fabs(master_vol.target_gain - master_vol.current_gain) < MASTER_VOL_SNAP)
      master_vol.current_gain = master_vol.target_gain;
    right[i] *= master_vol.current_gain;
  }
}

// ---------------------------------------------------------------------------
// LFO destination routing (spec_lfo_fx.md §1.8, FUN_18003dbe0): dest 1
// "osc2" reaches osc2 only, dest 2 "osc1,2" both oscillators, dest 3 the
// filter (mirror-folded in CVcf), 4 amp, 5 pulse width, 6 FM gate. Dest 7
// (pan) has NO per-voice consumer (U4-b #196: the device consumer is the
// post-sum CPan chain stage FUN_10013d20 — fx_chain.pan holds the LFO
// taps and runs the cubic-width walk itself; routing dest 7 per-voice
// would be the wrong topology, m3-u4b-pan F1).
// ---------------------------------------------------------------------------
void Engine::lfo_route(const lfo::LfoState& lfo, voice::ModInputs& mod,
                       std::int32_t offset) {
  if (lfo.onOff == 0) return;
  int segLen = 0;
  const int v = lfo::read_at(lfo, offset, &segLen);
  switch (lfo.destination) {
    case lfo::DEST_OSC2: mod.pitch2 += v; break;
    case lfo::DEST_OSC12: mod.pitch += v; break;
    case lfo::DEST_FILTER: mod.cutoff += v; break;
    case lfo::DEST_AMP: mod.amp += v; break;
    case lfo::DEST_PW: mod.pw += v; break;
    case lfo::DEST_FM: mod.fm += v; break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// note routing
// ---------------------------------------------------------------------------
// Shared internal note-on (T5, m3-chorus0-arp-land): the device routes EVERY
// note-on — live MIDI AND arpeggiator steps — through the same owner
// dispatcher: FUN_10001ae0 (out/decomp_all_32_full.c:778-866) calls
// `(**(code **)(**(int **)(param_1 + 4) + 0x14))(note, vel)` at :861, i.e.
// vtable+0x14 = the internal noteOn that reaches the unison N-slot allocator
// (poly FUN_10030e40: vpn = (M+0x110 ? M+0x114 : 1) at :35078-35082, N-slot
// loop from :35118; mono/legato twin FUN_10030c20:34955-34959).
// note_on_live and Engine::arp_step_boundary both call this; the keyboard
// held-key bookkeeping and LFO key-sync stay in the keyboard path.
void Engine::note_on_voiced(int note, int vel) {
  // voice_alloc_decompile.md: no same-key suppression. Poly releases the
  // same-pitch voice and allocates a fresh one (retrigger); mono always
  // retriggers; legato retargets. The allocator (VoiceAllocator::note_on)
  // applies the per-mode law.
  // Unison (param 73 on, param 93 count): the note-on allocates N slots for
  // the same key (FUN_10030e40 walks the pool); each slot carries its
  // pan-spread value from FUN_180037250.
  const bool legato_retarget =
      allocator.play_mode() == voice::kPlayLegato && allocator.held_count() > 0;
  // U2 (#149): the device engage predicate is param73 ONLY — no play-mode
  // term. vpn = (M+0x110 ? M+0x114 : 1) is byte-identical in the poly
  // (FUN_10030e40:35072-35076) and mono/legato (FUN_10030c20:34955-34959)
  // allocators; 64-bit twins FUN_180036c00:33394 / FUN_180036930:33257-33261.
  const int n = (unison_on_ && unison_count_ >= 2) ? unison_count_ : 1;
  if (n > 1 && allocator.play_mode() != voice::kPlayPoly) {
    // FUN_10030c20 mode-1/mode-2 loops (decomp_all_32_full.c:34992-35031;
    // 64-bit FUN_180036930:33302-33350): unison in mono/legato allocates the
    // FIRST n slots of the pool (piVar2 = param_1+1 stepping), NOT the
    // round-robin chooser. Mono (38=1): FUN_10047e20 note+gate on every slot,
    // then FUN_10047e60 group + FUN_10047ea0 attack UNCONDITIONALLY (retrigger).
    // Legato (38=2): FUN_10047e20 on every slot; group+attack only where the
    // gate bit V+0x438 was 0 (:35022) — held slots retarget (legato hold).
    bool attacked[voice::MAX_VOICES] = {};
    allocator.note_on_first_n(note, vel, n, attacked);
    for (int j = 0; j < n; ++j) {
      // FUN_10047e60 group call runs with the attack in both branches:
      // unconditionally in mono (:35001-35003), only on ungated slots in
      // legato (:35022-35025). Gated legato slots keep their assignment.
      if (allocator.play_mode() == voice::kPlayMono || attacked[j]) {
        // #259 (m3-105-pair F3): the device dispatch FUN_10025940 clamps the
        // value to the per-param range BEFORE the case call (case 85
        // @decomp_all_32_full.c:29735 → FUN_10031300 gets the clamped fVar5);
        // p85 range [0,48] (dispatch_ranges.gen.h:104). Device dump 85=64:
        // odd-slot adder +2400.00 = (clamp(64,0,48)−24)·100, not +4000.
        voices[j].set_unison_slot(
            j,
            unison_slot_pitch_cents(j, n, dispatch_clamp(75, raw_params[75]),
                                    dispatch_clamp(85, raw_params[85])),
            unison_slot_filter_term(j, n, dispatch_clamp(75, raw_params[75])));
      }
      if (attacked[j]) {
        voices[j].note_on(note, vel, sample_rate, *bank_);
        voices[j].set_abs_base(cur_event_abs_);  // m3-sse-lanes-land
      } else {
        voices[j].retarget_note(note, vel, sample_rate, *bank_);
      }
    }
    sync_voice_gates();
    return;
  }
  for (int k = 0; k < n; ++k) {
    const int idx = (k == 0) ? allocator.note_on(note, vel)
                             : allocator.note_on_force(note, vel);
    // #270: device FUN_10030830 :34883-34887 indexes the unison detune/filter
    // tables by POOL SLOT: table[poolslot % vpn] — not the note-loop index k.
    // #259: dispatch-clamped 75/85 (FUN_10025940 case 85 @:29735 clamps before
    // the case call; p85 [0,48] dispatch_ranges.gen.h:104).
    voices[idx].set_unison_slot(
        idx % n,
        unison_slot_pitch_cents(idx % n, n, dispatch_clamp(75, raw_params[75]),
                                dispatch_clamp(85, raw_params[85])),
        unison_slot_filter_term(idx % n, n, dispatch_clamp(75, raw_params[75])));
    if (legato_retarget && voices[idx].active) {
      voices[idx].retarget_note(note, vel, sample_rate, *bank_);
    } else {
      // Device truth (32-bit DLL, LCG state dump at +0x8253c): the state is
      // still the seed 1 at the first note-on of every patch — no draws are
      // consumed by patch load, resume, or per-voice init.  The first Branch-C
      // note-on consumes exactly draws 1..10 (osc1, osc2, det[0..7]); later
      // note-ons free-run (Branch B).  No prime.
      voices[idx].note_on(note, vel, sample_rate, *bank_);
      voices[idx].set_abs_base(cur_event_abs_);  // m3-sse-lanes-land
    }
  }
  // Poly same-pitch retrigger: drive the released old engine voice to its
  // release stage in the same event as the new voice's attack.
  sync_voice_gates();
}

void Engine::note_on_live(int note, int vel) {
  note_on_voiced(note, vel);
}

void Engine::note_off_live(int note) {
  // U2 (#149): the allocator's mono/legato note-off releases the FIRST
  // vpn slots when the held stack empties (FUN_10030c20:34961-34972);
  // with a non-empty stack no attack loop runs (device overlap probe:
  // note-off re-attack refuted) — sync_voice_gates drives the engine
  // voices whose slot gate cleared to their release stage.
  allocator.note_off(note);
  sync_voice_gates();
}

// Release every engine voice whose allocator slot no longer holds the gate
// (spec_env.md §7 FUN_1800525f0: CVca AND CVcf → release).
void Engine::sync_voice_gates() {
  for (int i = 0; i < voice::MAX_VOICES; ++i) {
    if (voices[i].active && voices[i].gate_held &&
        !allocator.voice(i).gate_held) {
      voices[i].note_off();
    }
  }
}

// m3-porta-land: flush the note events recorded for the current sample position
// through the device per-position held-key-table law (FUN_10031070 →
// FUN_10030c20 / FUN_10030e40). All key-table mutations run first (last-wins per
// key, LIFO), then ONE evaluation decides gates/glides, then the engine voices
// are driven from the evaluation. Because gate_held is cleared ONLY in the
// evaluation (mono/legato count==0, poly key-entry==0), a same-position
// off-then-on (gap-0 retrigger) keeps the gate held and the auto glide fires;
// a single note (on@pos, off@later pos) reproduces the old synchronous path
// exactly → byte-identical for the n48 gate.
void Engine::flush_note_position() {
  if (pos_notes_n_ == 0) return;
  // Replay the position's events in array order (device: FUN_10001ae0 pops the
  // events at a position in queue order; the table sees the NET per-key state
  // before the single evaluation — last-wins per key).
  for (int i = 0; i < pos_notes_n_; ++i) {
    const PosNote& p = pos_notes_[i];
    if (p.is_on) allocator.key_on(p.note, p.vel);
    else allocator.key_off(p.note);
  }
  allocator.evaluate();  // FUN_10031070: gates/glides decided here, gate cleared here
  const int n = (unison_on_ && unison_count_ >= 2) ? unison_count_ : 1;
  for (int slot = 0; slot < voice::MAX_VOICES; ++slot) {
    const int act = allocator.eval_action(slot);
    if (act == voice::VoiceAllocator::kEvalAttack ||
        act == voice::VoiceAllocator::kEvalRetarget) {
      const int si = slot % n;  // #270: table[poolslot % vpn], FUN_10030830 :34883-34887
      const voice::VoiceSlot& vs = allocator.voice(slot);
      // #259: dispatch-clamped 75/85 (FUN_10025940 case 85 @:29735 clamps
      // before the case call; p85 [0,48] dispatch_ranges.gen.h:104).
      voices[slot].set_unison_slot(
          si,
          unison_slot_pitch_cents(si, n, dispatch_clamp(75, raw_params[75]),
                                  dispatch_clamp(85, raw_params[85])),
          unison_slot_filter_term(si, n, dispatch_clamp(75, raw_params[75])));
      if (act == voice::VoiceAllocator::kEvalAttack) {
        voices[slot].note_on(vs.note, vs.velocity, sample_rate, *bank_);
        voices[slot].set_abs_base(cur_event_abs_);  // m3-sse-lanes-land
      } else {
        voices[slot].retarget_note(vs.note, vs.velocity, sample_rate, *bank_);
      }
    }
  }
  sync_voice_gates();
  pos_notes_n_ = 0;
}


// Arpeggiator step boundary (FUN_180001b40): release the current note,
// pick the next per mode/octaves, emit it through the SAME internal note-on
// as live MIDI (FUN_10001ae0:861 → owner vtable+0x14 → FUN_10030e40
// :35078-35118; T5 m3-chorus0-arp-land: on a unison patch an arp step
// allocates the unison group, not one voice).
int Engine::arp_step_boundary() {
  if (arp_note != -1) arp_release_note();
  const int to_gate = arpeggiator.emit_step();
  const int n = arpeggiator.current_note();
  if (n >= 0) {
    const int vel = arpeggiator.velocity();
    note_on_voiced(n, vel);
    // Arp notes go through FUN_1800280b0 (second keyboard): no LFO key-sync.
    arp_note = n;
    ++arp_notes_emitted;
  }
  return to_gate;
}

void Engine::arp_release_note() {
  if (arp_note < 0) return;
  allocator.note_off(arp_note);
  sync_voice_gates();
  arp_note = -1;
}

// ---------------------------------------------------------------------------
// event handling
// ---------------------------------------------------------------------------
// FUN_180028020 keyboard update: mark the key held, report whether this
// press makes the held-key count 1 (the key-sync / arp re-arm gate).
bool Engine::note_on_keyboard(int note) {
  bool first_key = false;
  if (note >= 0 && note < 128 && !live_keys_[note]) {
    live_keys_[note] = true;
    first_key = (++live_key_count_ == 1);
  }
  return first_key;
}

// The LFO key-sync resets (FUN_1800152e0, which also redraws the S&H
// value from the shared CRT rand() stream), gated on held-count 1.
void Engine::note_on_keysync_reset() {
  if (lfo1.keySync != 0) lfo::key_sync(lfo1);
  if (lfo2.keySync != 0) lfo::key_sync(lfo2);
}

// Note routing after the keyboard update: arp held-key tracker vs the
// allocator path.
void Engine::note_on_route(int note, int vel, bool first_key) {
  if (arpeggiator.enabled()) {
    arpeggiator.key_down(note, vel);  // held-key tracker (keys+0xc)
    if (first_key) arpeggiator.note_on_reset(vel);  // FUN_1800020f0
  } else {
    note_on_live(note, vel);
  }
}

// ---------------------------------------------------------------------------
// Slot dest re-notify (m3-engine-events). FUN_10031700 tail (:35726-35730):
// after storing the slot value, fetch the dest's current raw
// (FUN_1001af20) and re-dispatch it through FUN_10025940, whose header
// applies the slot delta + clamp. The engine's raw_params[] holds the
// unmodulated stored value (the readback stays raw, cf. the #134
// dispatch_clamp note), so re-dispatching raw_params[assign] reproduces
// the device's getter->apply round trip.
// ---------------------------------------------------------------------------
void Engine::re_notify_slot(int s) {
  const int a = mod_wheel.slot[s].assign;
  if (a >= 0 && a < 255) dispatch_param(a, (int)raw_params[a]);
}

// Assign setter FUN_100318f0 (:35764) ASSIGNABLE-LIST validation (#206,
// m3-206d): the new dest must be in the device assignable-param list
// DAT_10158e78 = {−1} ∪ FLAG1, where FLAG1 = the 52 ids whose 7-int param-
// table entry (VA 0x1007f5a8) has field [5] != 0 — the list FUN_10031550
// (:35584-35640) builds, List[0] = −1 ("off"). A value outside the list is
// REJECTED: the setter returns without writing, the slot keeps its previous
// dest (ctest test_dispatch_clamp: 0, 1, 94, 96, 300 rejected; 19, 44, −1
// stored). The previous engine validated only 0..98 minus 86-89 and accepted
// 43 flag-0 ids the device rejects (osc1 shape 0 = the #206 sust_d0o
// divergence). The device's old/new dest RE-NOTIFY tail (#164, U4-c) IS
// landed: FUN_100318f0 re-notifies the OLD assign ONLY if old != -NAN
// (:35783; re_notify_slot's a>=0 guard is that test), sets the new assign
// (:35791), then dispatches the NEW assign iff new != -1 (:35792-94). A
// rejected value returns with NO write and NO re-notify. Live-event dest
// re-notify (apply_event paths) is unaffected and stays. init_inject keeps
// the store-only form.
void Engine::set_ctrl_assign(int s, int assign) {
  // FLAG1: the 52 assignable ids (param-table entry[5] != 0), in the device
  // list's TABLE order (not numeric). Absent: 86-89, 50/51, 94 (no entry),
  // and every discrete/enum id — the range 0..98 minus 86-89 was too wide.
  static constexpr int kAssignable[52] = {
      2, 5, 8, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27,
      28, 29, 34, 35, 36, 37, 39, 43, 44, 45, 48, 49, 52, 53, 54, 55, 56,
      60, 61, 62, 63, 75, 76, 79, 80, 81, 83, 84, 85, 90, 91, 92, 95, 98};
  if (assign == -1) {  // List[0] "off": re-notify OLD (iff != -1), store -1,
                       // NO NEW dispatch (new == -1, FUN_100318f0 :35792-94)
    re_notify_slot(s);
    mod_wheel.slot[s].assign = -1;
    return;
  }
  for (const int id : kAssignable) {
    if (id == assign) {
      re_notify_slot(s);  // re-notify OLD assign (its current store value)
      mod_wheel.slot[s].assign = assign;
      dispatch_param(assign, (int)raw_params[assign]);  // dispatch NEW
      return;
    }
  }
  // not in {−1} ∪ FLAG1 → FUN_100318f0 returns: slot keeps its previous dest
}

// ---------------------------------------------------------------------------
// m3-u4c-land (#164 + U4-c): the load-sequence convergence law.
// Boot table = the fixed init-sound store DAT_1007f1e0 (@file 0x7d1e0 in
// install/Synth1 VST.dll), replayed through FUN_10024640 in boot-table
// ORDER (the canonical store order; file row order is irrelevant).
// Values are the init-sound display bytes (base_params.txt 0..98); -1 =
// non-integer display (no boot store value; the row still replays at load
// from raw_params, which is what arms 50/51 need).
// ---------------------------------------------------------------------------
namespace {
const int kBootOrder[99] = {
  0, 45, 76, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  71, 72, 91, 95, 96, 97, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
  26, 27, 28, 29, 30, 59, 31, 32, 33, 34, 65, 82, 35, 83, 36, 98, 37, 66,
  64, 52, 53, 54, 55, 56, 60, 61, 62, 63, 90, 77, 78, 79, 80, 81, 38, 94,
  39, 74, 73, 93, 75, 84, 85, 92, 40, 86, 50, 87, 88, 51, 89, 57, 41, 42,
  43, 44, 67, 68, 58, 46, 47, 48, 49, 69, 70,
};
const int kBootVal[99] = {
  2, 1, 0, -1, 1, -1, 0, 0, 64, 0,        // 0..9
  0, 0, 0, 0, 1, 0, 64, 32, 64, 81,       // 10..19
  14, 65, 64, 0, 1, 64, 64, 107, 64, 107, // 20..29
  64, 1, 0, -1, 64, -1, 40, -1, 0, 0,     // 30..39
  12, 2, 1, 64, 0, 0, 5, 1, 64, 64,       // 40..49
  -1, -1, -1, 64, -1, -1, 40, 1, 1, 0,    // 50..59
  64, -1, -1, 64, 2, 1, 1, 0, 0, 0,       // 60..69
  0, 0, -1, 0, 0, 22, 0, 0, 0, 64,        // 70..79
  64, 64, 0, -1, 64, 0, 45057, 44, 45057, 43,  // 80..89 (84 raw 64, m3-u4c-land-4 step 4)
  -1, 0, 0, 2, 16, 0, 1, 1, 64,           // 90..98
};
}  // namespace

void Engine::boot_replay_ctor() {
  // Ctor replay (FUN_10021750 -> FUN_10024640): assign slots are -1, so
  // the 87/89 NEW dispatch is the only re-notify step: LFO1 gets 89new
  // (43) + 43 + 67 = +3, LFO2 gets 48 + 69 = +2 (after_entry readback).
  // m3-u4c-ab-land rebase note: prepare() pre-stamps the #206 boot dests
  // (slot 0/1 = 44/43) for LIVE-event semantics (a later rejected file row
  // keeps the boot dest, sust_d0o cctap F6). At ENTRY the device's assign
  // fields are still -NAN (m3-u4c-split F3: "the entry replay ... assign
  // slots = -1, so the 86/88/87/89 re-notify arms do NOT re-notify"), so
  // the replay itself must start from -1: reset here, the 87/89 rows then
  // re-establish 44/43 (FLAG1-valid) exactly as the device entry replay
  // does. Without the reset the 89-row OLD(43) re-notify adds a 4th LFO1
  // entry slew step (device = 3 + 1 sr step = 4 total).
  mod_wheel.slot[0].assign = -1;
  mod_wheel.slot[1].assign = -1;
  // Ids 84/93 replay their RAW boot-table bytes (DAT_1007f1e0: 84=64,
  // 93=2, tasks/m3-u4c-split/scratch/boot_table.json):
  //   84: store 64 + dispatch; consumer FUN_100313b0 (:35478,
  //       (raw-0x40)*scale, engine case 84 pans); dispatcher pre-switch
  //       range clamp (FUN_10025940 :29127-29146, kDispatchRanges).
  //   93: dispatch raw 2 -> clamp [2,8] (#134) -> unison N=2; the param
  //       SLOT keeps the initial-sound seed 9: the setter arm
  //       FUN_10031330 (:35436-48) writes the unison struct +0x114 only,
  //       never the param array. Device fresh-instance readback
  //       (frozen host -def -getchunk, tasks/m3-u4c-land-4/renders/
  //       devboot_chunk_params.txt): 84 raw 64 (display 0), 93 raw 9
  //       (0.0714285746*127 = 9, display 2 = 2+round(9*6/127)).
  // The store is filled with the WHOLE init table before the replay
  // (FUN_1001b350 then the dispatch loop), so the 89 NEW dispatch sees
  // the boot value of 43, not a zero. The 93 store-fill is skipped: the
   // ctor mirror already stamps the initial-sound seed 9 (prepare()).
   // m3-u4c-prog0 FIX: rows the PROGRAM-0 FILE supplies must reach the consumer
   // even when their boot-table display byte is -1 (p3 osc2 fine, p5 osc mix);
   // the old "display-byte skip" dropped them so OSC2 stayed off.
   bool from_file[99] = {false};
   for (int i = 0; i < 99; ++i) {
     const int id = kBootOrder[i];
     if (id == 93) continue;  // param slot keeps seed 9 (FUN_10031330 cite)
     if (kBootVal[id] >= 0) raw_params[id] = kBootVal[id];
   }
  // R1 split law (m3-u4c-split F1/F5, DECISIVE device capture): the entry
  // replay store is the boot table MERGED with the PROGRAM-0 file
  // (001.sy1 of bankfolder0 — host INI hostselectprogram=1). after_entry
  // L1/L2 speed+target = the file rows 43/48 (c001 61/6, c012 68/65,
  // c103 54/65); the assign=-1 replay steps freqCur toward the FILE
  // targets (c001 L2 target 0.1177 < 1.0 -> steps DOWN -2).
  if (boot_prog_rows_ != nullptr && boot_prog_count_ > 0) {
    for (int i = 0; i < boot_prog_count_; ++i) {
      const int id = boot_prog_rows_[2 * i];
      const int val = boot_prog_rows_[2 * i + 1];
      if (id >= 0 && id < 255) raw_params[id] = val;
      if (id >= 0 && id < 99) from_file[id] = true;  // m3-u4c-prog0 FIX
    }
    current_program_ = 0;  // entry load makes program 0 current (c001
                           // effSetProgram(0) = NO-OP, 0 load dispatches)
  }
  for (int i = 0; i < 99; ++i) {
    const int id = kBootOrder[i];
    if (kBootVal[id] < 0 && !from_file[id]) continue;  // m3-u4c-prog0 FIX: dispatch file-supplied rows too
    const int b = raw_params[id];    // merged store value (R1)
    if (b < 0) continue;
    if (id == prev_disp_id_ && b == prev_disp_val_) continue;
    prev_disp_id_ = id; prev_disp_val_ = b;
    if (id == 93) {
      // The 93 arm FUN_10031330 (:35436-48) writes the unison struct
      // +0x114 only; the param slot keeps the initial-sound seed 9
      // (device fresh-instance readback raw 9, display 2). The DISPATCH
      // value is the doc value: boot-table raw 2 (DAT_1007f1e0), or the
      // program-0 file row if present. Restore the slot after dispatch.
      int doc93 = kBootVal[93];
      if (boot_prog_rows_ != nullptr) {
        for (int j = 0; j < boot_prog_count_; ++j) {
          if (boot_prog_rows_[2 * j] == 93) { doc93 = boot_prog_rows_[2 * j + 1]; break; }
        }
      }
      const int seed = raw_params[93];
      prev_disp_val_ = doc93;  // dup-guard pair carries the doc value
      dispatch_param(id, doc93, true);
      raw_params[93] = seed;
      norm_params[93] = (float)seed * (1.0f / 127.0f);
      continue;
    }
    dispatch_param(id, b, true);
  }
}

void Engine::load_program(const std::int32_t* rows, int count, int program) {
  // 1. INI entry-load equivalent (hostselectprogram=1): the file VALUES
  //    land in the store; the speed fields + targets are applied WITHOUT
  //    a slew step (after_entry: sp/tgt = file values, cur = boot-replay
  //    count). tempoSync is applied only by the replay below (c012
  //    after_sr ts=0).
  // Bounds guard (m3-u4c-land #5 crash): a malformed/oversized row list must
  // never walk past the producer's buffer (the old fixed-buffer overflow on
  // silencing events at offset 0).
  if (rows == nullptr || count < 0 || count > 512) return;
  for (int i = 0; i < count; ++i) {
    const int id = rows[2 * i];
    const int val = rows[2 * i + 1];
    if (id >= 0 && id < 255) raw_params[id] = val;
  }
  lfo1.speed = raw_params[43];
  lfo2.speed = raw_params[48];
  lfo::set_speed_target(lfo1, tempo_bpm);
  lfo::set_speed_target(lfo2, tempo_bpm);

  // 2. effSetProgram(N): N == current program -> NO-OP (c001 = 001.sy1 =
  //    program 0, already current from the entry load: after_prog
  //    unchanged, blk0 355).
  if (program >= 0 && program == current_program_) return;
  current_program_ = program;

  // #280 (m3-21-280-small): the program doc re-seeds every absent slot
  // from the boot initial-sound table on each effSetProgram load
  // (FUN_18001dea0 :21143-21162 "never absent for ids 0..98",
  // preset.cpp:337-339/662-669), so an absent row 93 reads back the boot
  // value 2 (DAT_1007f1e0) at ver>=113 — independent of p38/p73 and of
  // the program-0 file (device cells A/B/C: ver113 x 93-absent -> 2 for
  // unison off/poly/mono; cell D: program-0 93=77 -> still 2 = re-seed,
  // not sticky; tasks/m3-21-280-small; chunk pins absent 93 at
  // ver109/110/112 -> 2, tasks/m3-160-upgrade-93 F3). The ctor seed 9
  // (engine.cpp:158) models the FRESH-INSTANCE param slot only
  // (engine.cpp:1284-1301); after a program load the chunk carries the
  // doc value. The chain already injects 93 for every ver<113 load
  // (upgrade_v105 pre-fill/#160), so this fires only on files whose
  // upgraded row list omits 93 (ver>=113, ver109/110/112 holes) — zero
  // library files (census, tasks/m3-21-280-small/RESULT.md).
  bool row93 = false;
  for (int i = 0; i < count; ++i)
    if (rows[2 * i] == 93) { row93 = true; break; }
  if (!row93) raw_params[93] = kBootVal[93];

  // 3. Replay the whole 99-row store in boot-table order (FUN_10024640
  //    @28070): skip a row iff (id,val) equals the immediately previous
  //    DISPATCHED pair (0xacc0/0xacc4; re-notifies bypass the guard by
  //    calling the dispatcher directly). Each 43/48/67/69 dispatch = one
  //    slew step iff freqCur != freqTarget (FUN_10013140 :14196).
  //    HOST DEPENDENCE (named deviation, m3-u4c-law answer): the device's
  //    load-time step count also depends on how many times the host calls
  //    effSetSampleRate (+1 step per call per LFO). The engine models
  //    exactly ONE call = the frozen render host.
  for (int i = 0; i < 99; ++i) {
    const int id = kBootOrder[i];
    const int val = raw_params[id];
    if (id == prev_disp_id_ && val == prev_disp_val_) continue;
    prev_disp_id_ = id; prev_disp_val_ = val;
    dispatch_param(id, val, false);
  }
}


void Engine::apply_event(const events::Event& ev) {
  switch (ev.type) {
    case events::EventType::kNoteOn: {
      const int note = ev.payload.note.note;
      const int vel = ev.payload.note.velocity;
      const bool first_key = note_on_keyboard(note);
      if (first_key) note_on_keysync_reset();
      if (arpeggiator.enabled()) {
        arpeggiator.key_down(note, vel);  // held-key tracker (keys+0xc)
        if (first_key) arpeggiator.note_on_reset(vel);  // FUN_1800020f0
      } else if (pos_notes_n_ < (int)(sizeof(pos_notes_) / sizeof(pos_notes_[0]))) {
        // m3-porta-land: defer the live note-on into the per-position batch
        // (flushed through the held-key table + evaluate at the position).
        pos_notes_[pos_notes_n_] = {note, vel, true};
        ++pos_notes_n_;
      }
      break;
    }
    case events::EventType::kNoteOff: {
      const int note = ev.payload.note.note;
      if (note >= 0 && note < 128 && live_keys_[note]) {
        live_keys_[note] = false;
        --live_key_count_;
      }
      if (arpeggiator.enabled()) {
        arpeggiator.key_up(note);
        // FUN_180027f80 / FUN_18002a560: when the LAST held key is released
        // (keyboardInfo+8 heldCount == 0) the arp is re-armed with
        // FUN_1800020f0(arp, 0) — velocity 0 — so every subsequent step emits
        // a velocity-0 (silent) note until a fresh key press re-arms it with a
        // real velocity (FUN_180028020 → note_on_reset(vel)).
        if (live_key_count_ == 0) arpeggiator.note_on_reset(0);
      } else if (pos_notes_n_ < (int)(sizeof(pos_notes_) / sizeof(pos_notes_[0]))) {
        // m3-porta-land: defer the live note-off into the per-position batch.
        pos_notes_[pos_notes_n_] = {note, 0, false};
        ++pos_notes_n_;
      }
      break;
    }
    case events::EventType::kCC: {
      // m3-cc-src1-land (S1 fix, law m3-cc-src1-locate.answer.md):
      // FUN_10012730 case 0xb0 case 1 -> 0x10012809 ff 50 1c
      // call *0x1c(%eax) -> CInst vftable 0x10063dbc slot +0x1c =
      // 0x10012370 = c2 04 00 ret $0x4 NO-OP stub (bytes verified on
      // install/Synth1 VST.dll; slot +0x2c = 0x10012aa0). CC1 has NO
      // dedicated path: every CC, CC1 included, is consumed ONLY by
      // the generic slot scan FUN_10031700 (:35669) — slot.value =
      // cc/127 iff slot.src == (0xb0<<8)|cc, dest re-notify. The
      // former hardcoded `if (cc == 1)` slot-0 write was a
      // reconstruction artifact (made slot 0 respond to CC1
      // independent of src1).
      const int cc = ev.payload.cc.controller;
      const int v = ev.payload.cc.value;
      for (int s = 0; s < 2; ++s) {
        if (mod_wheel.slot_matches(s, 0xb0, cc)) {
          mod_wheel.slot[s].value = midi::norm_cc(v);
          re_notify_slot(s);
        }
      }
      break;
    }
    case events::EventType::kAftertouch: {
      // 0xd0 reaches the slots ONLY through the generic scan with
      // src = 0xD000 (sweep §3: AT LIVE iff src = 0xD000; no hardcoded
      // consumer). value = at/127 (FUN_10031700: (float)param_2 *
      // DAT_10063520).
      for (int s = 0; s < 2; ++s) {
        if (mod_wheel.slot_matches(s, 0xd0, 0)) {
          mod_wheel.slot[s].value = (float)ev.payload.perf.value * (1.0f / 127.0f);
          re_notify_slot(s);
        }
      }
      break;
    }
    case events::EventType::kPitchBend: {
      // 0xe0: hardcoded bend path ALWAYS (sweep §3: bend LIVE in every
      // src row) + generic slot path iff src = 0xE000 with value =
      // signed14/8192 (FUN_10031700: * DAT_10065fc8 = 1/8192).
      const int b14 = ev.payload.perf.value;
      pitch_bend.set_bend14(b14);
      for (int s = 0; s < 2; ++s) {
        if (mod_wheel.slot_matches(s, 0xe0, 0)) {
          mod_wheel.slot[s].value = (float)b14 * (1.0f / 8192.0f);
          re_notify_slot(s);
        }
      }
      break;
    }
    case events::EventType::kParameterSet: {
      const int id = ev.payload.param.param_id;
      if (id < 255) dispatch_param(id, ev.payload.param.raw_value,
                             ev.payload.param.init_inject != 0);
      break;
    }
    case events::EventType::kProgramLoad:
      load_program(ev.payload.load.rows, ev.payload.load.count,
                   ev.payload.load.program);
      break;
    case events::EventType::kSetSampleRate: {
      sample_rate = ev.payload.io.sample_rate;
      allocator.set_sample_rate(sample_rate);
      arpeggiator.set_sample_rate((int)sample_rate);
      master_vol.prepare(sample_rate);
      fx_chain.set_sample_rate(sample_rate);
      fx_chain.set_tempo_bpm(tempo_bpm);
      lfo1.sampleRate = lfo2.sampleRate = (int)sample_rate;
      // FUN_10024ad0: effSetSampleRate runs the sr setter FUN_10013360 on
      // both LFOs (1 step iff cur!=tgt + unconditional period recompute).
      lfo::sr_setter(lfo1);
      lfo::sr_setter(lfo2);
      for (auto& v : voices) {
        v.sample_rate = sample_rate;
        v.cur_pitch_mod = 0x7fffffff;  // force phase-increment recompute
      }
      break;
    }
    case events::EventType::kSetBlockSize:
      block_size = ev.payload.io.block_size;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// dispatch_param — the portable FUN_180028fe0 subset (param ids per
// params.gen.h). Voice-targeted params are applied to all 32 slots
// (control rate); arp / LFO / allocator / bend / master params route to
// their owners.
// ---------------------------------------------------------------------------
void Engine::apply_param_events(const std::vector<events::Event>& evs) {
  for (const events::Event& ev : evs) {
    if (ev.type == events::EventType::kParameterSet)
      dispatch_param((int)ev.payload.param.param_id, ev.payload.param.raw_value,
                     ev.payload.param.init_inject != 0);
    else if (ev.type == events::EventType::kProgramLoad)
      load_program(ev.payload.load.rows, ev.payload.load.count,
                   ev.payload.load.program);
  }
}

void Engine::dispatch_param(int id, int band, bool init_inject) {
  raw_params[id] = (std::int32_t)band;
  norm_params[id] = (float)band * (1.0f / 127.0f);
  // Init-sound values (upgrade injection / boot replay) bypass the clamp:
  // the device applies them through a store+setter path that never runs
  // FUN_180028fe0's range step (083 p74=32 -> ALWAYS-glide #112; boot
  // 73=32 -> unison OFF; boot 93=9 -> N=2 #57).
  if (!init_inject) {
    // m3-engine-events: the 32-bit param dispatcher IS FUN_10025940
    // (:29090). Its header (:29127-29134) adds the slot modulation to
    // EVERY write of a param that is a slot's dest:
    //   v += (hi-lo+1) * slot.value * slot.sens   (per matching slot)
    // then round + clamp [lo,hi] (range table FUN_10031520 = the same
    // table as dispatch_clamp). At program load the slot values are 0,
    // so the delta is 0 and the frozen-host renders are unchanged; the
    // law matters for event re-notifies and load-time re-notify (#164).
    const params::DispatchRange& dr = params::kDispatchRanges[id];
    if (dr.min != -1 || dr.max != -1) {
      float v = (float)band;
      bool driven = false;
      for (int s = 0; s < 2; ++s) {
        if (mod_wheel.slot[s].assign == id) {
          v += (float)(dr.max - dr.min + 1) *
               mod_wheel.slot[s].value * mod_wheel.slot[s].sens;
          driven = true;
        }
      }
      if (driven) band = (int)std::lround(v);
      static const bool dbg_delta = std::getenv("S1_DBG_DELTA") != nullptr;  // #235
      if (driven && dbg_delta)
        std::fprintf(stderr, "DELTA id=%d band=%d v=%g s0{a=%d,val=%g,sen=%g} s1{a=%d,val=%g,sen=%g}\n",
                     id, band, v, mod_wheel.slot[0].assign, mod_wheel.slot[0].value,
                     mod_wheel.slot[0].sens, mod_wheel.slot[1].assign,
                     mod_wheel.slot[1].value, mod_wheel.slot[1].sens);
    }
    band = dispatch_clamp(id, band);  // consumers only
  }

  for (auto& v : voices) {
    switch (id) {
      case 0: v.osc_params.osc1_wave = band; break;   // 0 sine 1 saw 2 pulse 3 tri
      case 1: v.osc_params.osc2_wave = band; break;   // 1 saw 2 pulse 3 tri 4 noise
      case 2: v.set_osc2_pitch_band(band); break;
      case 3: v.set_osc2_fine_band(band); break;
      case 4: v.set_osc2_kbd_track(band != 0); break;
      case 5: v.set_osc_mix_band(band); break;          // FUN_18003b8c0 law
      case 8: v.set_pulse_width_band(band); break;      // pulseWidth = band/127
      case 91: v.set_osc_phase_band(band); break;       // FUN_18003e390 (case 0x5b)
      case 9: v.set_key_shift(band); allocator.set_key_shift(band); break;  // semitones −24..+24; CVco+0x15c (FUN_10036670) feeds glide delta FUN_10036730
      case 6: v.osc_params.sync = band != 0; break;
      case 7: v.osc_params.ring = band != 0; break;
      case 10: v.set_mod_env_on(band != 0); break;
      case 11: v.set_mod_env_amount_band(band); break;
      case 12:
      case 13: v.set_mod_env_bands(raw_params[12], raw_params[13]); break;
      case 14: v.filt.set_filter_type(band); break;
      case 15: case 16: case 17: case 18:
        v.set_filter_env_bands(raw_params[15], raw_params[16],
                               raw_params[17], raw_params[18]);
        break;
      case 19: v.set_filter_cutoff_band(band); break;
      case 20: v.set_filter_resonance_band(band); break;
      case 21: v.set_filter_env_amount_band(band); break;
      case 22: v.set_filter_keytrack_band(band); break;
      case 23: v.set_filter_saturation_band(band); break;
      case 24: v.set_filter_vel_on(band != 0); break;
      case 25: case 26: case 27: case 28:
        v.set_amp_env_bands(amp_attack_src_band(raw_params), raw_params[26],
                            raw_params[27], raw_params[28]);
        break;
      case 29: v.set_amp_gain_band(band); break;
      case 30: v.set_amp_vel_sens_band(band); break;
      case 45: v.set_fm_amount_band(band); break;
      case 76:  // osc1 detune (law B #62): FUN_180028fe0 case 0x4c
                // (decomp_all.c:29318): fVar16 = (float)band *
                // DAT_180072a00(1/127) * DAT_180074a40(20.0), applied to
                // all 32 voices via FUN_18003e470 (decomp_all.c:38650:
                // stores cv+0x104; branch gate 0.0 < detune, §6.8/§8).
        v.osc_params.detune = (float)band * (1.0f / 127.0f) * 20.0f;
        v.regen_detune();  // law K #165: setter regenerates the Hz slots iff >0
        break;
      case 71: v.set_mod_dest(band); break;
      case 72: v.set_fine_tune_band(band); break;
      case 95: v.set_sub_gain_band(band); break;
      case 96: v.set_sub_shape(band); break;
      case 97: v.set_sub_octave(band); break;
      default: break;
    }
  }

  switch (id) {
    case 25: case 28:
      // Keep the allocator's steal-level envelope times in sync.
      allocator.set_env_times(
          env::seconds_to_samples(env::amp_attack_seconds(amp_attack_src_band(raw_params)), sample_rate),
          env::seconds_to_samples(env::amp_release_seconds(raw_params[28]), sample_rate));
      break;
    case 31: arpeggiator.set_mode(band); break;   // 0 up, 1 updown, 4 random
    case 32: arpeggiator.set_octaves(band); break;
    case 33: arpeggiator.set_beat_index(band); break;
    case 34: arpeggiator.set_gate(band); break;
    case 38: allocator.set_play_mode((voice::PlayMode)band); break;
    case 94: allocator.set_polyphony(band); break;  // FUN_1800370e0
    case 39: allocator.set_porta_band(band); break;
    case 40: pitch_bend.set_range(band); break;
    case 41: lfo1.destination = band; lfo::depth_to_range(lfo1); break;
    case 42: lfo::set_wave(lfo1, band); break;  // FUN_180015450
    case 43: lfo1.speed = band; lfo::speed_to_hz(lfo1, tempo_bpm); break;
    case 44: lfo1.depth = band; lfo::depth_to_range(lfo1); break;
    case 46: lfo2.destination = band; lfo::depth_to_range(lfo2); break;
    case 47: lfo::set_wave(lfo2, band); break;  // FUN_180015450
    case 48: lfo2.speed = band; lfo::speed_to_hz(lfo2, tempo_bpm); break;
    case 49: lfo2.depth = band; lfo::depth_to_range(lfo2); break;
    case 50: mod_wheel.set_sens(0, midi::sens_from_raw(band)); re_notify_slot(0); break;  // FUN_100319a0
    case 51: mod_wheel.set_sens(1, midi::sens_from_raw(band)); re_notify_slot(1); break;  // FUN_100319a0
    case 57: lfo1.onOff = band != 0 ? 1 : 0; break;
    case 58: lfo2.onOff = band != 0 ? 1 : 0; break;
    case 59: {  // arpeggiator on/off (FUN_180001ec0): release the live arp note
      const bool on = band != 0;
      if (!on && arpeggiator.enabled()) arp_release_note();
      arpeggiator.set_enabled(on);
      break;
    }
    case 67: lfo1.tempoSync = band != 0 ? 1 : 0; lfo::speed_to_hz(lfo1, tempo_bpm); break;
    case 68: lfo1.keySync = band != 0 ? 1 : 0; break;
    case 69: lfo2.tempoSync = band != 0 ? 1 : 0; lfo::speed_to_hz(lfo2, tempo_bpm); break;
    case 70: lfo2.keySync = band != 0 ? 1 : 0; break;
    case 73:  // unison mo + EQ gate (FUN_180037150: on = (param == 1))
      fx_chain.set_param(id, band);
      unison_on_ = (band == 1);
      // The mono/legato note-off branch re-reads M+0x110/M+0x114 per event
      // (FUN_10030c20:34955-34959 runs for note-offs too, :34961-34972), so
      // the allocator's first-N release width tracks the live params.
      allocator.set_unison_vpn(unison_on_ && unison_count_ >= 2 ? unison_count_ : 1);
      recompute_voice_pans();
      recompute_voice_detune();
      break;
    case 74:  // portamento mode (#112): raw band stored verbatim per voice
      // (setter case 0x4a, spec_env.md §6.4) and tested with STRICT equality
      // at note-on: FUN_10047e20 (decomp_all_32_full.c:50205) / FUN_1800526e0
      // (decomp_all.c:50898) `portaEnable = (voice+0x420 == 1) ? gate_held : 1`
      // → raw == 1 = legato/auto; ANY other raw value (0, 2..127 — incl. the
      // boot init 32) glides on EVERY note-on. There is no "off" value of
      // p74: porta off = p39 == 0 (dur 0 — glide evaluator FUN_10036250
      // :38980 forces rem = 0 when pos >= dur).
      allocator.set_porta_mode(band == 1 ? voice::kPortaAuto : voice::kPortaOn);
      break;
    case 75:  // unison detune (FUN_180037130): raw 0..127 → 0..100 cents
      recompute_voice_detune();
      break;
    case 84:  // unison pa spread (FUN_180037250): (x−64)/64
      recompute_voice_pans();
      break;
    case 85:  // unison pitch offset (FUN_1800371a0): (raw−24)·100 cents
      recompute_voice_detune();
      break;
    case 93: {  // unison vo count (FUN_1800371d0): the device stores the
      // dispatch-clamped band (table [2, 8], #134) VERBATIM and FUN_180036460
      // uses it AS the voice count — loop bounds, % count tap pick, and the
      // /(N−1) pan/detune spread (decomp_all.c:33069, 33191-33192, 33229-
      // 33231, 33261). The old 2+round(raw·6/127) form is the UI DISPLAY
      // mapping (boot raw 9 -> display 2), not the consumer law; with the
      // dispatch clamp its input is already 2..8, so the consumer is the
      // clamped band itself (file domain: bank02 93=2 -> N=2; upgrade law
      // p38->93 writes 2/4, preset.cpp).
      int n = band;  // dispatch-clamped to [2, 8]
      if (n < 2) n = 2;    // defensive: table guarantees 2..8
      if (n > 8) n = 8;
      unison_count_ = n;
      allocator.set_unison_vpn(unison_on_ && unison_count_ >= 2 ? unison_count_ : 1);
      recompute_voice_pans();
      recompute_voice_detune();
      break;
    }
    // m3-u4c-land (#164 LANDED): the src/assign setters carry the device
    // re-notify arms (FUN_100319e0 src set + current-assign re-notify;
    // FUN_100318f0 old+new assign re-notify; FUN_100319a0 sens + re-notify).
    // A re-notified dispatch of 43/48/67/69 steps freqCur ONLY iff
    // freqCur != freqTarget (FUN_10013140 :14196) — with the ctor boot
    // replay + one sr step now modelled, the load-time convergence state
    // matches the device and the b_lfo1/b_lfo2 1237 pin holds.
    case 86:
      if (midi::ModWheel::src_in_table(band)) {
        mod_wheel.set_src(0, band);
        re_notify_slot(0);
      }
      break;
    case 87:
      set_ctrl_assign(0, band);
      break;
    case 88:
      if (midi::ModWheel::src_in_table(band)) {
        mod_wheel.set_src(1, band);
        re_notify_slot(1);
      }
      break;
    case 89:
      set_ctrl_assign(1, band);
      break;
    case 224: {  // pitch bend (FUN_180028fb0): 7-bit approximation, center 64
      // m3-engine-events: arms the chunk-cap/slew flag exactly like the
      // 64-bit setter (uVar1 = current != target). Never dispatched at
      // program load (224 is not a file/boot id) -> frozen renders inert.
      int t = band - 64;
      if (t < -63) t = -63;
      if (t > 64) t = 64;
      pitch_bend.target = t;
      pitch_bend.slewing = pitch_bend.current != t;
      break;
    }
    case MASTER_VOL_PARAM: master_vol.set_raw(band); break;  // 0xfe

    // Post-voice FX chain params (dsp_flow.md §3, fx_chain.h param map):
    // delay 35/36/37/65/82/83/98, chorus 52..56/64/66, EQ 60..63/73,
    // effector 77..81, pan 90.
    case 35: case 36: case 37:
    case 52: case 53: case 54: case 55: case 56:
    case 60: case 61: case 62: case 63: case 64: case 65: case 66:
    case 77: case 78: case 79: case 80: case 81: case 82: case 83:
    case 90:
    case 98:
      fx_chain.set_param(id, band);
      break;
    default: break;  // global params without a portable consumer yet
  }
}

}  // namespace synth1::engine
