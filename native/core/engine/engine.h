// engine.h — the Synth1 engine: events → sample-accurate sub-blocks.
//
// Spec: dsp_flow.md §1/§2/§5 (control plane, per-voice chain, one-line
//       path), spec_env.md §7 (32-voice pool, lifecycle, stealing),
//       spec_lfo_fx.md §1 (2× LFO routing), §7 (arpeggiator), §8 (play
//       mode), §13 (per-block evaluation order); spec_mastervol.md
//       (master volume tail + anti-zipper smoothing).
// Ghidra: the 99-case dispatcher FUN_180028fe0 (portable subset below);
//         common-settings FUN_1800288e0 (param 0xfe master volume);
//         CVco/CVcf/CVca voice stride 0x4a8 × 32 slots.
//
// Design (plan §5):
// - The engine splits each block at event offsets and arpeggiator gate /
//   step edges, and advances the control plane (LFOs, pitch-bend slew,
//   allocator glide) per sub-block.
// - NO allocations during process() (allocation trap after init): the
//   wavetable bank is fetched once in prepare(), the mix scratch is a
//   fixed member array, and every subsystem is POD / fixed-capacity.
// - No virtual dispatch in the hot path.
//
// Wiring (dsp_flow.md §5):
//   MIDI → Arpeggiator (param 59 on) or VoiceAllocator (poly/mono/legato,
//   param 38) → 32 × CVoice (osc → filter → amp) → Σ → MasterVolume → out.
//   LFO1/LFO2 route pitch / cutoff / amp / p-w sums into every active
//   voice per sub-block; PitchBend slews ±1/sample (FUN_180028300) and
//   feeds the per-voice 1/128-octave bend offset (spec_env.md §6.5).
//
// The post-voice CSignalDevice chain (effector / EQs / pan / delay /
// chorus, dsp_flow.md §3) is wired as fx::FxChain: the voice pool sums
// mono, the block is duplicated to L/R, the chain runs in place, then the
// master-volume tail (FUN_1800285a0) scales both channels (one-pole gain
// advanced ONCE per frame).

#pragma once

#include <cstdint>
#include <vector>
#include "../fx/fx_chain.h"
#include "events/event.h"
#include "voice/voice.h"
#include "voice/voice_allocator.h"
#include "arp/arpeggiator.h"
#include "lfo/lfo.h"
#include "midi/mod_wheel.h"
#include "master_volume.h"
#include "tables/wavetable_gen.h"

