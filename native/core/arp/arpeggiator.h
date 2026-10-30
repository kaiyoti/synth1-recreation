// arpeggiator.h — CArpeggiator note re-voicer (Stage 7).
//
// Spec: spec_lfo_fx.md §7 (CArpeggiator, engine+0x9d08): §7.1 layout/clock
//       (FUN_1800019b0 step clock, FUN_1800018f0 gate-off edge distance),
//       §7.2 step length (FUN_180001e30, param 33, BEAT[19] table, phase
//       rescale), §7.3 gate (FUN_180001f20, param 34), §7.4 mode (param 31:
//       0 up / 1 updown / 4 random; reset on mode/on-off change, param 59;
//       octave span param 32), §7.5 note generator (FUN_180001b40: held-key
//       scans, octave walk, turnaround, emitted pitch = key + octIdx·12).
// Ghidra: FUN_180001240 (ctor: tempo 120.0f, stepLen 22050, octaveSpan 1,
//         gate 0.5f, beatDiv 24, mode 1), FUN_180001390/1350 (held-key up/
//         down scans), FUN_180001930 (random held key), BEAT @0x180093000.

#pragma once

#include <cstdint>
#include "rng/crt_rand.h"
#include <vector>

namespace synth1::arp {

class Arpeggiator {
 public:
  // spec_lfo_fx.md §7.2: beat divisors in 1/24-beat units (@0x180093000).
  static constexpr int kNumBeat = 19;
  static constexpr int BEAT[kNumBeat] = {96, 84, 72, 48, 42, 36, 32, 24, 21,
                                         18, 16, 12, 9,  8,  6,  4,  3,  2,  1};

  // Ctor defaults (FUN_180001240): tempo 120, beatDiv 24 (quarter), gate
  // 0.5, octave span 2 (octaves_ = 1), mode 1 (up/down), off.
  explicit Arpeggiator(int sample_rate = 44100) : sr_(sample_rate) {}

  void set_sample_rate(int sr) { sr_ = sr; recompute_step_len(); }
  void set_tempo(float bpm);            // recomputes stepLen + rescale
  void set_beat_index(int idx);         // param 33 "beat" (0..18)
  void set_gate(int raw);               // param 34: gate = raw / 127.0f
  void set_mode(int mode);              // param 31: 0 up, 1 updown, 4 random
  void set_octaves(int oct);            // param 32: 0..3 (1..4 octaves)
  void set_enabled(bool on);            // param 59 (arp on/off)

  // Held-key tracker (the keys+0xc 128-slot array).
  void key_down(int midi_note, int velocity);
  void key_up(int midi_note);
  // FUN_1800020f0: a MIDI note-on while the arp is running restarts the step
  // grid (pos/octave/last-key) and adopts the new velocity.
  void note_on_reset(int velocity);
  bool key_held(int midi_note) const { return held_[midi_note]; }

  // Clock (FUN_1800019b0): advance by n samples; returns whole steps elapsed.
  int advance(int n);

  // Samples until the next gate-off edge (FUN_1800018f0).
  int samples_to_gate_off_edge() const;

  // Note generator (FUN_180001b40), called at pos == 0: releases the
  // current note, picks the next per mode/octaves, emits
  // midi = key + octIdx·12. Returns samples until the gate-off edge.
  int emit_step();

  // State accessors (CVco arp field map §7.1).
  int current_note() const { return cur_note_; }   // +0x4c (-1 = none)
  bool gate_on() const { return gate_on_; }        // +0x30
  int step_len() const { return step_len_; }       // +0x1c
  int position() const { return pos_; }            // +0x2c
  float gate() const { return gate_; }             // +0x24
  int octave_index() const { return oct_idx_; }    // +0x44
  int direction() const { return dir_; }           // +0x40
  int last_key_index() const { return last_idx_; } // +0x48
  int beat_div() const { return beat_div_; }       // +0x28
  float tempo() const { return tempo_; }           // +0x18
  int mode() const { return mode_; }               // +0x54
  int octaves() const { return octaves_; }         // +0x20
  bool enabled() const { return enabled_; }        // +0x58
  int velocity() const { return velocity_; }       // +0x5c

  // The arp draws from the device's shared CRT rand() stream (FUN_180055b98).
  void seed_rng(unsigned s) { rng::crt_srand(s); }

 private:
  int next_held_key_up() const;     // FUN_180001390, from last_idx_ + 1
  int first_held_key_up() const;    // scan 0..127
  int next_held_key_down() const;   // FUN_180001350, from last_idx_ − 1
  int last_held_key_down() const;   // scan 127..0
  int last_held_key_list() const;   // keys+0x208 held-key list, end → start
  int random_held_key();            // FUN_180001930
  void remove_held_key(int midi_note);
  void recompute_step_len();        // §7.2 formula + phase rescale

  int sr_ = 44100;
  float tempo_ = 120.0f;
  int step_len_ = 22050;
  int octaves_ = 1;
  float gate_ = 0.5f;
  int beat_div_ = 24;
  int beat_idx_ = 7;
  int pos_ = 0;
  bool gate_on_ = true;
  int dir_ = 0;
  int oct_idx_ = 0;
  int last_idx_ = -1;
  int cur_note_ = -1;
  int velocity_ = 127;
  int mode_ = 1;
  bool enabled_ = false;
  bool held_[128] = {};
  std::vector<int> held_list_;  // keyboardInfo+0x20c held-key list
};

}  // namespace synth1::arp
