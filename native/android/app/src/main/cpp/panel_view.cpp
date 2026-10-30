#include "panel_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <initializer_list>

#include "ui_font.hpp"

namespace synth1_android {

using synth1::panel::Panel;
namespace ui = synth1::panel;

namespace {
constexpr uint32_t kClear = 0xFF87C8C8u;   // RGBA bytes (200,200,135,255): the classic frame colour

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

void fillRect(std::vector<uint8_t>& b, int W, int H, int x0, int y0, int x1, int y1, int r, int g, int bl) {
  for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
    for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
      uint8_t* p = &b[((size_t)y * W + x) * 4];
      p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)bl; p[3] = 255;
    }
}

// ---- anti-aliased primitives at device resolution (wheels) ----
inline void blendPx(uint8_t* dst, int dw, int dh, int rowBytes, int x, int y, const uint8_t rgb[3], float a) {
  if (x < 0 || y < 0 || x >= dw || y >= dh || a <= 0.f) return;
  uint8_t* p = dst + (size_t)y * rowBytes + (size_t)x * 4;
  if (a > 1.f) a = 1.f;
  for (int i = 0; i < 3; ++i) p[i] = (uint8_t)(p[i] + (rgb[i] - p[i]) * a + 0.5f);
  p[3] = 255;
}
float sdRoundBox(float px, float py, float cx, float cy, float hw, float hh, float r) {
  const float qx = std::fabs(px - cx) - hw + r, qy = std::fabs(py - cy) - hh + r;
  const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}
// fill (stroke <= 0) or outline (stroke width) of a rounded box [x,x+w) x [y,y+h)
void roundBox(uint8_t* dst, int dw, int dh, int rowBytes, float x, float y, float w, float h, float r, const uint8_t rgb[3],
              float stroke) {
  const float cx = x + w / 2, cy = y + h / 2, hw = w / 2, hh = h / 2;
  for (int py = (int)std::floor(y) - 1; py <= (int)std::ceil(y + h) + 1; ++py)
    for (int px = (int)std::floor(x) - 1; px <= (int)std::ceil(x + w) + 1; ++px) {
      const float d = sdRoundBox(px + 0.5f, py + 0.5f, cx, cy, hw, hh, r);
      const float a = stroke > 0 ? 0.5f - (std::fabs(d + stroke / 2) - stroke / 2) : 0.5f - d;
      blendPx(dst, dw, dh, rowBytes, px, py, rgb, std::min(1.f, a));
    }
}
// axis-aligned box with fractional edges (area coverage)
void boxAA(uint8_t* dst, int dw, int dh, int rowBytes, float x0, float y0, float x1, float y1, const uint8_t rgb[3]) {
  for (int py = (int)std::floor(y0); py < (int)std::ceil(y1); ++py)
    for (int px = (int)std::floor(x0); px < (int)std::ceil(x1); ++px) {
      const float cx = std::min<float>(px + 1, x1) - std::max<float>(px, x0), cy = std::min<float>(py + 1, y1) - std::max<float>(py, y0);
      blendPx(dst, dw, dh, rowBytes, px, py, rgb, cx * cy);
    }
}
}  // namespace