namespace synth1::engine {

// Maximum voices (32 slots × 0x4A8 in the decompile; spec_osc.md §1).
// Kept as an engine-level alias for backward compatibility.
inline constexpr std::uint32_t MAX_VOICES = voice::MAX_VOICES;

// Fixed mix scratch per voice (no allocations in process()). Sub-blocks
// longer than this are rendered in chunks — chunk boundaries are derived
// from sub-block starts (absolute event/edge offsets), so block-size
// invariance (test_invariance) is preserved.
inline constexpr int ENGINE_SCRATCH_FRAMES = 1024;

// Fixed right-channel scratch for the mono process() overloads (the left
// channel renders straight into the caller's output). Mono blocks longer
// than this are tiled into windows; window boundaries are not control
// cuts, so block-size invariance is preserved.
inline constexpr int ENGINE_FX_SCRATCH_FRAMES = 32768;

// Fixed control-rate tick (validation_report.md §6). The control plane
// (LFO phase/eval, pitch-bend slew, porta glide sample points, per-voice
// modulation routing) is re-evaluated on a tick grid of CONTROL_TICK_FRAMES
// samples anchored at ABSOLUTE sample 0 and intersected with the absolute
// event / arp-edge offsets. The tick sequence is therefore a function of
// absolute sample position only — never of the caller's block size. This is
// what makes bs 64/128/256/512 bit-exact for patches with continuous
// modulation (bank0_005 Rhodes, bank0_020 Church Organ).
inline constexpr std::int32_t CONTROL_TICK_FRAMES = 32;

// The device's voice processor caps one process-loop chunk at 256 frames and
// returns the capped length, which the process loop then feeds to the LFO
// advance+slew (FUN_1800155d0 :14117-14119 `iVar27 = 0x100; if (param_2 <
// 0x100) iVar27 = param_2;` = 32-bit FUN_100135b0 :14540-14542, called from
// FUN_1800285a0 :28260-28267 / FUN_10024ea0 :28536-28545). The LFO startup
// freq-slew + phase rescale therefore commits once per <=256-frame chunk:
// twice per 512-block, once per block at bs<=256 (law #85).
inline constexpr std::int32_t DEVICE_PROCESS_CHUNK_FRAMES = 256;

// law #123: the same 0x100 cap bounds the CVca/CVcf gain-apply CALL budget
// itself. FUN_100135b0 returns local_10 = min(param_1, 0x100)
// (decomp_all_32_full.c :14540-14542 + :14714 `return local_10;`) and the
// caller loop FUN_10024ea0 advances by the returned length and re-enters at
// that cut (:28536-28545 `iVar9 = (**...+0xc)(iVar9,...); iVar3 -= iVar9`),
// so the env process FUN_1000b3a0 receives that <=256 budget (:8131
// `local_8 = param_1`) and the walk FUN_1000b580 re-evaluates the rounded
// level at EVERY call cut (:8366-8390, step = min(+0x60, call-rem,
// stage-rem)). The env-call span therefore also cuts at the 256 edges —
// corrects the law-90-era "FUN_1800155d0 internal buffering" ruling
// (reports/123_lfo_grid_settle.md §1/§2/§5).
// m3-258 (#258, tasks/m3-258-cutset F2): the 0x100 cap anchors at the
// enclosing HOST-range start — law #159-L1 as amended by
// m3-freeze-anchor-land, the same anchoring gain_call_cap82 /
// flt_call_cap172 use. The absolute-256 anchor cut the CVco walk at
// edges the device does not make (first at abs 22272 on 062 voice 1:
// engine 22272 vs device 22306 = 22050+256). s1/s2 are the segment cuts
// of the CVco-ROUTED LFOs only (lfo_wired_cvco, FUN_100345b0
// dest∈{1,2,5,6}); w1/w2 the wave-3/4 wrap caps.
inline std::int32_t amp_call_cap123(std::int64_t abs,
                                    std::int64_t range_start_abs,
                                    std::int32_t block_rem,
                                    std::int32_t rem, std::int32_t s1,
                                    std::int32_t s2, std::int32_t w1,
                                    std::int32_t w2) {
  std::int32_t cap = block_rem;
  if (rem < cap) cap = rem;
  if (s1 < cap) cap = s1;
  if (s2 < cap) cap = s2;
  if (w1 < cap) cap = w1;
  if (w2 < cap) cap = w2;
  const std::int32_t c256 = DEVICE_PROCESS_CHUNK_FRAMES -
                            (std::int32_t)((abs - range_start_abs) %
                                           DEVICE_PROCESS_CHUNK_FRAMES);
  if (c256 < cap) cap = c256;
  if (cap < 1) cap = 1;
  return cap;
}

// law #82 (LANDED; reports/82_sinedemod.md §3/§4, settle
// tasks/m2b-law82-gaingrid-2 RESULT F3): the CVca GAIN-APPLY call span
// is the device's block-anchored grid — FUN_1800155d0 :14113-14116
// `iVar27 = 0x100; if (param_2 < 0x100) iVar27 = param_2;` chunks the
// voice process call at min(host-block-rem, 0x100) ANCHORED TO THE HOST
// RENDER BLOCK, and the CVca driver FUN_18000ca70 (:8192-8310) splits
// the walk ONLY at the event distances of the LFOs WIRED to this env
// (param_1[0xe]/[0xf] + flags +0x80/+0x84 via FUN_180014ce0) —
// modulation events, NOT wave wraps (FUN_180014cd0's period wrap caps
// the LFO ADVANCE chunk, law #85, never the env call). The engine's
// single amp_call_cap123 cut the CVca span at EVERY LFO's segment
// boundaries (s1/s2: every 32/64 f for continuous waves) and wave-3/4
// wraps regardless of destination — 083's LFOs are dest 5 (PW) and
// 121's are dest 2/3, so the device cuts neither on their CVca leg
// while the engine did (gaingrid-2 F3: "the DEST_AMP gate was
// byte-right; the caller-block size was the wrong part"). The 256 cap
// STAYS (law #123's ENV budget stands: FUN_100135b0 :14540-14542/
// :14714; the device's bs512==bs256 62-64 Hz floors prove the <=256
// caller block). amp_call_cap123 remains the CVcf/CVco env-walk grid
// (law-121 062 dump: wrap cuts verified there; report §4(ii) "keep
// wrap cuts for CVco/CVcf only").
inline bool lfo_wired_to_amp(const lfo::LfoState& l) {
  return l.onOff != 0 && l.destination == lfo::DEST_AMP;
}

// law #172 (tasks/consults/m3-172-cvcf-level-gate4-stop.answer.md §2): the
// CVcf env-walk CALL cap is the law-123 grid DESTINATION-GATED exactly the way
// law #82 gated the CVca gain call. FUN_1000b3a0 (:8160-8193) splits a call
// ONLY for the LFOs flagged on THAT object (+0x6c L1 / +0x70 L2); the CVcf
// flags are set by FUN_10032280 (:36247-36261) iff the LFO is on AND its
// destination == 3 (DEST_FILTER). Unwired LFOs contribute NO segment cuts and
// NO wave-wrap cuts to the CVcf walk (device -envtap: 083/062 CVcf calls at
// {0,256} per block; 121's filter-wired LFO2 cuts it at its segment
// distances — tasks/m3-172-cvcf-level-land-2-rc RESULT Finding C). The CVco
// leg keeps the wave-wrap cuts and gates its segment cuts by
// lfo_wired_cvco (m3-258; law-121 062 wrap cuts were on the CVco walk).
inline bool lfo_wired_to_filter(const lfo::LfoState& l) {
  return l.onOff != 0 && l.destination == lfo::DEST_FILTER;
}

// The CVcf env-call cap: block-rem + 256 + range end + segment cuts of
// FILTER-WIRED LFOs only. No wave-wrap cuts. law #159-L1 anchoring
// (amended m3-freeze-anchor-land): the 0x100 cap re-anchors at the
// enclosing HOST-range start (FUN_100173a0 re-entry per host-event
// segment, pieces of min(seg_rem, 0x100) from the segment start; the
// FUN_10024ea0 arp/LFO sub-cuts do NOT re-anchor — device
// -envtap 083: note-off at 17640 re-arms the CVcf call at
// {17640, 17896=17640+256}, not the absolute 256 edge 17920).
inline std::int32_t flt_call_cap172(std::int64_t abs, std::int64_t range_start_abs,
                                    std::int32_t block_rem,
                                    std::int32_t rem, std::int32_t s1,
                                    std::int32_t s2) {
  std::int32_t cap = block_rem;
  if (rem < cap) cap = rem;
  if (s1 < cap) cap = s1;
  if (s2 < cap) cap = s2;
  const std::int32_t c256 = DEVICE_PROCESS_CHUNK_FRAMES -
                            (std::int32_t)((abs - range_start_abs) %
                                           DEVICE_PROCESS_CHUNK_FRAMES);
  if (c256 < cap) cap = c256;
  if (cap < 1) cap = 1;
  return cap;
}

// m3-258 (#258, tasks/m3-258-cutset F2; advisor-verified): the CVco
// (mod-env) walk routing. FUN_100345b0 (decomp_all_32_full.c :37627-37637)
// sets the CVco object's +0x6c/+0x70 segment-cut flags (written by
// FUN_1000b1f0 :7999) iff the LFO is on AND routed to the mod-env consumer
// family: destination in {1 OSC2, 2 OSC12, 5 PW, 6 FM}
// (`param_3==1||param_3==2||param_3==5||param_3==6`). FUN_1000b3a0
// (:8160-8193) then splits the CVco walk call at that LFO's segment
// distances (FUN_10012be0). This is NOT lfo_wired_any ({1,2,3,4,6}):
// dest 5 (PW) IS CVco-wired (062's LFO1: the missing device cuts at the
// LFO1 vertices 864/928/992/1088...), dests 3/4 are NOT.
inline bool lfo_wired_cvco(const lfo::LfoState& l) {
  return l.onOff != 0 &&
         (l.destination == lfo::DEST_OSC2 || l.destination == lfo::DEST_OSC12 ||
          l.destination == lfo::DEST_PW || l.destination == lfo::DEST_FM);
}

// m3-u4c-resume-land (m3-lfo-nodecut-law F1): the CVca wired flag CVca+0x6c/
// +0x70 is set when the LFO is routed to the voice at all (any destination,
// non-degenerate range = amount != 0) — the FUN_1000b3a0 chunk walk then
// cuts at that LFO's floored segLen (FUN_10012c00).
// The law's consumer family (m3-lfo-nodecut-law F4): amp / fc / pitch / FM
// index — dests 1,2,3,4,6. PW (5) is a kernel-sum consumer on the validated
// read grid (law #82: the device cuts neither CVca leg for dest-5 LFOs);
// PAN (7) is the chain stage (U4-b). Neither joins the node-cut walk.
inline bool lfo_wired_any(const lfo::LfoState& l) {
  return l.onOff != 0 && l.lo != l.hi &&
         (l.destination == lfo::DEST_OSC2 || l.destination == lfo::DEST_OSC12 ||
          l.destination == lfo::DEST_FILTER || l.destination == lfo::DEST_AMP ||
          l.destination == lfo::DEST_FM);
}

// The CVca gain-call cap: block-rem + 256 + range end + segment cuts of
// AMP-WIRED LFOs only. No wave-wrap cuts.
// law #159-L1 (m3-159b-law §b, amended m3-freeze-anchor-land): the 0x100
// cap is anchored at the enclosing HOST-range start (FUN_100173a0
// re-entry per host-event segment), not at absolute 256 edges and NOT at
// arp sub-segment starts. FUN_100135b0 :14539-14542
// `local_10 = 0x100; if (param_1 < 0x100) local_10 = param_1;` bounds
// EACH voice-processor call at min(range-rem, 0x100) from the call's own
// start; FUN_100173a0 renders each host segment in min(seg_rem, 0x100)
// pieces FROM THE SEGMENT START, so a host event re-anchors the steps
// while an arp edge is an in-piece sub-cut inside FUN_10024ea0
// (:28497-28545, iVar3 -= iVar9 keeps the piece budget) and does not.
// A note-off therefore does NOT create a CVca
// gain-call boundary at the event-split edge (measured: device hook at
// event+256 = 17896 for the block-34 release, engine pre-fix 17664).
// For ranges starting at a block edge (bs multiple of 256) this is
// identical to the absolute anchoring — all law-82/90/123 pins unchanged.
inline std::int32_t gain_call_cap82(std::int64_t abs, std::int64_t range_start_abs,
                                    std::int32_t block_rem,
                                    std::int32_t rem, std::int32_t s1,
                                    std::int32_t s2) {
  std::int32_t cap = block_rem;
  if (rem < cap) cap = rem;
  if (s1 < cap) cap = s1;
  if (s2 < cap) cap = s2;
  const std::int32_t c256 = DEVICE_PROCESS_CHUNK_FRAMES -
                            (std::int32_t)((abs - range_start_abs) %
                                           DEVICE_PROCESS_CHUNK_FRAMES);
  if (c256 < cap) cap = c256;
  if (cap < 1) cap = 1;
  return cap;
}

// The engine state.
struct Engine {
  float sample_rate = 44100.0f;
  std::int32_t block_size = 64;
  std::int64_t absolute_sample = 0;  // running absolute sample counter
  float tempo_bpm = 120.0f;          // engine+0xbb48 (LFO tempo sync)

