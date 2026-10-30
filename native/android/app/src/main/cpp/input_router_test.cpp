// input_router_test.cpp — gate D: MIDI router, octave shift, and the shell wiring (host, no Android).
//   input_router_test <skinsRoot> <preset.sy1>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "input_router.hpp"
#include "panel_view.hpp"
#include "synth_host.hpp"

using namespace synth1_android;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Rec : MidiSink {
  std::vector<std::string> log;
  static std::string f(const char* k, int a = -999, int b = -999) {
    char buf[64];
    if (b != -999) std::snprintf(buf, sizeof buf, "%s %d %d", k, a, b);
    else if (a != -999) std::snprintf(buf, sizeof buf, "%s %d", k, a);
    else std::snprintf(buf, sizeof buf, "%s", k);
    return buf;
  }
  void noteOn(int n, int v) override { log.push_back(f("on", n, v)); }
  void noteOff(int n) override { log.push_back(f("off", n)); }
  void cc(int c, int v) override { log.push_back(f("cc", c, v)); }
  void pitchBend(int s) override { log.push_back(f("bend", s)); }
  void aftertouch(int v) override { log.push_back(f("at", v)); }
  void modWheelUi(int v) override { log.push_back(f("modui", v)); }
  void pitchBendUi(int s) override { log.push_back(f("bendui", s)); }
  void programSelect(int b, int p) override { log.push_back(f("prog", b, p)); }
  std::string dump() const { std::string s; for (auto& l : log) s += l + ";"; return s; }
};

static void feed(MidiRouter& r, std::initializer_list<int> b) {
  std::vector<uint8_t> v;
  for (int x : b) v.push_back((uint8_t)x);
  r.feed(v.data(), v.size());
}

static void testRouter() {
  printf("MIDI router\n");
  Rec s; MidiRouter r(&s);
  std::vector<int> slots; for (int i = 0; i < 12; ++i) slots.push_back(i);
  r.setBankSlots(slots);
  feed(r, {0x90, 60, 100});            CHECK(s.dump() == "on 60 100;");           s.log.clear();
  feed(r, {0x80, 60, 0});              CHECK(s.dump() == "off 60;");              s.log.clear();
  feed(r, {0x90, 61, 0});              CHECK(s.dump() == "off 61;");              s.log.clear();   // vel 0 = off
  feed(r, {0x93, 64, 90, 0x8F, 64, 1}); CHECK(s.dump() == "on 64 90;off 64;");    s.log.clear();   // omni channel
  feed(r, {0xB0, 7, 100});             CHECK(s.dump() == "cc 7 100;");            s.log.clear();
  feed(r, {0xB0, 1, 77});              CHECK(s.dump() == "cc 1 77;modui 77;");    s.log.clear();   // mod wheel
  feed(r, {0xB0, 74, 5});              CHECK(s.dump() == "cc 74 5;");             s.log.clear();
  feed(r, {0xE0, 0x00, 0x40});         CHECK(s.dump() == "bend 0;bendui 0;");     s.log.clear();   // centre
  feed(r, {0xE0, 0x7F, 0x7F});         CHECK(s.dump() == "bend 8191;bendui 8191;"); s.log.clear();
  feed(r, {0xE0, 0x00, 0x00});         CHECK(s.dump() == "bend -8192;bendui -8192;"); s.log.clear();
  feed(r, {0xE0, 0x01, 0x41});         CHECK(s.dump() == "bend 129;bendui 129;");   s.log.clear();   // (65<<7|1)-8192
  feed(r, {0xD0, 99});                 CHECK(s.dump() == "at 99;");               s.log.clear();   // channel aftertouch
  feed(r, {0xC0, 5});                  CHECK(s.dump() == "prog -1 5;");           s.log.clear();   // current bank
  feed(r, {0xB0, 0, 3, 0xC0, 9});      CHECK(s.dump() == "prog 3 9;");            s.log.clear();   // bank select + PC
  CHECK(r.selectedBank() == 3);
  feed(r, {0xC1, 10});                 CHECK(s.dump() == "prog 3 10;");           s.log.clear();   // bank sticks (web law)
  feed(r, {0xB0, 0, 99, 0xC0, 1});     CHECK(s.dump() == "prog 3 1;");            s.log.clear();   // unknown slot ignored
  feed(r, {0xB0, 0, 11, 0xC0, 127});   CHECK(s.dump() == "prog 11 127;");         s.log.clear();
  feed(r, {0xB0, 32, 4});              CHECK(s.dump() == "cc 32 4;");             s.log.clear();   // CC32 passes through
  // running status, split chunks, realtime, sysex, system common
  feed(r, {0x90, 60, 100, 62, 100, 64, 100});
  CHECK(s.dump() == "on 60 100;on 62 100;on 64 100;");                            s.log.clear();
  feed(r, {0x90, 65}); CHECK(s.log.empty()); feed(r, {100}); CHECK(s.dump() == "on 65 100;"); s.log.clear();
  feed(r, {0x90, 66, 0xF8, 100, 0xFE});  CHECK(s.dump() == "on 66 100;");         s.log.clear();   // realtime interleaved
  feed(r, {0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7, 0x90, 67, 100});
  CHECK(s.dump() == "on 67 100;");                                                s.log.clear();   // sysex skipped
  feed(r, {0xF0, 1, 2, 3}); feed(r, {4, 0xF7}); CHECK(s.log.empty());              // sysex across chunks
  feed(r, {0xF2, 1, 2, 0x90, 68, 100}); CHECK(s.dump() == "on 68 100;");          s.log.clear();   // song position
  feed(r, {0xF0, 1, 0xF7, 0x61, 0x62}); CHECK(s.log.empty());                      // sysex clears running status
  // Receive Bank Select / Program Change switches
  Rec s2; MidiRouter r2(&s2); r2.setBankSlots(slots); r2.setReceiveBank(false); r2.setReceiveProgram(false);
  feed(r2, {0xB0, 0, 4, 0xC0, 3}); CHECK(s2.log.empty() && r2.selectedBank() == -1);
  r2.setReceiveProgram(true);
  feed(r2, {0xB0, 0, 4, 0xC0, 3}); CHECK(s2.dump() == "prog -1 3;");
  // non-contiguous slots: CC0 value is the bank SLOT, the sink gets the table INDEX
  Rec s3; MidiRouter r3(&s3); r3.setBankSlots({0, 10, 20});
  feed(r3, {0xB0, 0, 20, 0xC0, 2}); CHECK(s3.dump() == "prog 2 2;");
}

