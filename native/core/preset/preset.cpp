// preset.cpp — Synth1 .sy1 reader + getChunk 0xA40 round-trip + bank routing.
//
// Spec: out/spec_preset.md (authoritative for plan stage 8 / M4).
// Cited addresses are in preset.h's header.

#include "preset/preset.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace synth1::preset {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

// Trim leading/trailing ASCII whitespace (the .sy1 uses CRLF; a stray \r or
// space would break an id/value parse, so trim defensively).
std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  std::size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return std::string();
  std::size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

// Parse an integer from a string (no external deps; the values are small).
bool to_int(const std::string& s, int& out) {
  if (s.empty()) return false;
  int v = 0; bool neg = false; std::size_t i = 0;
  if (s[0] == '-') { neg = true; i = 1; }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  out = neg ? -v : v;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// .sy1 parser (spec_preset.md §3: read FUN_18001e350 @21481-21560 [C])
//
// Format (verified against install/bank0/001.sy1 raw bytes):
//   line 1 = name
//   line 2 = "color=<default|name|hex>"
//   line 3 = "ver=%d"
//   then "<id>,<value>" lines (001.sy1 = 73 pairs).
// ---------------------------------------------------------------------------

bool parse_sy1(const std::string& text, PresetState& out, std::string* err) {
  out = PresetState();
  // Split into lines (CRLF or LF).
  std::vector<std::string> lines;
  {
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t nl = text.find('\n', start);
      std::string line = (nl == std::string::npos)
        ? text.substr(start) : text.substr(start, nl - start);
      // Strip a trailing \r.
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
  if (lines.size() < 3) {
    if (err) *err = "sy1: too few header lines (need name/color/ver)";
    return false;
  }
  // line 1 = name (spec_preset.md §3).
  out.name = trim(lines[0]);
  // line 2 = "color=<...>" (parse fn FUN_180004490 [C]).
  const std::string& l2 = lines[1];
  const std::size_t eq = l2.find('=');
  if (l2.compare(0, 6, "color=") == 0 && eq != std::string::npos)
    out.color = trim(l2.substr(eq + 1));
  else
    out.color = l2;  // fallback: keep the raw line
  // line 3 = "ver=%d" (105 in the factory samples).
  const std::string& l3 = lines[2];
  const std::size_t veq = l3.find('=');
  if (l3.compare(0, 4, "ver=") == 0 && veq != std::string::npos) {
    to_int(trim(l3.substr(veq + 1)), out.ver);
  }
  // Remaining lines: "<id>,<value>" pairs.
  for (std::size_t li = 3; li < lines.size(); ++li) {
    const std::string& ln = trim(lines[li]);
    if (ln.empty()) continue;  // tolerate blank lines
    const std::size_t comma = ln.find(',');
    if (comma == std::string::npos) {
      if (err) *err = "sy1: malformed pair line: " + ln;
      return false;
    }
    int id = 0, val = 0;
    if (!to_int(trim(ln.substr(0, comma)), id) ||
        !to_int(trim(ln.substr(comma + 1)), val)) {
      if (err) *err = "sy1: non-numeric pair: " + ln;
      return false;
    }
    if (id < 0 || id >= (int)PARAM_SLOTS) {
      if (err) *err = "sy1: param id out of 0..254: " + ln;
      return false;
    }
    // Values are dispatcher bands, stored verbatim: signed for key shift
    // (9: −24..+24) and > 127 for the MIDI-ctrl sources (86/88).
    out.pairs.push_back({(std::uint16_t)id, (std::int32_t)val});
    out.raw_params[(std::size_t)id] = (std::int32_t)val;
    out.present[(std::size_t)id] = true;
  }
  out.pair_count = (int)out.pairs.size();
  return true;
}

bool load_sy1(const std::string& path, PresetState& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (err) *err = "sy1: cannot open " + path; return false; }
  std::ostringstream ss; ss << f.rdbuf();
  return parse_sy1(ss.str(), out, err);
}

// ---------------------------------------------------------------------------
// getChunk 0xA40 layout (spec_preset.md §3: getChunk FUN_180027db0 @27832 [C])
//
//   +0x00   "Synth1 VST Chunk Data"  (20-char magic; +0x20 = next int)
//   +0x20   int 2                    (version)
//   +0x24   int ini "hostselectprogram" (spec-faithful: 0; not in .sy1)
//   +0x28   char[0x2c] bank name     (44 bytes; doc+0x354)
//   per param (id 0..255):
//     +0x47 + id     presence byte
//     +0x23c + id*8  value dword
//   +0xa0c   int 0x71 (113 = flag-block param count)
//   +0xa14   int bank
//   +0xa1c   int sound
//   +0xa2c   int program (AEffectX+0x28)
//
//   The 262-param region +0x47..+0xa0b (904 B = 113*8) tiles 262 contiguous
//   (presence dword + value dword) entries, id 0..261. The 255 engine slots
//   (ids 0..254) occupy the first 255; ids 255..261 are the original's
//   unused superset slots (padded 0). The 13 trailing dwords at +0xa0c..+0xa3f
//   carry flag-count (0x71=113) + bank + sound + program + 10 pad.
//
//   NOTE on the original's "presence byte at +0x47+id / value at +0x23c+id*8":
//   taken literally that is a byte+byte layout, but 0xa40 - 0xa0c = 0x34 (52)
//   = 13 dwords (113*8 = 904 = 226 dwords for the 262 params only fits a
//   dword+dword per-param tile). The spec's own §3 setChunk text confirms the
//   dword pair: "flag/value pairs from +0x1c stride 8 (251 slots)" and "pairs
//   from +0x238 stride 8" — i.e. an 8-byte (flag dword, value dword) tile per
//   param. We use the 8-byte (flag, value) tile, which is the layout the
//   setChunk (decompile) actually reads and which makes 0xA40 self-consistent.
//   (This is the spec detail flagged as the round-trip gate; see
//   docs/reconstruction-status.md "Preset stage 8 / M4 (Subagent A)".)
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t MAGIC_LEN = 0x20;      // "Synth1 VST Chunk Data" is 20 B
constexpr std::uint32_t PARAM_REGION_LO = 0x47;  // first presence byte
constexpr std::uint32_t PARAM_TILE = 8;        // (flag dword, value dword)
constexpr std::uint32_t PARAM_SLOTS_CHUNK = 262; // the 262-entry region (0..261)
constexpr std::uint32_t TRAIL_LO = 0xa0c;      // flag count
constexpr std::uint32_t TRAIL_BANK = 0xa14;
constexpr std::uint32_t TRAIL_SOUND = 0xa1c;
constexpr std::uint32_t TRAIL_PROGRAM = 0xa2c;
constexpr std::uint32_t FLAG_BLOCK_COUNT = 0x71;  // 113 (spec)

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = (std::uint8_t)(v & 0xff);
  p[1] = (std::uint8_t)((v >> 8) & 0xff);
  p[2] = (std::uint8_t)((v >> 16) & 0xff);
  p[3] = (std::uint8_t)((v >> 24) & 0xff);
}
std::uint32_t get32(const std::uint8_t* p) {
  return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
         ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

}  // namespace