bool PanelView::init(const std::string& root, int w, int h, std::string& err, bool kbVisible, float dpi) {
  root_ = root; surfW_ = w; surfH_ = h; kbVisible_ = kbVisible; dpi_ = dpi;
  int w1 = kPanelW, h1 = kPanelH;
  {
    Panel probe;
    std::string e;
    if (probe.load(root + "/200", 2, e)) { w1 = probe.width() / 2; h1 = probe.height() / 2; }
  }
  const double mmpx = dpi / 25.4;
  // ---- orientation + the largest integer scale that fits the WHOLE screen ----
  landscape_ = w >= h && w >= w1 + 2 * kMinColW;
  int k = 1;
  if (landscape_) {
    for (int c = std::min(w / w1, h / h1); c >= 1; --c)
      if (w - w1 * c >= 2 * kMinColW * c) { k = c; break; }
  } else {
    k = std::max(1, w / w1);
    while (k > 1 && h1 * k > h) --k;
  }
  skin_ = (k % 2 == 0) ? 2 : 1;
  mult_ = k / skin_;
  panel_.reset(new Panel());
  if (!panel_->load(root + (skin_ == 2 ? "/200" : "/100"), skin_, err)) return false;
  panel_->setTouchMode(true);
  panel_->setSelectLabels(true);
  selScale_ = std::max(1, std::min(w / (panel_->selectClientWidth() + 2), h / (panel_->selectClientHeight() + 2)));

  // ---- geometry (surface px) ----
  const int pwk = w1 * k, phk = h1 * k;
  boxL_.scale = boxR_.scale = k;
  int kbH;
  if (landscape_) {
    kbH = (int)std::max(0.40 * h, 250.0);
    overlay_ = true;
    const int leftover = w - pwk, lpx = leftover / 2, rpx = leftover - lpx;
    const int py0 = (h - phk) / 2;                                   // panel centred vertically
    panelR_ = {lpx, py0, pwk, phk};
    boxL_.lw = (lpx + k - 1) / k; boxL_.lh = (h + k - 1) / k; boxL_.px = lpx - boxL_.lw * k; boxL_.py = 0;
    boxR_.lw = (rpx + k - 1) / k; boxR_.lh = boxL_.lh; boxR_.px = lpx + pwk; boxR_.py = 0;
    ovR_ = {0, h - kbH, w, kbH};
  } else {
    kbH = (int)std::max(250.0, 32.0 * mmpx);                         // a normal keyboard height (~32 mm)
    const int block = phk + kStripH * k + kbH;
    overlay_ = block > h;
    const int y0 = overlay_ ? 0 : (h - block) / 2;
    panelR_ = {(w - pwk) / 2, y0, pwk, phk};
    boxL_.lw = 0; boxL_.lh = 0; boxL_.px = 0; boxL_.py = 0;
    boxR_.px = 0; boxR_.py = y0 + phk; boxR_.lw = (w + k - 1) / k; boxR_.lh = kStripH;
    ovR_ = overlay_ ? Rect{0, h - kbH, w, kbH} : Rect{0, boxR_.py + kStripH * k, w, kbH};
    if (ovR_.y + ovR_.h > h) ovR_.h = std::max(0, h - ovR_.y);
  }
  boxL_.title = "KEYS"; boxR_.title = "VIEW";
  boxL_.buf.assign((size_t)boxL_.lw * boxL_.lh * 4, 0);
  boxR_.buf.assign((size_t)boxR_.lw * boxR_.lh * 4, 0);

  // ---- overlay content: wheels (<= 12 mm each) at the left end, keys (~6.5 mm white keys) ----
  {
    const double ww = std::min(12.0 * mmpx, 0.26 * ovR_.h);
    const int iww = std::max(8, (int)ww), gap = iww / 2, pad = std::max(4, iww * 3 / 10);
    const int wl = 2 * iww + gap + 2 * pad + pad / 2;
    const int y0 = ovR_.y + ovBorder_ + pad, hh = std::max(8, ovR_.h - ovBorder_ - 2 * pad);
    wheelRc_[0] = {ovR_.x + pad, y0, iww, hh};
    wheelRc_[1] = {ovR_.x + pad + iww + gap, y0, iww, hh};
    keysR_ = {ovR_.x + wl, ovR_.y + ovBorder_, ovR_.w - wl, ovR_.h - ovBorder_};
    kbLogicalW_ = (keysR_.w + k - 1) / k;
    kbLogicalH_ = std::max(8, (keysR_.h + k - 1) / k);
    const double keyPx = 6.5 * mmpx;
    octaves_ = std::max(1, std::min(8, (int)std::lround((keysR_.w / keyPx) / 7.0)));
    kbLow_ = 60 - 12 * (octaves_ / 2);
    synth1::keyboard::Keyboard::Options o;
    o.low = kbLow_; o.high = kbLow_ + 12 * octaves_; o.height = kbLogicalH_; o.wheels = false;
    kb_.reset(new synth1::keyboard::Keyboard(o));
    kb_->layout(kbLogicalW_, kbLogicalH_);
  }
  wireKeyboard();
  layoutCells();
  region_.clear(); btn_.clear(); heldWheel_.clear(); activeWheel_ = -1;
  return true;
}