static void testOctave() {
  printf("Octave shift\n");
  OctaveShift o;
  CHECK(o.octave() == 0 && o.noteOn(60) == 60 && o.noteOff(60) == 60);
  CHECK(o.step(+1) && o.octave() == 1);
  CHECK(o.noteOn(60) == 72);
  CHECK(o.step(+1) && o.octave() == 2);
  CHECK(o.noteOff(60) == 72);                 // releases what was started, not what the shift now says
  CHECK(o.noteOn(60) == 84);
  CHECK(o.set(-3) && o.octave() == -3);
  CHECK(o.noteOn(96) == 60);
  CHECK(o.noteOn(36) == 0);                   // clamped at 0
  CHECK(!o.set(-9) && o.octave() == -3);      // clamped, unchanged
  CHECK(o.set(9) && o.octave() == 3);
  CHECK(o.noteOn(96) == 127);                 // clamped at 127
  CHECK(!o.step(+1));
  auto left = o.releaseAll();
  CHECK(left.size() == 3);                    // kb60->84, kb36->0, kb96->127 (retriggered key keeps one entry)
  CHECK(o.releaseAll().empty());
}

static void testEnginePath(const std::string& preset) {
  printf("Engine path (SynthHost)\n");
  std::ifstream f(preset, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  const std::string sy1 = ss.str();
  CHECK(!sy1.empty());
  auto render = [&](int mode) {   // 0: ring noteOn, 1: audio-thread noteOn, 2: audio noteOn + bend, 3: + CC1 + AT
    SynthHost h;
    h.load(sy1, 0, 44100.f, 512);
    std::vector<float> out, L(512), R(512);
    for (int b = 0; b < 40; ++b) {
      if (b == 0) { if (mode == 0) h.noteOn(60, 100); else h.audioNoteOn(60, 100); }
      if (b == 10 && mode >= 2) h.audioPitchBend(8191);
      if (b == 10 && mode == 3) { h.audioCc(1, 127); h.audioAftertouch(120); }
      h.process(L.data(), R.data(), 512);
      out.insert(out.end(), L.begin(), L.end());
    }
    return out;
  };
  auto a = render(0), b = render(1), c = render(2), d = render(3);
  double e = 0; for (float x : a) e += x * x;
  CHECK(e > 1e-6);                                              // sound comes out
  CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 4) == 0);   // ring == audio-thread path
  CHECK(std::memcmp(a.data(), c.data(), a.size() * 4) != 0);    // pitch bend reaches the engine
  CHECK(std::memcmp(c.data(), d.data(), a.size() * 4) != 0);    // CC1 / aftertouch reach the engine
  // before the bend the renders are identical (events applied at the right block)
  CHECK(std::memcmp(a.data(), c.data(), 10 * 512 * 4) == 0);
}

