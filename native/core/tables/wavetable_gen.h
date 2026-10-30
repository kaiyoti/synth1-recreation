// wavetable_gen.h — 2048-entry float wavetables.
//
// Spec: spec_osc.md §0 (2048 float entries per cycle, index (phase>>16)&0x7ff,
//       linear 1/65536 blend), §5 (wavetable construction, FUN_18003cdf0).
// Ghidra: DAT_1801a4298 (sine table, 2048 floats, single, unbanded);
//         DAT_1801a42a0 (saw, 36×2048), DAT_1801a42a8 (triangle, 36×2048),
//         DAT_1801a42b0 (square, 36×2048);
//         DAT_180072b50 = 1/65536 (phase fraction);
//         FUN_18003cad0 (wave index → table), FUN_180051b30 (noise LFSR);
//         FUN_1800520b0 (canonical kernel: lerp + phase write-back).
//
// Precision: wavetable GENERATION uses double, storage float (spec_osc.md §0).
// Per-sample DSP is single-precision float; the phase-increment derivation uses
// a double intermediate (spec_osc.md §0).
//
// STAGE 2 (spec_osc.md §5.1, §5.3–§5.6, §6.3): 36-band band-limited saw,
// triangle and square tables, the frequency → band lookup, and the 21-bit
// Galois noise LFSR.

#pragma once

#include <cstdint>
#include <cmath>

namespace synth1::tables {

inline constexpr std::uint32_t WAVE_TABLE_SIZE = 2048u;
// FUN_18003cdf0 generator: 23 quarter-octave ladder bands (thr = 27.5·2^(j/4),
// j = 0..22, while (int)(22050/thr) >= 16) + 12 extension bands (H = 14..3,
// 4 per iteration) + 2 tail bands (H = 2,1) = 37 descriptors (DAT_1801a42e8).
inline constexpr std::uint32_t NUM_BANDS = 37u;

// Waveform types (spec_osc.md §2 field map + §5.6 wave-index table).
enum class Waveform : std::uint8_t {
  kSine = 0,     // spec_osc.md §5.2 — single unbanded table
  kSaw = 1,      // spec_osc.md §5.3 — 36 band-limited tables
  kPulse = 2,    // spec_osc.md §6.2 — uses the saw tables (runtime two-saw difference)
  kTriangle = 3, // spec_osc.md §5.4 — 36 band-limited tables
  kNoise = 4,    // spec_osc.md §6.3 — LFSR, no table (lookup skipped)
  kSquare = 5,   // spec_osc.md §5.5 — 36 tables, sub-oscillator only
};

// Band descriptor (spec_osc.md §5.1): 12-byte {int len; int offset; float thr}.
struct BandDesc {
  int len;     // 2048
  int offset;  // band index * 2048
  float thr;   // band threshold, ascending with the index
};

// Fill the 36 band descriptors (spec_osc.md §5.1):
//   harmonics[j] = 27.5f * cos(j / 36.0f)
//   thr[j]       = max(22050.0f / harmonics[j], 1.0f)
void compute_band_descriptors(BandDesc* desc);

// Harmonic count of a band: H_b = (int)(22050.0f / thr_b) (spec_osc.md §5.3).
int band_harmonics(const BandDesc& desc);

// Fill a single 2048-entry table.
// kSine: the unbanded sine (§5.2). kSaw/kTriangle/kSquare: the band-0 table
// (§5.3–§5.5). kPulse/kNoise: zero-filled (pulse uses the saw tables at
// runtime; noise has no table).
void generate(Waveform wf, float* table, std::uint32_t size = WAVE_TABLE_SIZE);

// Fill 36 × 2048 band-limited tables for kSaw (and kPulse, same tables),
// kTriangle and kSquare (spec_osc.md §5.3–§5.5).
void generate_banded(Waveform wf, float* tables);

// Wave index → table pointer (spec_osc.md §5.6, FUN_18003cad0): binary search
// across the 36 band thresholds for the band whose harmonic count just fits
// below Nyquist. kSine returns the unbanded sine table, kPulse the saw tables,
// kNoise returns nullptr (no table).
const float* get_table(Waveform wf, float freq);

// 21-bit Galois noise LFSR (spec_osc.md §6.3, FUN_180051b30): seed 10,
// taps 0x43 = bits 0, 1, 6 (x^6 + x + 1) fed into the low bits, output taken
// from bit 19, advanced once per sample (not phase-locked).
struct LfsrNoise {
  std::uint32_t s;
  explicit LfsrNoise(std::uint32_t seed = 10u) : s(seed) {}

  // Advance one sample; bipolar ±1 output taken from bit 19 before the shift
  // (spec_osc.md §6.3: bit 0 → −g2, bit 1 → +g2).
  float next() {
    const float out = ((s >> 19) & 1u) ? 1.0f : -1.0f;
    if (((s >> 19) & 1u) == 0u) {
      s = s << 1;
    } else {
      s = ((s ^ 0x43u) << 1) | 1u;
    }
    return out;
  }
};

// WavetableBank — a zero-allocation VIEW of the lazily built global table
// set (the original's one-time DAT_180094f1c guard, spec_osc.md §5). The
// bank owns no memory: every pointer references the static global tables,
// so passing it by const reference into the voice costs nothing and never
// allocates during audio processing.
struct WavetableBank {
  const float* sine = nullptr;     // 2048 floats (DAT_1801a4298)
  const float* saw = nullptr;      // 36×2048 (DAT_1801a42a0; pulse uses these)
  const float* triangle = nullptr; // 36×2048 (DAT_1801a42a8)
  const float* square = nullptr;   // 36×2048 (DAT_1801a42b0; sub oscillator)
  const BandDesc* desc = nullptr;  // 36 band descriptors (spec_osc.md §5.1)

  // Wave index → band table (spec_osc.md §5.6, FUN_18003cad0).
  // kNoise returns nullptr (the LFSR kernel skips the lookup).
  const float* table(Waveform wf, float freq) const;
};

// The global bank. The tables are built once on first call (allocations
// happen HERE, at init — never during process()).
const WavetableBank& global_bank();

// Read a sample from the wavetable at the given phase (32-bit, 2^27 cycle).
// spec_osc.md §0: index i = (phase >> 16) & 0x7ff,
//                fraction u = (phase & 0xffff) · (1/65536),
//                v = (T[i+1] − T[i]) · u · INV65536 + T[i]   (fused chain, as emitted)
inline float read(const float* table, std::uint32_t phase) {
  const std::uint32_t i = (phase >> 16) & 0x7ffu;
  const float u = (float)(phase & 0xffffu) * (1.0f / 65536.0f);
  return (table[(i + 1u) & 0x7ffu] - table[i]) * u + table[i];
}

}  // namespace synth1::tables
