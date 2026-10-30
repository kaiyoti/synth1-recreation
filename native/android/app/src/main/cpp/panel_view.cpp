#include "panel_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "ui_font.hpp"

namespace synth1_android {

using synth1::panel::Panel;

namespace {
void replicate(uint8_t* dst, int dw, int dh, int rowBytes, const uint8_t* src, int sw, int sh, int ox,
               int oy, int m) {
  const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
  for (int y = 0; y < sh * m; ++y) {
    const int dy = oy + y;
    if (dy < 0 || dy >= dh) continue;
    uint32_t* row = reinterpret_cast<uint32_t*>(dst + (size_t)dy * rowBytes);
    const uint32_t* srow = s32 + (size_t)(y / m) * sw;
    for (int x = 0; x < sw * m; ++x) {
      const int dx = ox + x;
      if (dx < 0 || dx >= dw) continue;
      row[dx] = srow[x / m] | 0xFF000000u;
    }
  }
}
}  // namespace

bool PanelView::init(const std::string& root, int w, int h, std::string& err) {
  surfW_ = w; surfH_ = h;
  // 1x panel size from the 200 % skin (exactly 2x); fall back to the known 466x340.
  int w1 = 466, h1 = 340;
  {
    Panel probe;
    std::string e;
    if (probe.load(root + "/200", 2, e)) { w1 = probe.width() / 2; h1 = probe.height() / 2; }
  }
  int k = std::min(w / w1, h / (h1 + kBarH + kKbMin));
  if (k < 1) k = 1;                      // smaller than 1x: clipped (documented limitation)
  skin_ = (k % 2 == 0) ? 2 : 1;
  mult_ = k / skin_;
  panel_.reset(new Panel());
  if (!panel_->load(root + (skin_ == 2 ? "/200" : "/100"), skin_, err)) return false;
  panel_->setTouchMode(true);            // P1 touch semantics: preview on down / select on lift
  panel_->setSelectLabels(true);         // bank tree / heading text from banks.json (web paintSel)

  selScale_ = std::max(1, std::min(w / (panel_->selectClientWidth() + 2), h / (panel_->selectClientHeight() + 2)));
  const int lw = std::max(1, w / k), lh = std::max(1, h / k);
  const int kbH = std::max(kKbMin, std::min(kKbMax, lh - h1 - kBarH));
  const int total = h1 + kBarH + kbH;
  const int oy = std::max(0, (lh - total) / 2);
  panelR_ = {(w - w1 * k) / 2, oy * k, w1 * k, h1 * k};
  barR_ = {(w - lw * k) / 2, (oy + h1) * k, lw * k, kBarH * k};
  kbR_ = {(w - lw * k) / 2, (oy + h1 + kBarH) * k, lw * k, kbH * k};
  kbLogicalW_ = lw; kbLogicalH_ = kbH;

  synth1::keyboard::Keyboard::Options o;
  o.low = 36; o.high = 96; o.height = kbH;
  kb_.reset(new synth1::keyboard::Keyboard(o));
  kb_->layout(lw, kbH);
  wireKeyboard();
  region_.clear(); barBtn_.clear(); activeWheel_ = -1;
  paintBar();
  return true;
}

void PanelView::wireKeyboard() {
  kb_->onNoteOn = [this](int n, int v) { const int e = oct_.noteOn(n); if (onNoteOn) onNoteOn(e, v); };
  kb_->onNoteOff = [this](int n) { const int e = oct_.noteOff(n); if (onNoteOff) onNoteOff(e); };
  kb_->onPitchBend = [this](int v) { if (onPitchBend) onPitchBend(v); };
  kb_->onModWheel = [this](int v) { if (onModWheel) onModWheel(v); };
}

void PanelView::barRects(int& minusX, int& plusX, int& btnW) const { minusX = 4; plusX = 48; btnW = 40; }

void PanelView::paintBar() {
  const int lw = kbLogicalW_;
  barFrame_.assign((size_t)lw * kBarH * 4, 0);
  for (size_t i = 0; i < barFrame_.size(); i += 4) {
    barFrame_[i] = 0x22; barFrame_[i + 1] = 0x22; barFrame_[i + 2] = 0x28; barFrame_[i + 3] = 255;
  }
  int mx, px, bw;
  barRects(mx, px, bw);
  auto button = [&](int x, const char* label, bool pressed) {
    for (int y = 2; y < kBarH - 2; ++y)
      for (int xx = x; xx < x + bw; ++xx) {
        uint8_t* p = &barFrame_[((size_t)y * lw + xx) * 4];
        const bool edge = y == 2 || y == kBarH - 3 || xx == x || xx == x + bw - 1;
        const uint8_t v = edge ? 0x90 : (pressed ? 0x78 : 0x44);
        p[0] = p[1] = p[2] = v;
      }
    const uint8_t white[3] = {255, 255, 255};
    const auto& f = synth1::panel::uiFontBold();
    const int tw = synth1::panel::uiTextWidth(f, label);
    synth1::panel::uiDrawText(barFrame_.data(), lw, kBarH, x + (bw - tw) / 2, 12, label, white, f);
  };
  bool pm = false, pp = false;
  for (auto& kv : barBtn_) { if (kv.second == 0) pm = true; else pp = true; }
  button(mx, "Oct -", pm);
  button(px, "Oct +", pp);
  char buf[64];
  const int o = oct_.octave();
  std::snprintf(buf, sizeof buf, "Octave %s%d  (keys C%d-C%d)", o > 0 ? "+" : "", o, 2 + o, 7 + o);
  const uint8_t grey[3] = {0xd0, 0xd0, 0xd8};
  synth1::panel::uiDrawText(barFrame_.data(), lw, kBarH, 96, 12, buf, grey, synth1::panel::uiFontRegular());
}

void PanelView::composePanel(std::vector<uint8_t>& rgba) {
  const int pw = panelW(), ph = panelH();
  rgba.assign((size_t)pw * ph * 4, 0);
  panel_->render(rgba.data());
}

void PanelView::blit(uint8_t* dst, int dw, int dh, int rowBytes) {
  composePanel(frame_);
  for (int y = 0; y < dh; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(dst + (size_t)y * rowBytes);
    for (int x = 0; x < dw; ++x) row[x] = 0xFF000000u;   // opaque black (RGBA bytes 0,0,0,255)
  }
  replicate(dst, dw, dh, rowBytes, frame_.data(), panelW(), panelH(), panelR_.x, panelR_.y, mult_);
  const int k = nativeScale();
  paintBar();
  replicate(dst, dw, dh, rowBytes, barFrame_.data(), kbLogicalW_, kBarH, barR_.x, barR_.y, k);
  kbFrame_.assign((size_t)kbLogicalW_ * kbLogicalH_ * 4, 0);
  std::set<int> lit;
  for (int n : kb_->activeNotes()) lit.insert(n);
  kb_->render(kbLogicalW_, kbLogicalH_, lit, activeWheel_, kbFrame_.data());
  replicate(dst, dw, dh, rowBytes, kbFrame_.data(), kbLogicalW_, kbLogicalH_, kbR_.x, kbR_.y, k);
  if (panel_->selectOpen()) {
    const int cw = panel_->selectClientWidth(), ch = panel_->selectClientHeight(), W = cw + 2, H = ch + 2;
    const auto fc = panel_->selectFrameColor();
    selFrame_.assign((size_t)W * H * 4, 0);
    for (size_t i = 0; i < selFrame_.size(); i += 4) {   // WS_BORDER frame colour
      selFrame_[i] = (uint8_t)fc[0]; selFrame_[i + 1] = (uint8_t)fc[1]; selFrame_[i + 2] = (uint8_t)fc[2];
    }
    const auto& cl = panel_->selectClient();
    for (int y = 0; y < ch; ++y) std::memcpy(&selFrame_[((size_t)(y + 1) * W + 1) * 4], &cl[(size_t)y * cw * 4], (size_t)cw * 4);
    const Rect r = selectRect();
    replicate(dst, dw, dh, rowBytes, selFrame_.data(), W, H, r.x, r.y, selScale_);
  }
}

PanelView::Rect PanelView::selectRect() const {
  const int w = (panel_->selectClientWidth() + 2) * selScale_, h = (panel_->selectClientHeight() + 2) * selScale_;
  int x = panelR_.x + panelR_.w / 2 - w / 2, y = panelR_.y + panelR_.h / 2 - h / 2;
  x = std::max(0, std::min(x, surfW_ - w));
  y = std::max(0, std::min(y, surfH_ - h));
  return {x, y, w, h};
}

bool PanelView::toPanel(float sx, float sy, int& px, int& py) const {
  const int x = (int)((sx - panelR_.x) / mult_ + (sx < panelR_.x ? -1 : 0));
  const int y = (int)((sy - panelR_.y) / mult_ + (sy < panelR_.y ? -1 : 0));
  px = x; py = y;
  return x >= 0 && y >= 0 && x < panelW() && y < panelH();
}

void PanelView::releaseAll() {
  std::vector<int> ids;
  for (auto& kv : region_) ids.push_back(kv.first);
  for (int id : ids) pointer(2, id, -100000.f, -100000.f);   // up far away: keys release, drags end
}

void PanelView::pointer(int action, int id, float sx, float sy, long long t) {
  if (action == 3) { releaseAll(); return; }
  const int k = nativeScale();
  if (action == 0) {
    region_[id] = kNone;
    int x, y;
    const bool inside = toPanel(sx, sy, x, y);
    if (panel_->selectOpen()) {
      const Rect sr = selectRect();
      if (sx >= sr.x && sx < sr.x + sr.w && sy >= sr.y && sy < sr.y + sr.h) region_[id] = kSelect;
      else panel_->selectDeactivate();            // click outside closes (WM_ACTIVATE 0), consumed
    } else if (sx >= kbR_.x && sx < kbR_.x + kbR_.w && sy >= kbR_.y && sy < kbR_.y + kbR_.h) {
      region_[id] = kKeys;
    } else if (sx >= barR_.x && sx < barR_.x + barR_.w && sy >= barR_.y && sy < barR_.y + barR_.h) {
      const float lx = (sx - barR_.x) / k, ly = (sy - barR_.y) / k;
      int mx, px, bw;
      barRects(mx, px, bw);
      if (ly >= 2 && ly < kBarH - 2) {
        if (lx >= mx && lx < mx + bw) { region_[id] = kBar; barBtn_[id] = 0; }
        else if (lx >= px && lx < px + bw) { region_[id] = kBar; barBtn_[id] = 1; }
      }
    } else if (inside) {
      region_[id] = kPanel;
    }
  }
  auto it = region_.find(id);
  if (it == region_.end()) return;
  const Region r = it->second;
  int x, y;
  toPanel(sx, sy, x, y);
  switch (r) {
    case kPanel:
      if (action == 0) panel_->pointerDown(id, x, y, t);
      else if (action == 1) panel_->pointerMove(id, x, y, t);   // drags continue outside the rect
      else panel_->pointerUp(id, x, y, t);
      break;
    case kSelect: {
      if (!panel_->selectOpen()) break;
      const Rect sr = selectRect();
      const int cx = (int)std::floor((sx - sr.x) / (float)selScale_) - 1,
                cy = (int)std::floor((sy - sr.y) / (float)selScale_) - 1;
      if (action == 0) panel_->selectPointerDown(cx, cy);
      else if (action == 1) panel_->selectPointerMove(cx, cy);
      else panel_->selectPointerUp(cx, cy);
      break;
    }
    case kKeys: {
      const double kx = (sx - kbR_.x) / (double)k, ky = (sy - kbR_.y) / (double)k;
      if (action == 0) { activeWheel_ = kb_->wheelAt(kx, ky); kb_->press(id, kx, ky); }
      else if (action == 1) kb_->move(id, kx, ky);
      else { kb_->release(id); activeWheel_ = -1; }
      break;
    }
    case kBar:
      if (action == 2) {
        const int which = barBtn_[id];
        const float lx = (sx - barR_.x) / k, ly = (sy - barR_.y) / k;
        int mx, px, bw;
        barRects(mx, px, bw);
        const int bx = which == 0 ? mx : px;
        if (lx >= bx && lx < bx + bw && ly >= 2 && ly < kBarH - 2) oct_.step(which == 0 ? -1 : 1);
        barBtn_.erase(id);
      }
      break;
    default: break;
  }
  if (action == 2) region_.erase(id);
}

}  // namespace synth1_android
