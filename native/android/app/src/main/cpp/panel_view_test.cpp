// panel_view_test.cpp — host check (gate C) of the shell's presentation: panel + octave bar +
// keyboard compose at integer native scales, select window with the 12-bank tree.
//   panel_view_test <skinsRoot> <outdir>      skinsRoot holds classic/{100,200} and s80/{100,200}
// Checks: blit panel rect == Panel::render replicated at the native scale (0 differing px);
// bar/keyboard rects inside the surface and disjoint from the panel; the select window opens by a
// tap on the program LCD and the bank table has 12 banks; writes PPMs (tools/ppm2png.py -> PNG).
#include <cstdio>
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

static bool overlap(const PanelView::Rect& a, const PanelView::Rect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string root = argv[1], out = argv[2];
  int fails = 0;
  struct S { const char* skin; int w, h; const char* tag; } sizes[] = {
      {"classic", 466, 412, "classic_1x_466x412"},     {"classic", 932, 824, "classic_2x_932x824"},
      {"classic", 1398, 1236, "classic_3x_1398x1236"}, {"classic", 2400, 1080, "classic_phone_2400x1080"},
      {"classic", 1280, 800, "classic_1280x800"},      {"classic", 1920, 1080, "classic_1920x1080"},
      {"s80", 932, 824, "s80_2x_932x824"}};
  for (auto s : sizes) {
    PanelView v;
    std::string err;
    if (!v.init(root + "/" + s.skin, s.w, s.h, err)) { printf("init %s FAIL %s\n", s.tag, err.c_str()); return 1; }
    std::vector<uint8_t> surf((size_t)s.w * s.h * 4, 0x55), ref;
    v.blit(surf.data(), s.w, s.h, s.w * 4);
    synth1::panel::Panel p; std::string e2;
    p.load(root + "/" + s.skin + (v.skinScale() == 2 ? "/200" : "/100"), v.skinScale(), e2);
    ref.assign((size_t)p.width() * p.height() * 4, 0); p.render(ref.data());
    const int m = v.mult(), pw = p.width(), ph = p.height();
    const auto pr = v.panelRect(), br = v.barRect(), kr = v.keyboardRect();
    long diff = 0;
    for (int y = 0; y < ph * m; ++y) for (int x = 0; x < pw * m; ++x) {
      const uint8_t* d = &surf[((size_t)(pr.y + y) * s.w + pr.x + x) * 4];
      const uint8_t* r = &ref[((size_t)(y / m) * pw + x / m) * 4];
      if (d[0] != r[0] || d[1] != r[1] || d[2] != r[2]) ++diff;
    }
    const bool geo = !overlap(pr, br) && !overlap(pr, kr) && !overlap(br, kr) && pr.x >= 0 &&
                     kr.y + kr.h <= s.h && pr.y >= 0 && pr.w == 466 * v.nativeScale() && pr.h == 340 * v.nativeScale();
    printf("%-26s k=%d (skin %d x%d) panel(%d,%d %dx%d) bar(%d,%d %dx%d) kbd(%d,%d %dx%d): panel_diff_px=%ld geometry=%s\n",
           s.tag, v.nativeScale(), v.skinScale(), m, pr.x, pr.y, pr.w, pr.h, br.x, br.y, br.w, br.h, kr.x, kr.y,
           kr.w, kr.h, diff, geo ? "ok" : "BAD");
    if (diff || !geo) ++fails;
    writePpm(out + "/view_" + s.tag + ".ppm", surf, s.w, s.h);

    // select window: tap the program LCD (touch path: down + lift), then tree row 11
    const std::string tag = s.tag;
    if (tag == "classic_2x_932x824" || tag == "classic_phone_2400x1080" || tag == "s80_2x_932x824" || tag == "classic_1x_466x412") {
      const int k = v.nativeScale();
      const float tx = pr.x + 167.f * k, ty = pr.y + 307.f * k;
      v.pointer(0, 1, tx, ty, 1000); v.pointer(2, 1, tx, ty, 1040);
      const bool open = v.panel().selectOpen();
      const int nb = v.panel().bankCount();
      printf("  select window: open=%d banks=%d\n", (int)open, nb);
      if (!open || nb != 12) ++fails;
      v.blit(surf.data(), s.w, s.h, s.w * 4);
      writePpm(out + "/select_" + tag + "_bank0.ppm", surf, s.w, s.h);
      const auto sr = v.selectRect();   // tree row 11 ("11:Unsorted"): client (30,224)
      const float rx = sr.x + (1 + 30) * v.selectScale(), ry = sr.y + (1 + 224) * v.selectScale();
      v.pointer(0, 2, rx, ry, 2000); v.pointer(2, 2, rx, ry, 2040);
      printf("  after tapping tree row 11: select open=%d\n", (int)v.panel().selectOpen());
      v.blit(surf.data(), s.w, s.h, s.w * 4);
      writePpm(out + "/select_" + tag + "_bank11.ppm", surf, s.w, s.h);
    }
  }
  printf("panel_view_test: %s\n", fails ? "FAIL" : "PASS");
  return fails != 0;
}
