// voice_allocator.h — 32-voice pool, allocation/stealing, play modes,
// portamento.
//
// Spec: spec_osc.md §1 (voice pool: 32 slots, stride 0x4a8), spec_env.md §7
//       (voice lifecycle: +0x480 active, +0x49c gate held, +0x4a0 key,
//       +0x4a4 velocity; re-trigger suppression when the held voice has the
//       same key; allocation walks the 32-voice array reusing gate-clear
//       voices; play mode = engine+0x18c, param 38: 0 poly, 1 mono/legato
//       retrigger-all, 2 legato trigger-only-unreleased), spec_env.md §6
//       (portamento: param 39 time law §6.1, glide curve §6.2
//       remaining = delta·e^(−4·t/dur)·(1−t/dur), delta in 1/128-octave
//       units via (key<<7)/12 §6.3, unfinished glides accumulate; param 74
//       mode #112: raw value stored, strict `== 1` test at note-on
//       (FUN_10047e20 :50205 / FUN_1800526e0 :50898) — 1 = glide only when
//       the gate is still held, ANY other raw value = glide every note-on;
//       "off" is p39 == 0, not a p74 value).
// Ghidra: FUN_1800526e0 (note-on handler), FUN_1800525f0 (note-off),
//         FUN_180052580 (voice state query), FUN_18003e0c0 (glide trigger),
//         FUN_18003e3d0 (porta time setter), FUN_18003dbe0 (glide law).

#pragma once

#include <cstdint>

#include "env/adsr.h"

namespace synth1::voice {

// Voice pool: 32 slots (spec_osc.md §1, spec_env.md §7).
inline constexpr int MAX_VOICES = 32;

// Play mode — param 38 (engine+0x18c, spec_env.md §7).
enum PlayMode {
  kPlayPoly = 0,    // one voice per note; steal when the pool is full
  kPlayMono = 1,    // one voice, envelope retriggered on every note-on
  kPlayLegato = 2,  // overlapping notes glide without envelope retrigger
};

// Portamento mode — param 74 semantics (#112, spec_env.md §6.4).
// The device stores the raw band and tests `== 1` at note-on; kPortaOff is
// not reachable from p74 (porta off = p39 == 0 → glide duration 0).
enum PortaMode {
  kPortaOff = 0,  // never glide (instant jump; internal only)
  kPortaOn = 1,   // every note-on glides (raw p74 != 1 — incl. 0 and 32)
  kPortaAuto = 2, // glide only when the gate is still held (raw p74 == 1)
};

// Portamento time law (spec_env.md §6.1, param 39):
//   portaTime(band) = e^(2.5·(band/127 − 1)) · (band/127) · 2.5 seconds
// band 0 → 0 (no glide), band 127 → 2.5 s.
float porta_time_seconds(int band);

// Pitch in 1/128-octave units (spec_env.md §6.3): (key << 7) / 12.
inline int key_pitch_unit(int key) { return (key << 7) / 12; }

// One voice slot in the pool (CVoice fields per spec_env.md §7 + the
// portamento subset of CVco §6.2).
struct VoiceSlot {
  bool active = false;         // voice+0x480
  bool gate_held = false;      // voice+0x49c
  int note = -1;               // voice+0x4a0 (current target key)
  int velocity = 0;            // voice+0x4a4
  std::uint64_t timestamp = 0;      // note-on order (age)
  std::uint64_t release_clock = 0;  // note-off order (steal tie-break)
  env::ADSR env;               // amp envelope; cur_level = envelope level

