// arpeggiator.cpp — CArpeggiator note re-voicer (Stage 7).
//
// Spec: spec_lfo_fx.md §7.1–§7.5. The clock unit is 1/24 of a quarter note
// (24.0f = DAT_180072a08); stepLen = (int)((float)(sr·BEAT·60)/(tempo·24)).

#include <cmath>

#include "arpeggiator.h"

namespace synth1::arp {

// m3r-arpround-land (m3r-111-envgrid ACCEPTED): the 32-bit render DLL converts
// the gate length (float32 stepLen·gate, mulss) with x87 flds+fistpl =
// round-to-nearest ties-to-even (FUN_10001890 0x100018ac-8b8, FUN_10001ae0
// tail; decomp_all_32_full.c:578/778 `(int)ROUND(...)`). std::nearbyint under
// the default FE_TONEAREST mode mirrors fistpl exactly.
namespace {
inline int gate_on_len(int step_len, float gate) {
  return (int)std::nearbyint((float)step_len * gate);  // mulss + fistpl
}
}  // namespace

void Arpeggiator::recompute_step_len() {
  // §7.2: rescale the phase position when the step length changes.
  const int old = step_len_;
  step_len_ = (int)((float)(sr_ * beat_div_ * 60) / (tempo_ * 24.0f));
  if (old != 0) pos_ = (int)(((float)pos_ * (float)step_len_) / (float)old);
}

void Arpeggiator::set_tempo(float bpm) {
  tempo_ = bpm;
  recompute_step_len();
}

void Arpeggiator::set_beat_index(int idx) {
  if (idx < 0) idx = 0;
  if (idx > kNumBeat - 1) idx = kNumBeat - 1;
  if (beat_div_ != BEAT[idx]) {
    beat_div_ = BEAT[idx];
    beat_idx_ = idx;
    recompute_step_len();
  }
}

void Arpeggiator::set_gate(int raw) {
  gate_ = (float)raw / 127.0f;  // FUN_180001f20, param 34
}

void Arpeggiator::set_mode(int mode) {
  // FUN_180002090: reset pos, octave index, gate state, direction; release
  // the current note.
  mode_ = mode;
  pos_ = 0;
  oct_idx_ = 0;
  gate_on_ = true;
  dir_ = (mode == 3) ? 1 : 0;
  cur_note_ = -1;
}

void Arpeggiator::set_octaves(int oct) {
  if (oct < 0) oct = 0;
  if (oct > 3) oct = 3;
  octaves_ = oct;  // FUN_18000d2b0, param 32
}

void Arpeggiator::set_enabled(bool on) {
  // FUN_180001ec0 (param 59): same reset as a mode change.
  enabled_ = on;
  pos_ = 0;
  oct_idx_ = 0;
  gate_on_ = true;
  cur_note_ = -1;
}

void Arpeggiator::remove_held_key(int midi_note) {
  for (auto it = held_list_.begin(); it != held_list_.end(); ++it) {
    if (*it == midi_note) {
      held_list_.erase(it);
      return;
    }
  }
}

void Arpeggiator::key_down(int midi_note, int velocity) {
  if (midi_note < 0 || midi_note > 127) return;
  // FUN_180028290: a re-trigger removes the old list entry and appends the
  // new one at the end (the keyboardInfo+0x20c held-key list).
  if (held_[midi_note]) remove_held_key(midi_note);
  held_[midi_note] = true;
  held_list_.push_back(midi_note);
  velocity_ = velocity;
}

void Arpeggiator::key_up(int midi_note) {
  if (midi_note < 0 || midi_note > 127) return;
  held_[midi_note] = false;
  remove_held_key(midi_note);
}

void Arpeggiator::note_on_reset(int velocity) {
  // FUN_1800020f0.
  pos_ = 0;
  oct_idx_ = 0;
  velocity_ = velocity;
  last_idx_ = -1;
  dir_ = (mode_ == 3) ? 1 : 0;
  gate_on_ = true;
}

int Arpeggiator::advance(int n) {
  // FUN_1800019b0.
  pos_ += n;
  if (step_len_ <= pos_) {
    const int steps = pos_ / step_len_;
    pos_ = pos_ % step_len_;
    return steps;
  }
  return 0;
}

int Arpeggiator::samples_to_gate_off_edge() const {
  // FUN_1800018f0.
  if (pos_ == 0) return 0;
  const int on_len = gate_on_len(step_len_, gate_);
  if (pos_ < on_len) return on_len - pos_;
  if (pos_ != on_len) return step_len_ - pos_;
  return 0;  // exactly at the edge → the caller emits the event
}

int Arpeggiator::next_held_key_up() const {
  const int start = (last_idx_ < 0) ? 0 : last_idx_ + 1;
  for (int k = start; k < 128; ++k) {
    if (held_[k]) return k;
  }
  return -1;
}

int Arpeggiator::first_held_key_up() const {
  for (int k = 0; k < 128; ++k) {
    if (held_[k]) return k;
  }
  return -1;
}

int Arpeggiator::next_held_key_down() const {
  const int start = (last_idx_ < 0) ? 127 : last_idx_ - 1;
  for (int k = start; k >= 0; --k) {
    if (held_[k]) return (k == 0) ? -1 : k;  // FUN_180001b40 treats key 0 as "none"
  }
  return -1;
}

int Arpeggiator::last_held_key_down() const {
  for (int k = 127; k >= 0; --k) {
    if (held_[k]) return (k == 0) ? -1 : k;
  }
  return -1;
}

int Arpeggiator::last_held_key_list() const {
  // keys+0x208: the keyboardInfo held-key list, scanned from the end.
  for (auto it = held_list_.rbegin(); it != held_list_.rend(); ++it) {
    if (*it != 0) return *it;
  }
  return -1;
}

int Arpeggiator::random_held_key() {
  int keys[128];
  int n = 0;
  for (int k = 0; k < 128; ++k) {
    if (held_[k]) keys[n++] = k;
  }
  if (n == 0) return -1;
  return keys[rng::crt_rand() % n];  // FUN_180001930
}

int Arpeggiator::emit_step() {
  // FUN_180001b40.
  int note = -1;
  if (cur_note_ != -1) {
    cur_note_ = -1;  // owner->noteOff(owner, curNote)
  }
  if (pos_ != 0) {
    gate_on_ = true;
    return step_len_ - pos_;
  }
  if (gate_ == 0.0f) return step_len_;

  if (mode_ == 4) {  // random
    oct_idx_ = rng::crt_rand() % (octaves_ + 1);  // FUN_180001b40
    note = random_held_key();
  } else if (dir_ == 0) {  // ---- up ----
    note = next_held_key_up();
    if (note == -1) {
      const int i = oct_idx_;
      if (i < octaves_) {  // step up one octave, rescan from the bottom
        last_idx_ = -1;
        note = next_held_key_up();
        oct_idx_ = i + 1;
      } else if (mode_ == 1) {  // updown: turn around at the top
        dir_ = 1;
        note = next_held_key_down();
        // FUN_180001b40 assigns note = lastIdx before testing octaves, so a
        // single-octave up/down arp repeats the turnaround key instead of
        // emitting a rest.
        if (note == -1 && last_idx_ >= 0 && held_[last_idx_]) {
          note = last_idx_;
          if (octaves_ > 0) oct_idx_ = i - 1;
        }
      } else {  // up: wrap to the bottom
        last_idx_ = -1;
        note = first_held_key_up();
        oct_idx_ = 0;
      }
    }
  } else {  // ---- down ----
    note = next_held_key_down();
    if (note == -1) {
      const int i = oct_idx_;
      if (i > 0) {  // step down one octave: FUN_180001b40 scans keys+0x208
                   // = flag[127] downward — the HIGHEST held key (press order
                   // is irrelevant; the +0x20c list is not the scan source).
        last_idx_ = -1;
        note = last_held_key_down();
        oct_idx_ = i - 1;
      } else if (mode_ != 1) {  // wrap to the top: same highest-key scan
        last_idx_ = -1;
        note = last_held_key_down();
        oct_idx_ = octaves_;
      } else {  // updown: turn around at the bottom, rescan ascending
        dir_ = 0;
        note = (last_idx_ == -1) ? first_held_key_up() : next_held_key_up();
        // Same comma-operator side effect as the top turnaround: repeat the
        // held turnaround key even when octaves_ == 0.
        if (note == -1 && last_idx_ >= 0 && held_[last_idx_]) {
          note = last_idx_;
          if (octaves_ > 0) oct_idx_ = i + 1;
        }
      }
    }
  }

  last_idx_ = note;
  if (note != -1) {
    const int midi = note + oct_idx_ * 12;  // octave shift
    if ((unsigned)midi < 128u) cur_note_ = midi;  // owner->noteOn(...)
  }
  if (gate_ < 1.0f) gate_on_ = false;
  return gate_on_len(step_len_, gate_);  // samples until the gate-off edge
}

}  // namespace synth1::arp