static void tapAt(PanelView& v, int id, float x, float y, long long t) { v.pointer(0, id, x, y, t); v.pointer(2, id, x, y, t + 40); }
static void tapItem(PanelView& v, PanelView::Item it, int id, long long t) {
  const auto r = v.itemRect(it);
  tapAt(v, id, r.x + r.w / 2.f, r.y + r.h / 2.f, t);
}
static bool keyPos(PanelView& v, int note, float& sx, float& sy) {
  const auto kr = v.keyboardRect();
  auto& kb = v.keyboard();
  const int k = v.nativeScale();
  for (double x = 0; x < kb.width; x += 0.5) {
    const double y = kb.height * 0.85;
    if (kb.noteAt(x, y) == note) { sx = kr.x + (float)(x * k); sy = kr.y + (float)(y * k); return true; }
  }
  return false;
}

static void testWiring(const std::string& root) {
  printf("Shell wiring (keyboard, OCT+/-, PANIC, INFO, wheels in the WHEEL column)\n");
  PanelView v; std::string err;
  CHECK(v.init(root + "/classic", 2400, 1080, err, true, 420.f));
  CHECK(v.landscape() && v.octaves() == 3 && v.whiteKeys() == 22);
  std::vector<std::string> ev;
  int panics = 0;
  v.onNoteOn = [&](int n, int vel) { ev.push_back("on " + std::to_string(n) + " " + std::to_string(vel > 0)); };
  v.onNoteOff = [&](int n) { ev.push_back("off " + std::to_string(n)); };
  v.onPitchBend = [&](int s) { ev.push_back("bend " + std::to_string(s)); };
  v.onModWheel = [&](int m) { ev.push_back("mod " + std::to_string(m)); };
  v.onAllNotesOff = [&]() { ++panics; };
  const int k = v.nativeScale();
  float x60, y60, x64, y64;
  CHECK(keyPos(v, 60, x60, y60) && keyPos(v, 64, x64, y64));
  v.pointer(0, 0, x60, y60, 1);
  CHECK(ev.size() == 1 && ev[0].rfind("on 60", 0) == 0);
  v.pointer(0, 1, x64, y64, 2);
  CHECK(ev.size() == 2 && ev[1].rfind("on 64", 0) == 0);
  tapItem(v, PanelView::kOctUp, 2, 3);                       // OCT +
  CHECK(v.octave().octave() == 1);
  v.pointer(2, 0, x60, y60, 5);                              // note started before the shift releases as 60
  CHECK(ev.size() == 3 && ev[2] == "off 60");
  v.pointer(0, 3, x60, y60, 6);                              // new press is shifted +12
  CHECK(ev.size() == 4 && ev[3].rfind("on 72", 0) == 0);
  v.pointer(2, 3, x60, y60, 7);
  CHECK(ev.back() == "off 72");
  v.pointer(2, 1, x64, y64, 8);
  CHECK(ev.back() == "off 64");
  tapItem(v, PanelView::kOctDown, 5, 10); tapItem(v, PanelView::kOctDown, 5, 20);   // OCT - twice
  CHECK(v.octave().octave() == -1);
  for (int i = 0; i < 6; ++i) tapItem(v, PanelView::kOctDown, 5, 100 + i * 100);
  CHECK(v.octave().octave() == -3);                          // clamps
  for (int i = 0; i < 6; ++i) tapItem(v, PanelView::kOctUp, 5, 1000 + i * 100);
  CHECK(v.octave().octave() == 3);
  for (int i = 0; i < 3; ++i) tapItem(v, PanelView::kOctDown, 5, 2000 + i * 100);
  CHECK(v.octave().octave() == 0);
  // wheels in the WHEEL column: pitch = left wheel, mod = right wheel
  const auto wp = v.wheelRect(0), wm = v.wheelRect(1);
  CHECK(wp.w > 0 && wp.h >= 200 && wm.x > wp.x && wp.w <= 12 * 420 / 25.4 + 1);
  ev.clear();
  const float py = wp.y + wp.h * 0.25f;
  v.pointer(0, 7, wp.x + wp.w / 2.f, py, 3000);
  CHECK(!ev.empty() && ev[0].rfind("bend", 0) == 0 && ev[0] != "bend 0");
  v.pointer(2, 7, wp.x + wp.w / 2.f, py, 3010);
  CHECK(ev.back() == "bend 0");                              // pitch springs back
  ev.clear();
  v.pointer(0, 8, wm.x + wm.w / 2.f, py, 3020);
  CHECK(!ev.empty() && ev[0].rfind("mod", 0) == 0 && ev[0] != "mod 0");
  v.pointer(2, 8, wm.x + wm.w / 2.f, py, 3030);
  CHECK(ev.back().rfind("mod", 0) == 0);                     // mod wheel persists
  // wheels + keys at the same time (multitouch)
  ev.clear();
  v.pointer(0, 9, wp.x + wp.w / 2.f, py, 3100); v.pointer(0, 10, x60, y60, 3101); v.pointer(0, 11, x64, y64, 3102);
  int ons = 0, bends = 0;
  for (auto& e : ev) { if (e.rfind("on", 0) == 0) ++ons; if (e.rfind("bend", 0) == 0) ++bends; }
  CHECK(ons == 2 && bends >= 1);
  v.pointer(2, 9, wp.x + wp.w / 2.f, py, 3110); v.pointer(2, 10, x60, y60, 3111); v.pointer(2, 11, x64, y64, 3112);
  // PANIC: hold two keys, tap PANIC -> both released + all-notes-off hook
  ev.clear(); panics = 0;
  v.pointer(0, 12, x60, y60, 4000); v.pointer(0, 13, x64, y64, 4001);
  tapItem(v, PanelView::kPanic, 14, 4100);
  int offs = 0; for (auto& e : ev) if (e.rfind("off", 0) == 0) ++offs;
  CHECK(offs == 2 && panics == 1);
  v.pointer(3, 0, 0, 0, 4200);
  // INFO toggles the overlay flag, nothing else changes
  const auto kr0 = v.keyboardRect();
  CHECK(!v.infoVisible());
  tapItem(v, PanelView::kInfo, 15, 5000);
  CHECK(v.infoVisible() && v.keyboardVisible() && v.keyboardRect().h == kr0.h && v.nativeScale() == k);
  tapItem(v, PanelView::kInfo, 15, 5100);
  CHECK(!v.infoVisible());
  // cancel releases everything
  ev.clear();
  v.pointer(0, 16, x60, y60, 6000); v.pointer(0, 17, x64, y64, 6001);
  v.pointer(3, 0, 0, 0, 6002);
  offs = 0; for (auto& e : ev) if (e.rfind("off", 0) == 0) ++offs;
  CHECK(offs == 2);
  // panel pointer API: two fingers (knob drag + tap elsewhere)
  const auto pr = v.panelRect();
  v.pointer(0, 18, pr.x + 91.f * k, pr.y + 50.f * k, 7000);
  v.pointer(0, 19, pr.x + 21.f * k, pr.y + 107.f * k, 7001);
  v.pointer(2, 19, pr.x + 21.f * k, pr.y + 107.f * k, 7060);
  v.pointer(2, 18, pr.x + 91.f * k, pr.y + 50.f * k, 7070);
  CHECK(v.panel().activePointerCount() == 0);
  int gotBank = -1, gotProg = -1;
  v.panel().onProgram = [&](int b, int p) { gotBank = b; gotProg = p; };
  v.panel().loadProgram(5, 3);
  CHECK(gotBank == 5 && gotProg == 3);
}

