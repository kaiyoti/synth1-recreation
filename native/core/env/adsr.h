// adsr.h — the CEnvelopedObject state machine (amp envelope).
//
// Spec: spec_env.md §0 (CEnvelopedObject field map: +0x28 scale, +0x2c
//       decayMode, +0x30 releaseMode, +0x38 stage, +0x40 level, +0x44
//       sustainLevel, +0x4c attackSamples, +0x50 decaySamples, +0x54
//       sustainRatio, +0x58 releaseSamples, +0x5c curLevel, +0x60
//       progress, +0x64 offLevel, +0x68 recomputeAt, +0x6c chunkLen),
//       §2.1 (time-constant mapping + FUN_18000d210/0d260/0d3a0
//       seconds→samples), §2.2 (sustain curve + FUN_18000d3f0), §2.3
//       (per-sample level update — the exact state machine: attack linear,
//       decay/release exponential via expf((r−1)·8)·r·delta + target or
//       linear, sustain constant; adaptive chunk size FUN_18000c780 with
//       CVca +0x34 = 1 → half-size chunks), §2.4 (note-on/note-off
//       triggers), §3.1 (amp gain curve — param 29), §4.2 (velocity
//       curve).
// Ghidra: FUN_18000ccc0 (envelope state machine), LAB_18000cead /
//         LAB_18000ced3 (decay/release level), FUN_18000c780 (adaptive
//         chunk size), FUN_18000ca70 (block/chunk driver — walks the block
//         in chunks, one level per chunk), FUN_18000c930 (note-on),
//         FUN_18000c900 / FUN_180037bb0 (note-off, captures +0x64),
//         FUN_18000c950 (hard reset), FUN_180037c30 (CVca per-block gain
//         application, vftable+0x38), FUN_180037bc0 (amp velocity factor +
//         note-on), FUN_180037b00 (CVca ctor: scale 0x800 = 2048, ampGain
//         0.5 default, +0x34 = 1), FUN_180028fe0 (param→DSP dispatcher,
//         cases 0x19–0x1e).
//
// Precision (spec_env.md §9): the time-constant curves (pow, exp) are
// evaluated in double and stored as float; envelope levels are INTEGERS
// (scale 2048); the final gain multiply is pure float.
//
// The envelope is NOT evaluated per sample: FUN_18000ca70 walks the block
// in chunks and FUN_18000ccc0 computes one target level per chunk.
// advance_chunk() below reproduces that walk exactly: it consumes `len`
// samples, splitting them into chunks (attack = one chunk; decay/release =
// min(chunk_len, remaining-in-stage) per chunk; sustain = one chunk),
// evaluating each chunk with the spec's exact formulas and advancing
// progress. The level of the LAST chunk consumed is the level the device
// would apply at the end of this call.
//
// The CVca gain stage's per-block linear ramp (spec_env.md §3.2, anti-click
// smoothing) is not reproduced here — stage 1 applies the chunk level
// directly (see docs/reconstruction-status.md, ambiguity #2).

#pragma once

#include <cstdint>
#include <cmath>