bool PanelView::rebuild(std::string& err) {
  std::map<int, int> vals;
  for (int id = 0; id <= 98; ++id) if (panel_->hasParam(id)) vals[id] = panel_->getParam(id);
  if (panel_->hasParam(254)) vals[254] = panel_->getParam(254);
  const int bank = panel_->currentBank(), prog = panel_->currentProgram();
  if (!init(root_, surfW_, surfH_, err, kbVisible_, dpi_)) return false;
  panel_->loadProgram(bank, prog);
  panel_->setValues(vals);
  if (onRebuilt) onRebuilt();
  return true;
}

void PanelView::wireKeyboard() {
  kb_->onNoteOn = [this](int n, int v) { const int e = oct_.noteOn(n); if (onNoteOn) onNoteOn(e, v); };
  kb_->onNoteOff = [this](int n) { const int e = oct_.noteOff(n); if (onNoteOff) onNoteOff(e); };
}

// Buttons: landscape: VIEW column (right) = KBD, INFO, PANIC; KEYS column (left) = OCT -, readout,
// OCT +, MIDI lamp; portrait: one strip with all of them. Always above the overlay's top edge.
void PanelView::layoutCells() {
  cells_.clear();
  const int gap = 4, m = 6, pitch = 24;
  auto grid = [&](int box, std::initializer_list<std::pair<Item, int>> items, int lw, int maxCols, int top) {
    int cols = std::max(1, std::min(maxCols, (lw - 2 * m + gap) / (48 + gap)));
    const int cw = (lw - 2 * m - (cols - 1) * gap) / cols;
    int row = 0, col = 0;
    for (auto& it : items) {
      const int span = std::min(it.second, cols);
      if (col + span > cols) { ++row; col = 0; }
      cells_.push_back({it.first, box, m + col * (cw + gap), top + row * pitch, cw * span + (span - 1) * gap, kBtnH});
      col += span;
      if (col >= cols) { ++row; col = 0; }
    }
  };
  const int top = kHeaderH + 6;
  if (landscape_) {
    grid(1, {{kKbd, 1}, {kInfo, 1}, {kPanic, 1}}, boxR_.lw, 3, top);
    grid(0, {{kOctDown, 1}, {kOctLcd, 1}, {kOctUp, 1}, {kMidi, 3}}, boxL_.lw, 3, top);
  } else {
    const int stripTop = kHeaderH + std::max(4, (boxR_.lh - kHeaderH - 44) / 2);
    grid(1, {{kKbd, 1}, {kInfo, 1}, {kPanic, 1}, {kMidi, 1}, {kOctDown, 1}, {kOctLcd, 1}, {kOctUp, 1}}, boxR_.lw, 4, stripTop);
  }
}

PanelView::Rect PanelView::itemRect(Item i) const {
  for (const Cell& c : cells_)
    if (c.item == i) {
      const Box& b = c.box == 0 ? boxL_ : boxR_;
      const int k = nativeScale();
      return {b.px + c.x * k, b.py + c.y * k, c.w * k, c.h * k};
    }
  return {0, 0, 0, 0};
}

// classic keycap (tools/classic.py): outline, face+70 bevel, grey (200) unlit / (255,255,140) lit
void PanelView::drawKeycap(Box& b, const Cell& c, const char* label, bool lit, bool pressed) {
  const int W = b.lw, H = b.lh, x = c.x, y = c.y, w = c.w, h = c.h;
  const int fr = lit ? 255 : 200, fg = lit ? 255 : 200, fb = lit ? 140 : 200;
  auto lighter = [](int v) { return std::min(255, v + 70); };
  fillRect(b.buf, W, H, x, y, x + w, y + h, pressed ? 96 : 0, pressed ? 96 : 0, pressed ? 96 : 0);
  fillRect(b.buf, W, H, x, y, x + w - 1, y + h - 1, pressed ? 0 : lighter(fr), pressed ? 0 : lighter(fg),
           pressed ? 0 : lighter(fb));
  fillRect(b.buf, W, H, x + 1, y + 1, x + w - 1, y + h - 1, fr, fg, fb);
  const int cx[4] = {x, x + w - 1, x, x + w - 1}, cy[4] = {y, y, y + h - 1, y + h - 1};
  for (int i = 0; i < 4; ++i) fillRect(b.buf, W, H, cx[i], cy[i], cx[i] + 1, cy[i] + 1, 200, 200, 135);
  const uint8_t black[3] = {0, 0, 0};
  const auto& mono = ui::uiFontMonoBold();
  const int tw = ui::uiTextWidth(mono, label), o = pressed ? 1 : 0;
  ui::uiDrawText(b.buf.data(), W, H, x + (w - tw) / 2 + o, y + h / 2 + 4 + o, label, black, mono);
}