std::vector<std::uint8_t> write_chunk(const PresetState& s) {
  std::vector<std::uint8_t> b(CHUNK_SIZE, 0);
  // +0x00 magic "Synth1 VST Chunk Data" (20 bytes).
  const char magic[] = "Synth1 VST Chunk Data";
  std::memcpy(b.data(), magic, 20);
  // +0x20 version = 2.
  put32(b.data() + 0x20, 2);
  // +0x24 ini "hostselectprogram" (spec-faithful: 0; not present in .sy1).
  put32(b.data() + 0x24, 0);
  // +0x28 bank name (44 bytes; doc+0x354). Use the .sy1 name as a stand-in for
  // the bank display name (the bank name is a separate doc field; we store the
  // preset name here so the round-trip preserves something identifiable).
  {
    const std::string bn = s.name;
    std::memcpy(b.data() + 0x28, bn.c_str(),
               bn.size() < 43 ? bn.size() : 43);
  }
  // per param (id 0..254): presence at PARAM_REGION_LO + id*? — spec says
  // "presence byte at +0x47 + id", value dword at "+0x23c + id*8". We use the
  // 8-byte (flag, value) tile: flag dword at 0x47 + id*8, value at 0x47 + id*8 + 4.
  // (The literal "+0x23c + id*8" for the value would overlap the flag region;
  // the setChunk "stride 8" pairs confirm the 8-byte tile. See comment above.)
  for (std::uint32_t id = 0; id < PARAM_SLOTS_CHUNK; ++id) {
    std::uint8_t present_byte = 0;
    std::int32_t val = 0;
    if (id < PARAM_SLOTS) {
      present_byte = s.present[id] ? 1 : 0;
      val = s.raw_params[id];
    }
    std::uint32_t off = PARAM_REGION_LO + id * PARAM_TILE;
    put32(b.data() + off, present_byte ? 1 : 0);   // flag dword
    put32(b.data() + off + 4, (std::uint32_t)val);  // value dword
  }
  // +0xa0c flag-block param count = 0x71 (113).
  put32(b.data() + TRAIL_LO, FLAG_BLOCK_COUNT);
  // +0xa14 bank, +0xa1c sound, +0xa2c program.
  put32(b.data() + TRAIL_BANK, (std::uint32_t)s.bank);
  put32(b.data() + TRAIL_SOUND, (std::uint32_t)s.sound);
  put32(b.data() + TRAIL_PROGRAM,
        (s.bank < 0) ? PROGRAM_EXT : program_of(s.bank, s.sound));
  return b;
}