namespace synth1::env {

// ---------------------------------------------------------------------------
// Time-constant curves (spec_env.md §2.1–§2.2, §3.1 — params 25–29;
// implemented in adsr.cpp).
// ---------------------------------------------------------------------------
float amp_attack_seconds(int band);   // param 25
float amp_decay_seconds(int band);    // param 26
float amp_sustain_ratio(int band);    // param 27
float amp_release_seconds(int band);  // param 28
float amp_gain_curve(int band);       // param 29
float amp_velocity_sens(int band);    // param 30

// U5-a′ seconds→samples (d-opus-u5a-scope.answer.md; 32-bit setters
// FUN_1000baa0/bb00/bc70 decomp_all_32_full.c:8491/8518/8620, shared by
// CVca/CVcf/CVco): `cvtdq2ps (float)fs; mulss t` — a FLOAT32 product —
// then FUN_1004c2c0 (:54803, CRT float→int64, mantissa shift = TRUNC).
// Device dumps: 13=0 decS 441 (f32 441.0 vs double 440.99999), 13=64
// decS 35349 (round would give 35350). The sample-rate re-fire
// FUN_1000b280 (:8068) is unreachable at a fixed per-render fs (no code).
inline int seconds_to_samples(float t, float fs) {
  const float p = fs * t;  // single-precision product (mulss)
  return (int)p;           // truncate toward zero
}

// stage values (spec_env.md §0: +0x38)
inline constexpr int STAGE_IDLE = 0;
inline constexpr int STAGE_ATTACK = 1;
inline constexpr int STAGE_DECAY = 2;
inline constexpr int STAGE_SUSTAIN = 3;
inline constexpr int STAGE_RELEASE = 4;

// CVca scale (spec_env.md §0: CVca = 2048; ctor FUN_180037b00 0x800).
inline constexpr int AMP_SCALE = 2048;

// CVca +0x34 = 1 → half-size envelope chunks (spec_env.md §2.3). Only the
// amp envelope has this flag; CVcf/CVco use the full chunk length.
inline constexpr bool AMP_HALF_CHUNKS = true;

// ---------------------------------------------------------------------------
// ADSR state (the CEnvelopedObject fields that stage 1 uses).
// ---------------------------------------------------------------------------
struct ADSR {
  // stage (+0x38)
  int stage = STAGE_IDLE;
  // levels (+0x40 / +0x44 / +0x5c / +0x64) — integers, spec_env.md §9
  int target_level = AMP_SCALE;   // +0x40  = (int)(scale·amount); amp: 2048
  int sustain_level = AMP_SCALE;  // +0x44  = (int)(scale·amount·sustainRatio)
  int cur_level = 0;             // +0x5c  level handed to the device per chunk
  int off_level = 0;             // +0x64  level captured at note-off
  // stage durations in samples (+0x4c / +0x50 / +0x58)
  int attack_samples = 0;
  int decay_samples = 0;
  int release_samples = 0;
  // stage progress (+0x60)
  int progress = 0;
  // adaptive chunk state (+0x68 / +0x6c)
  int recompute_at = 0;
  int chunk_len = 1;
  // sustain ratio (+0x54) — float 0..1
  float sustain_ratio = 1.0f;
  // decay/release shape (+0x2c / +0x30); amp default = exponential (0)
  int decay_mode = 0;    // 1 = linear, 0 = exponential
  int release_mode = 0;  // 1 = linear, 0 = exponential
  // CEnvelopedObject+0x34: only CVca sets this (half-size chunks).
  bool half_chunks = false;

  // ---- time setters (spec_env.md §2.1; FUN_18000d210/0d260/0d3a0) ----
  // The seconds→samples conversion is seconds_to_samples() (U5-a′, below).
  // Setting attack time
  // while in attack rescales progress so the ramp stays continuous
  // (spec_env.md §2.1, FUN_18000d210: if stage==1, progress = (progress·
  // iVar2)/oldAttackSamples).
  void set_attack_seconds(float t, float fs) {
    const int old = attack_samples;
    attack_samples = seconds_to_samples(t, fs);
    if (stage == STAGE_ATTACK && old != 0 && attack_samples != 0)
      progress = (progress * attack_samples) / old;
  }
  void set_decay_seconds(float t, float fs) {
    decay_samples = seconds_to_samples(t, fs);
  }
  void set_release_seconds(float t, float fs) {
    release_samples = seconds_to_samples(t, fs);
  }
  // Sustain ratio (spec_env.md §2.2: FUN_18000d3f0 stores +0x54 and
  // recomputes +0x44 = (int)(scale·amount·ratio)).
  void set_sustain_ratio(float r) {
    sustain_ratio = r;
    sustain_level = (int)((float)AMP_SCALE * 1.0f * r);
  }
  // Amount (spec_env.md §5.2, FUN_18000d2c0): the amp envelope amount = 1.0
  // (CVca ctor, spec_env.md §0), so target = (int)(scale·1.0) = 2048.
  void set_amount(float a) {
    target_level = (int)((float)AMP_SCALE * a);
    sustain_level = (int)((float)AMP_SCALE * a * sustain_ratio);
  }

