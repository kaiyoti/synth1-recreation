#include "synth_host.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "preset/preset.h"
#include "../../../../../plugin/boot_prog0.gen.h"   // frozen boot rows (read-only reference)

namespace synth1_android {

using synth1::events::Event;
using Engine = synth1::engine::Engine;

// ---------------------------------------------------------------------------
std::int64_t x87_frames(double value, double sr) {
#if LDBL_MANT_DIG == 64 && !defined(S1_FORCE_X87_EMU)
  return (std::int64_t)((long double)value * (long double)sr);
#else
  // Exact emulation of the x87 product: 53x53-bit mantissas -> 106-bit product, rounded to
  // nearest-even at 64 significant bits, then truncated toward zero.
  if (value <= 0.0 || sr <= 0.0) return (std::int64_t)(value * sr);
  int ea, eb;
  double fa = std::frexp(value, &ea), fb = std::frexp(sr, &eb);   // in [0.5,1)
  unsigned __int128 ma = (std::uint64_t)std::ldexp(fa, 53);
  unsigned __int128 mb = (std::uint64_t)std::ldexp(fb, 53);
  unsigned __int128 p = ma * mb;                 // value*sr = p * 2^(ea+eb-106)
  int e = ea + eb - 106;
  int bits = 0;
  for (unsigned __int128 t = p; t; t >>= 1) ++bits;
  if (bits > 64) {                               // round to 64 bits (nearest-even)
    int sh = bits - 64;
    unsigned __int128 rem = p & (((unsigned __int128)1 << sh) - 1);
    unsigned __int128 half = (unsigned __int128)1 << (sh - 1);
    p >>= sh; e += sh;
    if (rem > half || (rem == half && (p & 1))) ++p;
  }
  if (e >= 0) return (std::int64_t)(p << e);
  if (-e >= 128) return 0;
  return (std::int64_t)(p >> (-e));
#endif
}

// ---------------------------------------------------------------------------
SynthHost::SynthHost() { staged_.reserve(1536); }
SynthHost::~SynthHost() {
  delete next_.exchange(nullptr);
  delete retired_.exchange(nullptr);
}

static std::unique_ptr<Engine> make_engine(const std::vector<std::int32_t>& rows, int program,
                                           float sr, int bs, bool havePatch) {
  std::unique_ptr<Engine> e(new Engine());   // heap: Engine is ~2.3 MB
  e->set_boot_program(synth1::boot::kBootRows, synth1::boot::kBootRowCount);
  e->prepare(sr, bs);
  if (havePatch) {
    std::vector<Event> load{Event::ProgramLoad(0, rows.data(), (std::int32_t)rows.size() / 2,
                                               program)};
    e->apply_param_events(load);
  }
  return e;
}

static bool parse_rows(const std::string& text, std::vector<std::int32_t>& rows) {
  synth1::preset::PresetState st;
  std::string err;
  if (!synth1::preset::parse_sy1(text, st, &err)) return false;
  synth1::preset::upgrade_v105(st);
  rows.clear();
  for (const auto& p : st.pairs)
    if (p.id < synth1::preset::PARAM_SLOTS) {
      rows.push_back((std::int32_t)p.id);
      rows.push_back((std::int32_t)p.value);
    }
  return true;
}

bool SynthHost::load(const std::string& text, int program, float sr, int bs) {
  std::vector<std::int32_t> rows;
  bool have = !text.empty();
  if (have && !parse_rows(text, rows)) return false;
  delete retired_.exchange(nullptr);
  rows_ = rows;
  std::unique_ptr<Engine> e = make_engine(rows_, program, sr, bs, have);
  if (!active_) {           // first load: no audio thread running yet
    active_ = std::move(e);
    return true;
  }
  delete next_.exchange(e.release());   // a not-yet-consumed engine is dropped here
  return true;
}

void SynthHost::push(int type, int a, int b) {
  unsigned h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= (unsigned)kRing) return;
  ring_[h % kRing] = Ctl{type, a, b};
  head_.store(h + 1, std::memory_order_release);
}
void SynthHost::noteOn(int n, int v) { push(0, n, v); }
void SynthHost::noteOff(int n) { push(1, n, 0); }
void SynthHost::paramSet(int id, int raw) { push(2, id, raw); }
void SynthHost::cc(int c, int v) { push(3, c, v); }
void SynthHost::pitchBend(int s14) { push(4, s14, 0); }
void SynthHost::aftertouch(int v) { push(5, v, 0); }

