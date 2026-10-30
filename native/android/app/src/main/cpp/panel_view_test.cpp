// panel_view_test.cpp — host check (gate C) of the shell's presentation: panel at the largest integer
// scale that fits the whole screen, side columns with the buttons, keyboard overlay (wheels + keys).
//   panel_view_test <skinsRoot> <outdir> [webrefDir]
// Per surface x {keyboard on, off, on+INFO}:
//  - panel position + scale identical between keyboard on and off;
//  - panel pixels == Panel::render (startup program) replicated, 0 differing px (keyboard on: outside the
//    overlay area; off: whole panel); == the headless web render when webrefDir is given;
//  - startup LCD text non-empty, loaded != unloaded;
//  - every button rect is inside the surface and never intersects the overlay rect; the KBD button's face
//    colour is visible at its pixels in both states (lit yellow / grey);
//  - full coverage, no stray pure-black pixel; wheels are device-resolution anti-aliased (not k-blocks);
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
static bool overlap(const PanelView::Rect& a, const PanelView::Rect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string root = argv[1], out = argv[2], webref = argc > 3 ? argv[3] : "";
  auto readRaw = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
  int fails = 0;
  struct S { int w, h; float dpi; } sizes[] = {{2400, 1080, 420}, {1600, 720, 320}, {1920, 1080, 420}, {2340, 1080, 400}, {1080, 2400, 420}};
  const char* variants[] = {"kbd_on", "kbd_off", "info"};
  for (auto s : sizes) {
    PanelView::Rect panelOn{}; int scaleOn = 0;
    for (int var = 0; var < 3; ++var) {
      const bool kb = var != 1, info = var == 2;
      char tag[64];
      std::snprintf(tag, sizeof tag, "%dx%d_%s", s.w, s.h, variants[var]);
      PanelView v;
      std::string err;
      if (!v.init(root + "/classic", s.w, s.h, err, kb, s.dpi)) { printf("init %s FAIL %s\n", tag, err.c_str()); return 1; }
      std::vector<uint8_t> unloaded;
      v.composePanel(unloaded);
      v.loadInitialProgram();
      if (info) { v.setInfoVisible(true); v.setInfoText("audio: float/stereo rate=48000 burst=96 buf=192 | callbacks=1234 | classic scale=3 (skin 1 x3) | midi in=1 msgs=3"); }
      v.setMidiLamp(info);
      std::vector<uint8_t> surf((size_t)s.w * s.h * 4, 0), ref;
      v.blit(surf.data(), s.w, s.h, s.w * 4);
      synth1::panel::Panel p; std::string e2;
      p.load(root + "/classic" + (v.skinScale() == 2 ? "/200" : "/100"), v.skinScale(), e2);
      p.loadProgram(0, 0);
      ref.assign((size_t)p.width() * p.height() * 4, 0); p.render(ref.data());
      const int m = v.mult(), pw = p.width(), ph = p.height();
      const auto pr = v.panelRect();
      const auto ov = v.overlayRect();
      const bool covered = kb;     // overlay drawn
      long diff = 0, webSurf = -1, unc = 0, blackStray = 0;
      std::vector<uint8_t> web;
      if (!webref.empty()) { web = readRaw(webref + "/web_ref_" + (v.skinScale() == 2 ? "200" : "100") + ".rgba"); if (web.size() != ref.size()) web.clear(); }
      if (!web.empty()) webSurf = 0;
      for (int y = 0; y < ph * m; ++y) for (int x = 0; x < pw * m; ++x) {
        const int sx = pr.x + x, sy = pr.y + y;
        if (sx < 0 || sy < 0 || sx >= s.w || sy >= s.h) continue;
        if (covered && sy >= ov.y && !info) continue;            // under the overlay
        if (info && sy >= ov.y) continue;                        // INFO text sits in the overlay area
        const uint8_t* d = &surf[((size_t)sy * s.w + sx) * 4];
        if (memcmp(d, &ref[((size_t)(y / m) * pw + x / m) * 4], 3)) ++diff;
        if (!web.empty() && memcmp(d, &web[((size_t)(y / m) * pw + x / m) * 4], 3)) ++webSurf;
      }
      // geometry: panel inside the surface, identical for on/off, buttons clear of the overlay, KBD visible
      bool btnOk = true;
      for (int i = 0; i < PanelView::kItemCount; ++i) {
        const auto b = v.itemRect((PanelView::Item)i);
        if (b.w <= 0 || b.x < 0 || b.y < 0 || b.x + b.w > s.w || b.y + b.h > s.h || overlap(b, ov)) btnOk = false;
      }
      const auto kbd = v.itemRect(PanelView::kKbd);
      const uint8_t* kc = &surf[((size_t)(kbd.y + kbd.h / 2) * s.w + kbd.x + 3 * v.nativeScale()) * 4];
      const bool kbdVisible = kb ? (kc[0] == 255 && kc[1] == 255 && kc[2] == 140) : (kc[0] == 200 && kc[1] == 200 && kc[2] == 200);
      if (var == 0) { panelOn = pr; scaleOn = v.nativeScale(); }
      const bool same = pr.x == panelOn.x && pr.y == panelOn.y && pr.w == panelOn.w && pr.h == panelOn.h && v.nativeScale() == scaleOn;
      const bool geo = pr.x >= 0 && pr.y >= 0 && pr.x + pr.w <= s.w && pr.y + pr.h <= s.h && pr.w == 466 * v.nativeScale();
      // coverage + stray black
      const auto wp = v.wheelRect(0), wm = v.wheelRect(1), keys = v.keysRect();
      for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        const uint8_t* d = &surf[((size_t)y * s.w + x) * 4];
        if (d[3] != 255) ++unc;
        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && !inR(pr, x, y) && !(kb && inR(keys, x, y)) && !(kb && (inR(wp, x, y) || inR(wm, x, y)))) {
          bool btn = false;
          for (int i = 0; i < PanelView::kItemCount; ++i) {
            auto b = v.itemRect((PanelView::Item)i);
            if (inR({b.x - 1, b.y - 1, b.w + 2, b.h + 2}, x, y)) btn = true;
          }
          if (!btn && !(info && inR(ov, x, y))) ++blackStray;
        }
      }
      // wheels: anti-aliased at device resolution
      bool wheelAA = true;
      long aaEdge = 0; size_t ncol = 0;
      if (kb) {
        std::set<uint32_t> cols;
        for (int y = wp.y; y < wp.y + wp.h; ++y) for (int x = wp.x; x < wm.x + wm.w; ++x) { uint32_t c; memcpy(&c, &surf[((size_t)y * s.w + x) * 4], 4); cols.insert(c); }
        for (int i = 0; i < 2; ++i) {
          const auto r = v.wheelRect(i);
          const int span = (int)std::max(3.0, r.w * 0.18) + 3;
          for (int y = r.y; y < r.y + span; ++y) for (int x = r.x; x < r.x + span; ++x) {
            const uint8_t* d = &surf[((size_t)y * s.w + x) * 4];
            const bool clear = d[0] == 200 && d[1] == 200 && d[2] == 135, wellc = d[0] == 0x14 && d[1] == 0x14, edgec = d[0] == 0x7c && d[1] == 0x7c;
            if (!clear && !wellc && !edgec) ++aaEdge;
          }
        }
        ncol = cols.size();
        wheelAA = ncol >= 60 && aaEdge >= 4 && (double)wp.w <= 12 * s.dpi / 25.4 + 1;
      }
      // startup LCD (panel-only compare of the LCD rows; under the overlay -> use the keyboard-off case)
      const auto& lcd = v.panel().lcd();
      const std::string lp = lcd.count("program") ? lcd.at("program") : "", lb = lcd.count("bank") ? lcd.at("bank") : "";
      long lcdDiff = 0;
      for (int y = 299 * v.skinScale(); y < 331 * v.skinScale(); ++y) for (int x = 52 * v.skinScale(); x < 282 * v.skinScale(); ++x) {
        const size_t i = ((size_t)y * pw + x) * 4;
        if (memcmp(&ref[i], &unloaded[i], 3)) ++lcdDiff;
      }
      const bool ok = diff == 0 && webSurf <= 0 && unc == 0 && blackStray == 0 && !lp.empty() && !lb.empty() && lcdDiff > 0 &&
                      geo && same && btnOk && kbdVisible && wheelAA;
      printf("%-20s %s k=%d panel %dx%d at (%d,%d) overlay(y=%d h=%d)%s | diff=%ld web=%ld unc=%ld black=%ld btn_clear=%d kbd_visible=%d same_as_on=%d wheelAA=%d(%zu col) lcd='%s'/'%s' keys=%d oct/%d white %.1fmm -> %s\n",
             tag, v.landscape() ? "land" : "port", v.nativeScale(), pr.w, pr.h, pr.x, pr.y, ov.y, ov.h, v.keyboardOverlaysPanel() ? " overlays-panel" : " below-panel",
             diff, webSurf, unc, blackStray, (int)btnOk, (int)kbdVisible, (int)same, (int)wheelAA, ncol, lp.c_str(), lb.c_str(), v.octaves(), v.whiteKeys(),
             (double)v.keysRect().w / v.whiteKeys() / (s.dpi / 25.4), ok ? "ok" : "FAIL");
      if (!ok) ++fails;
      writePpm(out + "/view_" + tag + ".ppm", surf, s.w, s.h);
      if (std::string(tag) == "2400x1080_kbd_off") {      // select window: tap the program LCD, then tree row 11
        const int k = v.nativeScale();
        const float tx = pr.x + 167.f * k, ty = pr.y + 307.f * k;
        v.pointer(0, 1, tx, ty, 1000); v.pointer(2, 1, tx, ty, 1040);
        const bool open = v.panel().selectOpen();
        printf("  select window: open=%d banks=%d\n", (int)open, v.panel().bankCount());
        if (!open || v.panel().bankCount() != 12) ++fails;
        v.blit(surf.data(), s.w, s.h, s.w * 4);
        writePpm(out + "/select_" + tag + "_bank0.ppm", surf, s.w, s.h);
        const auto sr = v.selectRect();
        const float rx = sr.x + (1 + 30) * v.selectScale(), ry = sr.y + (1 + 224) * v.selectScale();
        v.pointer(0, 2, rx, ry, 2000); v.pointer(2, 2, rx, ry, 2040);
        v.blit(surf.data(), s.w, s.h, s.w * 4);
        writePpm(out + "/select_" + tag + "_bank11.ppm", surf, s.w, s.h);
      }
    }
  }
  printf("panel_view_test: %s\n", fails ? "FAIL" : "PASS");
  return fails != 0;
}
