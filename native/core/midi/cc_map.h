// cc_map.h — MIDI CC→param map + MIDI Learn state machine.
//
// Spec: out/spec_midi_settings.md (§1 map, §2 learn, §4 CSV).
// Ghidra cites:
//   DAT_1801a0be0        global map, 128 × int32, value = param id or -1
//   FUN_180037660 @34340 CC handler: map lookup + learn capture (data1 < 0x78)
//   FUN_180037310 @34170 default map builder (fill -1)
//   FUN_180037aa0 @34544 learn ENTER (flag=1, target=-1)
//   FUN_180037270 @34142 learn EXIT  (flag=0, target=-1)
//   FUN_180037620 @34319 assignable check (row[3] != -2)
//   FUN_1800279b0 @27623 CSV write (128 ints, comma separated, no trailing comma)
//   FUN_180027a70 @27658 CSV read (exactly 128 tokens, values < 0x100)

#pragma once

#include <cstdint>
#include <string>

namespace synth1::midi {

inline constexpr int CC_COUNT = 128;

// CCs 120..127 are running-status / reserved (All Notes Off, Reset Controllers,
// …) and are excluded from the map/learn path (spec_midi_settings §1: data1 < 0x78).
inline constexpr int CC_RESERVED_LO = 120;

// The four "midi ctrl src1/src2/assign1/assign2" params (row[3] == -2) are not
// assignable as learn targets (spec_midi_settings §1, FUN_180037620).
bool cc_param_assignable(int param_id);

class CcMap {
 public:
  int  map[CC_COUNT];   // param id (0..97) or -1 = unmapped
  bool learn_active = false;  // engine+0xbb5c
  int  learn_target = -1;     // engine+0xbb60

  CcMap();  // factory default: all -1 (FUN_180037310)

  static bool is_reserved(int cc) { return cc >= CC_RESERVED_LO && cc <= 127; }

  // Lookup: -1 for out-of-range or reserved CCs, else map[cc].
  int lookup(int cc) const;

  // Learn state machine.
  void learn_start();                 // flag=1, target=-1 (FUN_180037aa0)
  void learn_set_target(int param_id);// engine+0xbb60 = paramId (if assignable)
  void learn_end();                   // flag=0, target=-1 (FUN_180037270)

  // Capture during learn (FUN_180037660 learn branch): requires learn_active and
  // target != -1 and cc in 0..119. Clears any existing entry equal to target
  // (only one CC per param), then map[cc] = target. Returns true if applied.
  bool learn_capture(int cc);

  // Manual assign (dialog id 0x406): clear entries == param, then map[cc]=param.
  // cc must be 0..119 (reserved CCs excluded). Returns true if applied.
  bool assign(int cc, int param_id);

  // Clear a param's binding (map entries equal to param → -1). Returns count.
  int unassign_param(int param_id);

  // CSV (spec_midi_settings §4): exactly 128 decimal ints, comma separated,
  // no trailing comma. from_csv requires exactly 128 tokens, values < 0x100.
  std::string to_csv() const;
  bool from_csv(const std::string& csv);
};

}  // namespace synth1::midi