void PanelView::paintBoxes() {
  const uint8_t title[3] = {200, 200, 200};
  const auto& mono = ui::uiFontMonoBold();
  for (Box* b : {&boxL_, &boxR_}) {
    if (b->lw <= 0) continue;
    fillRect(b->buf, b->lw, b->lh, 0, 0, b->lw, b->lh, 60, 60, 60);                 // 1 px line
    fillRect(b->buf, b->lw, b->lh, 1, 1, b->lw - 1, b->lh - 1, 200, 200, 135);      // box fill
    fillRect(b->buf, b->lw, b->lh, 1, 1, b->lw - 1, kHeaderH, 60, 60, 60);          // header bar
    ui::uiDrawText(b->buf.data(), b->lw, b->lh, 4, 10, b->title, title, mono);
  }
  for (const Cell& c : cells_) {
    Box& bx = c.box == 0 ? boxL_ : boxR_;
    bool pressed = false;
    for (auto& kv : btn_) if (kv.second == c.item) pressed = true;
    if (c.item == kKbd) drawKeycap(bx, c, "KBD", kbVisible_, pressed);
    else if (c.item == kInfo) drawKeycap(bx, c, "INFO", info_, pressed);
    else if (c.item == kPanic) drawKeycap(bx, c, "PANIC", false, pressed);
    else if (c.item == kOctDown) drawKeycap(bx, c, "OCT -", false, pressed);
    else if (c.item == kOctUp) drawKeycap(bx, c, "OCT +", false, pressed);
    else if (c.item == kOctLcd) {   // classic teal LCD: black window, teal rim, teal text
      fillRect(bx.buf, bx.lw, bx.lh, c.x, c.y, c.x + c.w, c.y + c.h, 0, 86, 75);
      fillRect(bx.buf, bx.lw, bx.lh, c.x + 1, c.y + 1, c.x + c.w - 1, c.y + c.h - 1, 0, 0, 0);
      char buf[16];
      std::snprintf(buf, sizeof buf, "C%d", kbLow_ / 12 - 1 + oct_.octave());
      const uint8_t teal[3] = {0, 210, 185};
      const int tw = ui::uiTextWidth(mono, buf);
      ui::uiDrawText(bx.buf.data(), bx.lw, bx.lh, c.x + (c.w - tw) / 2, c.y + c.h / 2 + 4, buf, teal, mono);
    } else if (c.item == kMidi) {    // MIDI-in lamp (classic green LED)
      const uint8_t dark[3] = {60, 60, 60};
      ui::uiDrawText(bx.buf.data(), bx.lw, bx.lh, c.x + 2, c.y + c.h / 2 + 4, "MIDI", dark, mono);
      const int lx = c.x + 2 + ui::uiTextWidth(mono, "MIDI") + 8, ly = c.y + c.h / 2;
      for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
          const int d2 = dx * dx + dy * dy;
          if (d2 > 20) continue;
          if (d2 > 14) fillRect(bx.buf, bx.lw, bx.lh, lx + dx, ly + dy, lx + dx + 1, ly + dy + 1, 60, 60, 60);
          else if (midiLamp_) fillRect(bx.buf, bx.lw, bx.lh, lx + dx, ly + dy, lx + dx + 1, ly + dy + 1, 40, 255, 40);
          else fillRect(bx.buf, bx.lw, bx.lh, lx + dx, ly + dy, lx + dx + 1, ly + dy + 1, 0, 72, 0);
        }
    }
  }
}