Event SynthHost::toEvent(const Ctl& c) {
  switch (c.type) {
    case 0: return Event::NoteOn(0, (std::uint8_t)c.a, (std::uint8_t)c.b);
    case 1: return Event::NoteOff(0, (std::uint8_t)c.a, 0);
    case 3: return Event::CC(0, (std::uint8_t)c.a, (std::uint8_t)c.b);
    case 4: return Event::PitchBend(0, c.a);
    case 5: return Event::Aftertouch(0, c.a);
    default: return Event::ParameterSet(0, (std::uint16_t)c.a, c.b);
  }
}

void SynthHost::process(float* L, float* R, int n) {
  if (Engine* nx = next_.exchange(nullptr)) {
    Engine* old = active_.release();
    active_.reset(nx);
    delete retired_.exchange(old);    // normally null; freed by load() on the UI thread
  }
  if (!active_) { std::memset(L, 0, n * sizeof(float)); std::memset(R, 0, n * sizeof(float)); return; }
  staged_.clear();
  for (int i = 0; i < audioN_; ++i) staged_.push_back(toEvent(audioQ_[i]));
  audioN_ = 0;
  unsigned t = tail_.load(std::memory_order_relaxed);
  const unsigned h = head_.load(std::memory_order_acquire);
  for (; t != h; ++t) {
    const Ctl& c = ring_[t % kRing];
    staged_.push_back(toEvent(c));
  }
  tail_.store(t, std::memory_order_release);
  int done = 0;
  while (done < n) {
    const int k = std::min(1024, n - done);
    active_->process(L + done, R + done, k, done == 0 && !staged_.empty() ? staged_.data() : nullptr,
                     done == 0 ? staged_.size() : 0);
    done += k;
  }
}

// ---------------------------------------------------------------------------
bool SynthHost::renderProtocol(const std::string& text, int program, const std::vector<int>& notes,
                               double secs, double gap, double sr, int bs,
                               std::vector<float>& out, int& frames) {
  std::vector<std::int32_t> rows;
  const bool have = !text.empty();
  if (have && !parse_rows(text, rows)) return false;
  std::unique_ptr<Engine> e = make_engine(rows, program, (float)sr, bs, have);
  const std::int64_t fpn = x87_frames(secs, sr), fg = x87_frames(gap, sr);
  const std::int64_t total = (fpn + fg) * (std::int64_t)notes.size() + x87_frames(3.0, sr);
  frames = (int)(((total + bs - 1) / bs) * bs);
  std::vector<Event> evs;
  for (size_t i = 0; i < notes.size(); ++i) {
    const std::int64_t on = (fpn + fg) * (std::int64_t)i;
    evs.push_back(Event::NoteOn(on, (std::uint8_t)notes[i], 100));
    evs.push_back(Event::NoteOff(on + fpn, (std::uint8_t)notes[i], 0));
  }
  std::stable_sort(evs.begin(), evs.end(),
                   [](const Event& a, const Event& b) { return a.sample_offset < b.sample_offset; });
  std::vector<float> L(frames), R(frames);
  for (int block = 0; block < frames; block += bs) {
    const int n = std::min(bs, frames - block);
    std::vector<Event> be;
    for (auto ev : evs) {
      const std::int64_t rel = ev.sample_offset - block;
      if (rel >= 0 && rel < n) { ev.sample_offset = rel; be.push_back(ev); }
    }
    e->process(L.data() + block, R.data() + block, n, be.data(), be.size());
  }
  out.resize((size_t)frames * 2);
  for (int i = 0; i < frames; ++i) { out[2 * i] = L[i]; out[2 * i + 1] = R[i]; }
  return true;
}

}  // namespace synth1_android
