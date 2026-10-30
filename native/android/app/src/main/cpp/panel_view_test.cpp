// panel_view_test.cpp — host check (gate C) of the shell's presentation: [WHEEL | panel | VIEW] + keyboard.
//   panel_view_test <skinsRoot> <outdir> [webrefDir]
// For each surface (landscape + portrait, keyboard on/off, INFO once):
//  - panel rect == Panel::render (startup program, bank 00 prog 1) replicated, 0 differing px, and == the
//    headless web render when webrefDir is given;
//  - startup LCD gate: program/bank LCD text non-empty and the loaded panel differs from the unloaded one;
//  - full coverage (no uncovered pixel) and no pure-black pixel outside panel/keyboard/wheels/buttons;
//  - writes PPMs (tools/ppm2png.py -> PNG).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "panel_view.hpp"

using synth1_android::PanelView;

static void writePpm(const std::string& path, const std::vector<uint8_t>& s, int w, int h) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (size_t i = 0; i < (size_t)w * h; ++i) fwrite(&s[i * 4], 1, 3, f);
  fclose(f);
}
static bool inR(const PanelView::Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string root = argv[1], out = argv[2], webref = argc > 3 ? argv[3] : "";
  auto readRaw = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
  int fails = 0;
  struct S { int w, h; float dpi; bool kb, info; const char* tag; } sizes[] = {
      {2400, 1080, 420, true, false, "2400x1080_kbd_on"}, {2400, 1080, 420, false, false, "2400x1080_kbd_off"},
      {2400, 1080, 420, true, true, "2400x1080_info"},
      {1600, 720, 320, true, false, "1600x720_kbd_on"},   {1600, 720, 320, false, false, "1600x720_kbd_off"},
      {1600, 720, 320, true, true, "1600x720_info"},
      {1920, 1080, 420, true, false, "1920x1080_kbd_on"}, {1920, 1080, 420, false, false, "1920x1080_kbd_off"},
      {1920, 1080, 420, true, true, "1920x1080_info"},
      {2340, 1080, 400, true, false, "2340x1080_kbd_on"}, {2340, 1080, 400, false, false, "2340x1080_kbd_off"},
      {2340, 1080, 400, true, true, "2340x1080_info"},
      {1080, 2400, 420, true, false, "1080x2400_kbd_on"}, {1080, 2400, 420, false, false, "1080x2400_kbd_off"},
      {1080, 2400, 420, true, true, "1080x2400_info"}};
  for (auto s : sizes) {
    PanelView v;
    std::string err;
    if (!v.init(root + "/classic", s.w, s.h, err, s.kb, s.dpi)) { printf("init %s FAIL %s\n", s.tag, err.c_str()); return 1; }
    std::vector<uint8_t> unloaded;
    v.composePanel(unloaded);
    v.loadInitialProgram();
    if (s.info) { v.setInfoVisible(true); v.setInfoText("audio: float/stereo rate=48000 burst=96 buf=192 | callbacks=1234 | classic scale=2 (skin 2 x1) | midi in=1 msgs=3"); }
    v.setMidiLamp(s.info);
    std::vector<uint8_t> surf((size_t)s.w * s.h * 4, 0), ref;
    v.blit(surf.data(), s.w, s.h, s.w * 4);
    synth1::panel::Panel p; std::string e2;
    p.load(root + "/classic" + (v.skinScale() == 2 ? "/200" : "/100"), v.skinScale(), e2);
    p.loadProgram(0, 0);
    ref.assign((size_t)p.width() * p.height() * 4, 0); p.render(ref.data());
    const int m = v.mult(), pw = p.width(), ph = p.height();
    const auto pr = v.panelRect();
    long diff = 0, webSurf = -1, unc = 0, blackStray = 0;
    for (int y = 0; y < ph * m; ++y) for (int x = 0; x < pw * m; ++x) {
      const uint8_t* d = &surf[((size_t)(pr.y + y) * s.w + pr.x + x) * 4];
      const uint8_t* r = &ref[((size_t)(y / m) * pw + x / m) * 4];
      if (memcmp(d, r, 3)) ++diff;
    }
    if (!webref.empty()) {
      const auto w = readRaw(webref + "/web_ref_" + (v.skinScale() == 2 ? "200" : "100") + ".rgba");
      if (w.size() == ref.size()) {
        webSurf = 0;
        for (int y = 0; y < ph * m; ++y) for (int x = 0; x < pw * m; ++x)
          if (memcmp(&surf[((size_t)(pr.y + y) * s.w + pr.x + x) * 4], &w[((size_t)(y / m) * pw + x / m) * 4], 3)) ++webSurf;
      }
    }
    // coverage + stray black (the old clear colour): outside panel, keyboard, wheels and the buttons
    const auto kr = v.keyboardRect(), wr = v.wheelRect();
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
      const uint8_t* d = &surf[((size_t)y * s.w + x) * 4];
      if (d[3] != 255) ++unc;
      if (d[0] == 0 && d[1] == 0 && d[2] == 0 && !inR(pr, x, y) && !inR(kr, x, y) && !inR(wr, x, y)) {
        bool btn = false;
        for (int i = 0; i < PanelView::kItemCount; ++i) {
          auto b = v.itemRect((PanelView::Item)i);
          if (inR({b.x - 1, b.y - 1, b.w + 2, b.h + 2}, x, y)) btn = true;
        }
        if (!btn && !(v.infoVisible() && inR(v.belowRect(), x, y))) ++blackStray;
      }
    }
    // wheels are device-resolution anti-aliased (not k-replicated blocks): many distinct colours + blended edge pixels
    long aaEdge = 0;
    std::set<uint32_t> cols;
    const auto wrAll = v.wheelRect();
    for (int y = wrAll.y; y < wrAll.y + wrAll.h; ++y) for (int x = wrAll.x; x < wrAll.x + wrAll.w; ++x) {
      uint32_t c; memcpy(&c, &surf[((size_t)y * s.w + x) * 4], 4); cols.insert(c);
    }
    for (int i = 0; i < 2; ++i) {   // rounded corner of each well: blends of frame/well/edge colours = AA edge pixels
      const auto r = v.wheelRect(i);
      const int span = (int)std::max(3.0, r.w * 0.18) + 3;
      for (int y = r.y; y < r.y + span; ++y) for (int x = r.x; x < r.x + span; ++x) {
        const uint8_t* d = &surf[((size_t)y * s.w + x) * 4];
        const bool clear = d[0] == 200 && d[1] == 200 && d[2] == 135, wellc = d[0] == 0x14 && d[1] == 0x14, edgec = d[0] == 0x7c && d[1] == 0x7c;
        if (!clear && !wellc && !edgec) ++aaEdge;
      }
    }
    const bool wheelAA = cols.size() >= 60 && aaEdge >= 4 && (double)v.wheelRect(0).w <= 12 * s.dpi / 25.4 + 1;
    // startup LCD gate
    const auto& lcd = v.panel().lcd();
    const std::string lp = lcd.count("program") ? lcd.at("program") : "", lb = lcd.count("bank") ? lcd.at("bank") : "";
    long lcdDiff = 0;
    for (int y = 299 * v.skinScale(); y < 331 * v.skinScale(); ++y) for (int x = 52 * v.skinScale(); x < 282 * v.skinScale(); ++x) {
      const size_t i = ((size_t)y * pw + x) * 4;
      if (memcmp(&ref[i], &unloaded[i], 3)) ++lcdDiff;
    }
    const bool geo = pr.x >= 0 && pr.y >= 0 && pr.x + pr.w <= s.w && pr.y + pr.h <= s.h &&
                     (s.kb ? kr.w == s.w && kr.h > 0 && kr.y >= pr.y + pr.h : kr.w == 0);
    const bool kbCap = !s.kb || v.landscape() || (kr.h >= 250 && kr.h <= std::max(250.0, 32.0 * s.dpi / 25.4) + v.nativeScale());
    const bool ok = wheelAA && kbCap && diff == 0 && (webSurf <= 0) && unc == 0 && blackStray == 0 && !lp.empty() && !lb.empty() && lcdDiff > 0 && geo;
    printf("%-20s %s k=%d(skin %d x%d) panel(%d,%d) kbd=%dx%d keys: %d octaves (%d white) | panel_diff=%ld web_diff=%ld uncovered=%ld stray_black=%ld lcd='%s'/'%s' lcd_px=%ld wheel_colours=%zu wheelAA=%d white_key=%.1fmm -> %s\n",
           s.tag, v.landscape() ? "land" : "port", v.nativeScale(), v.skinScale(), m, pr.x, pr.y, kr.w, kr.h, v.octaves(),
           v.whiteKeys(), diff, webSurf, unc, blackStray, lp.c_str(), lb.c_str(), lcdDiff, cols.size(), (int)wheelAA, (double)(s.w) / v.whiteKeys() / (s.dpi / 25.4), ok ? "ok" : "FAIL");
    if (!ok) ++fails;
    writePpm(out + "/view_" + s.tag + ".ppm", surf, s.w, s.h);

    if (std::string(s.tag) == "2400x1080_kbd_on") {      // select window: tap the program LCD, then tree row 11
      const int k = v.nativeScale();
      const float tx = pr.x + 167.f * k, ty = pr.y + 307.f * k;
      v.pointer(0, 1, tx, ty, 1000); v.pointer(2, 1, tx, ty, 1040);
      const bool open = v.panel().selectOpen();
      printf("  select window: open=%d banks=%d\n", (int)open, v.panel().bankCount());
      if (!open || v.panel().bankCount() != 12) ++fails;
      v.blit(surf.data(), s.w, s.h, s.w * 4);
      writePpm(out + "/select_" + s.tag + "_bank0.ppm", surf, s.w, s.h);
      const auto sr = v.selectRect();
      const float rx = sr.x + (1 + 30) * v.selectScale(), ry = sr.y + (1 + 224) * v.selectScale();
      v.pointer(0, 2, rx, ry, 2000); v.pointer(2, 2, rx, ry, 2040);
      v.blit(surf.data(), s.w, s.h, s.w * 4);
      writePpm(out + "/select_" + s.tag + "_bank11.ppm", surf, s.w, s.h);
    }
  }
  printf("panel_view_test: %s\n", fails ? "FAIL" : "PASS");
  return fails != 0;
}
