// spec_constants.h — decompile-derived constants for the Synth1 DSP engine.
//
// Convention (RECONSTRUCTION_PLAN.md §8): every constant is tagged with its
// Ghidra address (the FUN_/DAT_ it was read from in out/decomp_all.c) and a
// comment citing the spec file/section that states it. `static_assert`s tie
// the relationships together.
//
// The decompile is the 64-bit image (base 0x180000000); the wine render
// target is 32-bit (base 0x10000000). Addresses below are 64-bit decompile
// addresses — see plan §8 "The 32-bit vs 64-bit gotcha".

#pragma once

#include <cstdint>
#include <cmath>

namespace synth1::spec {

// ---------------------------------------------------------------------------
// Oscillator (spec_osc.md)
// ---------------------------------------------------------------------------

// 32-bit phase accumulator, full cycle = 2^27.
// Ghidra: FUN_1800398b0 (one of the 72 kernel fns); spec_osc.md §1.
inline constexpr std::uint32_t PHASE_BITS = 27u;
inline constexpr std::uint32_t PHASE_CYCLE = 1u << PHASE_BITS;  // 134217728
inline constexpr std::uint32_t PHASE_MASK = PHASE_CYCLE - 1u;  // 0x7ffffff

// 2048-entry float wavetables.
// Ghidra: DAT_180071c40 (wavetable base); spec_osc.md §2.
inline constexpr std::uint32_t WAVE_TABLE_SIZE = 2048u;
inline constexpr std::uint32_t WAVE_TABLE_MASK = WAVE_TABLE_SIZE - 1u;  // 0x7ff

// Index = (phase >> 16) & 0x7ff; linear blend with 1/65536 granularity.
// Ghidra: the index expression in each of the 72 leaf kernels; spec_osc.md §3.
inline constexpr std::uint32_t PHASE_INDEX_SHIFT = 16u;
inline constexpr std::uint32_t BLEND_SHIFT = 16u;  // 1/65536
inline constexpr float BLEND_SCALE = 1.0f / 65536.0f;

// 8-voice unison detune arrays detHz[0..7], detPhase[0..7].
// Ghidra: unison voice ctor; spec_osc.md §5.
inline constexpr std::uint32_t UNISON_VOICES = 8u;

// 21-bit noise LFSR (noise waveform).
// Ghidra: noise kernel leaf; spec_osc.md §4, kernel_map.md.
inline constexpr std::uint32_t NOISE_LFSR_BITS = 21u;

// ---------------------------------------------------------------------------
// Filter (spec_filter.md)
// ---------------------------------------------------------------------------

// 5 filter types: LP12, LP24, HP12, HP24, LPDL (ftype 4).
// Ghidra: filter dispatcher; spec_filter.md §1.
inline constexpr std::uint32_t FILTER_TYPE_COUNT = 5u;

// 1024-entry geometric cutoff table, 15.0..15891.43 Hz, ratio 1.006832.
// Ghidra: DAT_180072680 (cutoff table base); spec_filter.md §2.
inline constexpr std::uint32_t FC_TABLE_SIZE = 1024u;
inline constexpr float FC_TABLE_MIN = 15.0f;
inline constexpr float FC_TABLE_MAX = 15891.43f;
// ratio = (max/min)^(1/1023) = 1.006832
inline constexpr float FC_TABLE_RATIO = 1.006832f;

// Pole count n≈4 (Tustin one-pole biquad).
// Ghidra: coefficient fn; spec_filter.md §3.
inline constexpr std::uint32_t FILTER_POLES = 4u;

// tanh soft-clip + leveler (Padé approximation constants).
// Ghidra: DAT_180072f40 (soft-clip coeffs); spec_filter.md §4.
// (Padé [3/3] tanh approximation coefficients — see spec_filter.md §4 for the
// exact .rdata bytes; the leveler gain is 1.0f by default.)

// ---------------------------------------------------------------------------
// LPDL over-sampling (spec_lpdlosc.md)
// ---------------------------------------------------------------------------

// ftype-4 only: sub = (FS <= 48000) ? 2 : 1, per-call (no cached flag).
// Ghidra: FUN_1800398b0, `cmp $0xbb80`; spec_lpdlosc.md §1.
inline constexpr std::uint32_t LPDL_FS_THRESHOLD = 48000u;
inline constexpr std::uint32_t LPDL_MAX_SUB = 2u;
inline constexpr float LPDL_GAIN_BASE = 0.65f;  // gain = sub·0.65·(1+3·res)

// ---------------------------------------------------------------------------
// Master volume (spec_mastervol.md)
// ---------------------------------------------------------------------------

// param 0xfe/254 (common-setting, NOT in the 98-case dispatcher).
// Ghidra: FUN_1800288e0 (case 0xfe); spec_mastervol.md §1.
inline constexpr std::uint32_t MASTER_VOL_PARAM_ID = 0xfeu;
inline constexpr std::uint8_t MASTER_VOL_DEFAULT = 0x40u;  // raw 0..127, default 0x40

// Common-settings block: ids 0xfb..0xfe (only 0xfe touches gain fields).
// Ghidra: FUN_180028cb0 (loop 0xfb..0xff); spec_mastervol.md §1.
inline constexpr std::uint32_t COMMON_SETTINGS_LO = 0xfbu;
inline constexpr std::uint32_t COMMON_SETTINGS_HI = 0xfeu;

// ---------------------------------------------------------------------------
// Phase control (spec_phase.md)
// ---------------------------------------------------------------------------

// param 91 (osc) / 92 (unison); immobilize flag CVco+0x1b4, offset (p−1)/126 cycles.
// Ghidra: FUN_180028fe0 cases 0x5b/0x5c; spec_phase.md §1.
inline constexpr std::uint32_t PHASE_OSC_PARAM = 91u;
inline constexpr std::uint32_t PHASE_UNISON_PARAM = 92u;
inline constexpr float PHASE_OFFSET_DENOM = 126.0f;  // offset = (p-1)/126

// ---------------------------------------------------------------------------
// Mod wheel (spec_modwheel.md)
// ---------------------------------------------------------------------------

// CC1 → FUN_180037660 case1; value = cc·1/127.
// Ghidra: FUN_180037660; spec_modwheel.md §1.
inline constexpr float MOD_WHEEL_SCALE = 1.0f / 127.0f;

// ---------------------------------------------------------------------------
// Preset (spec_preset.md)
// ---------------------------------------------------------------------------

// 1280 presets: 10 banks × 128; preset table CBank[100] × stride 0x2a50.
// Ghidra: CBank ctor; spec_preset.md §1.
inline constexpr std::uint32_t PRESET_BANKS = 10u;
inline constexpr std::uint32_t PRESET_PER_BANK = 128u;
inline constexpr std::uint32_t PRESET_TOTAL = PRESET_BANKS * PRESET_PER_BANK;  // 1280
inline constexpr std::uint32_t PRESET_TABLE_SIZE = 100u;  // CBank[100]
inline constexpr std::uint32_t PRESET_STRIDE = 0x2a50u;  // bytes per preset

// Patch = CSoundData: 100 (id,value) pairs.
// Ghidra: getChunk 0xA40 layout; spec_preset.md §2.
inline constexpr std::uint32_t PATCH_PAIRS = 100u;
inline constexpr std::uint32_t PATCH_CHUNK_SIZE = 0xA40u;  // bytes

// ---------------------------------------------------------------------------
// MIDI CC map (spec_midi_settings.md)
// ---------------------------------------------------------------------------

// CC map DAT_1801a0be0 = 128×int32 (value = param id or −1; CC 120–127 excluded).
// Ghidra: DAT_1801a0be0; spec_midi_settings.md §1.
inline constexpr std::uint32_t CC_MAP_SIZE = 128u;

// ---------------------------------------------------------------------------
// 99-case parameter→DSP dispatcher (spec_midi_settings.md / plan §4)
// ---------------------------------------------------------------------------

// The 99-case dispatcher FUN_180028fe0 + 98-row param table DAT_180093f10
// (row = id,min,max,defaultCC,name).
// Ghidra: FUN_180028fe0, DAT_180093f10; plan §4.
inline constexpr std::uint32_t DISPATCHER_CASES = 99u;
inline constexpr std::uint32_t PARAM_TABLE_ROWS = 98u;

// ---------------------------------------------------------------------------
// Relationship static_asserts (plan §4: "every behavioral claim linked to a spec")
// ---------------------------------------------------------------------------

// (1u<<27) >> 16 == 2048  (phase index space)
static_assert(((1u << 27) >> 16) == 2048u,
  "phase index space: (2^27)>>16 must equal 2048 (WAVE_TABLE_SIZE)");

// 0x7ff == 2048-1
static_assert(0x7ffu == WAVE_TABLE_MASK,
  "0x7ff must equal 2048-1 (wavetable index mask)");

// PHASE_CYCLE >> PHASE_INDEX_SHIFT == WAVE_TABLE_SIZE
static_assert((PHASE_CYCLE >> PHASE_INDEX_SHIFT) == WAVE_TABLE_SIZE,
  "phase cycle shifted by index shift must equal wavetable size");

// FC table ratio check: (max/min)^(1/1023) ≈ 1.006832
static_assert(std::pow(FC_TABLE_MAX / FC_TABLE_MIN, 1.0 / 1023.0) > 1.006831f &&
              std::pow(FC_TABLE_MAX / FC_TABLE_MIN, 1.0 / 1023.0) < 1.006833f,
  "FC table geometric ratio must be ~1.006832 (spec_filter.md §2)");

// PRESET_TOTAL = 1280
static_assert(PRESET_TOTAL == 1280u, "10 banks × 128 = 1280 presets");

// MASTER_VOL_DEFAULT is in range
static_assert(MASTER_VOL_DEFAULT <= 127u, "master vol default must be 0..127");

// LPDL threshold
static_assert(LPDL_FS_THRESHOLD == 48000u, "LPDL FS threshold is 48000");

// CC map
static_assert(CC_MAP_SIZE == 128u, "CC map is 128 entries");

// Param table rows
static_assert(PARAM_TABLE_ROWS == 98u, "98-row param table (DAT_180093f10)");

// Common settings range
static_assert(COMMON_SETTINGS_LO == 0xfbu && COMMON_SETTINGS_HI == 0xfeu,
  "common-settings block is ids 0xfb..0xfe");

}  // namespace synth1::spec
