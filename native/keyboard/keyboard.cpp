#include "keyboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace synth1 {
namespace keyboard {

int jsRound(double x) { return static_cast<int>(std::floor(x + 0.5)); }

bool isBlackNote(int n) {
  static const bool black[12] = {false, true, false, true, false, false, true, false, true,
                                 false, true, false};
  return black[((n % 12) + 12) % 12];
}

namespace {
constexpr int V_MIN = 30, V_MAX = 127;
constexpr double BLACK_W = 0.62, BLACK_H = 0.62;
constexpr double WHEEL_W = 56;
constexpr int PITCH_MIN = -8192, PITCH_MAX = 8191, PITCH_STEP = 512;
constexpr int MOD_MIN = 0, MOD_MAX = 127, MOD_STEP = 7;

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

const std::map<std::string, int>& keyMap() {
  static const std::map<std::string, int> m = {
      {"KeyZ", 0},   {"KeyS", 1},   {"KeyX", 2},   {"KeyD", 3},   {"KeyC", 4},   {"KeyV", 5},
      {"KeyG", 6},   {"KeyB", 7},   {"KeyH", 8},   {"KeyN", 9},   {"KeyJ", 10},  {"KeyM", 11},
      {"Comma", 12}, {"KeyL", 13},  {"Period", 14}, {"Semicolon", 15}, {"Slash", 16},
      {"KeyQ", 12},  {"Digit2", 13}, {"KeyW", 14}, {"Digit3", 15}, {"KeyE", 16}, {"KeyR", 17},
      {"Digit5", 18}, {"KeyT", 19}, {"Digit6", 20}, {"KeyY", 21}, {"Digit7", 22}, {"KeyU", 23},
      {"KeyI", 24},  {"Digit9", 25}, {"KeyO", 26}, {"Digit0", 27}, {"KeyP", 28},
      {"BracketLeft", 29}};
  return m;
}
const std::map<std::string, int>& octaveKeys() {
  static const std::map<std::string, int> m = {
      {"Minus", -1}, {"NumpadSubtract", -1}, {"Equal", 1}, {"NumpadAdd", 1}};
  return m;
}
}   // namespace

Keyboard::Keyboard(const Options& o) : opt_(o) {
  if (opt_.high < opt_.low) opt_.high = opt_.low;
  height = opt_.height;
  wheels = opt_.wheels;
  wheelWidth = wheels ? WHEEL_W : 0;
  for (int n = opt_.low; n <= opt_.high; n++) {
    if (isBlackNote(n)) {
      blacks.push_back(n);
      blackBoundary[n] = static_cast<int>(whites.size());
    } else {
      whiteOf_[n] = static_cast<int>(whites.size());
      whites.push_back(n);
    }
  }
  layout(0, height);
}

void Keyboard::layout(double w, double h) {
  width = w;
  height = h;
  whiteW = (width - wheelWidth) / static_cast<double>(whites.size());
  blackW = whiteW * BLACK_W;
  blackH = height * BLACK_H;
}

int Keyboard::noteAt(double x, double y) const {
  if (width <= 0 || height <= 0) return -1;
  if (!(x >= 0) || !(y >= 0) || x >= width || y >= height) return -1;
  const double xk = x - wheelWidth;
  if (xk < 0) return -1;
  int slot = static_cast<int>(std::floor(xk / whiteW));
  if (slot >= static_cast<int>(whites.size())) slot = static_cast<int>(whites.size()) - 1;
  if (y < blackH) {
    const int b = jsRound(xk / whiteW);
    int nb = -1;
    for (const int n : blacks) {
      if (blackBoundary.at(n) == b) { nb = n; break; }
    }
    if (nb != -1) {
      const double cx = b * whiteW;
      const double half = blackW / 2;
      if (xk >= cx - half && xk <= cx + half) return nb;
    }
  }
  return whites[slot];
}

int Keyboard::velocityFromY(double y) const {
  double t = height <= 1 ? 1 : y / (height - 1);
  if (!(t > 0)) t = 0; else if (t > 1) t = 1;
  int v = jsRound(V_MIN + t * (V_MAX - V_MIN));
  return v < V_MIN ? V_MIN : (v > V_MAX ? V_MAX : v);
}

int Keyboard::wheelAt(double x, double y) const {
  if (!wheels || !(x >= 0) || x >= wheelWidth || !(y >= 0) || y >= height) return -1;
  return x < wheelWidth / 2 ? 0 : 1;
}

int Keyboard::pitchFromY(double y) const {
  double t = height <= 1 ? 1 : (height - 1 - y) / (height - 1);
  return static_cast<int>(clampd(std::floor(clampd(t, 0, 1) * (PITCH_MAX - PITCH_MIN + 1) + PITCH_MIN + 0.5),
                                 PITCH_MIN, PITCH_MAX));
}

int Keyboard::modFromY(double y) const {
  double t = height <= 1 ? 1 : (height - 1 - y) / (height - 1);
  return static_cast<int>(clampd(std::floor(clampd(t, 0, 1) * MOD_MAX + 0.5), MOD_MIN, MOD_MAX));
}

Event Keyboard::emit(const Event& e) {
  log.push_back(e);
  if (e.type == "on") { if (onNoteOn) onNoteOn(e.note, e.vel); }
  else if (e.type == "off") { if (onNoteOff) onNoteOff(e.note); }
  else if (e.type == "pitch") { if (onPitchBend) onPitchBend(e.value); }
  else if (e.type == "mod") { if (onModWheel) onModWheel(e.value); }
  return e;
}

std::vector<Event> Keyboard::pressNote(const std::string& src, int note, int vel) {
  (void)src;
  if (note < 0) return {};
  std::vector<Event> out;
  const int c = active_[note];
  active_[note] = c + 1;
  if (c == 0) out.push_back(emit(Event{"on", note, vel, 0}));
  return out;
}

std::vector<Event> Keyboard::releaseNote(const std::string& src, int note) {
  (void)src;
  if (note < 0) return {};
  std::vector<Event> out;
  const auto it = active_.find(note);
  const int c = it == active_.end() ? 0 : it->second;
  if (c <= 1) {
    active_.erase(note);
    out.push_back(emit(Event{"off", note, 0, 0}));
  } else {
    active_[note] = c - 1;
  }
  return out;
}

std::vector<Event> Keyboard::setPitch(double v, bool emitCb) {
  v = clampd(jsRound(v), PITCH_MIN, PITCH_MAX);
  if (static_cast<int>(v) == pitchBend) return {};
  pitchBend = static_cast<int>(v);
  Event e{"pitch", 0, 0, pitchBend};
  log.push_back(e);
  if (emitCb && onPitchBend) onPitchBend(pitchBend);
  return {e};
}

std::vector<Event> Keyboard::setMod(double v, bool emitCb) {
  v = clampd(jsRound(v), MOD_MIN, MOD_MAX);
  if (static_cast<int>(v) == modWheel) return {};
  modWheel = static_cast<int>(v);
  Event e{"mod", 0, 0, modWheel};
  log.push_back(e);
  if (emitCb && onModWheel) onModWheel(modWheel);
  return {e};
}

int Keyboard::setPitchBend(double v) { setPitch(v, false); return pitchBend; }
int Keyboard::setModWheel(double v) { setMod(v, false); return modWheel; }

std::vector<Event> Keyboard::nudgeWheel(int wheel, int dir) {
  if (wheel != 0 && wheel != 1) return {};
  const int step = wheel == 0 ? PITCH_STEP : MOD_STEP;
  const int d = dir > 0 ? 1 : (dir < 0 ? -1 : 0);
  const int cur = wheel == 0 ? pitchBend : modWheel;
  return wheel == 0 ? setPitch(cur + d * step, true) : setMod(cur + d * step, true);
}

std::vector<Event> Keyboard::press(int id, double x, double y) {
  const int w = wheelAt(x, y);
  if (w >= 0) {
    std::vector<Event> out;
    if (heldWheel_.find(id) == heldWheel_.end()) {
      const auto hp = heldPointer_.find(id);
      if (hp != heldPointer_.end()) out = releaseNote("p" + std::to_string(id), hp->second);
    }
    heldPointer_.erase(id);
    heldWheel_[id] = w;
    const int v = w == 0 ? pitchFromY(y) : modFromY(y);
    auto ev = w == 0 ? setPitch(v, true) : setMod(v, true);
    out.insert(out.end(), ev.begin(), ev.end());
    return out;
  }
  std::vector<Event> out;
  const auto hp = heldPointer_.find(id);
  if (hp != heldPointer_.end()) out = releaseNote("p" + std::to_string(id), hp->second);
  const int note = noteAt(x, y);
  heldPointer_[id] = note;
  if (note >= 0) {
    auto ev = pressNote("p" + std::to_string(id), note, velocityFromY(y));
    out.insert(out.end(), ev.begin(), ev.end());
  }
  return out;
}

std::vector<Event> Keyboard::move(int id, double x, double y) {
  const auto hw = heldWheel_.find(id);
  if (hw != heldWheel_.end()) {
    const int v = hw->second == 0 ? pitchFromY(y) : modFromY(y);
    return hw->second == 0 ? setPitch(v, true) : setMod(v, true);
  }
  const auto hp = heldPointer_.find(id);
  if (hp == heldPointer_.end()) return {};
  const int cur = hp->second;
  const int note = noteAt(x, y);
  if (note == cur) return {};
  auto out = releaseNote("p" + std::to_string(id), cur);
  heldPointer_[id] = note;
  if (note >= 0) {
    auto ev = pressNote("p" + std::to_string(id), note, velocityFromY(y));
    out.insert(out.end(), ev.begin(), ev.end());
  }
  return out;
}

std::vector<Event> Keyboard::release(int id) {
  const auto hw = heldWheel_.find(id);
  if (hw != heldWheel_.end()) {
    const int w = hw->second;
    heldWheel_.erase(hw);
    return w == 0 ? setPitch(0, true) : std::vector<Event>{};   // pitch springs home; mod stays
  }
  const auto hp = heldPointer_.find(id);
  if (hp == heldPointer_.end()) return {};
  auto out = releaseNote("p" + std::to_string(id), hp->second);
  heldPointer_.erase(hp);
  return out;
}

int Keyboard::setOctave(int o) {
  octave = std::max(-opt_.octaveRange, std::min(opt_.octaveRange, o));
  return octave;
}

std::vector<Event> Keyboard::keyDown(const std::string& code, bool repeat) {
  const auto ok = octaveKeys().find(code);
  if (ok != octaveKeys().end()) { setOctave(octave + ok->second); return {}; }
  const auto km = keyMap().find(code);
  if (km == keyMap().end() || repeat) return {};
  const int note = opt_.baseNote + octave * 12 + km->second;
  if (note < opt_.low || note > opt_.high) return {};
  const auto hk = heldKey_.find(code);
  if (hk != heldKey_.end() && hk->second == note) return {};
  heldKey_[code] = note;
  return pressNote("key:" + code, note, keyVelocity);
}

std::vector<Event> Keyboard::keyUp(const std::string& code) {
  const auto hk = heldKey_.find(code);
  if (hk == heldKey_.end()) return {};
  auto out = releaseNote("key:" + code, hk->second);
  heldKey_.erase(hk);
  return out;
}

std::vector<Event> Keyboard::allNotesOff() {
  std::vector<Event> out;
  for (const auto& kv : active_) out.push_back(emit(Event{"off", kv.first, 0, 0}));
  active_.clear();
  heldPointer_.clear();
  heldKey_.clear();
  heldWheel_.clear();
  auto ev = setPitch(0, true);   // a held pitch wheel springs home too
  out.insert(out.end(), ev.begin(), ev.end());
  return out;
}

std::vector<int> Keyboard::activeNotes() const {
  std::vector<int> v;
  for (const auto& kv : active_) v.push_back(kv.first);
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<int> Keyboard::heldPointers() const {
  std::vector<int> v;
  for (const auto& kv : heldPointer_) v.push_back(kv.first);
  return v;
}

/* ---- procedural RGBA render (port of the canvas paint; text skipped) ---- */
namespace {
struct Rgb { uint8_t r, g, b; };
Rgb hex(const char* s) {
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  return {static_cast<uint8_t>(v(s[1]) * 16 + v(s[2])), static_cast<uint8_t>(v(s[3]) * 16 + v(s[4])),
          static_cast<uint8_t>(v(s[5]) * 16 + v(s[6]))};
}
}   // namespace

void Keyboard::render(int w, int h, const std::set<int>& lit, int activeWheel, uint8_t* out) const {
  const double W = w, H = h;
  auto fill = [&](double x, double y, double bw, double bh, Rgb c) {
    const int x0 = static_cast<int>(std::floor(x));
    const int x1 = static_cast<int>(std::ceil(x + bw));
    const int y0 = static_cast<int>(std::floor(y));
    const int y1 = static_cast<int>(std::ceil(y + bh));
    for (int py = std::max(0, y0); py < std::min(h, y1); py++) {
      if (!(py + 0.5 >= y && py + 0.5 < y + bh)) continue;
      for (int px = std::max(0, x0); px < std::min(w, x1); px++) {
        if (!(px + 0.5 >= x && px + 0.5 < x + bw)) continue;
        const size_t p = (static_cast<size_t>(py) * w + px) * 4;
        out[p] = c.r; out[p + 1] = c.g; out[p + 2] = c.b; out[p + 3] = 255;
      }
    }
  };
  auto roundInside = [&](double x, double y, double bw, double bh, double r, double px, double py) {
    if (px < x || px >= x + bw || py < y || py >= y + bh) return false;
    double ox = 0, oy = 0;
    if (px < x + r) ox = x + r - px; else if (px > x + bw - r) ox = px - (x + bw - r);
    if (py < y + r) oy = y + r - py; else if (py > y + bh - r) oy = py - (y + bh - r);
    return ox * ox + oy * oy <= r * r;
  };
  auto roundFill = [&](double x, double y, double bw, double bh, double r, Rgb c) {
    for (int py = std::max(0, static_cast<int>(std::floor(y))); py < std::min(h, static_cast<int>(std::ceil(y + bh))); py++)
      for (int px = std::max(0, static_cast<int>(std::floor(x))); px < std::min(w, static_cast<int>(std::ceil(x + bw))); px++)
        if (roundInside(x, y, bw, bh, r, px + 0.5, py + 0.5)) {
          const size_t p = (static_cast<size_t>(py) * w + px) * 4;
          out[p] = c.r; out[p + 1] = c.g; out[p + 2] = c.b; out[p + 3] = 255;
        }
  };
  auto roundStroke = [&](double x, double y, double bw, double bh, double r, Rgb c) {
    for (int py = std::max(0, static_cast<int>(std::floor(y - 1))); py < std::min(h, static_cast<int>(std::ceil(y + bh + 1))); py++)
      for (int px = std::max(0, static_cast<int>(std::floor(x - 1))); px < std::min(w, static_cast<int>(std::ceil(x + bw + 1))); px++) {
        const double cx = px + 0.5, cy = py + 0.5;
        if (!roundInside(x - 0.5, y - 0.5, bw + 1, bh + 1, r + 0.5, cx, cy)) continue;
        if (roundInside(x + 0.5, y + 0.5, bw - 1, bh - 1, std::max(0.0, r - 0.5), cx, cy)) continue;
        const size_t p = (static_cast<size_t>(py) * w + px) * 4;
        out[p] = c.r; out[p + 1] = c.g; out[p + 2] = c.b; out[p + 3] = 255;
      }
  };

  fill(0, 0, W, H, hex("#101010"));
  for (size_t i = 0; i < whites.size(); i++) {
    const double x = wheelWidth + i * whiteW;
    fill(x, 0, whiteW, H, lit.count(whites[i]) ? hex("#b9d7f2") : hex("#f7f7f3"));
    fill(x + 0.5 - 0.5, 0.5, 1, H - 1, hex("#7c7c74"));   // 1px separator at x+0.5
  }
  for (int n : blacks) {
    const double x = wheelWidth + blackBoundary.at(n) * whiteW - blackW / 2;
    fill(x, 0, blackW, blackH, lit.count(n) ? hex("#3d7ec4") : hex("#151515"));
  }
  if (wheelWidth > 0) {
    const double HW = wheelWidth, colW = HW / 2, PAD = 6, CAP_W = 20, CAP_H = 22;
    const double travel = std::max(0.0, (H - CAP_H) / 2);
    const double midY = std::round((H - 32) / 2);
    const double cyMid = (H - CAP_H) / 2 + CAP_H / 2;
    for (int c = 0; c < 2; c++) {
      const bool isPitch = c == 0;
      const double t = isPitch ? (pitchBend - (-8192)) / 16383.0 : modWheel / 127.0;
      const double cx = c * colW + colW / 2;
      const double slabW = colW - PAD * 2;
      const double slabX = std::round(cx - slabW / 2), slabY = std::round(PAD);
      const double slabH = std::max(6.0, H - PAD * 2);
      roundFill(slabX, slabY, slabW, slabH, 4, hex("#141414"));
      roundStroke(slabX, slabY, slabW, slabH, 4, hex("#7c7c74"));
      const double gx1 = std::round(cx - slabW / 2 + 3 - 0.5) + 0.5;
      const double gx2 = std::round(cx + slabW / 2 - 3 - 0.5) + 0.5;
      for (int i = 1; i * 6 < slabH - 4; i++) {
        fill(gx1, std::round(slabY + 3 + i * 6), gx2 - gx1, 2, (i % 2) ? hex("#242424") : hex("#080808"));
      }
      if (isPitch) {
        fill(slabX, midY, 1.5, 32, hex("#8b8b84"));
        fill(slabX + slabW - 1.5, midY, 1.5, 32, hex("#8b8b84"));
      }
      const double cy = cyMid + (1 - 2 * t) * travel;
      const double capX = std::round(cx - CAP_W / 2 - 0.5) + 0.5;
      const double capY = std::round(cy - CAP_H / 2 - 0.5) + 0.5;
      roundFill(capX, capY, CAP_W, CAP_H, 3,
                activeWheel == (isPitch ? 0 : 1) ? hex("#3d7ec4") : hex("#f7f7f3"));
      roundStroke(capX, capY, CAP_W, CAP_H, 3, hex("#7c7c74"));
      for (int i = -1; i <= 1; i++) {
        fill(cx + i * 4 - 0.5, std::round(cy - CAP_H / 2) + 4, 1, CAP_H - 8,
             (i == 0 && isPitch) ? hex("#8b8b84") : hex("#080808"));
      }
    }
  }
}

}   // namespace keyboard
}   // namespace synth1
