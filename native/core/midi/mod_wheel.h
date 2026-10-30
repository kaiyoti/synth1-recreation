// mod_wheel.h — Mod Wheel (MIDI CC 0x01) two-slot controller + pitch bend.
//
// Spec: out/spec_modwheel.md (§1-§6) + out/spec_env.md §6.5 (pitch bend).
// Ghidra cites:
//   FUN_180037660 @34339  the real wheel/CC handler (two wheel-source slots)
//   FUN_180028fe0 @28693  dispatcher header: the modulation scaling law (28718-28746)
//   FUN_180028fb0 @28676  pitch-bend target (0xe0) → engine+0xbb22
//   FUN_180028300 @28091  pitch-bend per-sample slew (±1) + per-voice bend offset
//
// Engine wheel slots (ctor FUN_180025110, spec_modwheel §2): two slots, stride
// 0x10, each { int src; float sens; int assign; float value }. Default src =
// 0xb001 = (status 0xb0 << 8) | CC1 = "#1:mod wheel"; assign = -1 (unassigned).

#pragma once

#include <cstdint>

namespace synth1::midi {

// CC1 = modulation wheel; the default slot source descriptor (status<<8)|cc.
inline constexpr int   MOD_WHEEL_CC  = 1;
inline constexpr int   MOD_WHEEL_SRC = 0xb001;

// One modulation slot (spec_modwheel §2).
struct ModSlot {
  int   src    = MOD_WHEEL_SRC;  // (srcIndex<<8)|status → default #1 mod wheel
  float sens   = 0.0f;           // per-slot sensitivity, [-1,+1]
  int   assign = -1;             // destination param index; -1 = unassigned
  float value  = 0.0f;           // live normalized source value (0..1)
};

// Sensitivity mapping (spec_modwheel §3, cases 50/51): raw 0..127 → [-1,+1].
// Decomp: i=clamp(v,0,127); s = (i-63, i>63→i-64) * (1/63). Yields exactly
// [-1,+1] with a center dead zone (63 and 64 both → 0).
float sens_from_raw(int raw);

// Incoming CC normalized to 0..1 (spec_modwheel §1: data2 * 1/127).
inline float norm_cc(int cc) { return (float)cc * (1.0f / 127.0f); }

// The two-slot mod-wheel controller.
class ModWheel {
 public:
  ModSlot slot[2];  // slot[0] = slot 1 (sens1), slot[1] = slot 2 (sens2)

  ModWheel();

  // Setters mirror the engine setters (which re-dispatch the assign's value;
  // re-dispatch is the caller's responsibility here).
  void set_sens(int s, float sens);
  void set_src(int s, int src);     // also zeroes the slot value (FUN_100319e0)
  void set_assign(int s, int assign);

  // 32-bit src setter FUN_100319e0 (:35821) validates src against the
  // 13-entry src table (0xB001..0xB00B, 0xD000, 0xE000 — dumped from DLL
  // .data VA 0x10080064, reports/m3_event_sweep.md §3); a src outside the
  // table is REJECTED (no write, no dest re-notify). -1 is not in the
  // table (the sweep's src=-1 rows behave as rejection: the slot keeps its
  // previous src; the generic scan's nibble test would also never match).
  static bool src_in_table(int src) {
    const int hi = (src >> 8) & 0xff;
    const int lo = src & 0xff;
    if (hi == 0xb0) return lo >= 0x01 && lo <= 0x0b;
    return src == 0xd000 || src == 0xe000;
  }

  // Generic-handler slot match (FUN_10031700 :35712-35730): match iff
  // status == src>>8, and for 0xb0 additionally data1 == src&0xff.
  // The handler scans BOTH slots (loop 2x stride 0x10) — a control can
  // drive both.
  bool slot_matches(int s, int status, int data1) const {
    const int src = slot[s].src;
    return ((src >> 8) & 0xff) == (status & 0xff) &&
           (status != 0xb0 || (src & 0xff) == (data1 & 0xff));
  }

  // Feed a control message. Matches a slot when (status<<8)|data1 == slot.src;
  // on match stores value = data2/127. Returns the matched slot index or -1.
  int on_cc(int status, int data1, int data2);

  // Modulation scaling law (spec_modwheel §4):
  //   mod(p) = base(p) + (max-min+1) * value * sens   (summed over matching slots)
  // then round + clamp to [min,max]. Returns the modulated param value.
  float modulate(int param_id, float base, int min, int max) const;
};

// ---------------------------------------------------------------------------
// Pitch bend — 32-bit render DLL live path (m3-engine-events).
//   Dispatcher FUN_10012730 case 0xe0 (:13716) -> bend setter
//   FUN_10025900 (:29072): wheel = (bend14 + (bend14<0 ? 127 : 0)) >> 7,
//   -64 -> -63, stored engine+0xac8e; arms the slew flag engine+0xac90 = 1
//   (store [this+0xac84] with this = engine+0xc — report 90's displacement
//   scan missed it; the slew is LIVE, contrary to its "statically 0" note).
//   Render loop FUN_10024ea0 (:28511): while the flag is set the chunk is
//   capped at 0x20 and FUN_10024c20 (:28374) advances the wheel ±1 ONCE PER
//   CHUNK (1 wheel unit per 32 frames) and writes the per-voice offset
//   trunc(trunc(wheel * range * 128 / 63) / 12) (the 0x2aaaaaab sequence =
//   signed /12 truncation) — reproduces every clean sweep row exactly.
// ---------------------------------------------------------------------------
class PitchBend {
 public:
  int  target  = 0;     // engine+0xac8e (wheel units)
  int  current = 0;     // engine+0xac8c (slewed wheel)
  bool slewing = false; // engine+0xac90 (chunk-cap gate flag)
  int  range   = 2;     // engine+0x14 (param 40 band, semitones, short)

  // Build the signed 14-bit bend from a 0xe0 message (data1=LSB, data2=MSB):
  // ((MSB-0x40)*0x80 + LSB) ∈ [-8192,8191] (FUN_10012730 :13800).
  static int signed_bend(int data1, int data2);

  // Wheel target (FUN_10025900): (v + (v<0?127:0)) >> 7, -64 -> -63.
  static int wheel_from_bend14(int bend14);

  // Set the target from a signed14 bend and arm the slew (FUN_10025900).
  void set_bend14(int bend14);

  // Set the bend range (param 40, 0..24).
  void set_range(int semitones);

  // Advance the slew by one CHUNK step (FUN_10024c20 head): move current
  // ±1 toward target, snap on arrival, clear the flag when current == target.
  void slew_one_chunk();

  // Per-voice bend offset (FUN_10024c20): trunc(trunc(cur*range*128/63)/12),
  // 1/128-octave units. range 0 => 0 (bend INERT).
  int bend_offset() const;
};

}  // namespace synth1::midi