bool read_chunk(const std::uint8_t* p, std::size_t len, PresetState& out,
               std::string* err) {
  if (len < CHUNK_SIZE) { if (err) *err = "chunk: len < 0xa40"; return false; }
  // magic check.
  if (std::memcmp(p, "Synth1 VST Chunk Data", 20) != 0) {
    if (err) *err = "chunk: bad magic"; return false;
  }
  // version.
  const std::uint32_t ver = get32(p + 0x20);
  if (ver != 2) { if (err) *err = "chunk: bad version"; return false; }
  out = PresetState();
  // bank name (+0x28, 44 bytes).
  {
    const char* bn = (const char*)p + 0x28;
    std::size_t n = 0;
    while (n < 44 && bn[n]) ++n;
    out.name.assign(bn, n);
  }
  // per param (id 0..254).
  for (std::uint32_t id = 0; id < PARAM_SLOTS; ++id) {
    const std::uint32_t off = PARAM_REGION_LO + id * PARAM_TILE;
    const std::uint32_t flag = get32(p + off);
    const std::uint32_t val = get32(p + off + 4);
    out.present[id] = (flag != 0);
    out.raw_params[id] = (std::int32_t)val;  // full dword (signed band)
    if (out.present[id]) out.pairs.push_back({(std::uint16_t)id, out.raw_params[id]});
  }
  out.pair_count = (int)out.pairs.size();
  // trailing: bank, sound, program.
  out.bank = (std::int32_t)get32(p + TRAIL_BANK);
  out.sound = (std::int32_t)get32(p + TRAIL_SOUND);
  // color/ver are not in the chunk (they live in the .sy1 header only); leave
  // them default. The round-trip gate covers the 0xA40 fields (the spec's
  // getChunk layout); the .sy1 header is re-read from file, not the chunk.
  return true;
}

// ---------------------------------------------------------------------------
// Engine parameter application (plan stage 8 / M4: preset → engine params)
//
// params.gen.h has no explicit per-slot default field (only sy1_min/sy1_max
// display ranges), and the engine zero-initializes raw_params[255]
// (engine.h:46), so the fallback default for an absent slot is 0 (raw band 0).
// norm_params uses the same 0..1 normalization as engine.h:143
// (norm_params[id] = band * (1/127)).
// ---------------------------------------------------------------------------

namespace {

// Default raw value for a slot not specified by the preset (see comment above).
inline std::int32_t default_raw_value(std::size_t /*slot*/) { return 0; }

}  // namespace

void apply_preset_to_params(const PresetState& preset,
                            std::int32_t raw_params[PARAM_SLOTS],
                            float norm_params[PARAM_SLOTS]) {
  // 1. Default every engine slot (0..254).
  for (std::size_t i = 0; i < PARAM_SLOTS; ++i) {
    const std::int32_t d = default_raw_value(i);
    raw_params[i] = d;
    norm_params[i] = (float)d / 127.0f;
  }
  // 2. Overlay the preset's (id,value) pairs (file order; last write wins).
  for (const ParamPair& p : preset.pairs) {
    if (p.id < PARAM_SLOTS) {
      raw_params[p.id] = p.value;
      norm_params[p.id] = (float)p.value / 127.0f;
    }
  }
}

// ---------------------------------------------------------------------------
// Patch-version upgrade — the device load-time chain FUN_180026a10 (64-bit
// DLL, out/decomp_all.c:26946-27000; 32-bit twin FUN_10023230,
// decomp_all_32_full.c:27256), see preset.h. #137 = the LITERAL fall-through
// port (tasks/consults/m3-87-ver111-conflict.answer.md +
// m3-analysis-review-2.answer.md §2 + m3-matrix-accept.answer.md); device
// matrix: reports/m3_upgrade_matrix.md. Spec: reports/upgrade_model_spec.md.
//
// Precision follows the disassembly: curve evaluation in double, each new
// band's value rounded through float (cvtsd2ss/cvtss2sd) before the |Δ|
// compare, float arithmetic in FUN_1800276f0 (SSE scalar float32, byte-
// verified constants). Every nearest-band search is the original's walk:
// start from prev = 999.0 (DAT_180075760), stop at the first band whose |Δ|
// exceeds the previous one, return that band − 1.
//
// Version gating — switch(ver) WITH FALLTHROUGH (decomp_all.c:26958-26986),
// per pair with id < 99 || id > 250 (:26957 `0x97 < id-99U`), whole chain
// iff ver < 0x71 (:26948):
//
//   case 100        FUN_180026c60 (:27016)          ver == 100
//   case 0x69,0x6a  FUN_180027150 (:27258) + fixed  ver 100, 105, 106
//                   84=0x40, 85=0x18, 86..89=0 (:26963-26973)
//   case 0x6b,0x6c  FUN_1800276f0 (:27514)          ver 100, 105..108
//   case 0x6d       ids 95..97 := snapshot          ver 100, 105..109
//                   (:26978-26981 `iVar1-0x5fU < 3`)
//   case 0x6f       id 98 := snapshot               ver 100, 105..109, 111
//                   (:26982-26985 `iVar1 == 0x62`; NO case 0x6e/0x70 →
//                   NOT 110, NOT 112 — matrix + controls verified)
//
// Holes: ver 101..104, 110 and 112 hit `default` — NO transforms at all.
// A transformed value goes to a scratch list copied back AFTER the loop
// (:26994-27003), so every reference read during the chain (p44/p43 for
// p50/51, p38 for p75/p93) sees the FILE values, untransformed. The
// snapshot (param_1+0xb5b0) is the pre-load program state = the device boot
// initial_sound table (DAT_180093af0, 99 pairs, seeded by the ctor) on a
// fresh load; FUN_18001dea0 (:21143-21162) returns 0 only for an absent id
// and is never absent for ids 0..98. Finally list ver := 0x71 (113)
// (:27004).
// ---------------------------------------------------------------------------

