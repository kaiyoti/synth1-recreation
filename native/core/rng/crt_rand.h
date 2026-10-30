// crt_rand.h — the device's CRT rand() stream (FUN_180055b98).
//
// FUN_180055b98 is the statically linked MSVC rand(): the per-thread state
// _getptd()->_holdrand advances as s = s·0x343FD + 0x269EC3 and returns
// (s >> 16) & 0x7FFF. The DLL never calls srand(), so the stream starts at the
// CRT default _holdrand = 1 on the (single) host thread that drives the plugin.
// Every consumer shares this ONE stream, in device call order:
//   * FUN_180015450  LFO wave setter (params 42/47), waves 3/4 only
//   * FUN_1800152e0  LFO key-sync reset (first live key down), waves 3/4 only
//   * FUN_180014f60  LFO period wrap, waves 3/4 only
//   * FUN_180001b40  arpeggiator random mode: rand()%(octaves+1), then
//   * FUN_180001930  rand()%heldCount (the random held key)
// (The oscillator-phase LCG DAT_1800938d4 is a SEPARATE stream, seed 1 —
// core/voice/voice.cpp — and the noise osc is a per-voice LFSR.)

#pragma once

#include <cstdint>

namespace synth1::rng {

inline std::uint32_t g_crt_holdrand = 1u;

inline void crt_srand(std::uint32_t seed = 1u) { g_crt_holdrand = seed; }

inline int crt_rand() {
  g_crt_holdrand = g_crt_holdrand * 0x343FDu + 0x269EC3u;
  return (int)((g_crt_holdrand >> 16) & 0x7FFFu);
}

}  // namespace synth1::rng