  // Portamento state (CVco+0x190…0x1b0 subset, spec_env.md §6.2).
  // current pitch = target_unit + remaining; remaining(0) = delta =
  // old − new (1/128-octave), remaining(dur) = 0 → lands on target.
  // #114 (glide_g4_trace.md §1): the offset is re-evaluated ONLY at the
  // edges of a glide-relative adaptive bucket grid — bucket/step/zone
  // are slot-persistent, re-initialized at each trigger.
  int target_unit = 0;
  int glide_key = 0;  // mirror of CVco+0x190 (last trigger key; boot 0; written
                      // ONLY by the glide trigger FUN_10036730 at 0x1003675c)
  int delta = 0;      // +0x1a0 total delta
  int remaining = 0;  // +0x19c remaining offset
  int pos = 0;        // +0x198 glide position (samples)
  int duration = 0;   // +0x194 glide duration (samples)
  int bucket = 0;     // +0x1a4 bucket index (pos/step at last eval)
  int step = 0;       // +0x1a8 bucket width (samples)
  int zone = 0;       // +0x1b0 zone edge (dur/10 → 25% → 55% → dur)
  // m3r-111-porta: the glide was started by a LEGATO retarget (gate held,
  // no envelope retrigger — FUN_10047e20 retarget branch) rather than a
  // fresh attack. Gates the J1/J3 porta condition.
  bool glide_retarget = false;
};

class VoiceAllocator {
 public:
  VoiceAllocator() = default;

  // FUN_180037150 / FUN_1800370e0 → FUN_180036360: a play-mode or
  // polyphony change re-derives the pool and resets the round-robin cursor
  // (engine+0x1b4 = −1).
  void set_play_mode(PlayMode m) {
    if (m != play_mode_) { play_mode_ = m; rr_cursor_ = -1; }
  }
  // Param 94 (FUN_1800370e0): polyphony = clamp(band, 1, 32) → engine+0x190;
  // the poly-mode allocation pool (engine+0x1b0) is this count.
  void set_polyphony(int band) {
    if (band < 1) band = 1;
    if (band > MAX_VOICES) band = MAX_VOICES;
    if (band != polyphony_) { polyphony_ = band; rr_cursor_ = -1; }
  }
  PlayMode play_mode() const { return play_mode_; }
  void set_porta_mode(PortaMode m) { porta_mode_ = m; }
  void set_porta_band(int band) { porta_band_ = band; }  // param 39
  void set_key_shift(int ks) { key_shift_ = ks; }  // param 9 -> CVco+0x15c (FUN_10036670)
  void set_sample_rate(float fs) { sample_rate_ = fs; }
  void set_env_times(int attack_samples, int release_samples) {
    attack_samples_ = attack_samples;
    release_samples_ = release_samples;
  }

  // Note-on: poly allocates the next free slot (stealing the quietest /
  // oldest-released voice when all 32 are busy); mono retriggers the single
  // voice; legato re-targets the held voice without an envelope retrigger.
  // Returns the voice index. Same-key retrigger of a held voice is
  // suppressed (spec_env.md §7).
  int note_on(int note, int velocity);

  // Unison allocation (FUN_1800526e0 walks the pool for N slots of one key):
  // poly allocation WITHOUT the same-key re-trigger suppression — used for
  // the 2nd..Nth slot of a unison note-on. Mono/legato fall back to note_on.
  int note_on_force(int note, int velocity);

  // U2 (#149): mono/legato unison note-on — FUN_10030c20 mode-1/mode-2
  // first-vpn-slot loops (decomp_all_32_full.c:34992-35031; 64-bit
  // FUN_180036930:33302-33350). Allocates slots 0..n-1 (piVar2 = param_1+1
  // stepping — NOT the round-robin chooser). FUN_10047e20 (note+gate+glide)
  // runs on EVERY slot in both modes; the attack (FUN_10047e60 group +
  // FUN_10047ea0) is unconditional in mono (:35001) and gated on the slot's
  // gate bit V+0x438==0 in legato (:35022). attacked[j] = true where the
  // slot took the attack branch (engine runs the full note-on trigger),
  // false where it was retargeted (legato hold).
  void note_on_first_n(int note, int velocity, int n, bool attacked[]);

  // The mono/legato note-off branch reads the unison params live
  // (FUN_10030c20:34955-34959 precedes the E+4==0 release loop :34961-34972):
  // it releases the FIRST vpn slots. The engine keeps this width in sync
  // (param 73/93 setters).
  void set_unison_vpn(int n) {
    if (n < 1) n = 1;
    if (n > MAX_VOICES) n = MAX_VOICES;
    unison_vpn_ = n;
  }
  int unison_vpn() const { return unison_vpn_; }