static void testPane(const std::string& root, bool portrait) {
  printf("KBD toggle / relayout (%s)\n", portrait ? "portrait 1080x2400" : "landscape 2400x1080");
  PanelView v; std::string err;
  const int W = portrait ? 1080 : 2400, H = portrait ? 2400 : 1080;
  CHECK(v.init(root + "/classic", W, H, err, true, 420.f));
  CHECK(v.landscape() == !portrait);
  int rebuilt = 0;
  v.onRebuilt = [&]() { ++rebuilt; };
  std::vector<std::string> ev;
  v.onNoteOn = [&](int n, int) { ev.push_back("on " + std::to_string(n)); };
  v.onNoteOff = [&](int n) { ev.push_back("off " + std::to_string(n)); };
  const int k0 = v.nativeScale();
  v.panel().loadProgram(5, 3);
  v.panel().setParam(10, 33);
  const int p10 = v.panel().getParam(10);
  auto pr = v.panelRect();
  const auto vb = v.viewBoxRect();
  tapAt(v, 1, pr.x + 21.f * k0, pr.y + 107.f * k0, 10);                   // panel tap
  tapAt(v, 2, vb.x + 8.f * k0, vb.y + 4.f * k0, 20);                      // VIEW header bar
  tapAt(v, 3, vb.x + vb.w - 2.f * k0, vb.y + vb.h - 2.f * k0, 30);        // empty corner
  tapAt(v, 4, v.itemRect(PanelView::kMidi).x + 3.f, v.itemRect(PanelView::kMidi).y + 3.f, 35);   // MIDI lamp (not a button)
  CHECK(v.keyboardVisible() && rebuilt == 0 && v.nativeScale() == k0);
  const auto b1 = v.itemRect(PanelView::kKbd);
  CHECK(b1.w > 0 && b1.x >= vb.x && b1.x + b1.w <= vb.x + vb.w && b1.y + b1.h <= vb.y + vb.h);
  v.pointer(0, 5, b1.x + 5.f, b1.y + 5.f, 40); v.pointer(2, 5, pr.x + 10.f, pr.y + 10.f, 80);   // lift outside: no toggle
  CHECK(v.keyboardVisible() && rebuilt == 0);
  tapItem(v, PanelView::kKbd, 6, 100);                                    // KBD: off
  CHECK(!v.keyboardVisible() && rebuilt == 1);
  CHECK(portrait ? v.nativeScale() >= k0 : v.nativeScale() > k0);
  CHECK(v.keyboardRect().w == 0);
  CHECK(v.panel().currentBank() == 5 && v.panel().currentProgram() == 3 && v.panel().getParam(10) == p10);
  printf("  k %d -> %d with the keyboard hidden\n", k0, v.nativeScale());
  const int k1 = v.nativeScale();
  tapAt(v, 7, v.panelRect().x + 21.f * k1, v.panelRect().y + 107.f * k1, 200);
  tapAt(v, 8, W / 2.f, H - 5.f, 210);                                     // where the keyboard was: nothing
  CHECK(!v.keyboardVisible() && ev.empty() && rebuilt == 1);
  tapItem(v, PanelView::kKbd, 9, 300);                                    // KBD: on again
  CHECK(v.keyboardVisible() && rebuilt == 2 && v.nativeScale() == k0);
  CHECK(v.panel().currentBank() == 5 && v.panel().getParam(10) == p10);
  float xa, ya, xb, yb;
  CHECK(keyPos(v, 60, xa, ya) && keyPos(v, 64, xb, yb));
  v.pointer(0, 11, xa, ya, 400); v.pointer(0, 12, xb, yb, 401);
  CHECK(ev.size() == 2 && ev[0] == "on 60" && ev[1] == "on 64");
  v.pointer(2, 11, xa, ya, 402); v.pointer(2, 12, xb, yb, 403);
  CHECK(ev.size() == 4 && ev[2] == "off 60" && ev[3] == "off 64");
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: input_router_test <skinsRoot> <preset.sy1>\n"); return 2; }
  testRouter();
  testOctave();
  testEnginePath(argv[2]);
  testWiring(argv[1]);
  testPane(argv[1], false);
  testPane(argv[1], true);
  printf("input_router_test: %d checks, %d failed -> %s\n", g_n, g_fail, g_fail ? "FAIL" : "PASS");
  return g_fail != 0;
}
