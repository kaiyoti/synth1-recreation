// filter.cpp — CVcf filter stage implementation.
//
// Spec: spec_filter.md §1.1/§1.2/§1.3/§4/§5, spec_lpdlosc.md §1–§4,
//       fc_lockin.md §1. Ghidra: FUN_18003ad50 / FUN_180038300 /
//       FUN_1800390b0 / FUN_1800398b0 / FUN_18003a920 / FUN_18003af70 /
//       FUN_1800381f0.

#include "filter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(__SSE__)
#include <xmmintrin.h>  // m3-sse-lanes-land: _mm_rcp_ps (device rcpps form)
#endif

namespace synth1::filter {

namespace {
// DAT_180072b88 = π (double)
constexpr double PI = 3.1415926535897931;
// DAT_180077238 = 1.386249 (double) — the binary's ln2 approximation
constexpr double LN2_BIN = 1.386249;
// DAT_18007728c = 1066.66663f (table span); FUN_1800381f0 builds
// T[i] = 15.0 * 1066.66667^(i/1024)
constexpr double FC_SPAN = 1066.66667;
}  // namespace

// ---------------------------------------------------------------------------
// Cutoff table (FUN_1800381f0): T[i] = 15.0 * 1066.66667^(i/1024),
// i = 0..1023 → 15.0 Hz .. 15891.43 Hz, per-index ratio 1.006832114.
// Built once on first use; no allocation (static C array).
// ---------------------------------------------------------------------------
const float* fc_table() {
  static float tbl[spec::FC_TABLE_SIZE];
  static bool built = false;
  if (!built) {
    for (std::uint32_t i = 0; i < spec::FC_TABLE_SIZE; ++i) {
      tbl[i] = (float)(15.0 * std::pow(FC_SPAN, (double)i / 1024.0));
    }
    built = true;
  }
  return tbl;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
void Filter::prepare(float sample_rate) {
  fs = (sample_rate < 1.0f) ? 44100 : (int)sample_rate;
  slew_interval = fs / 20;              // +0x9c interval (50 ms), int div
  if (slew_interval < 1) slew_interval = 1;
  lpdl_sub = (fs <= (int)spec::LPDL_FS_THRESHOLD) ? 2 : 1;  // spec_lpdlosc §1
  // LP24 per-sample coefficient slew: the device stores a RETENTION at
  // CVcf+0x120 (32-bit; +0x138 64-bit) and the core updates
  // live += (target - live)·(1 - ret) with the step computed in float
  // (bytes 0x10032c38 subss xmm5,[rsi+0x120]; update 0x10032da5-0x10032dd8,
  // decompile @36593-36595). Prepare law, bytes 0x100336fc-0x10033732
  // (@36824-36826): cvtdq2pd FS, mulsd DAT_10063738 (0.001, double),
  // divsd 1.0/…, double pow base DAT_10066070 (0.02) via FUN_1004ce0a,
  // cvtsd2ss -> float store. So ret = 0.02^(1/(FS*0.001)): coefficients
  // close 98% of the gap to the target in 1 ms (step 0.0848873 @44.1 k).
  // Runtime-pinned at FS 44100/48000: device +0x120 = 0.915112734 /
  // 0.921732247, formula diff <= 2.1e-8 (reports/123_selfosc.md §5).
  // The previous 1 - exp(-1000/FS) was a convergence-argument guess
  // (2% in 3.89 ms, 3.78x slower than the device); spec_filter.md §6's
  // "exp(1000/FS)" sign-typo debate is moot — the bytes pin the law.
  const float ret = (float)std::pow(0.02, 1.0 / ((double)fs * 0.001));
  slew_c = 1.0f - ret;
  // +0xe0 leveler retention (FUN_18003a850 @0x18003a8b0): expf(DAT_180072dc8
  // (−1.0f) / (FS · DAT_1800771f8 (0.005f))) — a 5 ms time constant. The
  // leveler runs env = (env − v)·c + v; leveler_coef is the step 1 − c.
  leveler_coef = 1.0f - std::exp(-1.0f / ((float)fs * 0.005f));
  dbg = std::getenv("SYNTH1_STAGE_DEBUG") != nullptr;
  dbg_in = dbg_out = dbg_env = 0.0f; dbg_gain_min = 1.0f; dbg_clamp = 0;
  reset();
}

Filter::~Filter() {
  if (dbg)
    std::fprintf(stderr, "[stage] oscmix_in_peak %.4f filt_out_peak %.4f gain_min %.4f clamp %d | L0t %.4f L1t(g) %.4f L2t %.4f fc %.1f | i0 %d i_key %d i_env %d i_cur %d\n",
                 dbg_x, dbg_out, dbg_gain_min, dbg_clamp, dbg_L0, dbg_L1, dbg_L2, dbg_fc,
                 dbg_i0, dbg_i_key, dbg_i_env, dbg_i_cur);
}

void Filter::reset() {
  i_cur = i_start = i0;
  slew_n = slew_interval;               // +0x9c init FS/20
  cache_idx = -1;
  cache_res = -1.0f;
  g = A = rmin = K = 0.0f;
  s1 = s3 = 0.0f;
  L0t = L1t = L2t = L0 = L1 = L2 = 0.0f;
  q0 = q1 = q2 = q3 = 0.0f;
  aP = bP = cP = dP = 0.0f;
  lp24_live_init = false;
  kk = a1 = p2 = p3 = p4 = 0.0;
  w2 = w3 = g4 = g5 = 0.0;
  c2 = c3 = c4 = 0.0;
  u1 = rprime = m = gq = nout = 0.0;
  m4 = ls1 = ls2 = ls3 = m2 = 0.0;
  leveler_env = 0.0f;
}

// ---------------------------------------------------------------------------
// parameter setters
// ---------------------------------------------------------------------------
void Filter::set_env_level(float level01) {
  env_level = (level01 < 0.0f) ? 0.0f : (level01 > 1.0f ? 1.0f : level01);
  recompute_env();
}

// spec_filter.md §5 (FUN_180038070): for env_amt > 0,
//   d = pow(env_amt + 1, (fs_kHz − 48)/12);
//   i_env = clamp(int(1024·log(T[i0]·d/15)/log(1066.66663)), 0, 1024) − i0.
// Portable mapping: fs_kHz = 48 + 12·env_level (see filter.h header).
void Filter::recompute_env() {
  if (env_amt > 0.0f) {
    const double fs_khz = 48.0 + 12.0 * (double)env_level;
    const double d = std::pow((double)env_amt + 1.0, (fs_khz - 48.0) / 12.0);
    const double hz = (double)fc_table()[i0];
    int idx = (int)(1024.0 * std::log(hz * d / 15.0) / std::log(1066.66663));
    if (idx < 0) idx = 0;
    if (idx > 1024) idx = 1024;
    i_env = idx - i0;
  } else {
    i_env = 0;
  }
  retrigger_slew();
}

// FUN_180038070 — keyboard tracking (called with the key number at note-on
// via FUN_18003a780 and on param 22 via FUN_18003b720):
//   x     = pow(amt + 1, (key − 48)·(1/12))          DAT_180077200 = 1/12
//   idx   = clamp((int)(1024·logf(T[i0]·x/15)/logf(1066.66663)), 0, 1024)
//   i_key = idx − i0                                  (CVcf+0xac)
void Filter::recompute_key() {
  if (keytrack_amt > 0.0f) {
    const double x = std::pow((double)(keytrack_amt + 1.0f),
                              (double)((float)(note - 48) * (1.0f / 12.0f)));
    const float hz = fc_table()[i0];
    const float num = std::log((float)((double)hz * x) * (1.0f / 15.0f)) * 1024.0f;
    int idx = (int)(num / std::log(1066.66663f));
    if (idx < 0) idx = 0;
    if (idx > 1024) idx = 1024;
    i_key = idx - i0;
  } else {
    i_key = 0;
  }
  // FUN_180038070 does not touch +0x9c: the keytrack recompute at note-on
  // must not restart the 50 ms cutoff slew (only the param setters do).
}

// param 23 curve (spec_filter.md §4, set @0x1800298fb):
//   sat = (p/127)·exp(5.8·(p/127 − 1))
void Filter::set_saturation_param(int p) {
  if (p < 0) p = 0;
  if (p > 127) p = 127;
  const double x = (double)p / 127.0;
  sat = (float)(x * std::exp(5.8 * (x - 1.0)));
}

// ---------------------------------------------------------------------------
// cutoff index composition + mirror-fold + clamp + slew
// (spec_filter.md §dispatcher / §5, FUN_18003ad50)
// ---------------------------------------------------------------------------
void Filter::update_cutoff() {
  int t = i0 + i_key + i_env + i_vel + i_spread + extra_idx;
  const int mm = mirror_a + mirror_b;
  int mirror = (mm > 0) ? (1024 - t) : t;
  const int p = mirror * mm;
  t = t + ((p + ((p >> 31) & 0x3ff)) >> 10);   // mirror-fold (observed algebra)
  if (t > 1023) t = 1023;
  if (t < 0) t = 0;
  if (t == i_cur || slew_n >= slew_interval) {
    i_cur = t;                                  // snap
  } else {                                      // linear 50 ms slew
    i_cur = (int)(((long long)(t - i_start) * slew_n) / slew_interval) + i_start;
  }
  ++slew_n;
  if (dbg) { dbg_i0 = i0; dbg_i_key = i_key; dbg_i_env = i_env; dbg_i_vel = i_vel; dbg_i_cur = i_cur; }
}

// ---------------------------------------------------------------------------
// coefficient cache (spec_filter.md §7: recompute on index/res change)
// ---------------------------------------------------------------------------
void Filter::update_coeffs() {
  if (cache_idx == i_cur && cache_res == res) return;
  cache_idx = i_cur;
  cache_res = res;
  switch (ftype) {
    case FTYPE_LP12: case FTYPE_HP12: case FTYPE_HP24: setup_lp12(); break;
    case FTYPE_LP24: setup_lp24(); break;
    case FTYPE_LPDL: setup_lpdl(); break;
    default: setup_lp24(); break;
  }
}

// ---------------------------------------------------------------------------
// LP12 / HP12 / HP24 (FUN_180038300, spec_filter.md §1.1)
// ---------------------------------------------------------------------------
void Filter::setup_lp12() {
  const double hz = (double)fc_table()[i_cur];
  g = (float)(2.0 * std::sin(PI * hz / (2.0 * (double)fs)));
  rmin = (res < 1.0f) ? res : 1.0f;
  A = (float)(1.0 - 0.7 * (double)rmin);
  K = rmin - 1.0f;
}

float Filter::run_lp12hp(float x) {
  const float u = A * x;
  s1 += g * s3;
  s3 += g * ((u - s1) + K * s3);
  s1 += g * s3;
  // F1 type-2 law (m3-f1-filtertype-law.answer.md): FUN_10032370 type==2,
  // decomp_all_32_full.c:36403-36405 (DAT_10082ac8==0) and :36430-36432 —
  // h = (A·u−s1)+K·s3 is the HP12 output tap, evaluated BEFORE the second
  // s3 update; the update itself is s3 += g·h (same arithmetic as the
  // previous post-expression form, so LP12/HP24 state and taps unchanged).
  const float u2 = (u - s1) + K * s3;
  s3 += g * u2;
  float y;
  switch (ftype) {
    case FTYPE_LP12: y = s1; break;
    case FTYPE_HP12: y = u2; break;
    default:         y = s3; break;   // HP24
  }
  if (res > CLAMP_RES_TRIGGER && std::fabs(y) > CLAMP_HI)
    y = (y > 0.0f) ? CLAMP_HI : CLAMP_LO;
  return y;
}

// ---------------------------------------------------------------------------
// LP24 (FUN_1800390b0, spec_filter.md §1.2 + §6)
// ---------------------------------------------------------------------------
void Filter::setup_lp24() {
  const float fc = (float)((double)fc_table()[i_cur] * 2.0 / (double)fs);
  const double fcd = (double)fc;
  L0t = (float)(-1.6 * fcd * fcd + 3.6 * fcd - 1.0);
  L1t = (float)((1.0 + (double)L0t) * 0.5);
  // M2 law 2 (m2-filter-audit-2a): byte-confirmed device law, raw bytes
  // 0x180039135-0x1800391f6 (reports/filter_resonance_audit_2a.md item 1):
  // L2t = res * exp(DAT_180077238(1.386249) *
  //                 (1 - DAT_180072e98(0.5) * (DAT_180077258(4.3)*fc
  //                                            - DAT_180075710(2.5f)*fc*fc)))
  // The previous form 1-(4.3fc-1.6fc^2) mis-transcribed the L0t constant
  // DAT_180077240(1.6) into the fc^2 term and omitted the 0.5 factor.
  L2t = res * (float)std::exp(LN2_BIN * (1.0 - 0.5 * (4.3 * fcd - 2.5 * fcd * fcd)));
  if (dbg) { dbg_L0 = L0t; dbg_L1 = L1t; dbg_L2 = L2t; dbg_fc = fc; }
  if (!lp24_live_init) {   // cache-invalidated first use: live = targets
    L0 = L0t; L1 = L1t; L2 = L2t;
    lp24_live_init = true;
  }
}

float Filter::run_lp24(float x) {
  const float a = x - q3 * L2;
  const float b = (a + aP) * L1 - q0 * L0;
  const float c = (b + bP) * L1 - q1 * L0;
  const float d = (c + cP) * L1 - q2 * L0;
  float e = (d + dP) * L1 - q3 * L0;
  // M2b law 71 (#71): the LP24 core's only nonlinearity is the final-output
  // ±20 limiter, gated on res > 0.9 in BOTH DLL images. 32-bit FUN_10032a90
  // gate bytes 0x10032c82-0x10032ca4: cvtps2pd + comisd [0x10064cb8] + seta
  // (@0x10032c8b) — DAT_10064cb8 bytes cd cc cc cc cc cc ec 3f = DOUBLE 0.9,
  // so bVar2 = res > 0.9 (the old "DAT_10064cb8 = -1.07e8, always true" was
  // a misread of the double's LOW DWORD 0xcccccccd as a float; same error in
  // reports/cvcf_leveler_law.md §(c)). 64-bit FUN_1800390b0: DAT_1800756d0 =
  // same double 0.9 (@35425). Clamp site bytes 0x10032d40-0x10032d9e
  // (@36587-36591): DAT_10064528=+20.0f / DAT_10063790=−20.0f, applied to the
  // final stage e only, clamped e fed back into both taps (q3). Gate form
  // matches the LP12/HP12/HP24 site above (reports/123_selfosc.md §1).
  if (res > CLAMP_RES_TRIGGER && std::fabs(e) > CLAMP_HI * domain) {
    e = (e > 0.0f) ? CLAMP_HI * domain : CLAMP_LO * domain;
    if (dbg) ++dbg_clamp;
  }
  L0 += (L0t - L0) * slew_c;
  L1 += (L1t - L1) * slew_c;
  L2 += (L2t - L2) * slew_c;
  aP = a; bP = b; cP = c; dP = d;
  q0 = b; q1 = c; q2 = d; q3 = e;
  static const bool lp24_rms = std::getenv("SYNTH1_LP24_RMS") != nullptr;  // #235
  if (lp24_rms) {
    static thread_local double sx=0,sb=0,sc=0,sd=0,se=0;
    static thread_local long long n=0, sidx=0;
    static thread_local bool done=false;
    const long long lo=13230, hi=17640;
    if (sidx>=lo && sidx<hi) {
      sx+=(double)x*x; sb+=(double)b*b; sc+=(double)c*c; sd+=(double)d*d; se+=(double)e*e; ++n;
    }
    ++sidx;
    if (sidx==hi && !done && n>0) {
      done=true;
      std::fprintf(stderr,"[lp24] x %.4f y1 %.4f y2 %.4f y3 %.4f y4 %.4f | L0 %.4f L1 %.4f L2 %.4f\n",
                   std::sqrt(sx/n),std::sqrt(sb/n),std::sqrt(sc/n),std::sqrt(sd/n),std::sqrt(se/n),L0,L1,L2);
    }
  }
  return e;
}

// ---------------------------------------------------------------------------
// LPDL (FUN_1800398b0, spec_filter.md §1.3 + spec_lpdlosc.md §3/§4)
// ---------------------------------------------------------------------------
void Filter::setup_lpdl() {
  const double hz = (double)fc_table()[i_cur];
  const double fc = (1.5 * hz < 0.45 * (double)fs) ? 1.5 * hz : 0.45 * (double)fs;
  // F1 type-4 law (m3-f1-filtertype-law.answer.md): FUN_10032e70
  // decomp_all_32_full.c:36672-36673 — the main prewarp is
  // T = tan(π·fc/(FS·sub)); FUN_1005003d is the CRT tan (fptan, decomp
  // :66531) and its XMM0 return IS used (advisor objdump 0x10032f13;
  // Ghidra drops the return value).
  const double T = std::tan(PI * fc / ((double)fs * (double)lpdl_sub));
  kk = T / (2.0 * (1.0 + T));
  const double r = 15.0 * (double)res;
  a1 = 1.0 / (1.0 + T);
  const double k2 = kk * kk;
  const double A2 = 1.0 / (1.0 - k2);
  p2 = A2 * kk;
  w2 = k2 / (1.0 - k2);
  const double d2 = (1.0 - k2) / (1.0 - 2.0 * k2);
  p3 = d2 * kk;
  const double A3 = (1.0 - 2.0 * k2) / (1.0 - 3.0 * k2 + k2 * k2);
  p4 = 2.0 * kk * A3;
  c2 = A2 * a1;
  c3 = d2 * a1;
  c4 = A3 * a1;
  w3 = w2 * p3;
  g4 = 2.0 * kk / (1.0 - 2.0 * kk * p3);
  // m2 path (F1 law): FUN_10032e70 :36692-36705 — f_eff = 30 (fc<100) |
  // exp(ln30 + ln4·ln(fc/100)/ln3) (100≤fc<300: five CRT log calls
  // FUN_1004beeb on 100/30/fc/300/120 + exp FUN_1004ca9d — a LOG-LOG
  // interpolation, constants DAT_10064d08=30, DAT_10064d20=100,
  // DAT_10064d38=300, DAT_100660d0=120) | 120 (fc≥300).
  // :36706-36707: x' = tan(π·f_eff/(FS·sub)) (same tan call), then
  // u1 = 1/(1+x'), g5 = 2x'/(1+x') (dVar11·dVar16 in the loop).
  double f_eff;
  if (fc < 100.0) {
    f_eff = 30.0;
  } else if (fc < 300.0) {
    f_eff = std::exp(std::log(30.0) + std::log(4.0) * std::log(fc / 100.0) / std::log(3.0));
  } else {
    f_eff = 120.0;
  }
  const double xp = std::tan(PI * f_eff / ((double)fs * (double)lpdl_sub));
  u1 = 1.0 / (1.0 + xp);
  g5 = 2.0 * xp * u1;
  rprime = r * u1;
  m = 1.0 / (1.0 + rprime * w3 * g4);
  gq = T / (1.0 + T);
  nout = (double)lpdl_sub * 0.65 * (1.0 + 3.0 * (double)res);
}

float Filter::run_lpdl(float x) {
  const double xin0 = (double)x;
  double y = 0.0f;
  for (int j = 0; j < lpdl_sub; ++j) {
    const double xin = (j == 0) ? xin0 : 0.0;   // zero-stuffed input
    double a = ls3 * a1;
    double b = ls2 * c2 + a * p2;
    double c = ls1 * c3 + b * p3;
    double d = m4 * c4 + c * p4;
    const double fb = rprime * (a + b * kk + c * w2 + d * w3);
    const double e = (xin - fb + m2 * u1) * m;
    const double s = tanh3(e);
    d = LADDER_GAIN_97 * s * g4 + d;
    c = d * p3 + c;
    b = c * p2 + b;
    a = b * kk + a;
    ls1 = ((b + d) * 0.5 - ls1) * gq + c;
    ls2 = ((a + c) * 0.5 - ls2) * gq + b;
    ls3 = (b * 0.5 - ls3) * gq + a;
    m2 = m2 + (a * 15.0 * (double)res - m2) * g5;
    m4 = ((c + LADDER_GAIN_97 * s) - m4) * gq + d;
    y = b * nout;                                // last-sample decimation
  }
  return (float)y;
}

// ---------------------------------------------------------------------------
// per-sample chain: cutoff slew → coefficient cache → core → soft-clip →
// leveler (spec_filter.md chain order).
// ---------------------------------------------------------------------------
float Filter::process_sample(float x) {
  update_cutoff();
  update_coeffs();
  if (dbg && std::fabs(x) > dbg_x) dbg_x = std::fabs(x);

  float y;
  switch (ftype) {
    case FTYPE_LP12: case FTYPE_HP12: case FTYPE_HP24: y = run_lp12hp(x); break;
    case FTYPE_LP24: y = run_lp24(x); break;
    case FTYPE_LPDL: y = run_lpdl(x); break;
    default:         y = run_lp24(x); break;
  }

  // FUN_18003a920: the leveler FUN_18003af70 runs FIRST and always (it is a
  // no-op until |x| exceeds 1.1: env stays 0 and the gain is 1.1/1.1) ...
  {
    const float thr = leveler_target * domain;  // DAT_180073c1c in the CVcf signal domain
    float v1 = std::fabs(y) - thr;
    if (v1 < 0.0f) v1 = 0.0f;
    // env = (env − v1)·c + v1 with c = 1 − leveler_coef (retention form)
    leveler_env += (v1 - leveler_env) * leveler_coef;
    const float lgain = thr / (leveler_env + thr);
    y = lgain * y;
    if (dbg) {
      if (std::fabs(y) / (lgain + 1e-12f) > dbg_in) dbg_in = std::fabs(y) / (lgain + 1e-12f);
      if (std::fabs(y) > dbg_out) dbg_out = std::fabs(y);
      if (leveler_env > dbg_env) dbg_env = leveler_env;
      if (lgain < dbg_gain_min) dbg_gain_min = lgain;
    }
  }
  // ... then the soft-clip, only with saturation on (+0xc4 != 0):
  // y = x·(1+a)/(1+a·|x|), a = 100·sat
  // m3-sse-lanes-land (ruling tasks/consults/m3-291-scratch.answer.md A):
  // device FUN_10033740 per-call forms (m3-sse-lane-scratch F2) — 4-lane
  // group (y/den)·k, rcp+Newton tail group, scalar tail.
  if (sat != 0.0f) {
    const float a = 100.0f * sat;
    const float k = a + 1.0f;
    const float den = std::fabs(y) * a + 1.0f;
    int lane_lo = 0, lane_hi = 0;
    if (sse_sc_len > 6) {
      lane_lo = (4 - (sse_sc_start_mod4 & 3)) & 3;
      lane_hi = lane_lo + 4 * ((sse_sc_len - lane_lo) / 4);
    }
    const int i = sse_sc_pos;
    if (i >= lane_lo && i < lane_hi) {
      y = (y / den) * k;
    } else {
      const int tail = sse_sc_len - lane_hi;
      const int n4 = (tail > 3) ? (((tail - 4) >> 2) + 1) : 0;
      if (i >= lane_hi && i - lane_hi < 4 * n4) {
#if defined(__SSE__)
        const float r = _mm_cvtss_f32(_mm_rcp_ps(_mm_set1_ps(den)));
        y = (((r + r) - ((r * r) * den)) * y) * k;
#else
        y = (y * k) / den;
#endif
      } else {
        y = (y * k) / den;
      }
    }
  }
  return y;
}

}  // namespace synth1::filter
