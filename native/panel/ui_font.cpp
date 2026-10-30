#include "ui_font.hpp"

namespace synth1 {
namespace panel {
namespace {
#include "ui_font_data.inc"
const UiFont kRegFont = {kRegGlyphs, kRegData, kRegAscent, kRegDescent};
const UiFont kBoldFont = {kBoldGlyphs, kBoldData, kBoldAscent, kBoldDescent};
const UiGlyph* glyphOf(const UiFont& f, unsigned char c) {
  return (c < 32 || c > 126) ? nullptr : &f.glyphs[c - 32];
}
}   // namespace

const UiFont& uiFontRegular() { return kRegFont; }
const UiFont& uiFontBold() { return kBoldFont; }

int uiTextWidth(const UiFont& f, const std::string& s) {
  int w = 0;
  for (unsigned char c : s) {
    const UiGlyph* g = glyphOf(f, c);
    if (g) w += g->adv;
  }
  return w;
}

void uiDrawText(uint8_t* rgba, int W, int H, int x, int baseline, const std::string& s,
                const uint8_t fg[3], const UiFont& f, int scale) {
  int pen = 0;
  for (unsigned char c : s) {
    const UiGlyph* g = glyphOf(f, c);
    if (!g) continue;
    for (int yy = 0; yy < g->h; ++yy)
      for (int xx = 0; xx < g->w; ++xx) {
        const int a = f.data[g->off + yy * g->w + xx];
        if (!a) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) {
            const int X = x + (pen + g->x + xx) * scale + sx, Y = baseline + (g->y + yy) * scale + sy;
            if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            uint8_t* p = rgba + ((size_t)Y * W + X) * 4;
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)((fg[k] * a + p[k] * (255 - a) + 127) / 255);
            p[3] = 255;
          }
      }
    pen += g->adv;
  }
}

}   // namespace panel
}   // namespace synth1