  // ---- the wired subsystems ----
  voice::Voice voices[voice::MAX_VOICES];  // 32-voice pool (CVoice chain)
  voice::VoiceAllocator allocator;         // poly/mono/legato + stealing + porta
  arp::Arpeggiator arpeggiator;            // note re-voicer (param 59 on/off)
  lfo::LfoState lfo1, lfo2;                // 2× CLFO (spec_lfo_fx.md §1)
  midi::PitchBend pitch_bend;              // 14-bit bend + ±1/sample slew
  midi::ModWheel mod_wheel;                // CC1 two-slot wheel controller
  MasterVolume master_vol;                 // param 0xfe tail gain
  fx::FxChain fx_chain;                    // post-voice chain (dsp_flow.md §3)

  // Current raw parameter values (255 slots; plan §4).
  std::int32_t raw_params[255] = {};  // dispatcher bands (signed)
  // m3-u4c-land: effSetProgram state (FUN_10024640 dup-guard pair +
  // current program number; program 0 = the init sound).
  int current_program_ = 0;
  int prev_disp_id_ = 0;
  int prev_disp_val_ = 0;
  // Normalized 0..1 values (for DSP use; plan §4 normalized↔physical).
  float norm_params[255] = {};

  // Arpeggiator voice tracking (portable engine state): the note the arp
  // currently sounds through the allocator, and the step-relative sample
  // position where its gate closes (spec_lfo_fx.md §7.3).
  int arp_note = -1;
  int arp_gate_off_at = 0;
  int arp_notes_emitted = 0;
  bool live_keys_[128] = {};   // live keyboard (engine+0x9d58) key-down flags
  int live_key_count_ = 0;     // keyboard+0x08 held-key count  // diagnostic: arp note-ons through the engine

