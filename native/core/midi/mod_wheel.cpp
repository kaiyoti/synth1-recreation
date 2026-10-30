// mod_wheel.cpp — Mod Wheel two-slot controller + pitch bend slew/bend law.
//
// Spec: out/spec_modwheel.md; out/spec_env.md §6.5. Ghidra cites in mod_wheel.h.

#include "mod_wheel.h"

#include <cmath>

namespace synth1::midi {

// ---------------------------------------------------------------------------
// Sensitivity (spec_modwheel §3, cases 50/51): i=clamp(v,0,127);
// s = (i-63, i>63→i-64) * (1/63)  →  s ∈ [-1,+1] (DAT_180072e80 = 1/63).
// ---------------------------------------------------------------------------
float sens_from_raw(int raw) {
  int i = raw;
  if (i < 0) i = 0;
  if (i > 127) i = 127;
  const int centered = (i > 63) ? (i - 64) : (i - 63);
  float s = (float)centered * (1.0f / 63.0f);
  if (s < -1.0f) s = -1.0f;
  if (s > 1.0f) s = 1.0f;
  return s;
}

// ---------------------------------------------------------------------------
// ModWheel
// ---------------------------------------------------------------------------
ModWheel::ModWheel() {
  slot[0] = ModSlot{};  // src 0xb001, sens 0, assign -1, value 0
  slot[1] = ModSlot{};
}

void ModWheel::set_sens(int s, float sens) {
  if (s < 0 || s > 1) return;
  if (sens < -1.0f) sens = -1.0f;
  if (sens > 1.0f) sens = 1.0f;
  slot[s].sens = sens;
}

void ModWheel::set_src(int s, int src) {
  if (s < 0 || s > 1) return;
  slot[s].src = src;
  slot[s].value = 0.0f;  // FUN_180037a10 zeroes the live value on src change
}

void ModWheel::set_assign(int s, int assign) {
  if (s < 0 || s > 1) return;
  slot[s].assign = assign;
}

int ModWheel::on_cc(int status, int data1, int data2) {
  const int key = ((status & 0xff) << 8) | (data1 & 0xff);
  int first = -1;
  for (int s = 0; s < 2; ++s) {
    if (key == (slot[s].src & 0xffff)) {
      slot[s].value = norm_cc(data2);  // data2 * 1/127 (spec §1)
      if (first < 0) first = s;        // a CC can drive both slots; report the first
    }
  }
  return first;
}

float ModWheel::modulate(int param_id, float base, int min, int max) const {
  float v = base;
  for (int s = 0; s < 2; ++s) {
    if (slot[s].assign == param_id)
      v += (float)(max - min + 1) * slot[s].value * slot[s].sens;  // (hi-lo+1)*value*sens
  }
  v = std::round(v);                 // dispatcher header: clamp(round(v), lo, hi)
  if (v < (float)min) v = (float)min;
  if (v > (float)max) v = (float)max;
  return v;
}

// ---------------------------------------------------------------------------
// PitchBend
// ---------------------------------------------------------------------------
int PitchBend::signed_bend(int data1, int data2) {
  // decomp 13494: (param_4 - 0x40) * 0x80 + (ushort)param_3
  return (data2 - 64) * 128 + (data1 & 0x7f);
}

int PitchBend::wheel_from_bend14(int bend14) {
  // FUN_10025900: (bend14 + (bend14<0 ? 0x7f : 0)) >> 7 (arithmetic),
  // -0x40 -> -0x3f. +8191 -> 63, -8192 -> -63.
  const int adj = bend14 + (bend14 < 0 ? 127 : 0);
  int t = (adj >= 0) ? (adj >> 7) : -(((-adj) + 127) >> 7);
  if (t == -64) t = -63;
  return t;
}

void PitchBend::set_bend14(int bend14) {
  target = wheel_from_bend14(bend14);
  slewing = (current != target);  // FUN_10025900 arms engine+0xac90 = 1
}

void PitchBend::set_range(int semitones) {
  if (semitones < 0) semitones = 0;
  if (semitones > 24) semitones = 24;
  range = semitones;
}

void PitchBend::slew_one_chunk() {
  // FUN_10024c20 head: move current ±1 toward target, snap on arrival;
  // clear the flag (engine+0xac90) when current == target.
  if (current < target) {
    current = (current + 1 < target) ? current + 1 : target;
  } else if (current > target) {
    current = (current - 1 > target) ? current - 1 : target;
  }
  if (current == target) slewing = false;
}

int PitchBend::bend_offset() const {
  // FUN_10024c20: iVar3 = (cur * range * 0x80) / 0x3f (C trunc division),
  // then /12 with the 0x2aaaaaab sequence = signed trunc division by 12.
  // 1/128-octave units. range 0 => 0 (bend INERT, sweep p40=0 row).
  const int v = (current * range * 128) / 63;
  return v / 12;
}

}  // namespace synth1::midi
