// lfo.h — the 2× CLFO (LFO1 / LFO2).
//
// Spec: out/spec_lfo_fx.md §1 (LFOs — 2 of them), §1.1 layout, §1.3 speed→Hz,
//       §1.4 frequency slew + phase, §1.5 phase advance, §1.6 key sync,
//       §1.7 depth→modulation range, §1.8 destination routing (7 values),
//       §1.9 the 6 waveforms.
//
// Cited Ghidra addresses (out/decomp_all.c, 64-bit image base 0x180000000):
//   CLFO layout / ctor          LAB_180014c40 (CLFO ctor, dtor slot only —
//                               LFO methods are called directly, not virtual)
//   2× instance stride          engine+0xab70 (LFO1) / +0xacd0 (LFO2);
//                               FUN_180057494(engine+0xab70, 0x160, 2, ctor)
//   object size                 0x160
//   speed→Hz (free + tempo)     FUN_180015210; DIV[30] table @0x180073cf0
//   frequency slew + phase      FUN_180014ff0
//   phase advance               FUN_180014f60
//   depth→modulation range      FUN_180015080
//   key-sync reset              FUN_1800152e0 (gate per spec_lfo_fx.md U1 —
//                               the exact per-LFO gate is not 100 % resolved)
//   the 6 waveform generators   FUN_180014d10(lfo, &segLen, phase)
//   per-sample LFO accessor     FUN_180014ce0(lfo, &segLen, offset)
//   S&H table                   FUN_18003cad0 (2048-float smoothed-noise table)
//   engine tempo (BPM, float)   engine+0xbb48; written by FUN_180028830 from
//                               VstTimeInfo->tempo
//
// Precision (spec_lfo_fx.md §1.9): all waveform arithmetic is INTEGER
// (P = lfo->period, p = phase 0..P-1, mx = max(hi,lo), mn = min(hi,lo),
// flip = (hi < lo)). The only floating-point in the LFO is the free-run
// speed→Hz (powf), the tempo-sync freq, the freq slew, and the noise table
// (float). lfo_tick() below reconstructs the original phase-advance + the
// integer waveform generator.
//
// 6 waveforms (spec_lfo_fx.md §1.9; the spec says "6 waveforms"):
//   0 = saw (down), 1 = triangle, 2 = square, 3 = S&H,
//   4 = random glide, 5 = noise.
//
// 7 destinations (spec_lfo_fx.md §1.8; GUI label list @0x1800761b8
// " osc2" "osc1,2" "filter" " amp" " p/w" "  FM" " pan"; destination = index+1):
//   1 = osc2, 2 = osc1,2, 3 = filter, 4 = amp, 5 = p/w, 6 = FM, 7 = pan.

#pragma once

#include <cstdint>