  // m3-u4c-land-4 R1 (split law, tasks/m3-u4c-split/RESULT.md F1/F5):
  // the ctor/entry replay store is the boot table MERGED with the
  // PROGRAM-0 file (001.sy1 of bankfolder0 — the host INI
  // hostselectprogram=1 entry load). The producer (render host) supplies
  // the upgraded program-0 rows as flat (id,val) pairs BEFORE prepare();
  // rows must outlive prepare(). nullptr/0 = boot table only.
  void set_boot_program(const std::int32_t* rows, int count) {
    boot_prog_rows_ = rows;
    boot_prog_count_ = count;
  }

  // prepare(): set sample rate and block size (called once at init).
  // Builds the global wavetable bank HERE — never in process().
  void prepare(float sample_rate, std::int32_t block_size);

  // Process a stereo block of samples with events. NO allocations.
  // The engine splits the block at event offsets (block-relative) and at
  // arpeggiator gate/step edges, rendering each sub-block through the
  // active voices (dsp_flow.md §5, spec_lfo_fx.md §13). The voice pool
  // sums into stereo `left`/`right` with the per-voice linear pan law
  // (voice_pan_, FUN_180015a20), the post-voice fx_chain runs IN PLACE
  // over (left, right), and the master-volume tail scales both channels
  // (one-pole advanced once per frame).
  void process(float* left, float* right, std::int32_t num_frames,
               const events::Event* events, std::size_t num_events);

