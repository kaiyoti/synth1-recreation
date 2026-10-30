// osc_dispatch.cpp — CVco oscillator engine: kernel dispatch + dual-osc render.
//
// Spec: kernel_map.md §2 (leaf bijection), §4 (amplitude compensation),
//       §8 (arg frame / phase arithmetic); spec_osc.md §6.1 (canonical
//       kernel: osc2 term first, osc1 term second, sub third, then the
//       phase accumulators advance), §6.2 (pulse = difference of two
//       interpolated saw reads at index offset dutyIdx), §6.3 (noise: 21-bit
//       Galois LFSR, not phase-locked, osc2 phase still advances), §6.4 (sub
//       at ph1 >> subOct), §6.5 (hard sync: proportional restart from the
//       pre-increment ph1 — the division is the giveaway), §6.6 (ring:
//       out = v2·g2·v1 + g1·v1).

#include "osc_dispatch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace synth1::osc {

namespace {
// COSRAMP (DAT_1801a32e0): (1 + cos(i·π/257))·0.5, 256 doubles. Index 256
// (the first lookup, rem == rampTotal) reads zeroed BSS → 0.0, so the ramp
// starts exactly at rampStart (spec_osc.md §6.7).
const double* cosramp() {
  static const std::array<double, 256> table = [] {
    std::array<double, 256> t{};
    for (int i = 0; i < 256; ++i)
      t[i] = (1.0 + std::cos(i * 3.14159265358979311599796346854418516 / 257.0)) * 0.5;
    return t;
  }();
  return table.data();
}
}  // namespace

int compute_kernel_leaf(const OscParams& p) {
  const int sync = p.sync ? 1 : 0;
  const int pulse = (p.osc1_wave == 2) ? 1 : 0;
  const int osc2_class = (p.osc2_wave == 2) ? 0 : (p.osc2_wave == 4 ? 1 : 2);
  // D = 0.0 < cv.detune (spec_osc.md §8; law B #62 — the gate var is the
  // cv+0x104 float set by param 76, not a separate flag).
  const int det = (0.0f < p.detune) ? 1 : 0;
  const int g = p.ring
                    ? (4 + det)
                    : ((p.fm_active ? 1 : 0) * 2 + det);
  return sync * 36 + g * 6 + (1 - pulse) * 3 + osc2_class;
}

void apply_amplitude_compensation(OscParams& p) {
  // kernel_map.md §4, verbatim order.
  if (p.osc1_wave == 2 && !p.ring) {
    p.osc1_gain *= 0.5f;  // DAT_180072b60 (kernel_map §4: gated on ring==0)
  }
  // M2 law 1b: the saw x0.7316 compensation removed (reports/gain_ledger.md
  // divergence 2 / row 8: no DAT_ behind it; saw tables carry scale 1.0,
  // DAT_180094f20). Pulse/norm paths untouched (laws 1a / osc2-pulse law).
  if (0.0f < p.detune && !p.ring) {  // device gate: D = 0.0 < cv.detune
    p.osc1_gain *= 0.3f;  // DAT_180072b5c
    p.sub_gain *= 0.3f;   // DAT_180072b5c
  }
  // M2 law 3 (reports/osc2_pulse_gain.md): the dispatcher gate is
  // `((osc2Wave == 2) && !FM) || ((osc2Wave == 4 && (fVar5 = DAT_180077210,
  // !FM))` — Ghidra short-circuit (decomp_all.c:37110): DAT_180077210
  // (0.65) is assigned to fVar5 ONLY inside the noise clause, so pulse
  // keeps fVar5 = DAT_180072b60 = 0.5 (init :36954, applied :37111).
  // 0.65 is noise-only; the osc2 pulse gets ×0.5 like the osc1 pulse.
  if (p.osc2_wave == 2 && !p.fm_active) {
    p.osc2_gain *= 0.5f;   // DAT_180072b60 (decomp_all.c:36954, :37110-37111)
  } else if (p.osc2_wave == 4 && !p.fm_active) {
    p.osc2_gain *= 0.65f;  // DAT_180077210 (decomp_all.c:37110, noise clause only)
  }
}