namespace {

constexpr double kInv127d = 1.0 / 127.0;     // DAT_180072ff0
constexpr float  kInv127f = 1.0f / 127.0f;   // DAT_180072a00

// The snapshot list (param_1+0xb5b0) for the ids the upgrade chain snapshots
// or that a ver=105 file never carries — FUN_18001dea0(snapshot, id) reads
// the pre-load program state, which on a fresh load is the device BOOT
// initial_sound pair table DAT_180093af0 (64-bit, bin/Synth1_vst64_113b3.dll
// file off 0x914f0; 32-bit twin DAT_1007f1e0 off 0x7d1e0; 99 pairs ids 0..98,
// byte-identical both images; dump in tasks/m3-loadpath-75-98/RESULT.md and
// reports/m3_upgrade_matrix.md). #137-1: the table is the SNAPSHOT SOURCE —
// the old values (73=32, 74=32, 75=48, 85=64, 93=9, 96=48, 97=95) came from
// the v113-UPGRADED dump (install/bank0_v113_upgraded.json), a post-upgrade
// file state, not the boot table (the #16 divergence list; #81's 94=62→16
// was the first instance of that provenance error).
//
// 75 = 22 RAW: the store keeps the table value; the log formula
// (FUN_1800276f0 case 0x4b) applies at the stage — formula(22, s=100) = 48
// is the boot chunk readback, formula(22, s=180) = 70 the ver-105 sweep
// readback (tasks/m3-loadpath-75-98 Finding 1/3).
// −1 = no snapshot value used by the upgrade for that id.
constexpr std::int32_t kInitSound[99] = {
  // 0..66: always present in a patch file; never snapshot-read by the chain
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,
  // 67..72: FUN_180026c60 case 0x43..0x48 snapshot (:27237-27244)
  /* 67 */ 0, /* 68 */ 0, /* 69 */ 0, /* 70 */ 0, /* 71 */ 0, /* 72 */ 64,
  /* 73 */ 0, /* 74 */ 0, /* 75 */ 22, /* 76 */ 0, /* 77 */ 0, /* 78 */ 0,
  /* 79 */ 64, /* 80 */ 64, /* 81 */ 64, /* 82 */ 0, /* 83 */ 66,
  /* 84 */ 64, /* 85 */ 24, /* 86 */ 0xB001, /* 87 */ 44, /* 88 */ 0xB001,
  /* 89 */ 43, /* 90 */ 64, /* 91 */ 0, /* 92 */ 0, /* 93 */ 2, /* 94 */ 16,
  /* 95 */ 0, /* 96 */ 1, /* 97 */ 1, /* 98 */ 64,
};

inline std::int32_t init_of(int id) {
  return (id >= 0 && id < 99 && kInitSound[id] >= 0) ? kInitSound[id] : 0;
}

// Stage gates (decomp_all.c:26958-26986 fallthrough; upgrade_model_spec.md
// §1): each chain stage runs for exactly these file versions. ver 101..104,
// 110 and 112 hit default — no stage runs (NO case 0x6e/0x70); ver >= 113
// never gets here. The old single per-id boundary could not express these:
// p75 lives in both FUN_180027150 (snapshot, ver<=106) and FUN_1800276f0
// (formula, ver<=108); 50/51/75 keep transforming at ver108 (#14/#56);
// 77/78 stop at ver107 (#15); 95..97 stop at ver110; case 0x6f (id 98)
// fires at ver {100,105..109,111} — NOT 110/112 (matrix + 071 controls,
// reports/m3_upgrade_matrix.md; the old "never fires" model was a ver112-
// only sweep generalization, refuted by consult m3-87-ver111-conflict).
inline bool stage_100(int ver)   { return ver == 100; }
inline bool stage_105_106(int ver) { return ver == 100 || ver == 105 || ver == 106; }
inline bool stage_105_108(int ver) { return ver == 100 || (ver >= 105 && ver <= 108); }
inline bool stage_105_109(int ver) { return ver == 100 || (ver >= 105 && ver <= 109); }
inline bool stage_0x6f(int ver) {
  return ver == 100 || (ver >= 105 && ver <= 109) || ver == 111;
}

// Nearest-band walk over an explicit band function, mirroring the device
// loop exactly: prev = 999.0 (DAT_180075760), band b = 0..127, break at the
// first b whose |Δ| EXCEEDS the previous |Δ| (equal keeps walking), result
// b − 1; if the walk never breaks, result 127 (loop exit at 0x80, dec).
template <class Band>
std::int32_t band_walk(Band band, double target) {
  double prev = 999.0;
  for (int b = 0; b < 128; ++b) {
    const double d = std::fabs(band(b) - target);
    if (prev < d) return b - 1;
    prev = d;
  }
  return 127;
}

// FUN_180026c60 (:27016-27256) — case-100 entry (ver == 100 only), ported
// from its own body (64-bit asm bin/Synth1_vst64_113b3.dll 0x180026c60-
// 0x1800270a3; constants dumped from .rdata, see
// tasks/m3-law137-upgrade-chain/dump_consts.py). Quantize ids
// 3, 12, 15, 25, 27, 29, 30, 43, 48 (nearest-band walks); snapshot ids
// 67..72 (case 0x43..0x48, :27237-27244); ids 4..0x42 and default identity
// (goto caseD_4 / caseD_default, :27127-27249). Device-verified at ver100
// (reports/m3_upgrade_matrix.md cell ver100: p3 66→70, p25 12→28, p30
// 22→6, p43 19→34, p48 65→66; 12/15/27/29 fixed points; 67..72 snapshot =
// file values). No library file is ver <= 104, so this stage cannot move a
// library load-check row or a ref.
//
// Ghidra constant caveat: DAT_180073000 holds the double 4.800000190734863
// but the case-3 body reads it as FLOAT (mulss, low word 0x40000000 = 2.0f)
// — the "Globals overlap" warning on the function; the 4.8 double is the
// high-word neighbor. Same for the walk constants: float reads where the
// asm shows (double)(float) round-trips.
std::int32_t fun_180026c60(int id, std::int32_t v) {
  switch (id) {
    case 3: {
      // :27050-27066: f = float((v/127 − 0.5) · 2.0f); snap f to i ∈ {−1,0,1}
      // if −0.03 < i−f < 0.03 (DAT_180075628/0x1800757d0); target = 2^(f/12)
      // (pow via FUN_180004d80, DAT_180072b80=2.0, DAT_180073d88=1/12).
      float f = (float)((double)v * kInv127d - 0.5) * 2.0f;
      for (int i = -1; i < 2; ++i) {
        const double d = (double)((float)i - f);
        if (d < 0.03 && -0.03 < d) { f = (float)i; break; }
      }
      const double target = std::pow(2.0, (double)f * (1.0 / 12.0));
      // band walk (:27067-27097, asm 0x180026d84): log(2.0) hoisted
      // (FUN_180056870), band = exp(log2 · (k · 1/1200)), DAT_180075298 =
      // 1/1200; k = b<62 ? b−62 : (b<=66 ? 0 : b−66) (dead band 62..66).
      const double ln2 = std::log(2.0);
      return band_walk(
          [ln2](int b) {
            const int k = (b < 62) ? b - 62 : ((b <= 66) ? 0 : b - 66);
            return std::exp(ln2 * ((double)k * 0.0008333333333333334));
          },
          target);
    }
    case 12: case 15: case 25: {
      // :27148-27177: t = v/127; t < 0.5 (DAT_180072e98) →
      //   target = 1.75^(t²) − 1 + 0.0005 (DAT_1800756e0/72ea0/755e0);
      // else target = 30^((t)^4.6) − 1 (DAT_180075700/75720).
      // band = float(56000^(b/127) · 0.0005) (DAT_180075790/755e0).
      const double t = (double)v * kInv127d;
      double target;
      if (t < 0.5) {
        target = std::pow(1.75, std::pow(t, 2.0));
        target = (target - 1.0) + 0.0005;
      } else {
        target = std::pow(30.0, std::pow(t, 4.6));
        target = target - 1.0;
      }
      return band_walk(
          [](int b) {
            return (double)(float)(std::pow(56000.0, (double)b * kInv127d) *
                                   0.0005);
          },
          target);
    }
    case 27: case 29: {
      // :27178-27199 (asm 0x180026f20): target = v/127; band(0) = 0.0, else
      // float(2048^((b/127)^0.3) · 1/2048) (DAT_180075660=0.3,
      // DAT_180075768=2048.0, DAT_1800755d8=1/2048).
      const double target = (double)v * kInv127d;
      return band_walk(
          [](int b) {
            const double x = (double)b * kInv127d;
            if (x == 0.0) return 0.0;
            return (double)(float)(std::pow(2048.0, std::pow(x, 0.3)) *
                                   0.00048828125);
          },
          target);
    }
    case 30: {
      // :27200-27216: target = 1 − (v/127)·0.9 (DAT_180072ea0/756d0);
      // band = 10^(b·(1/127)·(−30)·0.05) (DAT_180073010=10.0,
      // DAT_1800757e0=−30.0, DAT_180072ff8=0.05; no float round-trip).
      const double target = 1.0 - (double)v * kInv127d * 0.9;
      return band_walk(
          [](int b) {
            return std::pow(10.0, ((double)b * kInv127d * -30.0) * 0.05);
          },
          target);
    }
    case 43: case 48: {
      // :27217-27236: target = 20^(v/127) − 59/60 (DAT_180075718=20.0,
      // DAT_1800756d8=0.9833333333333333); band = float(1500^(b/127) · 1/12)
      // (DAT_180073d98=1500.0, DAT_180073d88=1/12).
      const double target = std::pow(20.0, (double)v * kInv127d) - 0.9833333333333333;
      return band_walk(
          [](int b) {
            return (double)(float)(std::pow(1500.0, (double)b * kInv127d) *
                                   (1.0 / 12.0));
          },
          target);
    }
    case 67: case 68: case 69: case 70: case 71: case 72:
      // :27237-27244: param_2 = FUN_18001dea0(snapshot, id).
      return init_of(id);
    default:
      // :27127-27147 (4..0x42 identity list) + :27245-27248 default.
      return v;
  }
}

// Nearest new band: |(double)(float)curve(b) − old| walked from b = 0.
template <class Curve>
std::int32_t nearest_band(Curve curve, double old) {
  double prev = 999.0;
  for (int b = 0; b < 128; ++b) {
    const double d = std::fabs((double)(float)curve(b) - old);
    if (prev < d) return b - 1;
    prev = d;
  }
  return 127;
}

// FUN_10023ab0 (:27546; 64-bit FUN_180027150) — stage 0x69/0x6a (runs for
// ver 100/105/106): old→new curve remaps, plus the snapshot resets for
// p75..p85/p90 (:27735-27747) and p77/p78 (:27739-27740). The snapshot is
// the pre-load program state (init sound on a fresh load), NOT the file
// value; at ver >= 107 these params stay at their file values (#15: the
// old boundary 113 over-remapped 77/78 at ver108/109/112).
std::int32_t remap_v105(int id, std::int32_t v) {
  switch (id) {
    case 13: case 16: case 18: case 26: case 28: {
      // decay / release: old 60^((v/127)^2.95) − 1 s → new 6000^(b/127)·0.01 s
      const double old =
          std::pow(60.0, std::pow((double)v * kInv127d, 2.95)) - 1.0;
      return nearest_band(
          [](int b) { return std::pow(6000.0, (double)b * kInv127d) * 0.01; },
          old);
    }
    case 21: {
      // filter amount: old unipolar v/127 → new bipolar (b − 63)/64
      const double t = (double)v * kInv127d;
      return nearest_band(
          [](int b) { return (double)((float)(b - 63) * 0.015625f); }, t);
    }
    case 37:  // delay dry/wet
      return v / 2;
    case 52: {
      // chorus delay time: old (x<.5 ? 50^(2x)·0.3 : 30x) ms →
      // new (x<.5 ? 300^((2x)^0.6)·0.05 : 30x) ms
      const double x = (double)v * kInv127d;
      const double old =
          ((x >= 0.5) ? x * 30.0 : std::pow(50.0, x * 2.0) * 0.3) * 0.001;
      return nearest_band(
          [](int b) {
            const double y = (double)b * kInv127d;
            return ((y >= 0.5) ? y * 30.0
                               : std::pow(300.0, std::pow(y * 2.0, 0.6)) * 0.05) *
                   0.001;
          },
          old);
    }
    case 54: {
      // chorus rate: old 10^((v/127)^1.8) − 1 + 0.05 Hz →
      // new (b == 0 ? 0.01 : 20000^(b/127f)·0.02) Hz
      const double old =
          std::pow(10.0, std::pow((double)v * kInv127d, 1.8)) - 1.0 + 0.05;
      return nearest_band(
          [](int b) {
            if (b == 0) return 0.01;
            return std::pow(20000.0, (double)((float)b * kInv127f)) * 0.02;
          },
          old);
    }
    case 55: {
      // chorus feedback: target = trunc(−v/127)·0.9 (integer division: 0 for
      // v < 127 → no feedback); new (b − 64)/64·0.99
      const double t = (double)(-(v / 127)) * 0.9;
      return nearest_band(
          [](int b) {
            return (double)((float)(b - 64) * 0.015625f * 0.99f);
          },
          t);
    }
    case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82:
    case 83: case 84: case 85: case 90:
      return init_of(id);
    default:
      return v;
  }
}

// FUN_100242d0 (:27817) — stage 0x69/0x6a consts (runs for ver 100/105/106,
// after FUN_10023ab0 in the same fallthrough): p84 := 64 (:27821),
// p85 := 24 (0x18, :27823), p86..p89 := 0 (:27825-27829). The old engine
// set p85 := 64 (preset.cpp:522 bug, upgrade_model_spec.md §4) — the device
// constant is 24 (#61).
std::int32_t fun_100242d0(int id, std::int32_t v) {
  if (id == 84) return 64;
  if (id == 85) return 24;
  if (id >= 86 && id <= 89) return 0;
  return v;
}

// FUN_10024150 (:27761; 64-bit FUN_1800276f0) — stage 0x6b/0x6c (runs for
// ver 100/105..108): patch-dependent derivations. `orig` is the FILE pair
// list before the chain (the original reads references from the source
// document, while the rewritten values go to a scratch document copied back
// after the loop, :27326-27332). Snapshot class #60: p86-p89/p91/p92/p94 :=
// snapshot at ver <= 108 (:27793-27801) — a ver108 file carrying them gets
// the init sound, not the file value.
std::int32_t derive_v105(int id, std::int32_t v, const std::int32_t* orig) {
  switch (id) {
    case 50: case 51: {
      // MIDI ctrl sens: 64 − (int)((1 − ref/127)·v·(−1/127)·63),
      // ref = LFO1 depth (44) for slot 1, LFO1 speed (43) for slot 2
      const float ref = (float)orig[id == 50 ? 44 : 43];
      float x = 1.0f - ref * kInv127f;
      x *= (float)v;
      x *= -0.00787401572f;  // DAT_1800757cc
      x *= 63.0f;            // DAT_1800757b8
      return 64 - (std::int32_t)x;
    }
    case 75: {
      // unison detune from play mode: logf((e²−1)·0.01·(v·s/127 + 0.5) + 1)·63.5
      const float s = (orig[38] == 0) ? 100.0f : 180.0f;  // DAT_180072ea8/57c4
      float a = std::exp(2.0f) - 1.0f;
      a *= 0.00999999978f;  // DAT_180073388
      float b = (float)v * s;
      b *= kInv127f;
      b += 0.5f;
      const float r = std::log(a * b + 1.0f) * 63.5f;  // DAT_1800757bc
      const std::int32_t n = (std::int32_t)r;
      return n > 127 ? 127 : n;
    }
    case 86: case 87: case 88: case 89: case 91: case 92: case 94:
      return init_of(id);
    case 93:  // unison voice count from play mode
      return (orig[38] == 0) ? 2 : 4;
    default:
      return v;
  }
}

}  // namespace