  // Process a stereo block of samples with no events.
  void process(float* left, float* right, std::int32_t num_frames);

  // Backward-compatible mono overloads: the stereo render runs through a
  // fixed scratch (the right channel) and the LEFT channel is written to
  // `output`. Blocks longer than the scratch are tiled into windows —
  // bit-identical (the control tick grid is absolute-sample anchored).
  void process(float* output, std::int32_t num_frames,
               const events::Event* events, std::size_t num_events);

  // Process a block of samples with no events.
  void process(float* output, std::int32_t num_frames);

  // Reset: clear all voices and state.
  void reset();

  // Diagnostics / wiring access.
  int arp_current_note() const { return arp_note; }
  int active_voice_count() const { return allocator.active_count(); }

  // Offline preset-state audit (tools/param_state_audit.py): apply preset
  // ParameterSet events to the engine without rendering, and read back the
  // resulting dispatcher bands.
  void apply_param_events(const std::vector<events::Event>& evs);
  int raw_param(int id) const {
    return (id >= 0 && id < 255) ? raw_params[id] : 0;
  }
  int master_vol_raw() const { return master_vol.raw; }

 private:
  const tables::WavetableBank* bank_ = nullptr;  // set in prepare()
  float scratch_[ENGINE_SCRATCH_FRAMES];         // per-voice mix scratch
  float mono_right_[ENGINE_FX_SCRATCH_FRAMES];   // mono overload: right channel

