#include "panel_core.hpp"

#include <algorithm>
#include <cstring>

#include "panel_util.hpp"

namespace synth1 {
namespace panel {

bool PanelCore::load(const Json& data, const std::vector<uint8_t>& baseRGBA,
                    const std::vector<uint8_t>& atlasRGBA, int atlasWidth, std::string& err) {
  W_ = data.at("width").get<int>();
  H_ = data.at("height").get<int>();
  if (baseRGBA.size() != static_cast<size_t>(W_) * H_ * 4) {
    err = "base.png size mismatch";
    return false;
  }
  base_ = baseRGBA;
  atlas_ = atlasRGBA;
  atlasW_ = atlasWidth;

  for (const auto& kv : data.at("controls").items()) {
    (void)kv;
    const Json& c = kv.value();
    ControlDef d;
    d.param = c.at("param").get<int>();
    d.x = c.at("x").get<int>();
    d.y = c.at("y").get<int>();
    d.w = c.at("w").get<int>();
    d.h = c.at("h").get<int>();
    const std::vector<uint8_t> bits = base64Decode(c.at("mask").get<std::string>());
    for (int i = 0; i < d.w * d.h; i++) {
      if (bits[i >> 3] & (0x80 >> (i & 7))) d.idx.push_back(i);
    }
    if (c.contains("domain")) {
      d.hasDomain = true;
      int k = 0;
      for (const auto& dv : c.at("domain")) {
        const int v = dv.get<int>();
        d.domain.push_back(v);
        d.dmap[v] = k++;   // JS Map.set: last wins
      }
    }
    for (const auto& t : c.at("tiles")) {
      d.tileX.push_back(t[0].get<int>());
      d.tileY.push_back(t[1].get<int>());
    }
    controls_.push_back(std::move(d));
  }

  for (const auto& kv : data.at("fonts").items()) {
    auto& f = fonts_[kv.key()];
    for (const auto& gk : kv.value().items()) {
      const Json& g = gk.value();
      Glyph gh;
      gh.adv = g.at("adv").get<int>();
      gh.x = g.contains("x") ? g.at("x").get<int>() : 0;
      gh.y = g.contains("y") ? g.at("y").get<int>() : 0;
      gh.w = g.contains("w") ? g.at("w").get<int>() : 0;
      gh.h = g.contains("h") ? g.at("h").get<int>() : 0;
      if (g.contains("rgb") && !g.at("rgb").is_null()) gh.rgb = base64Decode(g.at("rgb").get<std::string>());
      f.emplace(std::stoi(gk.key()), gh);
    }
  }

  for (const auto& L : data.at("lcds")) {
    LcdDef d;
    d.id = L.at("id").get<std::string>();
    d.x = L.at("x").get<int>();
    d.y = L.at("y").get<int>();
    d.w = L.at("w").get<int>();
    d.h = L.at("h").get<int>();
    d.h2 = L.at("h2").get<int>();
    d.font = L.at("font").get<std::string>();
    lcds_.push_back(std::move(d));
  }

  const auto& fg = data.at("lcd_text_rgb");
  fg_[0] = fg[0].get<int>();
  fg_[1] = fg[1].get<int>();
  fg_[2] = fg[2].get<int>();

  auto tiles = [this](const Json& src, std::map<std::string, FootprintTile>& dst) {
    for (const auto& kv : src.items()) {
      const Json& t = kv.value();
      FootprintTile ft;
      ft.x = t.at("x").get<int>();
      ft.y = t.at("y").get<int>();
      ft.w = t.at("w").get<int>();
      const std::vector<uint8_t> bits = base64Decode(t.at("mask").get<std::string>());
      const int n = t.at("w").get<int>() * t.at("h").get<int>();
      for (int i = 0; i < n; i++) {
        if (bits[i >> 3] & (0x80 >> (i & 7))) ft.idx.push_back(i);
      }
      ft.rgb = base64Decode(t.at("rgb").get<std::string>());
      dst.emplace(kv.key(), std::move(ft));
    }
  };
  tiles(data.contains("pressed") ? data.at("pressed") : Json::object(), pressed_);
  tiles(data.contains("released") ? data.at("released") : Json::object(), released_);
  return true;
}

void PanelCore::blitTile(const FootprintTile& pk, uint8_t* out) const {
  for (const int i : pk.idx) {
    const int px = pk.x + (i % pk.w);
    const int py = pk.y + (i / pk.w);
    if (px < 0 || py < 0 || px >= W_ || py >= H_) continue;   // JS typed-array writes drop OOB
    const size_t p = (static_cast<size_t>(py) * W_ + px) * 4;
    out[p] = pk.rgb[static_cast<size_t>(i) * 3];
    out[p + 1] = pk.rgb[static_cast<size_t>(i) * 3 + 1];
    out[p + 2] = pk.rgb[static_cast<size_t>(i) * 3 + 2];
  }
}

int PanelCore::textWidth(const std::string& fontName, const std::string& s) const {
  const auto fit = fonts_.find(fontName);
  if (fit == fonts_.end()) return 0;
  int w = 0;
  for (const unsigned char ch : s) {
    const auto g = fit->second.find(ch);
    if (g != fit->second.end()) w += g->second.adv;
  }
  return w;
}

void PanelCore::drawLcd(uint8_t* out, const LcdDef& L, const std::string& s) const {
  const auto fit = fonts_.find(L.font);
  if (fit == fonts_.end()) return;
  const auto& font = fit->second;
  const int h2 = L.h2;
  const int left = 1;
  const int top = 1;
  if (s.empty()) return;
  const int calcW = textWidth(L.font, s);
  int w2 = calcW + (calcW & 1);
  w2 = std::min(w2, (L.w - left) * 2);
  if (w2 <= 0) return;
  const int ow = w2 >> 1;
  const int oh = h2 >> 1;
  const int ox = L.x + left;
  const int oy = L.y + top;
  std::vector<int32_t> tmp(static_cast<size_t>(w2) * h2 * 3);
  for (int y = 0; y < h2; y++) {
    for (int x = 0; x < w2; x++) {
      const size_t p = (static_cast<size_t>(oy + (y >> 1)) * W_ + ox + (x >> 1)) * 4;
      const size_t q = (static_cast<size_t>(y) * w2 + x) * 3;
      tmp[q] = out[p];
      tmp[q + 1] = out[p + 1];
      tmp[q + 2] = out[p + 2];
    }
  }
  const int clipW = std::min(w2, calcW);   // DT_CALCRECT rect = (0,0,calcW,tmHeight=h2)
  const int clipH = h2;
  int pen = 0;
  for (const unsigned char ch : s) {
    const auto git = font.find(ch);
    if (git == font.end()) continue;
    const Glyph& g = git->second;
    if (!g.rgb.empty()) {
      const int x0 = pen + g.x;
      const int y0 = g.y;
      for (int yy = 0; yy < g.h; yy++) {
        const int ty = y0 + yy;
        if (ty < 0 || ty >= clipH) continue;
        for (int xx = 0; xx < g.w; xx++) {
          const int tx = x0 + xx;
          if (tx < 0 || tx >= clipW) continue;
          const size_t a = (static_cast<size_t>(yy) * g.w + xx) * 3;
          const int ar = g.rgb[a];
          const int ag = g.rgb[a + 1];
          const int ab = g.rgb[a + 2];
          if (!(ar | ag | ab)) continue;
          const size_t q = (static_cast<size_t>(ty) * w2 + tx) * 3;
          tmp[q] = (fg_[0] * ar + tmp[q] * (255 - ar) + 127) / 255;
          tmp[q + 1] = (fg_[1] * ag + tmp[q + 1] * (255 - ag) + 127) / 255;
          tmp[q + 2] = (fg_[2] * ab + tmp[q + 2] * (255 - ab) + 127) / 255;
        }
      }
    }
    pen += g.adv;
  }
  for (int y = 0; y < oh; y++) {
    for (int x = 0; x < ow; x++) {
      const size_t a = ((static_cast<size_t>(2) * y) * w2 + 2 * x) * 3;
      const size_t b = a + 3;
      const size_t c = a + static_cast<size_t>(w2) * 3;
      const size_t d = c + 3;
      const size_t p = (static_cast<size_t>(oy + y) * W_ + ox + x) * 4;
      out[p] = static_cast<uint8_t>((tmp[a] + tmp[b] + tmp[c] + tmp[d]) >> 2);
      out[p + 1] = static_cast<uint8_t>((tmp[a + 1] + tmp[b + 1] + tmp[c + 1] + tmp[d + 1]) >> 2);
      out[p + 2] = static_cast<uint8_t>((tmp[a + 2] + tmp[b + 2] + tmp[c + 2] + tmp[d + 2]) >> 2);
    }
  }
}

void PanelCore::render(const State& st, uint8_t* out) const {
  std::memcpy(out, base_.data(), base_.size());
  for (const std::string& id : st.damage) {
    const auto it = released_.find(id);
    if (it != released_.end()) blitTile(it->second, out);
  }
  if (!st.pressed.empty()) {
    const auto it = pressed_.find(st.pressed);
    if (it != pressed_.end()) blitTile(it->second, out);
  }
  for (const ControlDef& c : controls_) {
    const auto vit = st.values.find(c.param);
    if (vit == st.values.end()) continue;
    int v = vit->second;
    if (c.hasDomain) {
      const auto dit = c.dmap.find(v);
      if (dit == c.dmap.end()) continue;   // GUI int -> tile
      v = dit->second;
    } else {
      v = std::max(0, std::min(static_cast<int>(c.tileX.size()) - 1, v));
    }
    const int tx = c.tileX[v];
    const int ty = c.tileY[v];
    for (const int i : c.idx) {
      const int lx = i % c.w;
      const int ly = i / c.w;
      const size_t p = (static_cast<size_t>(c.y + ly) * W_ + c.x + lx) * 4;
      const size_t q = (static_cast<size_t>(ty + ly) * atlasW_ + tx + lx) * 4;
      out[p] = atlas_[q];
      out[p + 1] = atlas_[q + 1];
      out[p + 2] = atlas_[q + 2];
      out[p + 3] = 255;
    }
  }
  for (const LcdDef& L : lcds_) {
    const auto it = st.lcd.find(L.id);
    if (it != st.lcd.end()) drawLcd(out, L, it->second);
  }
}

}   // namespace panel
}   // namespace synth1