bool upgrade_v105(PresetState& st) {
  if (st.ver >= SY1_VER_CURRENT) return false;

  // Ids a ver<=105 file does not carry come from the init sound. A ver108
  // file may still omit ids (bank03/012 ver108 carries only 0..90): the
  // device's program doc seeds every absent slot from the boot initial
  // sound before the chain runs (chunk pins: absent 93/96/97 at ver109/110/112
  // read back 2/1/1 = init sound, tasks/m3-160-upgrade-93 F3), so a chain
  // stage that TRANSFORMS an id fires for it even when the file omits it
  // (id 93, #160, below). The engine's load path seeds absent ids with the
  // init-sound boot replay instead (append_sy1_events).
  if (st.ver < 108) {
    for (int id = 73; id < 99; ++id) {
      if (!st.present[id]) {
        st.raw_params[id] = init_of(id);
        st.present[id] = true;
        st.pairs.push_back({(std::uint16_t)id, st.raw_params[id], 1});
      }
    }
  }

  // File values before the chain: the device reads every reference (p44/p43
  // for p50/51, p38 for p75/p93) from the source list, not from the scratch
  // list the transformed values go to (:27326-27332).
  std::int32_t orig[99];
  for (int id = 0; id < 99; ++id) orig[id] = st.raw_params[id];

  // switch(ver) fallthrough with per-stage gates (decomp_all.c:26958-26986;
  // holes ver 101..104, 110 and 112 run no stage). The device iterates the
  // file's pair list; ids 99..250 are skipped (:26957), ids > 250 enter the
  // switch but every callee/default leaves them verbatim.
  const int ver = st.ver;

  // #160: the 0x5d stage fires for id 93 even when the FILE omits it —
  // the device iterates its program doc, which seeds absent slots from
  // the boot initial sound (F1/F3), so case 0x5d (FUN_1800276f0,
  // decomp_all.c:27551) rewrites 93 := (file38==0 ? 2 : 4) for every
  // ver {100,105..108} load, present or absent (pins: ver108 x 93=77
  // present -> 4 override; ver108 x 38 in {0,1,2} absent -> 2/4/4;
  // ver109/110/112 controls inert). The ver<108 pre-fill above already
  // makes 93 present for older files; this covers ver107/108 files that
  // omit it (bank03/012: 38=2 -> 4, the #160 LOADCHECK divergence).
  if (stage_105_108(ver) && !st.present[93]) {
    st.raw_params[93] = (orig[38] == 0) ? 2 : 4;
    st.present[93] = true;
    st.pairs.push_back({93, st.raw_params[93], 0});
  }

  for (int id = 0; id < 99; ++id) {
    if (!st.present[id]) continue;
    std::int32_t v = st.raw_params[id];
    if (stage_100(ver)) v = fun_180026c60(id, v);  // case 100 (FUN_180026c60)
    if (stage_105_106(ver)) {                      // case 0x69/0x6a
      v = remap_v105(id, v);                       //   FUN_180027150
      v = fun_100242d0(id, v);                     //   fixed 84/85/86-89
    }
    if (stage_105_108(ver)) v = derive_v105(id, v, orig);  // case 0x6b/0x6c
    if (stage_105_109(ver) && id >= 95 && id <= 97) v = init_of(id);  // 0x6d
    // case 0x6f (:26982-26985): id 98 := snapshot at ver {100,105..109,111}.
    // The snapshot lookup always finds 98 = 64 (boot initial_sound table,
    // never absent for ids 0..98). The old "never fires" model came from the
    // ver112-only library sweep — ver112 keeps file values because the
    // switch has NO case 0x70, not because the lookup misses (consult
    // m3-87-ver111-conflict; matrix + 071@111/112 controls,
    // reports/m3_upgrade_matrix.md). No library file is ver <= 111 with a
    // present 98 (id 98 exists only in ver112 files), so this fires on no
    // library load-check row.
    if (stage_0x6f(ver) && id == 98) v = init_of(98);
    st.raw_params[id] = v;
  }
  for (ParamPair& p : st.pairs) {
    if (p.id < 99) p.value = st.raw_params[p.id];
  }
  st.pair_count = (int)st.pairs.size();
  st.ver = SY1_VER_CURRENT;
  return true;
}