  // Per-voice pan (voice+0x488, FUN_180037250): assigned by the unison
  // spread law; the mixer sums voices into stereo with the linear pan law
  // of FUN_180015a20 (pan>0: gL=max(1−pan,1e-7), gR=1; pan<0: gL=1,
  // gR=max(1+pan,1e-7); DAT_180073e18 = 1e-7).
  float voice_pan_[voice::MAX_VOICES] = {};
  bool unison_on_ = false;   // engine+0x194 (param 73 == 1, FUN_180037150)
  int unison_count_ = 1;     // param 93 → 2..8 (FUN_1800371d0 display)

  // Current stereo window buffer bases (process_stereo_window sets them;
  // render_tick_audio derives the right channel from the left pointer).
  float* win_left_ = nullptr;
  float* win_right_ = nullptr;

  // Absolute sample of the current render window's first frame
  // (process_stereo_window sets it to absolute_sample + wb). All positions
  // handed to render_range / render_sub / cut_control_tick are relative to
  // this base (== the buffer base of the window).
  std::int64_t render_abs_base_ = 0;

  // m3-freeze-anchor-land: absolute sample of the enclosing HOST-range start
  // (the render_range entry `begin`). FUN_100173a0 re-enters per host-event
  // segment and re-anchors the 0x100 voice-call grid there; arp/LFO cuts
  // inside FUN_10024ea0 sub-cut a piece but do NOT re-anchor it.
  std::int64_t host_range_anchor_abs_ = 0;
  // m3-bendslew-lfo-land (ruling tasks/consults/m3-bendslew-lfo-scratch.
  // answer.md): device render-piece chain anchor (FUN_10024ea0 pieces chain
  // from the host-segment start; a 0x20 slew-capped piece re-anchors the
  // 0x100 FUN_100135b0 budget chain at its end).
  std::int64_t piece_anchor_ = 0;
  std::int64_t arp_edge_abs_ = -1;  // m3r-111-j1cond J4=2: last arp gate/step edge
  std::int64_t wrap_head_abs_ = -1;  // m3r-124-land: next chunk head after a wave-3/4 wrap cut

  // In-progress control tick (render_sub): tick_start_abs_ = absolute sample
  // of the tick's first frame, tick_len_ = frames in it, tick_done_ = frames
  // rendered so far. tick_done_ >= tick_len_ means no tick is in progress.
  std::int64_t tick_start_abs_ = 0;
  std::int64_t cur_event_abs_ = 0;  // m3-sse-lanes-land: abs of event applied
  std::int32_t tick_len_ = 0;
  std::int32_t tick_done_ = 0;

