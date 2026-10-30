// voice_allocator.cpp — 32-voice pool, allocation/stealing, play modes,
// portamento.
//
// Spec: spec_env.md §6.1–§6.4 (portamento time law, glide curve, delta
// accumulation, auto mode), §7 (lifecycle: re-trigger suppression, gate
// flags, allocation walking the 32-voice array, release → CVco reset).

 #include <cstdlib>
#include "voice_allocator.h"

#include <cmath>

namespace synth1::voice {

float porta_time_seconds(int band) {
  // spec_env.md §6.1: e^(2.5·(band/127 − 1)) · (band/127) · 2.5
  if (band <= 0) return 0.0f;
  const double b = (double)band * (1.0 / 127.0);
  return (float)(std::exp(2.5 * (b - 1.0)) * b * 2.5);
}

void VoiceAllocator::trigger_env(VoiceSlot& v, int velocity) {
  v.env.attack_samples = attack_samples_;
  v.env.decay_samples = 0;
  v.env.release_samples = release_samples_;
  v.env.set_sustain_ratio(1.0f);
  v.env.note_on();  // FUN_18000c930
  v.velocity = velocity;
  v.timestamp = ++clock_;
}

int VoiceAllocator::allocate_voice() {
  // FUN_100310b0 (voice_alloc_decompile.md §Poly law step 5): a single forward
  // scan from cursor+1 with wrap, probing at most `pool` slots; the free test
  // is the gate bit alone (V+0x438 == 0), so a releasing voice that is still
  // ringing is allocatable mid-ramp — no idle-slot preference pass.
  const int pool = polyphony_;
  int idx = rr_cursor_;
  for (int k = 0; k < pool; ++k) {
    idx = idx + 1;
    if (idx >= pool) idx = 0;
    if (!voices_[idx].gate_held) { rr_cursor_ = idx; return idx; }
  }
  // No free slot: steal the HEAD of the insertion-ordered active list
  // (M+0x130), i.e. the oldest inserted voice — NOT the min-timestamp voice.
  // Releasing voices are removed from the active list at note-off, so the head
  // is always a gate-held voice: a mid-ramp voice is never stolen.
  if (active_n_ > 0) {
    const int stolen = active_list_[0];
    remove_active(stolen);
    rr_cursor_ = stolen;  // cursor = stolen slot's V+0x434 (line 35139)
    return stolen;
  }
  rr_cursor_ = 0;
  return 0;
}

void VoiceAllocator::push_active(int idx) {
  for (int i = 0; i < active_n_; ++i) if (active_list_[i] == idx) return;
  active_list_[active_n_++] = idx;
}

void VoiceAllocator::remove_active(int idx) {
  for (int i = 0; i < active_n_; ++i) {
    if (active_list_[i] == idx) {
      for (int j = i; j + 1 < active_n_; ++j) active_list_[j] = active_list_[j + 1];
      --active_n_;
      return;
    }
  }
}

int VoiceAllocator::release_same_pitch(int note) {
  // voice_alloc_decompile.md §Poly law step 4: release any active voice already
  // on this pitch (clear gate V+0x438=0, release env, drop from active list).
  int released = 0;
  for (int i = 0; i < MAX_VOICES; ++i) {
    if (voices_[i].active && voices_[i].gate_held && voices_[i].note == note) {
      voices_[i].gate_held = false;
      voices_[i].release_clock = ++clock_;
      voices_[i].env.note_off();
      remove_active(i);
      ++released;
    }
  }
  return released;
}

void VoiceAllocator::start_glide(VoiceSlot& v, int new_note, bool glide) {
  // spec_env.md §6.3 + #113: delta = remaining_old + (oldKey − newKey) in
  // 1/128-octave units — unfinished glides accumulate, so a mid-glide
  // re-target continues from the current pitch. The reference is the SLOT's
  // CVco key state UNCONDITIONALLY (active or not): FUN_10036730
  // (decomp_all_32_full.c:39086-39137) reads oldKey from CVco+0x190 before
  // writing the new key, and the CVco reset FUN_18000c950 (decomp_all.c:
  // 8113-8120, run when CVca release completes) clears only +0x38/+0x5c —
  // key state and glide fields PERSIST across note-off. Boot key state = 0
  // (target_unit/remaining init 0 = key 0) → the first note-on of a patch
  // in ALWAYS mode glides from key 0 (083 gate: delta −512 @frame 0).
  const int new_unit = key_pitch_unit(new_note);
  // m3r-111-porta: retarget (gate held, no retrigger) vs fresh attack at
  // this trigger — captured before the caller sets gate_held for attacks.
  v.glide_retarget = glide && v.gate_held;
  // FUN_10036730 (decomp_all_32_full.c:39086-39137; objdump bytes:
  // 0x10036734-3a oldU raw = +0x15c + CVco+0x190, clamp [0,0x80] at
  // 0x10036740-55; +0x190 = newKey written at 0x1003675c BEFORE the new
  // key is used; newU = +0x15c + +0x190 clamped at 0x1003676e-8f;
  // delta = +0x180 + ((oldU<<7)/12 - (newU<<7)/12) via magic-div /12 at
  // 0x100367bc-7f3, stored to +0x184/+0x180 at 0x100367fb-803):
  // the delta is formed from the KEY-SHIFTED keys (param 9 -> CVco+0x15c,
  // setter FUN_10036670 :39046-39052), each clamped to 0..0x80 BEFORE the
  // <<7 /12 unit conversion. oldKey = CVco+0x190 (boot 0, written only by
  // this trigger). With key_shift_ = 0 and keys in 0..127 this reduces
  // exactly to cur_unit - new_unit (glide_key mirrors the note that set
  // target_unit), so ks=0 patches stay bit-identical.
  auto shifted_unit = [&](int k) {
    int t = key_shift_ + k;
    if (t < 0) t = 0;
    if (t > 0x80) t = 0x80;
    return key_pitch_unit(t);
  };
  const int delta_dev = v.remaining + (shifted_unit(v.glide_key) - shifted_unit(new_note));
  v.glide_key = new_note;
  v.note = new_note;
  v.target_unit = new_unit;
  if (glide) {
    v.delta = delta_dev;
    v.duration = (int)((double)sample_rate_ * (double)porta_time_seconds(porta_band_));
    if (v.delta != 0 && v.duration > 0) {
      v.remaining = v.delta;
      v.pos = 0;
      // #114 trigger (FUN_10036730, decomp_all_32_full.c:39086-39137):
      // bucket=1 (forces the first eval), step=max(32,(dur/|delta|)/10),
      // zone=dur/10 — the grid is GLIDE-RELATIVE (pos=0 at the note-on
      // frame), hence bs-invariant.
      v.bucket = 1;
      int ad = v.delta < 0 ? -v.delta : v.delta;
      if (ad < 1) ad = 1;
      int step = (v.duration / ad) / 10;
      if (step < 32) step = 32;
      v.step = step;
      v.zone = v.duration / 10;
    } else {
      v.delta = 0;
      v.remaining = 0;
      v.pos = 0;
      v.duration = 0;
    }
  } else {
    v.delta = 0;
    v.remaining = 0;
    v.pos = 0;
    v.duration = 0;
  }
}

int VoiceAllocator::note_on(int note, int velocity) {
  if (play_mode_ == kPlayPoly) {
    // voice_alloc_decompile.md §Poly law step 4: a same-pitch note-on releases
    // the existing voice and allocates a fresh one (retrigger). No suppression.
    release_same_pitch(note);
    const int idx = allocate_voice();
    VoiceSlot& v = voices_[idx];
    // Auto mode glides only when the gate was still held (spec_env.md §6.4).
    const bool glide = (porta_mode_ == kPortaOn) ||
                       (porta_mode_ == kPortaAuto && v.gate_held);
    start_glide(v, note, glide);
    v.active = true;
    v.gate_held = true;
    trigger_env(v, velocity);
    push_active(idx);
    return idx;
  }

  if (play_mode_ == kPlayMono) {
    // voice_alloc_decompile.md §Mono law (mode 1): ALWAYS retrigger the envelope
    // on note-on, including the same pitch. No held_==note early-return.
    if (held_count_ < MAX_VOICES) held_[held_count_++] = note;

    int idx = main_voice_;
    if (idx < 0 || !voices_[idx].active) {
      idx = 0;  // FUN_10030c20: mono/legato always drive voices 0..n−1
      main_voice_ = idx;
      VoiceSlot& v = voices_[idx];
      // #113: the device mono note-on also runs FUN_10047e20 → FUN_10036730;
      // in ALWAYS mode (p74 != 1) it glides from the slot's CVco key state
      // (boot 0 on first use), active or not.
      const bool glide = (porta_mode_ == kPortaOn) ||
                         (porta_mode_ == kPortaAuto && v.gate_held);
      start_glide(v, note, glide);
      v.active = true;
      v.gate_held = true;
      trigger_env(v, velocity);
      push_active(idx);
    } else {
      VoiceSlot& v = voices_[idx];
      // Mono retriggers the envelope on every note-on. In ALWAYS mode
      // (p74 != 1, #112) FUN_10047e20 arms a glide even when the gate was
      // just released, as long as CVco has not been reset yet.
      const bool glide = (porta_mode_ == kPortaOn) ||
                         (porta_mode_ == kPortaAuto && v.gate_held);
      start_glide(v, note, glide);
      v.gate_held = true;
      trigger_env(v, velocity);
      push_active(idx);
    }
    return idx;
  }

  // kPlayLegato
  if (held_count_ > 0) {
    // A note is already held: change pitch (with portamento) WITHOUT
    // retriggering the attack phase (voice_alloc_decompile.md §Mono law mode 2:
    // attack only if the voice is not already gated).
    if (held_count_ < MAX_VOICES) held_[held_count_++] = note;
    const int idx = main_voice_;
    VoiceSlot& v = voices_[idx];
    const bool glide = (porta_mode_ != kPortaOff);
    start_glide(v, note, glide);
    v.velocity = velocity;
    return idx;
  }
  if (held_count_ < MAX_VOICES) held_[held_count_++] = note;
  const int idx = 0;  // FUN_10030c20: mono/legato always drive voices 0..n−1
  main_voice_ = idx;
  VoiceSlot& v = voices_[idx];
  // #113: the device glides the FIRST note-on from the CVco boot key state
  // (0): FUN_10036730 reads the slot key before writing the new one, and
  // nothing resets it (FUN_18000c950 clears only +0x38/+0x5c). In ALWAYS
  // mode (p74 != 1) the glide fires whether the slot is active or not
  // (083 gate seq: delta −512 @frame 0 on fresh voice 0).
  const bool glide = (porta_mode_ == kPortaOn) ||
                     (porta_mode_ == kPortaAuto && v.gate_held);
  start_glide(v, note, glide);
  v.active = true;
  v.gate_held = true;
  trigger_env(v, velocity);
  push_active(idx);
  return idx;
}

// U2 (#149) — FUN_10030c20 first-vpn loop body (decomp_all_32_full.c
// :34992-35031; 64-bit twin FUN_180036930:33302-33350). Slot selection:
// piVar2 = param_1+1 stepping → slots 0..n-1, never the chooser.
// FUN_10047e20 (pitch/vel + glide arm, glide flag V+0x420==1 ? V+0x438 : 1,
// #112) runs on EVERY slot in both modes (:35001 mono, :35019 legato).
// Mode 1 (:34992-35013): FUN_10047e60 group + FUN_10047ea0 attack run
// UNCONDITIONALLY on every slot (retrigger). Mode 2 (:35015-35031):
// group+attack+push only where the slot's gate bit V+0x438 == 0 (:35022);
// already-gated slots are retargeted only (legato hold).
void VoiceAllocator::first_n_drive(int note, int velocity, int n, bool attacked[]) {
  main_voice_ = 0;  // mono/legato drive voices 0..n-1 (note_on law)
  for (int j = 0; j < n; ++j) {
    VoiceSlot& v = voices_[j];
    const bool was_gated = v.gate_held;
    const bool glide = (porta_mode_ == kPortaOn) ||
                       (porta_mode_ == kPortaAuto && was_gated);
    start_glide(v, note, glide);
    v.velocity = velocity;
    if (play_mode_ == kPlayMono || !was_gated) {
      v.active = true;
      v.gate_held = true;
      trigger_env(v, velocity);
      push_active(j);
      attacked[j] = true;
    } else {
      attacked[j] = false;
    }
  }
}

void VoiceAllocator::note_on_first_n(int note, int velocity, int n, bool attacked[]) {
  if (n < 1) n = 1;
  if (n > MAX_VOICES) n = MAX_VOICES;
  // Q+0x204 pitch queue push (FUN_10030c20 note-on path).
  if (held_count_ < MAX_VOICES) held_[held_count_++] = note;
  first_n_drive(note, velocity, n, attacked);
}

int VoiceAllocator::note_on_force(int note, int velocity) {
  if (play_mode_ != kPlayPoly) return note_on(note, velocity);
  const int idx = allocate_voice();
  VoiceSlot& v = voices_[idx];
  // FUN_10030e40 calls FUN_10047e20 on EVERY unison voice it allocates, so
  // each slot arms its glide from its own CVco key state (#112 mode table).
  const bool glide = (porta_mode_ == kPortaOn) ||
                     (porta_mode_ == kPortaAuto && v.gate_held);
  start_glide(v, note, glide);
  v.active = true;
  v.gate_held = true;
  trigger_env(v, velocity);
  push_active(idx);
  return idx;
}

void VoiceAllocator::note_off(int note) {
  if (play_mode_ == kPlayPoly) {
    for (int i = 0; i < MAX_VOICES; ++i) {
      if (voices_[i].active && voices_[i].gate_held && voices_[i].note == note) {
        voices_[i].gate_held = false;          // V+0x438 = 0
        voices_[i].release_clock = ++clock_;
        voices_[i].env.note_off();             // FUN_10047d70
        remove_active(i);
      }
    }
    return;
  }

  // Mono / legato: pop the note from the held stack; fall back to the last
  // held note, release the voice only when the stack empties.
  for (int i = 0; i < held_count_; ++i) {
    if (held_[i] == note) {
      for (int j = i; j + 1 < held_count_; ++j) held_[j] = held_[j + 1];
      --held_count_;
      break;
    }
  }

  if (held_count_ > 0) {
    // Queue non-empty: the sounding slots already track the LAST note-on
    // (the mode-1/mode-2 loops drove every first-vpn slot at that event),
    // so the pop leaves the voice state on the last held pitch. Device
    // overlap probe (notes 48,60,64,67,72 g-0.2, 38=1 73=1): a note-off
    // re-attack is REFUTED (engine 41 envelope onsets vs device 34) —
    // the note-off path never runs the FUN_10047ea0 attack loop; the pop
    // only glides the sounding slots to the new last held pitch.
    const bool glide = (porta_mode_ != kPortaOff);
    if (unison_vpn_ > 1) {
      // The mode loops drove slots 0..vpn-1; the pop retargets the same
      // slots to the new last held pitch (glide only, no attack).
      for (int j = 0; j < unison_vpn_; ++j) start_glide(voices_[j], held_[held_count_ - 1], glide);
      return;
    }
    const int idx = main_voice_;
    if (idx < 0) return;
    VoiceSlot& v = voices_[idx];
    start_glide(v, held_[held_count_ - 1], glide);
    return;
  }

  // Queue empty: FUN_10030c20:34961-34972 releases the FIRST vpn slots
  // (gate clear + FUN_10047d70 per slot), not just the main voice.
  if (unison_vpn_ > 1) {
    for (int j = 0; j < unison_vpn_; ++j) {
      VoiceSlot& v = voices_[j];
      if (!v.gate_held) continue;
      v.gate_held = false;
      v.release_clock = ++clock_;
      v.env.note_off();
      remove_active(j);
    }
    return;
  }
  const int idx = main_voice_;
  if (idx < 0) return;
  VoiceSlot& v = voices_[idx];
  v.gate_held = false;
  v.release_clock = ++clock_;
  v.env.note_off();
  remove_active(idx);
}

// ---------------------------------------------------------------------------
// m3-porta-land — per-position event batching into the held-key table,
// mirroring the device render-loop dispatch FUN_10031070
// (out/decomp_all_32_full.c:35167). The engine calls key_on/key_off for every
// note event sharing a sample position, then ONE evaluate() for the position.
// gate_held (V+0x438) is cleared ONLY by the evaluation (mono/legato count==0,
// poly key-entry==0) — never by the note-off event — so a same-position
// off-then-on (gap-0 retrigger) keeps the gate held and the auto glide fires.
// The existing synchronous note_on/note_off paths stay for the direct API
// (unit tests, arpeggiator single notes) — this path is the live render path.
// ---------------------------------------------------------------------------
void VoiceAllocator::remove_held_key(int note) {
  for (int i = 0; i < held_count_; ++i) {
    if (held_[i] == note) {
      for (int j = i; j + 1 < held_count_; ++j) held_[j] = held_[j + 1];
      --held_count_;
      return;
    }
  }
}

// FUN_10024950: if the key is already held, FUN_10024a50 removes it first
// (LIFO stack + per-key entry), then set per-key velocity, push, count++.
void VoiceAllocator::key_on(int note, int velocity) {
  remove_held_key(note);
  if (held_count_ < MAX_VOICES) held_[held_count_++] = note;
  if (note >= 0 && note < 128) key_vel_[note] = velocity;
  pos_has_on_ = true;
  int dup = false;
  for (int i = 0; i < pos_on_n_; ++i) if (pos_on_keys_[i] == note) { dup = true; break; }
  if (!dup && pos_on_n_ < MAX_VOICES) pos_on_keys_[pos_on_n_++] = note;
  table_dirty_ = true;
}

// FUN_10024a50: remove the key from the LIFO stack, clear its per-key entry.
// Voice gates are untouched (the device clears them only in FUN_10031070).
void VoiceAllocator::key_off(int note) {
  remove_held_key(note);
  if (note >= 0 && note < 128) key_vel_[note] = 0;
  table_dirty_ = true;
}

// FUN_10031070: dispatch on the play mode ([0x42]) — poly first, mono/legato
// for 1/2 — then clear the dirty flag (FUN_100311e0).
void VoiceAllocator::evaluate() {
  if (!table_dirty_) return;
  for (int i = 0; i < MAX_VOICES; ++i) eval_action_[i] = kEvalNone;
  if (play_mode_ == kPlayPoly) poly_eval();
  else mono_legato_eval();
  table_dirty_ = false;
  pos_has_on_ = false;
  pos_on_n_ = 0;
}

// FUN_10030c20 (mono mode 1 / legato mode 2 first-vpn-slot loops,
// decomp_all_32_full.c:34941-35031). Count==0 → release the first vpn gated
// slots (FUN_10047d70) and return (:34961-34971). Else newest key = LIFO top
// (:34976-34982), velocity = per-key entry (:34993); FUN_10047e20 glides arm
// (portaEnable = (voice+0x420==1) ? gate : 1, :50197-50208) on every slot.
//
// The attack (FUN_10047e60 + FUN_10047ea0) fires ONLY when the position batch
// contains a key-ON (a fresh note press). The device overlap probe (notes
// 48,60,64,67,72 g-0.2, 38=1 73=1: 41 engine onsets vs 34 device) REFUTES a
// note-off re-attack — a note-off with count>0 glides the sounding slots to the
// new last-held pitch (retarget, NO attack loop), exactly like the validated
// synchronous note_off count>0 path. Mode-2 additionally attacks only where the
// slot's gate bit was 0 (:35022); a gated slot retargets (legato hold).
void VoiceAllocator::mono_legato_eval() {
  const int vpn = unison_vpn_;
  const bool has_on = pos_has_on_;
  if (held_count_ == 0) {
    for (int j = 0; j < vpn; ++j) {
      VoiceSlot& v = voices_[j];
      if (v.gate_held) {
        v.gate_held = false;                 // FUN_10047d70 clears +0x438
        v.release_clock = ++clock_;
        v.env.note_off();
        remove_active(j);
        eval_action_[j] = kEvalRelease;
      }
    }
    return;
  }
  const int newest = held_[held_count_ - 1];
  // m3-porta-guard-land (ruling m3-porta-long-locate F3/F4, ACCEPTED):
  // FUN_10030c20 early-return (decomp_all_32_full.c:34977-34982) — when the
  // FIRST slot is gated (+0x438 != 0) AND its current key (+0x43c) equals the
  // newest held key, the allocation is a NO-OP: no FUN_10047e20, no
  // FUN_10036730 glide re-trigger, no attack. A note-off that pops back to the
  // sounding pitch must NOT re-arm a still-running glide (device GL-W tap:
  // pos/step/zone untouched across the pop; GLT count stays 2). Proof: p39_v96
  // bs512 -108.56 dB corr 1.000000 (was +4.47 / 0.6386).
  if (vpn > 0 && voices_[0].gate_held && voices_[0].note == newest) {
    return;
  }
  const int vel = (newest >= 0 && newest < 128) ? key_vel_[newest] : 0;
  for (int j = 0; j < vpn; ++j) {
    VoiceSlot& v = voices_[j];
    const bool was_gated = v.gate_held;
    const bool glide = (porta_mode_ == kPortaOn) ||
                       (porta_mode_ == kPortaAuto && was_gated);
    start_glide(v, newest, glide);           // FUN_10047e20 → FUN_10036730
    v.velocity = vel;
    const bool attack = has_on && (play_mode_ == kPlayMono || !was_gated);
    v.gate_held = true;                      // FUN_10047ea0 sets +0x438 = 1
    if (attack) {
      v.active = true;
      trigger_env(v, vel);
      push_active(j);
      eval_action_[j] = kEvalAttack;
    } else {
      eval_action_[j] = kEvalRetarget;
    }
    eval_slot_index_[j] = j;
  }
}

// FUN_10030e40 (poly, decomp_all_32_full.c:35041-35155), scoped to the events
// at this position. Pass 1 (defers the old-voice release, the device's
// per-position scan :35064-35071) releases every active voice whose current key
// (+0x43c) has a CLEARED table entry (+8+key*4 == 0) — a key that left the
// table at this position (a key-off that emptied it). Pass 2 (the newest-key
// steal/allocate pass :35086-35115) fires only for keys pressed at this
// position (a key-on): it frees the existing voices on that key, then allocates
// vpn fresh voices (FUN_100310b0 free-slot scan) with FUN_10047e20 (glide arm —
// a fresh slot's gate is 0, so auto never glides a poly note-on). A key that is
// merely held (no event this position) is left sounding — matching the
// validated synchronous note_on (allocate the new key) / note_off (release) for
// single notes and overlaps; the deferred pass-1 release reproduces the
// same-key gap-0 retrigger (the old voice survives pass-1, freed in pass-2).
void VoiceAllocator::poly_eval() {
  // Pass 1: release voices whose key left the table (key entry cleared).
  for (int i = 0; i < MAX_VOICES; ++i) {
    VoiceSlot& v = voices_[i];
    const int note = v.note;
    const bool key_held = (note >= 0 && note < 128 && key_vel_[note] != 0);
    if (v.gate_held && !key_held) {
      v.gate_held = false;
      v.release_clock = ++clock_;
      v.env.note_off();
      remove_active(i);
      eval_action_[i] = kEvalRelease;
    }
  }
  // Pass 2: keys pressed at this position → free + allocate vpn fresh voices.
  const int vpn = unison_vpn_;
  for (int k = 0; k < pos_on_n_; ++k) {
    const int key = pos_on_keys_[k];
    if (key < 0 || key >= 128) continue;
    for (int i = 0; i < MAX_VOICES; ++i) {
      VoiceSlot& v = voices_[i];
      if (v.active && v.note == key && v.gate_held) {
        v.gate_held = false;
        v.release_clock = ++clock_;
        v.env.note_off();
        remove_active(i);
        eval_action_[i] = kEvalRelease;
      }
    }
    const int vel = key_vel_[key];
    for (int s = 0; s < vpn; ++s) {
      const int slot = allocate_voice();
      VoiceSlot& v = voices_[slot];
      const bool glide = (porta_mode_ == kPortaOn) ||
                         (porta_mode_ == kPortaAuto && v.gate_held);
      start_glide(v, key, glide);
      v.active = true;
      v.gate_held = true;
      trigger_env(v, vel);
      push_active(slot);
      eval_action_[slot] = kEvalAttack;
      eval_slot_index_[slot] = s;
    }
  }
}

// ---------------------------------------------------------------------------

// decomp_all_32_full.c:38912-38990): the glide offset is re-evaluated ONLY
// at the edges of a glide-relative adaptive grid. Trigger (FUN_10036730):
// pos=0, rem=delta, bucket=1, step=max(32,(dur/|delta|)/10), zone=dur/10.
// Eval at an edge (bucket != pos/step): rem = delta·e^(−4·pos/dur)·
// (1 − pos/dur); on a zone crossing (zone <= pos) widen step and zone
// (dur/10 → (2dur/|d|)/5 @25 % → dur/|d| @55 % → 9dur/|d| @dur), then
// step = max(32, min(step, zone − pos)), >= 1; bucket = pos/step. The
// render chunk runs to the next edge: n = min(rem_frames,
// (bucket+1)·step − pos); past dur the offset is 0 and pos freezes
// (device else-branch does not advance pos).
// ---------------------------------------------------------------------------
void VoiceAllocator::glide_edge_eval(VoiceSlot& v) {
  const double frac = (double)v.pos / (double)v.duration;
  static const bool glide_linear =  // #235
      getenv("SYNTH1_GLIDE_LAW") && *getenv("SYNTH1_GLIDE_LAW") == 'l';
  const double law = glide_linear ? (1.0 - frac)
                                  : std::exp(frac * -4.0) * (1.0 - frac);
  v.remaining = (int)(law * (double)v.delta);
  if (v.zone <= v.pos) {
    int ad = v.delta < 0 ? -v.delta : v.delta;
    if (ad < 1) ad = 1;
    const int dur = v.duration;
    const int t55 = (dur * 0x37) / 100;   // 55 % mark
    if (v.zone < t55) {
      const int t25 = (dur * 0x19) / 100;  // 25 % mark
      if (v.zone < t25) {
        if (v.zone < dur / 10) {
          v.zone = dur / 10;
          v.step = (dur / ad) / 4;
        } else {
          v.step = ((dur * 2) / ad) / 5;
          v.zone = t25;
        }
      } else {
        v.step = dur / ad;
        v.zone = t55;
      }
    } else {
      v.zone = dur;
      v.step = (dur * 9) / ad;
    }
    if (v.step < 32) v.step = 32;
    const int zrem = v.zone - v.pos;
    int cand = v.step < zrem ? v.step : zrem;
    if (cand < 1) cand = 1;
    v.step = cand;
  }
  v.bucket = v.pos / v.step;
}

int VoiceAllocator::glide_frames_to_edge(int idx) const {
  const VoiceSlot& v = voices_[idx];
  if (v.duration <= 0 || v.delta == 0 || v.step < 1) return kNoGlideCut;
  if (v.pos >= v.duration) return kNoGlideCut;  // offset already 0
  const int q = v.pos / v.step;
  if (v.bucket != q) return 0;                  // edge sits AT pos
  return (q + 1) * v.step - v.pos;
}

void VoiceAllocator::glide_sync(int idx) {
  VoiceSlot& v = voices_[idx];
  if (v.duration <= 0 || v.delta == 0 || v.step < 1) return;
  if (v.pos >= v.duration) { v.remaining = 0; return; }
  if (v.bucket != v.pos / v.step) glide_edge_eval(v);
}

void VoiceAllocator::advance_glide(int idx, int frames) {
  VoiceSlot& v = voices_[idx];
  if (v.duration <= 0 || v.delta == 0 || v.step < 1) return;
  int rem = frames;
  while (rem > 0) {
    if (v.pos >= v.duration) { v.remaining = 0; break; }  // pos frozen
    const int q = v.pos / v.step;
    if (v.bucket != q) glide_edge_eval(v);
    const int to_edge = (v.bucket + 1) * v.step - v.pos;
    const int n = rem < to_edge ? rem : to_edge;
    v.pos += n;
    rem -= n;
  }
}

void VoiceAllocator::advance(int num_samples) {
  for (int i = 0; i < MAX_VOICES; ++i) {
    VoiceSlot& v = voices_[i];
    if (!v.active) continue;

    v.env.advance_chunk(num_samples);

    // Glide law (#114): curve re-evaluated only at bucket-grid edges.
    advance_glide(i, num_samples);

    // CVco is reset when CVca's release completes (spec_env.md §7).
    if (!v.gate_held && v.env.stage == env::STAGE_IDLE) {
      v.active = false;
      v.note = -1;
      v.delta = 0;
      v.remaining = 0;
      v.pos = 0;
      v.duration = 0;
      if (i == main_voice_) main_voice_ = -1;
    }
  }
}

void VoiceAllocator::advance_env(int num_samples) {
  for (int i = 0; i < MAX_VOICES; ++i) {
    VoiceSlot& v = voices_[i];
    if (!v.active) continue;

    v.env.advance_chunk(num_samples);

    // CVco is reset when CVca's release completes (spec_env.md §7).
    if (!v.gate_held && v.env.stage == env::STAGE_IDLE) {
      v.active = false;
      v.note = -1;
      v.delta = 0;
      v.remaining = 0;
      v.pos = 0;
      v.duration = 0;
      if (i == main_voice_) main_voice_ = -1;
    }
  }
}

int VoiceAllocator::active_count() const {
  int n = 0;
  for (int i = 0; i < MAX_VOICES; ++i) {
    if (voices_[i].active) ++n;
  }
  return n;
}

int VoiceAllocator::voice_index_for_note(int note) const {
  for (int i = 0; i < MAX_VOICES; ++i) {
    if (voices_[i].active && voices_[i].note == note) return i;
  }
  return -1;
}

}  // namespace synth1::voice