int pulse_duty_index(float pulse_width, int pw_mod) {
  // spec_osc.md §6.2 / spec_env.md §5.5 (DAT_1800755a8 = 1/128); clamp
  // [0.01, 1.99] (DAT_180073398 / DAT_180077378); ×1024.0 (DAT_180077390).
  float d;
  if (pw_mod < 1) {
    d = ((float)pw_mod * (1.0f / 128.0f) + 1.0f) * pulse_width;
  } else {
    d = (2.0f - pulse_width) * (float)pw_mod * (1.0f / 128.0f) + pulse_width;
  }
  d = std::clamp(d, 0.01f, 1.99f);
  return (int)((double)d * 1024.0);
}

void render_oscillators(const OscParams& params,
                        const float* osc1_table, const float* osc2_table,
                        const float* sub_table,
                        float* output, int num_samples,
                        std::int32_t inc1, std::int32_t inc2,
                        OscState& state,
                        const std::int32_t* det_inc) {
  OscParams p = params;

  // spec_osc.md §6.7: FM depth ramp state machine (FUN_18003bf50), run once
  // per render call (the device runs it per env sub-chunk). p.fm_depth is
  // the TARGET; the current depth lives in state.fm. The dispatcher's
  // FMactive flag (kernel selection + the osc2 pulse ×0.5 / noise ×0.65
  // compensation gate, decomp_all.c:37110) is
  // (depth != 0 || target != 0) AFTER the state machine — not the caller's
  // target-only fm_active — so a ramping-down depth keeps the FM kernel.
  FmRampState& fm = state.fm;
  const int fm_target = p.fm_depth;
  if (fm.last_target != fm_target) {
    const long long delta = (long long)fm.depth - fm_target;
    if (std::llabs(delta) < 0x4000001LL && fm.flag == 0) {
      if (fm.total <= fm.block_cur) {
        fm.total = fm.block_cur;   // ramp duration = time the old target held
        fm.rem = fm.block_cur;
        fm.start = fm.depth;
      }
    } else {
      // FUN_18003bf50: the snap is an 8-byte zero store at cv+0xb8 — it
      // clears BOTH flag (0xb8) and rampTotal (0xbc) — then depth = target.
      fm.flag = 0;
      fm.total = 0;
      fm.depth = fm_target;        // big jump: snap
    }
    if (fm.rem == 0) fm.depth = fm_target;
    fm.block_cur = 0;
    fm.last_target = fm_target;
  }
  const int ramp_max = p.sample_rate / 1000;  // cv+0x160 = sr/1000
  // m3-fm-walk-land rule B (FUN_10034740, byte-verified Finding 3): +0xa8
  // += the FULL requested call length BEFORE the call is cut at the ramp
  // remainder — 0x10034d17-0x10034d2e (mov +0xa8; mov +0x148; add %ebx,%eax
  // with ebx = requested length; cmp/cmovg cap +0x148; store), the cut at
  // 0x10034d44-53 happens AFTER the add. FUN_10034610 re-calls for the
  // remainder, which is counted again (below, at the ramp-end snap).
  if (fm.call_start > 0) {
    if (fm.block_cur < ramp_max)
      fm.block_cur = std::min(fm.block_cur + fm.call_start, ramp_max);
    fm.call_left = fm.call_start;
    fm.call_start = 0;
  }
  const bool fm_on = (fm.depth != 0) || (fm_target != 0);
  p.fm_active = fm_on;
  // m3-fmorder-land: the device's ramp-active flag is computed ONCE per call
  // at kernel entry (FUN_10041940 0x10041992-0x100419a8: cmp target,depth ->
  // cmovne -> [esp+0x30]), not per sample. While set, the re-interp test and
  // rem-- run every sample even after depth reaches target mid-call.
  const bool fm_ramp_active = (fm.depth != fm_target);
  const float g1_pre = p.osc1_gain, g2_pre = p.osc2_gain;  // pre-compensation mix gains
  apply_amplitude_compensation(p);

  const bool pulse = (p.osc1_wave == 2);
  const bool pulse2 = (p.osc2_wave == 2);
  const bool noise = (p.osc2_wave == 4);
  const std::uint32_t duty_idx =
      static_cast<std::uint32_t>(pulse_duty_index(p.pulse_width, p.pw_mod));
  const std::uint32_t duty = duty_idx << 16;
  // D1 (legion pulse_law.md): the device pulse is ALWAYS the two-saw difference
  // lerp(saw,ph) − lerp(saw,ph+duty_idx) on the same saw table, duty_idx clamped
  // to [10, 2037] (d∈[0.01,1.99]). The old d→0 "sawtooth limit" (plain saw below
  // duty_idx 24) is NOT in the device — the d→0 case is a band-limited derivative,
  // not a plain saw. Removed.

  std::uint32_t ph1 = state.osc1_phase;
  std::uint32_t ph2 = state.osc2_phase;
  tables::LfsrNoise lfsr(state.lfsr);

  // Law K (#57/#65/#166): the D=1 family (kernel_map §5 g=0,2,4 traversal)
  // reads the osc1/sub tables at NINE phases — osc1 + the 8 detune slots —
  // with RAW nearest-index reads (no interpolation on the taps; the lerp
  // survives only on osc2). Gate = 0.0 < detune (dispatcher :37110 bVar4).
  const bool detuned = (0.0f < p.detune) && (det_inc != nullptr);
  std::uint32_t dph[8];
  if (detuned) {
    for (int j = 0; j < 8; ++j) dph[j] = state.det_phase[j];
  }
  // Raw table read at the truncated index (FUN_100469c0: *(float*)(tab +
  // ((ph >> 16) & 0x7ff)*4) — no interp term).
  auto raw = [](const float* tab, std::uint32_t ph) {
    return tab[(ph >> 16) & 0x7ffu];
  };
  // Pulse tap: saw-difference at the INDEX (FUN_10045ca0: T[idx] −
  // T[(idx + dutyIdx) & 0x7ff], raw reads).
  auto raw_pulse = [&](const float* tab, std::uint32_t ph) {
    const std::uint32_t i = (ph >> 16) & 0x7ffu;
    return tab[i] - tab[(i + duty_idx) & 0x7ffu];
  };

  for (int i = 0; i < num_samples; ++i) {
    // spec_osc.md §6.1: osc2 term first, osc1 term second, sub added third,
    // then the phase accumulators advance — sample n uses the phase left by
    // sample n−1.
    float v2 = noise ? lfsr.next()
                     : (osc2_table ? tables::read(osc2_table, ph2) : 0.0f);
    if (pulse2 && osc2_table) {
      // §6.2 / FUN_180051db0: osc2 pulse = saw(ph2) − saw(ph2 + dutyIdx).
      // M2 law 3 (reports/osc2_pulse_gain.md, gain_ledger.md row 9): the
      // ×0.447 RMS-norm here was UNCITED — the kernel's only multiply on the
      // osc2 term is the gain slot (FUN_180051db0 :50597, :50616-50619);
      // the pulse compensation is the dispatcher ×0.5 (DAT_180072b60).
      // Norm removed from every path it touched (FM-on included).
      v2 -= tables::read(osc2_table, ph2 + duty);
    }
    const float v2_pre = v2;  // pre-FM-scaling pulse/noise sample (comp off)
    // m3-ring-fm-land (T2 law, tasks/consults/m3-triage-T1-T6.answer.md §T2):
    // the ring kernel family (g=4,5) NEVER evaluates FM — no 0.65/0.5
    // FM-kernel osc2 scaling inside a ring kernel. 32-bit twins
    // FUN_10043ce0/10043900/10044130 of FUN_180044510/44810/44b90
    // (dispatcher branch decomp_all.c:37494-37504 ↔
    // decomp_all_32_full.c:38069-38077): the only osc2-term multiply is the
    // fixed noise-sum constant DAT_100635e0 = 0.3 (mulss 0x10043e66,
    // 0x10043a9a, 0x100442c2; .rdata value 0.30000001) — DAT_180077210
    // (0.65) appears nowhere in the family. The dispatcher's osc2 comp gate
    // (decomp_all.c:37110, !FM — apply_amplitude_compensation above) still
    // skips the ×0.65/×0.5 gain comp while FM is active, so with ring+FM
    // the device's osc2 term carries NO 0.65/0.5 scaling at all.
    if (fm_on && !p.ring) {
      // FM-kernel modulator amplitude (FUN_18004ade0 noise: the LFSR sample
      // is ±0.65 — DAT_180077418/DAT_180077210 — not ±1; FUN_18004b180/
      // FUN_18004bc10 pulse: the saw difference is ×0.5 — DAT_180072b60).
      // The dispatcher's osc2 pulse ×0.5 / noise ×0.65 compensation is
      // skipped while FM is on (decomp_all.c:37110 bVar3), so the scaling
      // lives in the kernel and feeds BOTH the audible osc2 term and the
      // phase-modulation term (FUN_18004b180 :46566/:46582).
      if (noise) v2 *= 0.65f;
      else if (pulse2) v2 *= 0.5f;
    }
    float v1 = osc1_table ? tables::read(osc1_table, ph1) : 0.0f;
    if (pulse && osc1_table) {
      // §6.2: pulse(p) = saw(p) − saw(p + dutyIdx), same interp fraction:
      // the second read is the same phase with the index window shifted.
      v1 -= tables::read(osc1_table, ph1 + duty);
    }
    if (detuned && osc1_table) {
      // 9-tap osc1 sum, addition order mirrored from the bytes:
      // non-pulse FUN_100469c0: det0 + osc1 + det1..det7;
      // pulse FUN_10045ca0: (osc1 pair) + (det0 pair) + .. + (det7 pair).
      v1 = pulse ? raw_pulse(osc1_table, ph1)
                 : (raw(osc1_table, dph[0]) + raw(osc1_table, ph1));
      for (int j = (pulse ? 0 : 1); j < 8; ++j) {
        v1 += pulse ? raw_pulse(osc1_table, dph[j]) : raw(osc1_table, dph[j]);
      }
    }

    static const bool osc_comp = std::getenv("SYNTH1_OSC_COMP") != nullptr;  // #235
    if (osc_comp) {
      static thread_local double saw_ss = 0, pulse_ss = 0, pulse_peak = 0;
      static thread_local long long n = 0, sidx = 0, high = 0;
      static thread_local bool done = false;
      const long long lo = 13230, hi = 17640;  // 0.3-0.4s @ 44100
      if (sidx >= lo && sidx < hi) {
        saw_ss += (double)(v1 * g1_pre) * (v1 * g1_pre);
        pulse_ss += (double)v2_pre * v2_pre;
        if (std::fabs(v2_pre) > pulse_peak) pulse_peak = std::fabs(v2_pre);
        if (v2_pre > 0.0f) ++high;
        ++n;
      }
      ++sidx;
      if (sidx == hi && !done && n > 0) {
        done = true;
        const double s = std::sqrt(saw_ss / n), p2 = std::sqrt(pulse_ss / n);
        std::fprintf(stderr, "[osccomp] saw %.4f pulse %.4f pulse_peak %.4f high_frac %.3f duty_idx %u pw %.4f pwmod %d | total %.4f\n",
                     s, p2, pulse_peak, (double)high / n, duty_idx, p.pulse_width, p.pw_mod, std::sqrt(s * s + p2 * p2));
      }
    }

    float out;
    if (p.ring) {
      // §6.6: out = v2·g2·v1 + g1·v1 (multiplicative osc2 term).
      // M2b law 20 (#20): the leading ×0.5 here was UNCITED — the ring
      // kernel FUN_18004f0e0 @48936 computes
      // (osc2_interp · g2 · osc1_interp) + (g1 · osc1_interp) with no
      // 0.5 factor (reports/m2b_evidence.md §2 kernel comparison; the
      // engine differed from the device ONLY by this 0.5f).
      // M2b #117: the six NO-DETUNE osc1=pulse ring kernels multiply the
      // carrier INSIDE the kernel by DAT_180072b60 (0.5): sync=1
      // FUN_180044810 :42556-42565, FUN_180044510 :42431-42434/:42444,
      // FUN_180044b90 :42683-42689; sync=0 FUN_18004e4e0 :48494-48503,
      // FUN_18004e1f0 :48375/:48385, FUN_18004e830 :48614-48620 —
      // fVar19 = (saw−saw)·fVar8, out = osc2·g2slot·fVar19 +
      // g1slot·fVar19. The dispatcher gate `osc1==2 && ring==0`
      // (:36986-36987) is COMPLEMENTARY (apply_amplitude_compensation
      // above), so the engine needs the 0.5 on the ring carrier iff
      // osc1_wave==2. Detuned pulse-ring kernels FUN_18004c840/4c2e0/
      // 4ce20 (:37253-37285 branch) carry NO DAT_180072b60 — gate on
      // detune == 0 (M3/#57 family keeps the un-compensated carrier).
      // Byte-order mirror: the device scales the carrier BEFORE both
      // products, so scale v1, not the sum.
      float v1r;
      if (detuned) {
        // Detuned ring kernels scale the 9-tap carrier INSIDE the kernel:
        // non-pulse FUN_10043ce0 × DAT_100635e0 (0.3), pulse FUN_10042f10 ×
        // DAT_100637f4 (0.15 = 0.5·0.3). The dispatcher's ×0.3 detune gain
        // is gated !ring (apply_amplitude_compensation), so this replaces it.
        v1r = v1 * (pulse ? 0.15f : 0.3f);
      } else {
        v1r = (pulse && !(0.0f < p.detune)) ? v1 * 0.5f : v1;
      }
      out = v2 * p.osc2_gain * v1r + p.osc1_gain * v1r;
    } else {
      out = v2 * p.osc2_gain + v1 * p.osc1_gain;
    }

    // §6.4: sub oscillator at the same osc1 phase, or phase >> 1 / >> 2 for
    // the −1 / −2 octave modes. Added after the osc mix, unchanged by ring.
    // D=1 (FUN_100469c0/10045ca0/10042f10/10043ce0): 9 RAW reads at the 9
    // phases, order det0 + osc1 + det1..det7, mode 2 = (ph >> 17) & 0x7ff.
    if (detuned && sub_table) {
      float s = 0.0f;
      if (p.sub_oct == 2) {
        s = raw(sub_table, dph[0] >> 1) + raw(sub_table, ph1 >> 1);
        for (int j = 1; j < 8; ++j) s += raw(sub_table, dph[j] >> 1);
      } else {
        s = raw(sub_table, dph[0]) + raw(sub_table, ph1);
        for (int j = 1; j < 8; ++j) s += raw(sub_table, dph[j]);
      }
      out += s * p.sub_gain;
    } else {
      switch (p.sub_oct) {
        case 1: out += tables::read(sub_table, ph1) * p.sub_gain; break;
        case 2: out += tables::read(sub_table, ph1 >> 1) * p.sub_gain; break;
        case 3: out += tables::read(sub_table, ph1 >> 2) * p.sub_gain; break;
        default: break;
      }
    }
    output[i] = out;

    // §6.7 FM: osc2's raw waveform phase-modulates osc1 inside the phase
    // accumulator. m3-fmorder-land device order (FUN_10041940
    // 0x10041b36-0x10041b95; the same window audited in all 12 FM kernels —
    // tasks/m3-fmorder-land/RESULT.md F1): fm_inc is computed from the
    // CURRENT depth and converted with x87 fistp (fld 0x10041b47 / fistp
    // 0x10041b4b = round-to-nearest, ties-to-even), the phase advances
    // (0x10041b57), and ONLY THEN the COSRAMP re-interp runs: (rem & 0x1f)
    // == 0 test (0x10041b68), idiv/cosramp/cvttsd2si (0x10041b79-0x10041b90,
    // truncate), rem-- (0x10041b94) — while the per-call ramp-active flag
    // holds. m3-fmsnap-comb2 (THE LAW, tasks/consults/m3-fmkernel-tap.answer.md
    // §2/§3): a device call NEVER runs a sample with rem == 0 inside the loop
    // — the caller clamps the call length to rem (FUN_10034740 :37925-37926)
    // — and the ramp ENDS at the call exit: 0x10041bdc test edi,edi;
    // cmove edx,[esp+0x64] (decomp :46233 local_e4 = local_dc, stored at
    // 0x10041bf2 / :46236) puts the TARGET into the depth store. So the
    // engine snaps depth = fm_target the instant --rem lands on 0 and the
    // cosramp lookup is guarded rem > 0: no in-loop rem == 0 lookup, no
    // negative-rem re-hit (the unguarded (rem & 0x1f) == 0 test at
    // rem = -32, -64, ... dragged depth back to start — the device never
    // gets there). The earlier "rem keeps decrementing after that (no
    // rem > 0 guard, no separate snap branch)" comment audited only the
    // 0x10041b36-0x10041b95 window; the cmove at 0x10041bdc is the snap
    // branch, added here. The old engine re-interp'd
    // first and truncated fm_inc — a one-sample-late depth trajectory
    // (m3-fmkernel-k1 F4: first diff abs 1479, -8.57 dB).
    std::int32_t fm_inc = 0;
    // m3-ring-fm-land (T2 law): in the ring kernel family the osc1 phase
    // advances by inc1 ONLY — no fm_inc term — and the per-sample FM ramp
    // window (fm_inc compute + cosramp re-interp + rem--, FUN_10041940
    // 0x10041b36-0x10041b95, duplicated in the 12 FM kernels) is ABSENT
    // from the ring kernels. Machine-verified in install/Synth1 VST.dll:
    // the osc1 accumulator advances by exactly ONE add of the
    // loop-invariant struct[3] = inc1 (43-dword voice struct copied by
    // rep movsl 0x10043d13/0x10043933/0x10044163): FUN_10043ce0
    // add 0xcc(%esp),%eax @ 0x10044043, FUN_10043900 add 0xac(%esp),%esi
    // @ 0x10043c22, FUN_10044130 add 0x8c(%esp),%edi @ 0x1004446f; the 8
    // detune slots advance by paddd of constant detune-increment vectors
    // (0x10044051/0x1004405a), never by fm_inc. The per-call FM ramp state
    // machine (FUN_18003bf50, above the sample loop) keeps running
    // regardless — its state must stay live for when ring is switched off.
    if (fm_on && !p.ring) {
      const float fmp = (float)fm.depth * v2;  // mulss 0x10041b3d (float)
      const float fmf = ::floorf(fmp);         // fistp = RNE, explicit
      const float fmr = fmp - fmf;             // (adsr.h #95a idiom)
      fm_inc = (std::int32_t)fmf +
               (fmr > 0.5f || (fmr == 0.5f && (((std::int32_t)fmf) & 1)));
      if (fm_ramp_active) {
        if (fm.rem > 0 && (fm.rem & 0x1f) == 0) {
          const int idx = (fm.rem << 8) / fm.total;
          const double c = (idx >= 0 && idx < 256) ? cosramp()[idx] : 0.0;
          fm.depth = (int)((double)(fm_target - fm.start) * c) + fm.start;
        }
        --fm.rem;
        if (fm.rem == 0) {
          fm.depth = fm_target;  // 0x10041bdc cmove snap
          // rule B: the call was cut at rem (0x10034d44); FUN_10034610
          // re-calls FUN_10034740 for the remainder, which adds it to
          // +0xa8 again (call_left-1 samples left in the cut call).
          if (fm.call_left > 1 && fm.block_cur < ramp_max)
            fm.block_cur = std::min(fm.block_cur + (fm.call_left - 1), ramp_max);
        }
      }
    }
    // §6.5 hard sync. Non-noise osc2 (pulse/saw/tri, incl. FM): proportional
    // restart from the pre-increment ph1 when osc1's 27-bit window wraps this
    // sample — FUN_1800486c0/48390/402f0: slavePhase = (ph1 & 0x7ffffff)*inc2 /
    // inc1 (the divide is the giveaway; the denominator is inc1, not inc1+fm_inc).
    // The 32-bit authority forms the PRODUCT IN 32 BITS (mod 2^32): FUN_1003c4d0
    // 0x1003c6cc two-operand imul (high half discarded) + 0x1003c6d5 divl, echoed
    // in every sync kernel both images (see m3-triage-242). The 64-bit decomp text
    // promotes to ulonglong, but the emitted bytes truncate — the bytes are authority.
    // Noise osc2 (FUN_180047760/3e8f0): the osc2 accumulator is NEVER advanced
    // and hard sync re-seeds the LFSR to 10 (cv+0x134, *puVar3 = 10) on the
    // master wrap instead of restarting a phase — noise is not phase-locked.
    const std::uint32_t ph1n = ph1 + (std::uint32_t)(inc1 + fm_inc);
    if (detuned) {
      // FUN_10040430 :45505-45513: the FM increment is added to ALL nine
      // osc1-side accumulators (the whole detuned complex transposes with
      // the modulator), each slot advancing by fm_inc + its own detune inc.
      for (int j = 0; j < 8; ++j) {
        dph[j] += (std::uint32_t)(fm_inc + det_inc[j]);
      }
    }
    const bool master_wrap = (ph1n & 0x7ffffffu) < (ph1 & 0x7ffffffu);
    if (noise) {
      // Per-kernel re-seed matrix (audited 12 re-seed sites in decomp_all.c:
      // 3e8f0,3fd70,40eb0,41a20,424d0,43660,44510,44ec0,45790,46920,47760,480f0
      // — all in the sync!=0 dispatcher branch, cv+0x118). The sync==0 noise
      // kernels (4ade0 = 126 FM noise, 51230 = FM-off noise) have NO re-seed
      // and NO 0x7ffffff wrap test at all. So re-seed iff osc2 sync is on:
      // inside a sync noise kernel the re-seed is unconditional per wrap.
      if (p.sync && master_wrap) lfsr.s = 10u;  // *puVar = 10
    } else if (p.sync && master_wrap) {
      ph2 = ((std::uint32_t)(ph1 & 0x7ffffffu) * (std::uint32_t)inc2) /
            (std::uint32_t)inc1;
    } else {
      ph2 += (std::uint32_t)inc2;
    }
    ph1 = ph1n;
    if (fm.call_left > 0) --fm.call_left;  // rule B: count the call down per sample
  }

  state.osc1_phase = ph1;
  state.osc2_phase = ph2;
  if (detuned) {
    for (int j = 0; j < 8; ++j) state.det_phase[j] = dph[j];  // cv+0xcc[0..7]
  }
  state.lfsr = lfsr.s;
}

}  // namespace synth1::osc