  // ---- note-on / note-off / reset (spec_env.md §2.4) ----
  // FUN_18000c930: stage = 1 (attack); if cur_level == 0 → progress = 0,
  // else legato: progress = (attack_samples·cur_level)/target_level.
  void note_on() {
    stage = STAGE_ATTACK;
    if (cur_level == 0) progress = 0;
    else progress = (attack_samples * cur_level) / (target_level != 0 ? target_level : 1);
    cur_level = 0;
    recompute_at = 0;
  }
  // FUN_18000c900 / FUN_180037bb0: if in attack/decay/sustain → release;
  // capture off_level = cur_level; progress = 0; recompute_at = 0; recompute.
  void note_off() {
    if ((stage & ~4) != 0) {  // was in attack/decay/sustain (stage 1/2/3)
      stage = STAGE_RELEASE;
      off_level = cur_level;
      progress = 0;
      recompute_at = 0;
      recompute_chunk();
    }
  }
  // FUN_18000c950: hard reset (called from the end-of-release hook).
  void reset() {
    stage = STAGE_IDLE;
    cur_level = 0;
    progress = 0;
    off_level = 0;
    recompute_at = 0;
    chunk_len = 1;
  }

  // ---- adaptive chunk size (spec_env.md §2.3, FUN_18000c780) ----
  // CVca has +0x34 = 1 (half_chunks = true): chunk_len = recompute_at / 2.
  // CVcf/CVco have +0x34 = 0: chunk_len = rate * mult (rate / 9 .. rate * 5).
  void recompute_chunk() {
    int dur, delta;
    if (stage == STAGE_DECAY) { dur = decay_samples; delta = target_level - sustain_level; }
    else                      { dur = release_samples; delta = off_level; }
    if (dur < 1) dur = 1;
    if (delta < 1) delta = 1;
    const int rate = dur / delta;  // samples per unit of level change

    const int mode = (stage == STAGE_DECAY) ? decay_mode : release_mode;
    const int prev = recompute_at;  // +0x68: absolute progress mark
    if (mode == 1) {  // LINEAR
      if (half_chunks) {
        recompute_at = dur;
        chunk_len = dur > 1 ? dur : 1;
        return;
      }
      chunk_len = rate > 1 ? rate : 1;
      recompute_at = dur;
      return;
    }

    // FUN_18000c780 (exponential):
    // recompute marks: dur/20 -> dur/10 -> dur/4 -> dur/3 -> dur/2 -> dur
    if (prev < dur / 2) {
      if (dur / 3 <= prev)        { recompute_at = dur / 2;  chunk_len = rate * 2; }
      else if (dur / 4 <= prev)   { recompute_at = dur / 3;  chunk_len = rate; }
      else if (dur / 10 <= prev)  { recompute_at = dur / 4;  chunk_len = rate / 4; }
      else if (dur / 20 <= prev)  { recompute_at = dur / 10; chunk_len = rate / 6; }
      else                        { recompute_at = dur / 20; chunk_len = rate / 9; }
    } else {
      recompute_at = dur;
      chunk_len = rate * 5;
    }
    // CVca (+0x34 = 1): chunk = (+0x68)/2 — half the recompute mark
    if (half_chunks) chunk_len = recompute_at / 2;
    if (chunk_len < 1) chunk_len = 1;
  }

  // #171: device FUN_1000b580 LAB_1000b7ad loop-head ENTRY guard
  // (decomp_all_32_full.c:8352):
  //   if ((local_3c < 1) || (*(int *)(this + 0x50) <= (int)uVar4)) break;
  // — a decay/release stage whose level already sits at/below its target
  // breaks the chunk loop BEFORE any chunk is consumed (a release armed at
  // level 0 completes with ZERO samples walked). The `local_3c < 1` operand
  // is the walk-call remainder = the engine's while (len > 0) head.
  bool entry_guard() const {
    switch (stage) {
      case STAGE_DECAY:   return cur_level <= sustain_level;
      case STAGE_RELEASE: return cur_level <= 0;
      default: return false;
    }
  }