  // law #90: frames from the current tick's first sample to the next DEVICE
  // CVca amp cut (render-chunk remainder / modulation-event distance / range
  // end — everything the tick cuts at except the CONTROL_TICK_FRAMES base).
  // The CVca gain chunk spans ticks up to this cap; every other modulation
  // consumer keeps the tick grid.
  std::int32_t tick_amp_cap_ = 0;
  // CVcf/CVco env-walk call cap (law-123 grid, 256 cut INCLUDED) — equal to
  // tick_amp_cap_ (the CVca leg keeps the 256 cut too, FUN_1800155d0
  // :14113-14116; tasks/m2b-law82-gaingrid-2 RESULT F3).
  std::int32_t tick_env_cap_ = 0;
  // law #172: CVcf env-walk call cap — the law-123 grid destination-gated to
  // FILTER-WIRED LFOs (flt_call_cap172). The CVco leg keeps tick_env_cap_.
  std::int32_t tick_flt_cap_ = 0;

  // Device render chunk (FUN_1800285a0): the LFO phase / frequency slew advance
  // is committed once per CHUNK, not per control tick. A chunk is the remaining
  // frames of the caller block (block_size, absolute-sample aligned) capped at
  // (period - phase) for an ON wave-3/4 LFO and at CONTROL_TICK_FRAMES while
  // the pitch bend slews; arp gate/step edges close a chunk, note events do
  // not. chunk_done_ >= chunk_len_ means no chunk is open.
  std::int64_t chunk_start_abs_ = 0;
  std::int32_t chunk_len_ = 0;
  std::int32_t chunk_done_ = 0;
  void open_chunk(std::int64_t abs);
  void close_chunk();

  // U4-b (#196): the CPan dest-7 chain stage runs per core chunk with the
  // LFO state as of the chunk start (FUN_10024ea0 advances the LFOs after
  // each chunk's voice-chain call). Record the state at every commit; the
  // window hands CPan one segment per chunk.
  struct PanLfoSeg {
    std::int64_t start_abs;
    lfo::LfoState a, b;
  };
  std::vector<PanLfoSeg> pan_lfo_segs_;
  std::vector<fx::CPan::LfoSeg> pan_segs_scratch_;

  // Render [begin, end) of the output block, splitting at arp edges.
  // end_is_edge: `end` is a control-cutting edge (event / arp edge offset);
  // false when it is only the end of the caller's block.
  void render_range(float* output, std::int32_t begin, std::int32_t end,
                    bool end_is_edge);
  // Render [begin, end) on the fixed control-tick grid: one control-plane
  // evaluation per CONTROL_TICK_FRAMES-aligned absolute tick (cut at event /
  // edge offsets); a tick may span block boundaries (tick_len_ / tick_done_).
  void render_sub(float* output, std::int32_t begin, std::int32_t end,
                  bool end_is_edge);
  // Advance the control plane at a tick's first sample (LFO/bend/porta eval +
  // routing into the active voices); no state advance.
  void begin_control_tick();
  // Advance the control plane by the `len` frames of the completed tick
  // (bend slew, LFO phase/freq advance + S&H re-seed, allocator).
  void commit_control_tick(std::int32_t len);
  // End the running tick at the current render position (an event offset) so
  // the control plane is re-evaluated there.
  void cut_control_tick(std::int32_t pos);
  // Render + sum the voice pool for `len` frames of the current tick into
  // the stereo window (per-voice linear pan law, FUN_180015a20).
  void render_tick_audio(float* output, std::int32_t len);
  // Re-assign voice_pan_[] (FUN_180037250): unison off → all 0; on →
  // spread·(2·((slot mod N)/(N−1)) − 1), spread = (param84−64)/64, N ≥ 2.
  void recompute_voice_pans();
  // Re-assign per-voice unison detune (FUN_180037130 / FUN_1800371a0).
  void recompute_voice_detune();
  // Apply the master-volume tail (one-pole per frame, spec_mastervol §4/§5).
  // The one-pole is advanced ONCE per frame and the gain scales BOTH
  // channels (never call it per channel: that would double the smoothing).
  void apply_master_gain(float* left, float* right, std::int32_t num_frames);