namespace synth1::lfo {

// ---------------------------------------------------------------------------
// CLFO object layout (spec_lfo_fx.md §1.1 — the +0x.. fields, byte offsets).
//   +0x08 onOff, +0x0c wave(0..5), +0x10 destination(1..7), +0x14 tempoSync,
//   +0x18 keySync, +0x1c speed(0..127), +0x20 depth(0..127), +0x28 owner,
//   +0x30 sampleRate, +0x34 minSegment, +0x38 freqCur, +0x3c freqTarget,
//   +0x40 lo, +0x44 hi, +0x48 phase, +0x4c period, +0x50 rndCur, +0x54 rndNext.
// Ctor defaults: freqCur = freqTarget = 1.0f, period = 44100, rest 0.
// ---------------------------------------------------------------------------
struct LfoState {
  std::int32_t onOff        = 0;   // +0x08
  std::int32_t wave         = 0;   // +0x0c  0..5
  std::int32_t destination  = 0;   // +0x10  1..7 (0 = unset)
  std::int32_t tempoSync    = 0;   // +0x14
  std::int32_t keySync      = 0;   // +0x18
  std::int32_t speed        = 0;   // +0x1c  raw 0..127
  std::int32_t depth        = 0;   // +0x20  raw 0..127
  std::int32_t sampleRate   = 44100; // +0x30 (ctor default)
  std::int32_t minSegment   = 64;  // +0x34 (64, or 32 when freq > 10 Hz)
  float        freqCur      = 1.0f; // +0x38 Hz, slewed
  float        freqTarget   = 1.0f; // +0x3c Hz
  std::int32_t lo           = 0;   // +0x40 modulation range low
  std::int32_t hi           = 0;   // +0x44 modulation range high
  std::int32_t phase        = 0;   // +0x48 samples, 0 .. period-1
  std::int32_t period       = 44100; // +0x4c = (int)(sr / freqCur)
  std::int32_t rndCur       = 0;   // +0x50 S&H value
  std::int32_t rndNext      = 0;   // +0x54 S&H value for the next segment
};

// The 2× LFO (spec_lfo_fx.md §1: "two instances embedded in the engine").
struct LfoPair {
  LfoState lfo[2] = {};  // [0] = LFO1 @ engine+0xab70, [1] = LFO2 @ +0xacd0
};

// ---------------------------------------------------------------------------
// Speed → Hz (spec_lfo_fx.md §1.3, FUN_180015210).
//   free-run (tempoSync == 0):  freq = powf(1500, speed/127) / 12
//     (speed 0 → 0.0833 Hz, speed 127 → 125 Hz)
//   tempo-synced:               freq = tempo·48 / DIV[i] · (1/120),
//                               i = (speed·30) >> 7  (0..29)
//   then minSegment = (freq > 10) ? 32 : 64.
// tempo is engine->tempo (float BPM).
// ---------------------------------------------------------------------------
inline const std::int32_t kDiv[30] = {
  1536, 1152, 864, 768, 576, 432, 384, 288, 216, 192,
  144, 96, 84, 72, 48, 42, 36, 32, 24, 21,
  18, 16, 12, 9, 8, 6, 4, 3, 2, 1
};  // @0x180073cf0 (spec_lfo_fx.md §1.3 / §10)

void speed_to_hz(LfoState& lfo, float tempo_bpm);

// FUN_10013360 sample-rate setter: 1 slew step iff cur!=tgt + UNCONDITIONAL
// period recompute/rescale; once per effSetSampleRate (FUN_10024ad0).
void sr_setter(LfoState& lfo);

// INI entry-load: speed field + target applied WITHOUT a slew step.
void set_speed_target(LfoState& lfo, float tempo_bpm);

// ---------------------------------------------------------------------------
// Depth → modulation range (spec_lfo_fx.md §1.7, FUN_180015080).
//   dest 1,2 (pitch):  d = ((10^(depth/127)) - 1) / 9   (exponential, [0,1])
//   other dests:       d = depth / 127                 (linear, [0,1])
//   then the per-destination lo/hi scale (5 semitones × 128 for pitch;
//   1024 filter; -2048 amp; ±128 p/w; 512 FM; ±64 pan).
// ---------------------------------------------------------------------------
void depth_to_range(LfoState& lfo);

// ---------------------------------------------------------------------------
// Frequency slew + phase (spec_lfo_fx.md §1.4, FUN_180014ff0(lfo, n)).
// The LFO frequency is glide-limited to 1000 Hz/second and the phase is
// rescaled so the waveform does not jump when the period changes.
// ---------------------------------------------------------------------------
void freq_slew(LfoState& lfo, int n);

// ---------------------------------------------------------------------------
// The 6 waveforms (spec_lfo_fx.md §1.9, FUN_180014d10).
// Returns the integer LFO value v (in the [mn, mx] range), and writes
// *outSegLen (the block-split hint; the original's band-limited /
// discontinuity-free LFO). The noise waveform (wave 5) uses the 2048-float
// smoothed-noise table (FUN_18003cad0); a stable stand-in table is used for
// the port (docs/reconstruction-status.md, stage-4 ambiguity #1).
//
// The integer generator is exact (the spec's formulas). The S&H / random
// values (rndCur / rndNext) are seeded by the key-sync reset and the
// phase-wrap re-seed; a stable RNG keeps them deterministic.
// ---------------------------------------------------------------------------
int waveform(int wave, std::int32_t P, std::int32_t p,
            std::int32_t lo, std::int32_t hi,
            std::int32_t rndCur, std::int32_t rndNext,
            int minSegment, int* outSegLen);

// ---------------------------------------------------------------------------
// lfo_tick — advance the LFO phase by n samples and return the current LFO
// value (the spec's "value −1..1"; here the integer value in [mn, mx]).
// Reconstructs FUN_180014f60(lfo, n) (phase advance + S&H re-seed on wrap)
// followed by FUN_180014ff0(lfo, n) (freq slew). Called once per render
// chunk, after the device chain (spec_lfo_fx.md §1.5 / §13).
// ---------------------------------------------------------------------------
int lfo_tick(LfoState& lfo, int n);

// ---------------------------------------------------------------------------
// Device LFO read/advance law (FUN_1800285a0 + FUN_180014ce0 + FUN_180014d10).
//
// The device advances an LFO's phase / frequency slew ONCE PER RENDER CHUNK
// (FUN_180014f60(lfo, chunkLen)), where a chunk is the remaining frames of the
// caller block, capped at (period - phase) for an ON wave-3/4 LFO
// (FUN_180014cd0) and at 0x20 while the pitch bend is slewing. Note-on /
// note-off do NOT cut a chunk: the LFO free-runs continuously from construction
// (bank-001 load draws included) across notes and gaps; key-sync
// (FUN_1800152e0) is the only reset and it is gated per live-key.
//
// Inside a chunk the voice loop re-reads the LFO through FUN_180014ce0 at
// (phase + offset) mod period and renders exactly the segment length
// FUN_180014d10 writes into *outSegLen (clamped up to minSegment) before
// re-reading. read_at() / segment_at() are that accessor; chunk_cap() is the
// engine-level cap. A fixed control grid that advances the phase per tick
// instead of per chunk wraps mid-tick, carries a phase remainder and picks the
// redrawn S&H value up late — the "S&H period phase" residual in bank0_121 /
// bank0_124 — and reads a wave-5 (table sine) value twice per minSegment
// segment — the bank0_085 residual.
// ---------------------------------------------------------------------------
inline std::int32_t read_at(const LfoState& lfo, std::int32_t offset,
                            std::int32_t* outSegLen) {
  std::int32_t p = lfo.phase + offset;
  if (lfo.period <= p) p %= lfo.period;
  return waveform(lfo.wave, lfo.period, p, lfo.lo, lfo.hi,
                  lfo.rndCur, lfo.rndNext, lfo.minSegment, outSegLen);
}

// Samples until this LFO's next segment boundary inside a chunk (the value the
// voice holds until it re-reads). Large when the LFO imposes no boundary.
int segment_at(const LfoState& lfo, std::int32_t offset);

// m3-u4c-resume-land: the LFO-wired consumer chunk bound (FUN_10012c00
// *param_1 = max(segEnd - phase, minChunk); local non-persisting phase copy,
// FUN_10012be0). Waves 3/4 wrap-only. The caller caps at the render segment.
int consumer_cut(const LfoState& lfo, std::int32_t offset);

// FUN_180014cd0: frames until the period wrap, for an ON wave-3/4 LFO; large
// otherwise (the engine chunk cap).
inline std::int32_t chunk_cap(const LfoState& lfo) {
  if (lfo.onOff == 0 || lfo.period <= 0) return 0x7fffffff;
  if ((std::uint32_t)(lfo.wave - 3) < 2u) return lfo.period - lfo.phase;
  return 0x7fffffff;
}

// FUN_180015450 (params 42/47): set the wave; waves 3/4 seed rndNext =
// (lo+hi)/2 and draw rndCur from the shared CRT rand() stream.
void set_wave(LfoState& lfo, int wave);

// FUN_1800152e0: key-sync reset (phase 0; waves 3/4 redraw the S&H value).
// The device calls it only from the live note-on (FUN_180028020) when the
// held-key count becomes 1 and the LFO's keySync is on — never for
// arpeggiator-emitted notes (they go through FUN_1800280b0).
void key_sync(LfoState& lfo);

// The 6 waveforms (spec_lfo_fx.md §1.9).
inline constexpr std::int32_t WAVE_SAW    = 0;
inline constexpr std::int32_t WAVE_TRI    = 1;
inline constexpr std::int32_t WAVE_SQUARE = 2;
inline constexpr std::int32_t WAVE_SH     = 3;
inline constexpr std::int32_t WAVE_RGLIDE = 4;
inline constexpr std::int32_t WAVE_NOISE  = 5;
inline constexpr std::int32_t WAVE_COUNT  = 6;

// The 7 destinations (spec_lfo_fx.md §1.8; destination = index + 1).
inline constexpr std::int32_t DEST_OSC2   = 1;
inline constexpr std::int32_t DEST_OSC12  = 2;
inline constexpr std::int32_t DEST_FILTER = 3;
inline constexpr std::int32_t DEST_AMP    = 4;
inline constexpr std::int32_t DEST_PW     = 5;
inline constexpr std::int32_t DEST_FM     = 6;
inline constexpr std::int32_t DEST_PAN    = 7;
inline constexpr std::int32_t DEST_COUNT  = 7;

}  // namespace synth1::lfo
