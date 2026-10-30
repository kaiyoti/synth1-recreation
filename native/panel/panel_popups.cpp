#include "panel_popups.hpp"

namespace synth1 {
namespace panel {

namespace {

/* Blit an RGBA sub-rect; out-of-bounds writes are dropped (Uint8ClampedArray semantics). */
void blit(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sx, int sy, int w, int h, int dx, int dy) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const long long d = (static_cast<long long>(dy + y) * dw + dx + x) * 4;
      if (dx + x < 0 || dy + y < 0 || dx + x >= dw || dy + y >= dh) continue;
      const long long s = (static_cast<long long>(sy + y) * sw + sx + x) * 4;
      dst[d] = src[s];
      dst[d + 1] = src[s + 1];
      dst[d + 2] = src[s + 2];
      dst[d + 3] = 255;
    }
  }
}

}   // namespace

bool parsePopupAssets(const Json& p, PopupAssets& out, std::string& err) {
  for (const auto& kv : p.at("slider").items()) {
    SliderEntry e;
    for (int k = 0; k < 4; k++) e.win[k] = kv.value().at("win")[k].get<int>();
    for (const auto& vv : kv.value().at("values").items()) {
      e.values[std::stoi(vv.key())] = vv.value().get<int>();
    }
    out.slider[std::stoi(kv.key())] = std::move(e);
  }
  for (const auto& kv : p.at("list").items()) {
    ListEntry e;
    for (int k = 0; k < 4; k++) e.win[k] = kv.value().at("win")[k].get<int>();
    for (int k = 0; k < 4; k++) e.client[k] = kv.value().at("client")[k].get<int>();
    e.rows = kv.value().at("rows").get<int>();
    e.cols = kv.value().at("cols").get<int>();
    e.cellw = kv.value().at("cellw").get<int>();
    e.cellh = kv.value().at("cellh").get<int>();
    e.count = kv.value().at("count").get<int>();
    for (const auto& v : kv.value().at("values")) e.values.push_back(v.get<int>());
    e.base = kv.value().at("base").get<int>();
    for (const auto& c : kv.value().at("cells")) {
      std::array<int, 4> a;
      for (int k = 0; k < 4; k++) a[k] = c[k].get<int>();
      e.cells.push_back(a);
    }
    out.list[std::stoi(kv.key())] = std::move(e);
  }
  for (const auto& t : p.at("tiles")) {
    std::array<int, 4> a;
    for (int k = 0; k < 4; k++) a[k] = t[k].get<int>();
    out.tiles.push_back(a);
  }
  out.nslider = p.contains("nslider") ? p.at("nslider").get<int>() : 0;
  out.sliderAtlasH = p.contains("slider_atlas_h") ? p.at("slider_atlas_h").get<int>() : 0;
  return true;
}

bool composePopup(const Popup& p, const PopupAssets& pops, const Image& atlas, Composed& out) {
  if (p.kind == 1) {
    const auto sit = pops.slider.find(p.ownerId);
    if (sit == pops.slider.end()) return false;
    const auto vit = sit->second.values.find(p.value);
    if (vit == sit->second.values.end()) return false;
    const std::array<int, 4>& t = pops.tiles[vit->second];
    out.x = sit->second.win[0] + 1;
    out.y = sit->second.win[1] + 1;
    out.w = t[2];
    out.h = t[3];
    out.data.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    blit(out.data.data(), out.w, out.h, atlas.rgba.data(), atlas.w, t[0], t[1], t[2], t[3], 0, 0);
    return true;
  }
  if (p.kind == 2) {
    const auto lit = pops.list.find(p.ownerId);
    if (lit == pops.list.end()) return false;
    const ListEntry& e = lit->second;
    const int W = e.win[2];
    const int H = e.win[3];
    out.x = e.win[0] + 1;
    out.y = e.win[1] + 1;
    out.w = W;
    out.h = H;
    out.data.assign(static_cast<size_t>(W) * H * 4, 0);
    const std::array<int, 4>& b = pops.tiles[e.base];
    blit(out.data.data(), W, H, atlas.rgba.data(), atlas.w, b[0], b[1], W, H, 0, 0);
    const int ox = e.client[0] - e.win[0];
    const int oy = e.client[1] - e.win[1];
    for (int k = 0; k < e.count; k++) {
      const int v = (k == p.current ? 2 : 0) + (k == p.hover ? 1 : 0);   // plain, hover, cur, cur+hover
      const std::array<int, 4>& t = pops.tiles[e.cells[k][v]];   // tile size carries the edge-cell margin
      const int c = k / e.rows;
      const int r = k % e.rows;
      const int x0 = ox + 2 + c * e.cellw + 1;
      const int y0 = oy + 2 + r * e.cellh + 1;
      blit(out.data.data(), W, H, atlas.rgba.data(), atlas.w, t[0], t[1], t[2], t[3], x0, y0);
    }
    return true;
  }
  return false;
}

ListGeom listGeom(const PopupAssets& pops, int ownerId) {
  const auto it = pops.list.find(ownerId);
  if (it == pops.list.end()) return ListGeom();
  const ListEntry& e = it->second;
  ListGeom g;
  g.cl = e.client[0];
  g.ct = e.client[1];
  g.cw = e.client[2];
  g.ch = e.client[3];
  g.mx = 2;
  g.my = 2;
  g.cellw = e.cellw;
  g.cellh = e.cellh;
  g.ok = true;
  return g;
}

}   // namespace panel
}   // namespace synth1