// Keyboard overlay: opaque classic-khaki bar with a dark top edge, wheels at the left end, keys.
void PanelView::drawOverlay(uint8_t* dst, int dw, int dh, int rowBytes) {
  const int k = nativeScale();
  for (int y = ovR_.y; y < std::min(dh, ovR_.y + ovR_.h); ++y) {
    if (y < 0) continue;
    uint32_t* row = reinterpret_cast<uint32_t*>(dst + (size_t)y * rowBytes);
    const uint32_t c = y < ovR_.y + ovBorder_ ? 0xFF3C3C3Cu : kClear;   // (60,60,60) top edge
    for (int x = std::max(0, ovR_.x); x < std::min(dw, ovR_.x + ovR_.w); ++x) row[x] = c;
  }
  keysFrame_.assign((size_t)kbLogicalW_ * kbLogicalH_ * 4, 255);
  std::set<int> lit;
  for (int n : kb_->activeNotes()) lit.insert(n);
  kb_->render(kbLogicalW_, kbLogicalH_, lit, -1, keysFrame_.data());
  // clip the replicated keys to the keys rect (they can overhang by < k px)
  const int ow = std::min(dw, keysR_.x + keysR_.w), oh = std::min(dh, keysR_.y + keysR_.h);
  replicate(dst, ow, oh, rowBytes, keysFrame_.data(), kbLogicalW_, kbLogicalH_, keysR_.x, keysR_.y, k);
  drawWheels(dst, dw, dh, rowBytes);
}

// Wheels at native device resolution: dark well with ribs, light ribbed cap (classic palette of the
// keyboard port's wheel design), anti-aliased. Pitch cap position from pitch_, mod from mod_.
void PanelView::drawWheels(uint8_t* dst, int dw, int dh, int rowBytes) {
  const uint8_t well[3] = {0x14, 0x14, 0x14}, edge[3] = {0x7c, 0x7c, 0x74}, ribA[3] = {0x24, 0x24, 0x24},
                ribB[3] = {0x08, 0x08, 0x08}, capc[3] = {0xf7, 0xf7, 0xf3}, capHi[3] = {0x3d, 0x7e, 0xc4},
                notch[3] = {0x8b, 0x8b, 0x84};
  for (int i = 0; i < 2; ++i) {
    const Rect r = wheelRc_[i];
    const float x = (float)r.x, y = (float)r.y, w = (float)r.w, h = (float)r.h;
    const float sw = std::max(1.2f, w * 0.03f), rad = std::max(3.f, w * 0.18f);
    roundBox(dst, dw, dh, rowBytes, x, y, w, h, rad, well, 0);
    const float pitch = std::max(4.f, w * 0.14f);
    int n = 0;
    for (float ry = y + sw + 2; ry + pitch * 0.45f < y + h - sw - 2; ry += pitch, ++n)
      boxAA(dst, dw, dh, rowBytes, x + w * 0.12f, ry, x + w * 0.88f, ry + pitch * 0.45f, (n & 1) ? ribA : ribB);
    roundBox(dst, dw, dh, rowBytes, x + sw / 2, y + sw / 2, w - sw, h - sw, rad, edge, sw);
    if (i == 0) {   // pitch: centre notches on both sides
      const float my = y + h / 2 - w * 0.12f;
      boxAA(dst, dw, dh, rowBytes, x, my, x + sw * 1.5f, my + w * 0.24f, notch);
      boxAA(dst, dw, dh, rowBytes, x + w - sw * 1.5f, my, x + w, my + w * 0.24f, notch);
    }
    const float capW = w * 0.80f, capH = w * 0.95f, pad = w * 0.08f;
    const float travel = std::max(0.f, (h - capH) / 2 - pad);
    const float t = i == 0 ? (pitch_ + 8192) / 16383.f : mod_ / 127.f;
    const float cy = y + h / 2 - (2 * t - 1) * travel, cx = x + w / 2;
    const bool act = activeWheel_ == i;
    roundBox(dst, dw, dh, rowBytes, cx - capW / 2, cy - capH / 2, capW, capH, w * 0.14f, act ? capHi : capc, 0);
    roundBox(dst, dw, dh, rowBytes, cx - capW / 2 + sw / 2, cy - capH / 2 + sw / 2, capW - sw, capH - sw, w * 0.14f, edge, sw);
    for (int g = -1; g <= 1; ++g)
      boxAA(dst, dw, dh, rowBytes, cx + g * capW * 0.2f - sw * 0.6f, cy - capH * 0.32f, cx + g * capW * 0.2f + sw * 0.6f,
            cy + capH * 0.32f, (g == 0 && i == 0) ? notch : ribB);
  }
}

