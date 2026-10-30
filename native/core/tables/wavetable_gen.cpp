// wavetable_gen.cpp — 2048-entry float wavetables.
//
// Spec: spec_osc.md §5 (wavetable construction — FUN_18003cdf0, guarded
//       one-time by DAT_180094f1c); §5.1 (band descriptors), §5.2 (sine),
//       §5.3 (band-limited saw), §5.4 (triangle), §5.5 (square),
//       §5.6 (wave index → table, FUN_18003cad0); §6.3 (noise LFSR).
// Ghidra: DAT_1801a4298 (sine), DAT_1801a42a0 (saw/pulse), DAT_1801a42a8
//         (triangle), DAT_1801a42b0 (square), DAT_1801a3d60/af0/fd0 (descs);
//         DAT_180073008 (2π).
//
// Precision: generation in double, storage float (spec_osc.md §0); the saw
// series is accumulated in float and scaled (sum * 2.0) * (1/π) * 1.0 in
// double (spec_osc.md §5.3).

// MinGW hides M_PI from <cmath>/<math.h> under -std=c++17 (__STRICT_ANSI__);
// _USE_MATH_DEFINES is the MSVC/MinGW idiom to expose the math constants.
// Must precede every include (wavetable_gen.h includes <cmath>).
// No-op on glibc/Linux builds.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include "wavetable_gen.h"

#include <cmath>
#include <cstring>

namespace synth1::tables {
namespace {

void fill_sine(float* sine) {
  // spec_osc.md §5.2: sine[i] = sin(2π·i/2048), double generation, float storage.
  for (std::uint32_t i = 0; i < WAVE_TABLE_SIZE; ++i) {
    const double ang = 2.0 * M_PI * (double)i / (double)WAVE_TABLE_SIZE;
    sine[i] = (float)std::sin(ang);
  }
}

// spec_osc.md §5.3: saw_b[i] = (2/π) · Σ_{k=1..H} sine[(i·k) mod 2048] / k.
// Accumulated in float; final scale (Σ · 2.0) · (1/π) · 1.0 in double.
void fill_saw_band(const float* sine, int harmonics, float* out) {
  for (std::uint32_t i = 0; i < WAVE_TABLE_SIZE; ++i) {
    float sum = 0.0f;
    for (int k = 1; k <= harmonics; ++k) {
      sum += sine[(i * (std::uint32_t)k) & (WAVE_TABLE_SIZE - 1u)] / (float)k;
    }
    out[i] = (float)(((double)sum * 2.0) * (1.0 / M_PI) * 1.0);
  }
}

// spec_osc.md §5.4: tri_b[i] = (8/π²) · Σ_{k=1,3,5..≤H} (−1)^((k−1)/2) ·
// sine[(i·k) mod 2048] / k². Sign + when (k & 3) == 1, else −.
void fill_triangle_band(const float* sine, int harmonics, float* out) {
  for (std::uint32_t i = 0; i < WAVE_TABLE_SIZE; ++i) {
    float sum = 0.0f;
    for (int k = 1; k <= harmonics; k += 2) {
      const float term = sine[(i * (std::uint32_t)k) & (WAVE_TABLE_SIZE - 1u)] / (float)(k * k);
      if ((k & 3) == 1) {
        sum += term;
      } else {
        sum -= term;
      }
    }
    out[i] = (float)((double)sum * (8.0 / (M_PI * M_PI)));
  }
}

// spec_osc.md §5.5: square_b[i] = 0.5 · (saw_b[i] − saw_b[(i + 1024) mod 2048]).
void fill_square_band(const float* saw, float* out) {
  for (std::uint32_t i = 0; i < WAVE_TABLE_SIZE; ++i) {
    out[i] = 0.5f * (saw[i] - saw[(i + (WAVE_TABLE_SIZE / 2u)) & (WAVE_TABLE_SIZE - 1u)]);
  }
}

// FUN_100352f0 verbatim (m3-wavetable-band-scratch F3, objdump 0x100352f0):
// the binary search compares the RAW FLOAT BIT PATTERNS as signed ints
// (`mov ebx,[esp+0x18]` — no cvtss2si — then `cmp ebx,[ebp+ecx*4+0x8]`/jge),
// i.e. a FLOAT compare for positive values; the old (int)-truncation diverges
// whenever floor(thr)==floor(freq) but thr>freq. Then the DAT_1015c084
// adjustment (1575 = (int)(22050/14), breakInt=14 at j=23): float compares
// freq != thr[i] and freq <= 1575.0f, i>0 → i--.
int select_band(const BandDesc* desc, float freq) {
  int iVar4 = (int)NUM_BANDS - 1;
  int iVar3 = 0;
  std::int32_t iFreq;
  std::memcpy(&iFreq, &freq, sizeof iFreq);
  while (iVar3 < iVar4) {
    const int mid = (iVar4 + iVar3) / 2;
    std::int32_t thrBits;
    std::memcpy(&thrBits, &desc[mid].thr, sizeof thrBits);
    if (thrBits <= iFreq) {
      iVar3 = mid + 1;      // hi unchanged (device: iVar2 = iVar4, iVar4 = iVar2)
    } else {
      iVar4 = mid;
    }
  }
  if (freq != desc[iVar3].thr && freq <= 1575.0f && iVar3 > 0) {
    iVar3 = iVar3 - 1;
  }
  return iVar3;
}

// Lazily built global table set (the original's one-time DAT_180094f1c guard).
struct Tables {
  float sine[WAVE_TABLE_SIZE];
  float saw[NUM_BANDS * WAVE_TABLE_SIZE];
  float triangle[NUM_BANDS * WAVE_TABLE_SIZE];
  float square[NUM_BANDS * WAVE_TABLE_SIZE];
  BandDesc desc[NUM_BANDS];
  bool ready = false;