// ---------------------------------------------------------------------------
// Bank / program routing (spec_preset.md §1: CBank[100]; §2 program/bank SELECT)
// ---------------------------------------------------------------------------

void BankManager::add_bank(std::int32_t bank, const std::string& folder) {
  if (bank < 0 || bank >= BANKS_TOTAL) return;
  banks_[bank].folder = folder;
  banks_[bank].known = true;
}

std::string BankManager::bank_folder(std::int32_t bank) const {
  if (bank < 0 || bank >= BANKS_TOTAL) return std::string();
  return banks_[bank].folder;
}

bool BankManager::load_bank(std::int32_t bank, std::int32_t program,
                           PresetState& out, std::string* err) {
  if (bank < 0 || bank >= BANKS_TOTAL ||
      program < 0 || program >= PER_BANK) {
    if (err) *err = "bank: index out of range";
    return false;
  }
  if (!banks_[bank].known) {
    if (err) *err = "bank: not registered";
    return false;
  }
  // The .sy1 files are 1-indexed ("001.sy1" = the first slot; there is no
  // "000.sy1"). The original's lazy loader (FUN_18001e190) reads
  // "<folder>/%03d.sy1" with %03d = the 1-based slot number, so program 0
  // (the "initial sound" = 001) maps to 001.sy1. This matches the test's
  // expected routing (bank 0, program 0 → 001.sy1) and spec_preset.md §1's
  // "initial sound" slot.
  char fn[64];
  std::snprintf(fn, sizeof(fn), "%03d.sy1", program + 1);
  std::string path = banks_[bank].folder + "/" + fn;
  if (!load_sy1(path, out, err)) return false;
  upgrade_v105(out);  // FUN_180027c70 → FUN_180026a10 (ver < 113)
  out.bank = bank;
  out.sound = program;
  return true;
}

std::uint32_t BankManager::program_change(std::int32_t bank,
                                         std::int32_t program,
                                         std::int32_t raw_params[PARAM_SLOTS],
                                         std::string* err) {
  PresetState st;
  if (load_bank(bank, program, st, err))
    st.apply_raw(raw_params);
  return (bank < 0) ? PROGRAM_EXT : program_of(bank, program);
}

}  // namespace synth1::preset