// INFO overlay at a readable size (about 2 mm text), anti-aliased, over the keyboard area.
void PanelView::drawInfo(uint8_t* dst, int dw, int dh, int rowBytes) {
  if (!info_) return;
  const double mmpx = dpi_ / 25.4;
  int px = 0;
  const auto& f = ui::uiFontRegularPx((int)std::lround(2.0 * mmpx), &px);
  std::vector<std::string> lines;
  size_t s = 0;
  while (s <= infoText_.size()) {
    size_t e = infoText_.find('|', s);
    if (e == std::string::npos) e = infoText_.size();
    std::string l = infoText_.substr(s, e - s);
    while (!l.empty() && l.front() == ' ') l.erase(l.begin());
    while (!l.empty() && l.back() == ' ') l.pop_back();
    if (!l.empty()) lines.push_back(l);
    s = e + 1;
  }
  int tw = 0;
  for (auto& l : lines) tw = std::max(tw, ui::uiTextWidth(f, l));
  const int pad = px / 2, lh = px * 5 / 4;
  Rect a = ovR_;
  const int bw = std::min(a.w - 2 * pad, tw + 2 * pad), bh = std::min(a.h - 2 * pad, (int)lines.size() * lh + pad);
  if (bw <= 0 || bh <= 0) return;
  const int bx = a.x + pad, by = a.y + pad;
  std::vector<uint8_t> tmp((size_t)bw * bh * 4);
  for (int y = 0; y < bh; ++y)
    for (int x = 0; x < bw; ++x) {
      const int sx = bx + x, sy = by + y;
      uint8_t* t = &tmp[((size_t)y * bw + x) * 4];
      if (sx < 0 || sy < 0 || sx >= dw || sy >= dh) { t[0] = t[1] = t[2] = 0; t[3] = 255; continue; }
      const uint8_t* p = dst + (size_t)sy * rowBytes + (size_t)sx * 4;
      t[0] = (uint8_t)((p[0] * 40 + 20 * 215) / 255); t[1] = (uint8_t)((p[1] * 40 + 20 * 215) / 255);
      t[2] = (uint8_t)((p[2] * 40 + 24 * 215) / 255); t[3] = 255;
    }
  const uint8_t white[3] = {245, 245, 245};
  int base = pad / 2 + f.ascent;
  for (auto& l : lines) { ui::uiDrawText(tmp.data(), bw, bh, pad, base, l, white, f); base += lh; }
  for (int y = 0; y < bh; ++y)
    for (int x = 0; x < bw; ++x) {
      const int sx = bx + x, sy = by + y;
      if (sx < 0 || sy < 0 || sx >= dw || sy >= dh) continue;
      std::memcpy(dst + (size_t)sy * rowBytes + (size_t)sx * 4, &tmp[((size_t)y * bw + x) * 4], 4);
    }
}

void PanelView::setWheelFromY(int wheel, float sy) {
  const Rect r = wheelRc_[wheel];
  const float w = (float)r.w, capH = w * 0.95f, pad = w * 0.08f;
  const float travel = std::max(1.f, (r.h - capH) / 2 - pad);
  const float t = std::max(0.f, std::min(1.f, 0.5f + (r.y + r.h / 2.f - sy) / (2 * travel)));
  if (wheel == 0) {
    const int v = std::max(-8192, std::min(8191, (int)std::lround(-8192 + t * 16383)));
    if (v != pitch_) { pitch_ = v; if (onPitchBend) onPitchBend(v); }
  } else {
    const int v = std::max(0, std::min(127, (int)std::lround(t * 127)));
    if (v != mod_) { mod_ = v; if (onModWheel) onModWheel(v); }
  }
}

void PanelView::composePanel(std::vector<uint8_t>& rgba) {
  rgba.assign((size_t)panelW() * panelH() * 4, 0);
  panel_->render(rgba.data());
}