  void init() {
    fill_sine(sine);
    compute_band_descriptors(desc);
    for (int b = 0; b < (int)NUM_BANDS; ++b) {
      const int h = band_harmonics(desc[b]);
      float* s = saw + (std::size_t)b * WAVE_TABLE_SIZE;
      fill_saw_band(sine, h, s);
      fill_triangle_band(sine, h, triangle + (std::size_t)b * WAVE_TABLE_SIZE);
      fill_square_band(s, square + (std::size_t)b * WAVE_TABLE_SIZE);
    }
    ready = true;
  }
};

Tables& instance() {
  static Tables t;
  if (!t.ready) t.init();
  return t;
}

}  // namespace

// FUN_18003cdf0 band-descriptor generator, verbatim (disassembly-verified:
// pow base DAT_180077388 = 512.0, exponent step DAT_180077360 = 1/36 float,
// scale DAT_180077380 = 27.5, Nyquist DAT_1800773ac = 22050.0f, min 1.0f).
// Ladder: thr_j = (float)(pow(512, (float)j·(1/36f))·27.5) = 27.5·2^(j/4)
// (quarter-octave), kept while (int)(22050.0f/thr) >= 16 and the int changes
// → 23 bands. Extensions: 4 descriptors per iteration with thr = 22050/H for
// H = 14..3, then the tail H = 2,1 → 37 total (DAT_1801a42e8).
void compute_band_descriptors(BandDesc* desc) {
  int n = 0;
  int offset = 0;
  int lastInt = 0;
  int breakInt = 0;
  for (int j = 0; j < 0x24; ++j) {
    const double x = (double)(static_cast<float>(j) * 0.02777777798473835f);
    const float thr = static_cast<float>(std::pow(512.0, x) * 27.5);
    float q = 22050.0f / thr;
    if (q < 1.0f) q = 1.0f;
    const int qi = static_cast<int>(q);
    if (qi < 0x10) { breakInt = qi; break; }
    if (qi != lastInt) {
      desc[n].len = (int)WAVE_TABLE_SIZE;
      desc[n].offset = offset;
      desc[n].thr = thr;
      ++n;
      offset += (int)WAVE_TABLE_SIZE;
      lastInt = qi;
    }
  }
  int iVar31 = breakInt;
  if (3 < iVar31) {
    int iVar17 = iVar31 - 2;
    for (;;) {
      auto add = [&](int h) {
        float t = 22050.0f / (float)h;
        if (t < 1.0f) t = 1.0f;
        desc[n].len = (int)WAVE_TABLE_SIZE;
        desc[n].offset = offset;
        desc[n].thr = t;
        ++n;
        offset += (int)WAVE_TABLE_SIZE;
      };
      add(iVar31);
      add(iVar17 + 1);
      add(iVar17);
      add(iVar17 - 1);
      iVar31 -= 4;
      iVar17 -= 4;
      if (!(3 < iVar31)) break;
    }
  }
  while (0 < iVar31) {
    float t = 22050.0f / (float)iVar31;
    if (t < 1.0f) t = 1.0f;
    desc[n].len = (int)WAVE_TABLE_SIZE;
    desc[n].offset = offset;
    desc[n].thr = t;
    ++n;
    offset += (int)WAVE_TABLE_SIZE;
    --iVar31;
  }
  (void)n;
}

int band_harmonics(const BandDesc& desc) {
  return (int)(22050.0f / desc.thr);
}

void generate(Waveform wf, float* table, std::uint32_t size) {
  switch (wf) {
    case Waveform::kSine: {
      const std::uint32_t n = size < WAVE_TABLE_SIZE ? size : WAVE_TABLE_SIZE;
      for (std::uint32_t i = 0; i < n; ++i) {
        const double ang = 2.0 * M_PI * (double)i / (double)WAVE_TABLE_SIZE;
        table[i] = (float)std::sin(ang);
      }
      break;
    }
    case Waveform::kSaw:
    case Waveform::kPulse:
    case Waveform::kTriangle:
    case Waveform::kSquare: {
      // Band-0 table (the lowest band, H = 27). Full 36-band sets go through
      // generate_banded / get_table.
      BandDesc desc[NUM_BANDS];
      compute_band_descriptors(desc);
      float sine[WAVE_TABLE_SIZE];
      fill_sine(sine);
      if (wf == Waveform::kTriangle) {
        fill_triangle_band(sine, band_harmonics(desc[0]), table);
      } else if (wf == Waveform::kSquare) {
        float saw0[WAVE_TABLE_SIZE];
        fill_saw_band(sine, band_harmonics(desc[0]), saw0);
        fill_square_band(saw0, table);
      } else {
        fill_saw_band(sine, band_harmonics(desc[0]), table);
      }
      break;
    }
    case Waveform::kNoise:
    default: {
      // spec_osc.md §6.3: noise has no table (LfsrNoise generates per sample).
      for (std::uint32_t i = 0; i < size; ++i) table[i] = 0.0f;
      break;
    }
  }
}

void generate_banded(Waveform wf, float* tables) {
  float sine[WAVE_TABLE_SIZE];
  fill_sine(sine);
  BandDesc desc[NUM_BANDS];
  compute_band_descriptors(desc);
  for (int b = 0; b < (int)NUM_BANDS; ++b) {
    const int h = band_harmonics(desc[b]);
    float* out = tables + (std::size_t)b * WAVE_TABLE_SIZE;
    switch (wf) {
      case Waveform::kSaw:
      case Waveform::kPulse:
        fill_saw_band(sine, h, out);
        break;
      case Waveform::kTriangle:
        fill_triangle_band(sine, h, out);
        break;
      case Waveform::kSquare: {
        float sawb[WAVE_TABLE_SIZE];
        fill_saw_band(sine, h, sawb);
        fill_square_band(sawb, out);
        break;
      }
      case Waveform::kSine:
      case Waveform::kNoise:
      default:
        for (std::uint32_t i = 0; i < WAVE_TABLE_SIZE; ++i) out[i] = 0.0f;
        break;
    }
  }
}

const float* get_table(Waveform wf, float freq) {
  Tables& t = instance();
  switch (wf) {
    case Waveform::kSine:
      return t.sine;
    case Waveform::kSaw:
    case Waveform::kPulse:
      return t.saw + (std::size_t)select_band(t.desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kTriangle:
      return t.triangle + (std::size_t)select_band(t.desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kSquare:
      return t.square + (std::size_t)select_band(t.desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kNoise:
    default:
      return nullptr;  // spec_osc.md §5.6: noise lookup skipped
  }
}

// ---------------------------------------------------------------------------
// WavetableBank — view over the same lazily built global tables (the
// original's DAT_1801a4298… set). Zero allocation: pointers only.
// ---------------------------------------------------------------------------
const float* WavetableBank::table(Waveform wf, float freq) const {
  switch (wf) {
    case Waveform::kSine:
      return sine;
    case Waveform::kSaw:
    case Waveform::kPulse:
      return saw + (std::size_t)select_band(desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kTriangle:
      return triangle + (std::size_t)select_band(desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kSquare:
      return square + (std::size_t)select_band(desc, freq) * WAVE_TABLE_SIZE;
    case Waveform::kNoise:
    default:
      return nullptr;  // spec_osc.md §5.6: noise lookup skipped
  }
}

const WavetableBank& global_bank() {
  static WavetableBank bank;
  static bool built = false;
  if (!built) {
    Tables& t = instance();  // builds the tables once (init-time allocation)
    bank.sine = t.sine;
    bank.saw = t.saw;
    bank.triangle = t.triangle;
    bank.square = t.square;
    bank.desc = t.desc;
    built = true;
  }
  return bank;
}

}  // namespace synth1::tables
