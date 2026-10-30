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

static void testWiring(const std::string& root) {
  printf("Shell wiring (PanelView keyboard + octave bar)\n");
  PanelView v; std::string err;
  CHECK(v.init(root + "/classic", 2400, 1080, err));
  std::vector<std::string> ev;
  v.onNoteOn = [&](int n, int vel) { ev.push_back("on " + std::to_string(n) + " " + std::to_string(vel > 0)); };
  v.onNoteOff = [&](int n) { ev.push_back("off " + std::to_string(n)); };
  v.onPitchBend = [&](int s) { ev.push_back("bend " + std::to_string(s)); };
  v.onModWheel = [&](int m) { ev.push_back("mod " + std::to_string(m)); };
  const auto kr = v.keyboardRect(), br = v.barRect();
  const int k = v.nativeScale();
  auto& kb = v.keyboard();
  auto keyPos = [&](int note, float& sx, float& sy) {
    for (double x = 0; x < kb.width; x += 0.5) {
      const double y = kb.height * 0.85;
      if (kb.noteAt(x, y) == note) { sx = kr.x + (float)(x * k); sy = kr.y + (float)(y * k); return true; }
    }
    return false;
  };
  float x60, y60, x64, y64;
  CHECK(keyPos(60, x60, y60) && keyPos(64, x64, y64));
  v.pointer(0, 0, x60, y60, 1);
  CHECK(ev.size() == 1 && ev[0].rfind("on 60", 0) == 0);
  v.pointer(0, 1, x64, y64, 2);                                     // second finger: second note
  CHECK(ev.size() == 2 && ev[1].rfind("on 64", 0) == 0);
  // Oct+ button on the bar (logical x 48..88, y 2..14)
  const float bx = br.x + 60.f * k, by = br.y + 8.f * k;
  v.pointer(0, 2, bx, by, 3); v.pointer(2, 2, bx, by, 4);
  CHECK(v.octave().octave() == 1);
  v.pointer(2, 0, x60, y60, 5);                                     // lift the note started before the shift
  CHECK(ev.size() == 3 && ev[2] == "off 60");
  v.pointer(0, 3, x60, y60, 6);                                     // new press is shifted +12
  CHECK(ev.size() == 4 && ev[3].rfind("on 72", 0) == 0);
  v.pointer(2, 3, x60, y60, 7);
  CHECK(ev.back() == "off 72");
  v.pointer(2, 1, x64, y64, 8);
  CHECK(ev.back() == "off 64");
  // Oct- twice -> -1
  const float mx = br.x + 20.f * k;
  for (int i = 0; i < 2; ++i) { v.pointer(0, 5, mx, by, 10); v.pointer(2, 5, mx, by, 11); }
  CHECK(v.octave().octave() == -1);
  // wheels: pitch (left half of the wheel block) and mod (right half)
  ev.clear();
  v.pointer(0, 7, kr.x + 10.f * k, kr.y + 20.f * k, 20);
  CHECK(!ev.empty() && ev[0].rfind("bend", 0) == 0);
  v.pointer(2, 7, kr.x + 10.f * k, kr.y + 20.f * k, 21);
  CHECK(ev.back() == "bend 0");                                     // pitch springs back
  ev.clear();
  v.pointer(0, 8, kr.x + 42.f * k, kr.y + 20.f * k, 22);
  CHECK(!ev.empty() && ev[0].rfind("mod", 0) == 0);
  v.pointer(2, 8, kr.x + 42.f * k, kr.y + 20.f * k, 23);
  // cancel releases everything
  ev.clear();
  v.pointer(0, 9, x60, y60, 30); v.pointer(0, 10, x64, y64, 31);
  v.pointer(3, 0, 0, 0, 32);
  int offs = 0; for (auto& e : ev) if (e.rfind("off", 0) == 0) ++offs;
  CHECK(offs == 2);
  // panel pointer API: two fingers, knob drag + tap elsewhere do not crash and the panel stays consistent
  const auto pr = v.panelRect();
  v.pointer(0, 11, pr.x + 91.f * k, pr.y + 50.f * k, 40);
  v.pointer(0, 12, pr.x + 21.f * k, pr.y + 107.f * k, 41);
  v.pointer(2, 12, pr.x + 21.f * k, pr.y + 107.f * k, 60);
  v.pointer(2, 11, pr.x + 91.f * k, pr.y + 50.f * k, 70);
  CHECK(v.panel().activePointerCount() == 0);
  // program change from the select window (bank 5 program 3) reaches the panel's onProgram
  int gotBank = -1, gotProg = -1;
  v.panel().onProgram = [&](int b, int p) { gotBank = b; gotProg = p; };
  v.panel().loadProgram(5, 3);
  CHECK(gotBank == 5 && gotProg == 3);
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: input_router_test <skinsRoot> <preset.sy1>\n"); return 2; }
  testRouter();
  testOctave();
  testEnginePath(argv[2]);
  testWiring(argv[1]);
  printf("input_router_test: %d checks, %d failed -> %s\n", g_n, g_fail, g_fail ? "FAIL" : "PASS");
  return g_fail != 0;
}