  // Note-off: poly releases the voice holding that note; mono/legato pop
  // the held-note stack and fall back to the last held note, releasing the
  // voice only when the stack empties.
  //
  // U2 (#149) unison width (FUN_10030c20, decomp_all_32_full.c): queue
  // empty (:34961-34972) releases the FIRST vpn slots (gate clear +
  // FUN_10047d70 on each), not just the main voice. Queue non-empty runs
  // NO attack loop (device overlap probe 48,60,64,67,72 g-0.2 38=1 73=1:
  // note-off re-attack REFUTED — 41 engine onsets vs 34 device) — the pop
  // only glides the sounding slots to the new last held pitch.
  void note_off(int note);

  // Advance every active voice: envelope chunks + the glide curve
  // (remaining = delta·e^(−4·pos/dur)·(1 − pos/dur), spec_env.md §6.2),
  // re-evaluated ONLY at glide bucket-grid edges (#114, glide_g4_trace.md
  // §1). Voices whose release completed and whose gate is off go inactive
  // (CVco reset when CVca release completes, spec_env.md §7).
  void advance(int num_samples);

  // Advance only the envelope chunks + slot release (no glide): the engine
  // advances the glide per voice render span instead, cut at bucket edges
  // (#114 — glide_frames_to_edge / glide_sync / advance_glide).
  void advance_env(int num_samples);

  // #114 glide bucket grid (FUN_10036250): frames from the slot's current
  // glide pos to its next bucket edge; 0 = an edge sits AT the current pos
  // (call glide_sync before rendering); kNoGlideCut = no glide in progress
  // (offset already final).
  static constexpr int kNoGlideCut = 0x7fffffff;
  int glide_frames_to_edge(int idx) const;
  // m3r-111-porta: true while this slot's glide is in progress (same
  // predicate as glide_frames_to_edge() != kNoGlideCut).
  bool glide_active(int idx) const {
    const VoiceSlot& v = voices_[idx];
    return v.duration > 0 && v.delta != 0 && v.step >= 1 && v.pos < v.duration;
  }
  // m3r-111-porta: glide in progress AND started by a legato retarget.
  bool glide_retarget_active(int idx) const {
    return glide_active(idx) && voices_[idx].glide_retarget;
  }
  // Re-evaluate the offset at a bucket edge sitting at the current pos
  // (device FUN_10036250 bucket != pos/step branch: rem = curve(pos),
  // zone-crossing widening, bucket = pos/step). No-op otherwise.
  void glide_sync(int idx);
  // Advance the slot's glide pos by `frames`, re-evaluating the offset at
  // every bucket edge crossed (and forcing rem = 0 once pos >= dur).
  void advance_glide(int idx, int frames);

  // m3-porta-land: per-position event batching into the held-key table,
  // mirroring the device render-loop dispatch FUN_10031070 (out/decomp_all_32_full.c
  // :35167). The engine feeds every note event at a single sample position into
  // the table (key_on/key_off) and then runs ONE evaluation (evaluate) — the
  // device pops ALL events at a position before any voice evaluation
  // (FUN_10001ae0 :778). gate_held (V+0x438) is cleared ONLY by that evaluation
  // (mono/legato count==0, poly key-entry==0), never by the note-off event, so a
  // same-position off-then-on keeps the gate held → the gap-0 auto-glide fires.
  // FUN_10024950 key-on: if the key is already held, FUN_10024a50 removes it
  // first, then set per-key velocity, push key, count++.
  void key_on(int note, int velocity);
  // FUN_10024a50 key-off: remove the key from the LIFO stack, clear its per-key
  // entry. Does NOT touch any voice gate.
  void key_off(int note);
  // FUN_10031070: early-return unless the table is dirty; poly → poly_eval
  // (FUN_10030e40), mono/legato → mono_legato_eval (FUN_10030c20); clear dirty.
  void evaluate();
  // Per-slot action of the LAST evaluate(): 0 none, 1 attack (FUN_10047e60 +
  // FUN_10047ea0), 2 retarget only (legato hold, FUN_10047e20 no attack),
  // 3 release (FUN_10047d70, gate cleared). The engine drives voices[] from it.
  enum EvalAction { kEvalNone = 0, kEvalAttack = 1, kEvalRetarget = 2, kEvalRelease = 3 };
  int eval_action(int idx) const { return eval_action_[idx]; }
  int eval_slot_index(int idx) const { return eval_slot_index_[idx]; }