  // Stereo core: render the block window [wb, we) (block-relative offsets)
  // through the event-splitting logic into `left`, duplicate left into
  // `right` (buffer bases at wb), run fx_chain IN PLACE, then the
  // master-volume tail. absolute_sample is NOT advanced here.
  void process_stereo_window(float* left, float* right,
                             std::int32_t wb, std::int32_t we,
                             const events::Event* events, std::size_t num_events);

  // Apply one event (control-plane; may change voice/param state).
  void apply_event(const events::Event& ev);
  // The portable FUN_180028fe0 dispatcher subset (params 0–74, 224, 254).
  // init_inject: boot/init-sound value (upgrade injection) — skips the
  // #134 range clamp (the device's init path bypasses FUN_180028fe0).
  void dispatch_param(int id, int band, bool init_inject = false);
  // m3-engine-events: slot dest re-notify (FUN_10031700 tail /
  // FUN_100318f0 / FUN_100319e0): dispatch the dest's current raw
  // through the param dispatcher so the slot delta (FUN_10025940
  // header) is applied.
  void re_notify_slot(int s);
  // Assign setter FUN_100318f0: table-validated, old-dest re-notify
  // (value zeroed) + new-dest re-notify.
  void set_ctrl_assign(int s, int assign);

  // m3-u4c-land (#164 + U4-c): the load-sequence convergence law.
  // boot_replay_ctor: ctor replay of the fixed init table (DAT_1007f1e0
  // @file 0x7d1e0) through the dispatcher in BOOT-TABLE ORDER with the
  // assign slots at -1 (FUN_10024640 path; LFO1 +3 / LFO2 +2 slew steps).
  // load_program: the effSetProgram law — file rows merged into the
  // snapshot store, then the 99-row store replayed in boot-table order
  // with the consecutive-dup guard and the 50/51, 86/88, 87/89 re-notify
  // arms; NO-OP when program == the current program (c001/001.sy1).
  void boot_replay_ctor();
  void load_program(const std::int32_t* rows, int count, int program);
  const std::int32_t* boot_prog_rows_ = nullptr;  // R1 program-0 rows
  int boot_prog_count_ = 0;

  // Note routing.
  // E-place law (#189): the keyboard held-key bookkeeping and the LFO
  // key-sync resets are the note-on side effects; they are split out so
  // process_stereo_window can place them at event_sample − 1.
  bool note_on_keyboard(int note);
  void note_on_keysync_reset();
  void note_on_route(int note, int vel, bool first_key);
  void note_on_live(int note, int vel);
  // Shared internal note-on voicing (unison N-slot): used by note_on_live
  // AND arp_step_boundary (device vtable+0x14 dispatcher, FUN_10001ae0:861).
  void note_on_voiced(int note, int vel);
  void note_off_live(int note);
  int arp_step_boundary();
  void arp_release_note();
  void sync_voice_gates();
  // m3-porta-land: per-position event batching. process_stereo_window records
  // every live (non-arp) note event sharing a sample position, then flushes the
  // batch through the allocator held-key table (key_on/key_off) + ONE evaluate()
  // (FUN_10031070) and drives voices[] from the evaluation. gate_held clears only
  // in evaluate() → the gap-0 auto-glide fires (m3-porta-falsify §2/§3).
  void flush_note_position();
  // m3-porta-land: the current position's note events in ARRAY order (the
  // device pops events in queue order; the host emits off_i before on_{i+1} at
  // gap 0), replayed into the held-key table so the NET per-key state matches.
  struct PosNote { int note; int vel; bool is_on; };
  PosNote pos_notes_[voice::MAX_VOICES * 2] = {};
  int pos_notes_n_ = 0;

  // Route one LFO's current value into the per-voice modulation sums
  // (spec_lfo_fx.md §1.8; dest 6 FM / 7 pan have no portable consumer yet).
  void lfo_route(const lfo::LfoState& lfo, voice::ModInputs& mod,
                 std::int32_t offset);
};

}  // namespace synth1::engine
