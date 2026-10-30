// cc_map.cpp — MIDI CC→param map + MIDI Learn state machine + CSV.
//
// Spec: out/spec_midi_settings.md. Ghidra cites in cc_map.h.

#include "cc_map.h"

#include <cstdlib>

namespace synth1::midi {

// The four midi-ctrl src/assign params are not assignable (row[3] == -2).
bool cc_param_assignable(int param_id) {
  if (param_id < 0 || param_id > 97) return false;
  switch (param_id) {
    case 86: case 87: case 88: case 89: return false;  // src1/assign1/src2/assign2
    default: return true;
  }
}

CcMap::CcMap() {
  for (int i = 0; i < CC_COUNT; ++i) map[i] = -1;  // FUN_180037310 default
}

int CcMap::lookup(int cc) const {
  if (cc < 0 || cc >= CC_COUNT) return -1;
  if (is_reserved(cc)) return -1;  // CC 120..127 excluded
  return map[cc];
}

void CcMap::learn_start() {
  if (learn_active) return;  // FUN_180037aa0: already set → no-op
  learn_active = true;
  learn_target = -1;
}

void CcMap::learn_set_target(int param_id) {
  if (!cc_param_assignable(param_id)) return;  // FUN_180037620
  learn_target = param_id;
}

void CcMap::learn_end() {
  if (!learn_active) return;  // FUN_180037270: flag==0 → no-op
  learn_active = false;
  learn_target = -1;
}

bool CcMap::learn_capture(int cc) {
  if (!learn_active) return false;
  if (learn_target < 0) return false;      // no target picked → CC not consumed
  if (cc < 0 || cc >= CC_RESERVED_LO) return false;  // data1 < 0x78
  // Clear any existing entry equal to target (only one CC per param).
  for (int i = 0; i < CC_COUNT; ++i)
    if (map[i] == learn_target) map[i] = -1;
  map[cc] = learn_target;
  return true;
}

bool CcMap::assign(int cc, int param_id) {
  if (cc < 0 || cc >= CC_RESERVED_LO) return false;  // reserved excluded
  if (param_id < -1 || param_id > 97) return false;
  for (int i = 0; i < CC_COUNT; ++i)
    if (map[i] == param_id) map[i] = -1;
  map[cc] = param_id;
  return true;
}

int CcMap::unassign_param(int param_id) {
  int n = 0;
  for (int i = 0; i < CC_COUNT; ++i)
    if (map[i] == param_id) { map[i] = -1; ++n; }
  return n;
}

std::string CcMap::to_csv() const {
  // FUN_1800279b0: "%d," ×127 then map[127] with "%d" → 128 ints, no trailing comma.
  std::string out;
  for (int i = 0; i < CC_COUNT; ++i) {
    if (i) out += ',';
    out += std::to_string(map[i]);
  }
  return out;
}

bool CcMap::from_csv(const std::string& csv) {
  // FUN_180027a70: prefill -1, strtok on ", \t\n", atoi, accept < 0x100,
  // require exactly 128 tokens.
  int tmp[CC_COUNT];
  for (int i = 0; i < CC_COUNT; ++i) tmp[i] = -1;

  int count = 0;
  std::size_t i = 0;
  const std::size_t n = csv.size();
  while (i < n) {
    // skip delimiters
    while (i < n && (csv[i] == ',' || csv[i] == ' ' || csv[i] == '\t' || csv[i] == '\n')) ++i;
    if (i >= n) break;
    std::size_t start = i;
    while (i < n && !(csv[i] == ',' || csv[i] == ' ' || csv[i] == '\t' || csv[i] == '\n')) ++i;
    const std::string tok = csv.substr(start, i - start);
    if (tok.empty()) continue;
    long v = std::strtol(tok.c_str(), nullptr, 10);
    if (v >= 0x100) return false;  // reject values >= 256
    if (count >= CC_COUNT) return false;  // too many tokens
    tmp[count++] = (int)v;
  }
  if (count != CC_COUNT) return false;  // format error

  for (int k = 0; k < CC_COUNT; ++k) map[k] = tmp[k];
  return true;
}

}  // namespace synth1::midi
