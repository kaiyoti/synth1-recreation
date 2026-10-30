// preset.h — Synth1 preset (.sy1) file reader + getChunk 0xA40 round-trip.
//
// Plan stage 8 / M4 groundwork (RECONSTRUCTION_PLAN.md §3 stage 8, §4):
//   "original preset mapping, compatibility, remaining discrepancies".
//
// Spec: out/spec_preset.md (the authoritative spec for this task).
//
// ---------------------------------------------------------------------------
// .sy1 file format (spec_preset.md §3: "write FUN_18001e710; read
// FUN_18001e350 @21481-21560; sample install/001.sy1" [C])
//
//   Text, CRLF-terminated:
//     line 1 = preset name          (e.g. "Synth1 brastring")
//     line 2 = "color=<default|name|hex>"  (parse fn FUN_180004490)
//     line 3 = "ver=%d"            (105 in the factory samples)
//     then N lines of "<paramId>,<value>"   (N = selector+0x55c, the pair count)
//
//   File name: "%03d.sy1" (sound number) in the bank's folder (CBank+0x110).
//   Param ids are Synth1 param indices (0..0xff, sparse); values are raw 0..127.
//
//   The N "<id>,<value>" lines are the CSoundData (id,value) pairs — up to
//   100 (spec_preset.md §1: "CSoundData ... +0x2c int paramIds[100] (−1 =
//   empty); +0x1bc uint values[100]; +0x34c int count" [C]). The .sy1 files in
//   install/bank0/ carry fewer than 100 (001.sy1 = 73); the rest are "not
//   defined" and get the params.gen.h defaults (plan §4 "the 100 pairs are a
//   subset; the rest get defaults").
//
// ---------------------------------------------------------------------------
// getChunk 0xA40 layout (spec_preset.md §3: "getChunk (23) = FUN_180019280 →
// FUN_180027db0 @27832 [C]: returns size 0xa40")
//
//   +0x00   "Synth1 VST Chunk Data"        (20-byte magic; +0x20 is the next int)
//   +0x20   int 2                          (version)
//   +0x24   int ini "hostselectprogram"    (spec-faithful: 0; not in .sy1)
//   +0x28   char[0x2c] bank name           (doc+0x354; 44 bytes)
//   per param (id 0..255):
//     +0x47 + id     presence byte
//     +0x23c + id*8  value dword
//   +0xa0c   int 0x71 (113 = flag-block param count)
//   +0xa14   int bank
//   +0xa1c   int sound
//   +0xa2c   int program (AEffectX+0x28 = bank*0x80+sound)
//   size = 0xA40 (2624 bytes).
//
//   The 262-param region +0x47..+0xa0b (113*8=904 B) exactly tiles 262 params:
//   presence dword + value dword, contiguous, id 0..261. 0xA40 - 0xa0c = 0x34
//   (52) = 13 trailing dwords (flag count + bank + sound + program + 10 pad).
//   See preset.cpp comments for the id-mapping decision (spec-faithful: the
//   255 engine slots occupy param ids 0..254; the 261-slot region is the
//   original's superset — 255 engine params + 5 unused — and is padded).
//
// ---------------------------------------------------------------------------
// Cited spec addresses (spec_preset.md):
//   CBank[100] × stride 0x2a50      DAT_180098150 (CBank ctor FUN_18001c110 @19687;
//                                   malloc(0x2a50) FUN_18001d350 @20510)
//   lookup                          FUN_18001dc70(bank, folderName) @20999
//   1280 presets (10×128)           spec_preset.md §1 [C]
//   CSoundData (id,value)           FUN_18001d600 @20628 (reads pair i:
//                                   id=+0x2c+4i, val=+0x1bc+4i), count +0x34c
//   program routing bank*128+sound  spec_preset.md §1 ("VST program id =
//                                   bank*0x80 + sound")
//   .sy1 format                    spec_preset.md §3 (write FUN_18001e710,
//                                   read FUN_18001e350 @21481-21560)
//   getChunk 0xA40                 FUN_180027db0 @27832 [C]
//   setChunk                       FUN_18001a2d0 @17965 / FUN_180028cb0 @28534

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace synth1::preset {

// ---------------------------------------------------------------------------
// Program / bank routing constants (spec_preset.md §1)
// ---------------------------------------------------------------------------

// 10 internal banks × 128 = 1280 presets; 10..99 are user folder banks.
inline constexpr std::int32_t BANKS_INTERNAL = 10;
inline constexpr std::int32_t PER_BANK = 128;
inline constexpr std::int32_t BANKS_TOTAL = 100;          // CBank[100]
inline constexpr std::int32_t PRESET_TOTAL = 10 * 128;    // 1280
inline constexpr std::int32_t BANK_STRIDE = 0x2a50;       // bytes per CBank
inline constexpr std::int32_t BANK_INVALID = -1;          // ext / "not found"

// VST program id = bank*0x80 + sound (spec_preset.md §1, §2 effSetProgram).
inline std::uint32_t program_of(std::int32_t bank, std::int32_t sound) {
  return (std::uint32_t)((bank & 0x1ff) << 7) | (std::uint32_t)(sound & 0x7f);
}
// The 0x3200 (12800) pseudo-program = "Ext. sel. from Synth1…" (bank == -1).
inline constexpr std::uint32_t PROGRAM_EXT = 0x3200u;

// The 255 engine parameter slots (plan §4; params.gen.h kTotalSlots).
inline constexpr std::size_t PARAM_SLOTS = 255;

// The getChunk 0xA40 chunk size (spec_preset.md §3 [C]).
inline constexpr std::size_t CHUNK_SIZE = 0xA40;

// ---------------------------------------------------------------------------
// ParamPair: one (id,value) from a .sy1 file (the CSoundData pair).
// ---------------------------------------------------------------------------
struct ParamPair {
  std::uint16_t id;     // Synth1 param index (0..0xff; sparse)
  std::int32_t  value;  // dispatcher band (signed; see events::Event)
  // #134: 1 = value injected by upgrade_v105 from the init sound (not a
  // file-chunk pair); the engine dispatch skips the range clamp for these
  // (device init path bypasses FUN_180028fe0's clamp — see events::Event).
  std::uint8_t  init_inject = 0;
};

// ---------------------------------------------------------------------------
// PresetState: the loaded patch state.
//   raw_params[] = the 255 engine slots (raw 0..127).
//   present[]    = whether that slot was present in the .sy1 (vs default).
//   name/color   = the .sy1 header (line 1 / line 2).
//   ver          = the "ver=%d" (105 in the factory samples).
//   pair_count   = the number of (id,value) lines in the .sy1 (the CSoundData
//                 count; +0x34c in the decompile).
// ---------------------------------------------------------------------------
struct PresetState {
  std::int32_t  raw_params[PARAM_SLOTS] = {};
  bool          present[PARAM_SLOTS] = {};
  std::string   name;
  std::string   color;
  int           ver = 0;
  int           pair_count = 0;
  std::int32_t  bank = 0;
  std::int32_t  sound = 0;

  // The (id,value) pairs exactly as they appeared in the .sy1 (for round-trip
  // write-back in the same file order — the .sy1 writes them in file order).
  std::vector<ParamPair> pairs;

  // Apply to an engine's raw_params[] (plan §4: raw 0..127 is the band).
  // Fills raw_params for present slots; the caller may default the rest.
  void apply_raw(std::int32_t out[PARAM_SLOTS]) const {
    for (std::size_t i = 0; i < PARAM_SLOTS; ++i)
      if (present[i]) out[i] = raw_params[i];
  }
};

// ---------------------------------------------------------------------------
// Reader (spec_preset.md §3: read FUN_18001e350 @21481-21560 [C])
// ---------------------------------------------------------------------------

// Parse a .sy1 file (path) into a PresetState. Returns true on success.
// On failure, *err (if non-null) is set to a short message.
bool load_sy1(const std::string& path, PresetState& out, std::string* err = nullptr);

// Parse .sy1 content already in memory (same as load_sy1 but no I/O).
bool parse_sy1(const std::string& text, PresetState& out, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Patch-version upgrade — the device load-time chain FUN_10023230
// (32-bit DLL; 64-bit FUN_180027c70 → FUN_180026a10).
//
// Before dispatching a loaded patch, the original rewrites patches whose
// "ver=" is below 113 (0x71). It walks the file's pair list through
// switch(ver) WITH FALLTHROUGH, so each stage has its own version gate
// (reports/upgrade_model_spec.md §1; holes at ver 101..104 and 110):
//   ver 100            FUN_10023430 (curve remaps, not exercised)
//   ver 100/105/106    FUN_10023ab0 (curve remaps 13/16/18/21/26/28/37/52/
//                      54/55; snapshot resets 75..85/90 incl. 77/78) +
//                      FUN_100242d0 (84:=64, 85:=24, 86..89:=0)
//   ver 100/105..108   FUN_10024150 (50/51 rescaled by file 44/43; 75 from
//                      file 38; snapshot 86..89/91/92/94; 93 from file 38)
//   ver 100/105..109   95..97 := snapshot
//   ver 100/105..109/111/112  98 := snapshot
// References read the FILE values (scratch list copied back after the
// loop); the snapshot is the pre-load program state = boot init sound on a
// fresh load. All 128 bank0 presets are ver=105; the file-specified params
// are verified value-for-value against install/bank0_v113_upgraded.json by
// tests/test_preset.cpp (the dump's entries for file-ABSENT ids are the
// engine-table values, not the device native-load fill — see the test).
//
// Ids 73..98 absent from a ver<108 file are first filled from the init
// sound (engine+0xb5b0), as the original's patch document carries all of
// them. p94 = 16 is the device boot init sound (pool 16, #81).
// Returns true when the patch was upgraded (ver < 113); ver becomes 113.
// ---------------------------------------------------------------------------
inline constexpr int SY1_VER_CURRENT = 113;  // 0x71
bool upgrade_v105(PresetState& st);

// ---------------------------------------------------------------------------
// getChunk 0xA40 round-trip (spec_preset.md §3: getChunk FUN_180027db0 @27832)
// ---------------------------------------------------------------------------

// Serialize a PresetState into a 0xA40-byte buffer (the getChunk layout).
// Returns the buffer. The caller owns the returned bytes.
std::vector<std::uint8_t> write_chunk(const PresetState& s);

// Inverse of write_chunk: parse a 0xA40-byte buffer back into a PresetState.
// Returns true on success (size + magic + version must be valid).
bool read_chunk(const std::uint8_t* buf, std::size_t len, PresetState& out,
               std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Engine parameter application (plan stage 8 / M4: preset → engine params)
//
// Apply a PresetState to an engine's parameter buffers. Every slot 0..254 is
// first set to its default, then the preset's (id,value) pairs are overlaid.
//
//   raw_params[id]  = pair.value            (raw 0..127 band; plan §4)
//   norm_params[id] = pair.value / 127.0f   (normalized 0..1; matches
//                     engine.h's norm_params[id] = band * (1/127))
//
// Default for a slot not present in the preset: params.gen.h carries only the
// display ranges (sy1_min/sy1_max), not a per-slot default, and the engine
// zero-initializes raw_params[255] (engine.h:46), so the sensible fallback
// default is 0 (raw band 0). See docs/reconstruction-status.md stage 8.
// ---------------------------------------------------------------------------
void apply_preset_to_params(const PresetState& preset,
                            std::int32_t raw_params[PARAM_SLOTS],
                            float norm_params[PARAM_SLOTS]);

// ---------------------------------------------------------------------------
// Bank / program routing (spec_preset.md §1: CBank[100]; §2 program/bank SELECT)
// ---------------------------------------------------------------------------

// The 1280-preset table (10 banks × 128) + ext folder banks (bank 10..99 via
// ini bankfolder%d; spec_preset.md §1 [C]). A loaded bank holds its 128
// PresetStates (lazy in the original; here we load on demand).
class BankManager {
 public:
  // Register a bank's folder (bank 0..99). The bank lazily reads
  // "<folder>/<nnn>.sy1" on load_bank().
  void add_bank(std::int32_t bank, const std::string& folder);

  // Look up the folder for a registered bank ("" if unknown).
  std::string bank_folder(std::int32_t bank) const;

  // Load (bank, program) → PresetState (the .sy1 for <program> in <bank>).
  // bank < 0 (ext) or unregistered → returns false + err.
  bool load_bank(std::int32_t bank, std::int32_t program, PresetState& out,
                std::string* err = nullptr);

  // Program change: map (bank, program) → preset and apply to raw_params[].
  // Returns the VST program id (bank*0x80+program; 0x3200 if bank<0).
  std::uint32_t program_change(std::int32_t bank, std::int32_t program,
                               std::int32_t raw_params[PARAM_SLOTS],
                               std::string* err = nullptr);

  // Is this a valid internal bank index (0..9)?
  static bool is_internal(std::int32_t bank) {
    return bank >= 0 && bank < BANKS_INTERNAL;
  }
 private:
  struct BankRec { std::string folder; bool known = false; };
  BankRec banks_[BANKS_TOTAL];  // CBank[100] analogue
};

}  // namespace synth1::preset