  int active_count() const;
  int held_count() const { return held_count_; }
  int sustained_voice() const { return main_voice_; }  // mono/legato voice
  const VoiceSlot& voice(int idx) const { return voices_[idx]; }
  int voice_pitch_unit(int idx) const {
    return voices_[idx].target_unit + voices_[idx].remaining;
  }
  int voice_index_for_note(int note) const;

  // voice_alloc_decompile.md §Poly law step 4 / §Mono law: release every
  // currently-gated voice already on `note` (clear gate, release env, drop
  // from the active list) so a same-pitch note-on retriggers with a fresh
  // voice. Returns the number released.
  int release_same_pitch(int note);

 private:
  int allocate_voice();  // round-robin free slot (FUN_100310b0), else steal head
  // m3-porta-land per-position held-key-table helpers (FUN_10031070 dispatch).
  void remove_held_key(int note);   // splice `note` out of the LIFO stack
  void mono_legato_eval();          // FUN_10030c20 (:34941)
  void poly_eval();                 // FUN_10030e40 (:35041)
  // FUN_10030c20 mode-1/mode-2 first-vpn loop body (:34992-35031) on the
  // slot state; shared by note-on (note_on_first_n) and the note-off
  // fallback re-drive. Does NOT touch the held-note stack.
  void first_n_drive(int note, int velocity, int n, bool attacked[]);
  void start_glide(VoiceSlot& v, int new_note, bool glide);
  void glide_edge_eval(VoiceSlot& v);  // #114 bucket-edge re-eval + widening
  void trigger_env(VoiceSlot& v, int velocity);
  // Active list (M+0x130): insertion order; steal victim = head (oldest
  // inserted), per voice_alloc_decompile.md §Poly law step 5.
  void push_active(int idx);
  void remove_active(int idx);

  VoiceSlot voices_[MAX_VOICES];
  int active_list_[MAX_VOICES] = {};  // M+0x130 active-voice list (insertion order)
  int active_n_ = 0;                  // M+0x134 active-voice list size
  // m3-porta-land per-position held-key table (voice-mgr +0x124): count =
  // held_count_, LIFO stack = held_[] (top = newest), per-key velocity = key_vel_[]
  // (+8+key*4), dirty flag = table_dirty_ (+0x60c). eval_action_ = the last
  // evaluate() per-slot outcome for the engine to drive voices[] from.
  int key_vel_[128] = {};
  int eval_action_[MAX_VOICES] = {};
  int eval_slot_index_[MAX_VOICES] = {};  // unison slot index of the last evaluate()
  bool table_dirty_ = false;
  bool pos_has_on_ = false;  // the current position batch contained a key-on
  int pos_on_keys_[MAX_VOICES] = {};  // keys pressed at the current position
  int pos_on_n_ = 0;
  PlayMode play_mode_ = kPlayPoly;
  PortaMode porta_mode_ = kPortaOn;
  int porta_band_ = 0;
  int key_shift_ = 0;
  float sample_rate_ = 44100.0f;
  int attack_samples_ = 100;
  int release_samples_ = 100000;
  std::uint64_t clock_ = 0;
  int main_voice_ = -1;            // mono/legato sounding voice
  int held_[MAX_VOICES] = {};      // held-note stack (press order)
  int held_count_ = 0;
  int polyphony_ = MAX_VOICES;     // engine+0x190 (ctor 32, param 94)
  int rr_cursor_ = -1;             // engine+0x1b4: last allocated voice
  int unison_vpn_ = 1;             // M+0x110/M+0x114 live vpn (73/93 setters)
};

}  // namespace synth1::voice