  // Samples until the next envelope chunk boundary (spec_env.md §2.3,
  // FUN_18000ca70 / FUN_18000ccc0): chunk boundaries = min(next mark boundary,
  // remaining host-block samples).
  int next_chunk_len(int rem) const {
    switch (stage) {
      case STAGE_ATTACK: {
        const int a = attack_samples > 0 ? attack_samples : 1;
        int step = a;
        if (!half_chunks) {
          const int l = target_level < 0 ? -target_level : target_level;
          const int q = a / (l > 1 ? l : 1);
          step = q > 0 ? q : 1;
        }
        int c = a - progress;
        if (step < c) c = step;
        if (rem < c) c = rem;
        return c > 0 ? c : 1;
      }
      case STAGE_DECAY: {
        const int r = decay_samples - progress;
        const int c = chunk_len < r ? chunk_len : r;
        return c < rem ? c : rem;
      }
      case STAGE_RELEASE: {
        const int r = release_samples - progress;
        const int c = chunk_len < r ? chunk_len : r;
        return c < rem ? c : rem;
      }
      default:
        return rem;
    }
  }

  // ---- per-chunk level (spec_env.md §2.3, FUN_18000ccc0) ----
  // Consumes `len` samples starting at `progress`, splitting them into
  // chunks (attack = one chunk; decay/release = min(chunk_len,
  // remaining-in-stage) per chunk; sustain = one chunk), evaluating each
  // chunk with the spec's exact formulas and advancing progress. Returns
  // the level of the LAST chunk consumed — the level the device (CVca)
  // would apply at the end of this call.
  int advance_chunk(int len) {
    while (len > 0) {
      switch (stage) {
        case STAGE_ATTACK: {
          const int a = attack_samples > 0 ? attack_samples : 1;
          // spec_env.md §2.3 attack: level = (progress+chunk)·targetLevel /
          // attackSamples, clamped at target (lVar13 = target). On stage
          // completion the remainder of the chunk carries into decay (the
          // device's block walk FUN_18000ca70 splits the block at stage
          // boundaries).
          const int need = a - progress;
          if (len >= need) {
            progress = 0;
            stage = STAGE_DECAY;
            cur_level = target_level;
            recompute_at = 0;
            recompute_chunk();
            len -= need;
          } else {
            progress += len;
            // 64-bit like the device (progress·level overflows int32 for
            // attacks > ~24 s at 2048).
            int lvl = (int)(((long long)progress * target_level) / a);
            if (lvl > target_level) lvl = target_level;
            cur_level = lvl;
            len = 0;
          }
          break;
        }
        case STAGE_DECAY:
        case STAGE_RELEASE: {
          const int total = (stage == STAGE_DECAY) ? decay_samples : release_samples;
          const int start = (stage == STAGE_DECAY) ? target_level : off_level;
          const int target = (stage == STAGE_DECAY) ? sustain_level : 0;
          // #171: device loop-head ENTRY guard (FUN_1000b580 LAB_1000b7ad
          // :8352 `if (local_3c < 1 || cur(+0x50) <= target) break;`) —
          // tested BEFORE any chunk step: a stage already at/below its
          // target completes here with ZERO samples consumed (release
          // armed at level 0). Post-loop terminal :8440-8444.
          if (cur_level <= target) {
            if (stage == STAGE_DECAY) { stage = STAGE_SUSTAIN; cur_level = sustain_level; }
            else { stage = STAGE_IDLE; cur_level = 0; }
            len = 0;
            break;
          }
          const int delta = start - target;
          const int mode = (stage == STAGE_DECAY) ? decay_mode : release_mode;
          int n = len;
          if (n > chunk_len) n = chunk_len;
          const int rem = total - progress;
          if (n > rem) n = rem;
          if (n <= 0) {  // stage already exhausted
            if (stage == STAGE_DECAY) { stage = STAGE_SUSTAIN; cur_level = sustain_level; }
            else { stage = STAGE_IDLE; cur_level = 0; }
            len = 0;
            break;
          }
          const int end = progress + n;
          if (mode == 1) {  // LINEAR (spec_env.md §2.3)
            // m3r-071: device ratio association, bytes-first (32-bit DLL
            // FUN_1000b580 LAB_1000b7ad): divss 0x1000b7cf ratio=1.0f/(float)total,
            // mulss 0x1000b7dc r=(float)delta*ratio, mulss 0x1000b842
            // (float)end*r, _ftol3 0x1000b846 (FUN_1004c2de), sub 0x1000b854.
            // All float32 SSE. lvl = start - (int)((float)end*r).
            float f = 1.0f / (float)total;
            const float r = (float)delta * f;
            int lvl = start - (int)((float)end * r);
            if (lvl < target) lvl = target;  // clamp to target
            cur_level = lvl;
          } else {  // EXPONENTIAL (spec_env.md §2.3):
            // r = (total−progress−n)/total; level = ROUND(expf((r−1)·8)·r·delta) + target
            // #95a: the 32-bit render DLL FUN_1000b580 computes the product in
            // float (flds 0x1000b8d7) then converts with x87 fistpl
            // (0x1000b8db) = round-to-nearest, ties-to-even (no fldcw on the
            // envelope path), then adds target (0x1000b8e3). The 64-bit DLL
            // truncates (cvttss2si, spec_env.md §2.3) — genuine DLL
            // divergence; the 32-bit render DLL is primary. Explicit
            // ties-to-even, independent of the fesetround mode.
            float f = 1.0f / (float)total;
            float r = (float)(total - end) * f;
            float v = ::expf((r - 1.0f) * 8.0f) * r * (float)delta;  // v >= 0
            float fl = ::floorf(v);
            float fr = v - fl;
            int rv = (int)fl + (fr > 0.5f || (fr == 0.5f && (((int)fl) & 1)));
            cur_level = rv + target;
          }
          progress = end;
          len -= n;
          // FUN_18000ccc0: the stage ends once progress reaches the total OR
          // the level has landed on the target (loop guard target < level);
          // CVca's exponential release also ends at 0.9·total.
          // #122: the 32-bit render DLL threshold is ROUND(len·0.9f) —
          // FUN_1000b580 :8432 `(int)ROUND((float)iVar8 * DAT_100635f4)`,
          // DAT_100635f4 = 0.9f, x87 ROUND = ties-to-even (95a §1); the
          // 64-bit cross truncates (decomp_all.c:8501, DAT_180072b74) —
          // 32-bit primary. Explicit ties-to-even, as the level branch.
          bool done = !(progress < total && target < cur_level);
          if (!done && half_chunks && stage == STAGE_RELEASE && mode == 0) {
            const float t9 = (float)total * 0.9f;
            const float fl9 = ::floorf(t9);
            const float fr9 = t9 - fl9;
            const int thr9 =
                (int)fl9 + (fr9 > 0.5f || (fr9 == 0.5f && (((int)fl9) & 1)));
            if (progress >= thr9) done = true;
          }
          if (done) {  // stage complete
            if (stage == STAGE_DECAY) { stage = STAGE_SUSTAIN; cur_level = sustain_level; }
            else { stage = STAGE_IDLE; cur_level = 0; }
            len = 0;
            break;
          }
          if (progress >= recompute_at) recompute_chunk();
          break;
        }
        case STAGE_SUSTAIN: {
          cur_level = sustain_level;
          len = 0;
          break;
        }
        default: {  // STAGE_IDLE
          cur_level = 0;
          len = 0;
          break;
        }
      }
    }
    return cur_level;
  }
};

}  // namespace synth1::env