void PanelView::blit(uint8_t* dst, int dw, int dh, int rowBytes) {
  composePanel(frame_);
  for (int y = 0; y < dh; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(dst + (size_t)y * rowBytes);
    for (int x = 0; x < dw; ++x) row[x] = kClear;
  }
  const int k = nativeScale();
  paintBoxes();
  if (boxL_.lw > 0) replicate(dst, dw, dh, rowBytes, boxL_.buf.data(), boxL_.lw, boxL_.lh, boxL_.px, boxL_.py, k);
  replicate(dst, dw, dh, rowBytes, boxR_.buf.data(), boxR_.lw, boxR_.lh, boxR_.px, boxR_.py, k);
  replicate(dst, dw, dh, rowBytes, frame_.data(), panelW(), panelH(), panelR_.x, panelR_.y, mult_);
  if (kbVisible_) drawOverlay(dst, dw, dh, rowBytes);
  drawInfo(dst, dw, dh, rowBytes);
  if (panel_->selectOpen()) {
    const int cw = panel_->selectClientWidth(), ch = panel_->selectClientHeight(), W = cw + 2, H = ch + 2;
    const auto fc = panel_->selectFrameColor();
    std::vector<uint8_t> sel((size_t)W * H * 4, 0);
    for (size_t i = 0; i < sel.size(); i += 4) { sel[i] = (uint8_t)fc[0]; sel[i + 1] = (uint8_t)fc[1]; sel[i + 2] = (uint8_t)fc[2]; }
    const auto& cl = panel_->selectClient();
    for (int y = 0; y < ch; ++y) std::memcpy(&sel[((size_t)(y + 1) * W + 1) * 4], &cl[(size_t)y * cw * 4], (size_t)cw * 4);
    const Rect r = selectRect();
    replicate(dst, dw, dh, rowBytes, sel.data(), W, H, r.x, r.y, selScale_);
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
  for (int id : ids) pointer(2, id, -100000.f, -100000.f);
}

void PanelView::doItem(Item it) {
  switch (it) {
    case kKbd: kbVisible_ = !kbVisible_; break;          // overlay only: panel scale/position unchanged
    case kInfo: info_ = !info_; break;
    case kOctDown: oct_.step(-1); break;
    case kOctUp: oct_.step(+1); break;
    case kPanic:
      kb_->allNotesOff();
      for (int n : oct_.releaseAll()) if (onNoteOff) onNoteOff(n);
      if (onAllNotesOff) onAllNotesOff();
      break;
    default: break;
  }
}

void PanelView::pointer(int action, int id, float sx, float sy, long long t) {
  if (action == 3) { releaseAll(); return; }
  const int k = nativeScale();
  auto in = [](const Rect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
  if (action == 0) {
    region_[id] = kNone;
    int x, y;
    const bool inside = toPanel(sx, sy, x, y);
    if (panel_->selectOpen()) {
      if (in(selectRect(), sx, sy)) region_[id] = kSelect;
      else panel_->selectDeactivate();            // click outside closes (WM_ACTIVATE 0), consumed
    } else {
      bool hit = false;
      for (Item it : {kKbd, kInfo, kPanic, kOctDown, kOctUp})
        if (in(itemRect(it), sx, sy)) { region_[id] = kButton; btn_[id] = it; hit = true; break; }
      if (!hit && kbVisible_ && in(ovR_, sx, sy)) {        // the overlay swallows its whole area
        hit = true;
        region_[id] = kSink;
        for (int i = 0; i < 2; ++i) {
          const Rect r = wheelRc_[i];
          if (sx >= r.x - r.w * 0.25f && sx < r.x + r.w * 1.25f && sy >= r.y && sy < r.y + r.h) {
            region_[id] = kWheel; heldWheel_[id] = i; activeWheel_ = i;
          }
        }
        if (region_[id] == kSink && in(keysR_, sx, sy)) region_[id] = kKeys;
      }
      if (!hit && inside) region_[id] = kPanel;
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
      else if (action == 1) panel_->pointerMove(id, x, y, t);
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
      const double kx = (sx - keysR_.x) / (double)k, ky = (sy - keysR_.y) / (double)k;
      if (action == 0) kb_->press(id, kx, ky);
      else if (action == 1) kb_->move(id, kx, ky);
      else kb_->release(id);
      break;
    }
    case kWheel: {
      const int wi = heldWheel_[id];
      if (action == 2) {
        heldWheel_.erase(id);
        bool still = false;
        for (auto& kv : heldWheel_) if (kv.second == wi) still = true;
        if (!still && wi == 0 && pitch_ != 0) { pitch_ = 0; if (onPitchBend) onPitchBend(0); }   // pitch springs back
        if (!still && activeWheel_ == wi) activeWheel_ = -1;
      } else {
        setWheelFromY(wi, sy);
      }
      break;
    }
    case kButton:
      if (action == 2) {
        const Item bi = (Item)btn_[id];
        btn_.erase(id);
        if (in(itemRect(bi), sx, sy)) doItem(bi);
      }
      break;
    default: break;
  }
  if (action == 2) region_.erase(id);
}

}  // namespace synth1_android
