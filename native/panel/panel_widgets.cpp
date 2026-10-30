#include "panel_widgets.hpp"

#include <algorithm>
#include <cmath>

namespace synth1 {
namespace panel {

namespace {

inline int32_t s32(int64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v & 0xFFFFFFFFLL)); }

/* JS cdiv: (a / b) < 0 ? ceil(a / b) : floor(a / b) — C++11 truncating division. */
inline int64_t cdiv(int64_t a, int64_t b) { return a / b; }

inline bool inRect(int l, int t, int r, int b, int x, int y) {
  return x >= l && x < r && y >= t && y < b;
}

}   // namespace

bool parseWidgets(const Json& W, std::vector<Widget>& out, std::string& err) {
  if (!W.is_array()) {
    err = "widgets.json must be an array";
    return false;
  }
  out.clear();
  out.reserve(W.size());
  for (const Json& w : W) {
    Widget d;
    d.i = w.at("i").get<int>();
    d.cls = w.at("cls").get<std::string>();
    d.id = w.at("id").get<int>();
    for (int k = 0; k < 4; k++) d.rect[k] = w.at("rect")[k].get<int>();
    if (w.contains("label")) d.label = w.at("label").get<std::string>();
    for (const auto& kv : w.at("f").items()) {
      WidgetField fv;
      if (kv.value().is_object()) {
        fv.ref = kv.value().at("ref").get<int>();
      } else {
        fv.num = kv.value().get<int64_t>();
      }
      d.f[std::stoi(kv.key())] = fv;
    }
    if (w.contains("items") && w.at("items").is_array()) {
      d.hasItems = true;
      for (const Json& it : w.at("items")) {
        WidgetItem wi;
        wi.value = it.at("value").get<int>();
        if (it.contains("text") && !it.at("text").is_null()) wi.text = it.at("text").get<std::string>();
        d.items.push_back(std::move(wi));
      }
    }
    out.push_back(std::move(d));
  }
  return true;
}

Widgets::Widgets(const std::vector<Widget>& widgets, int slidertype) : slidertype(slidertype) {
  widgets_ = widgets;
  for (const Widget& w : widgets_) {
    byId_[w.id].push_back(w.i);   // hit-test / id order preserved
  }
}

int64_t Widgets::F(const Widget& w, int off) const {
  const auto it = w.f.find(off);
  if (it == w.f.end()) return 0;
  if (it->second.ref >= 0) return it->second.ref;   // JS: v.ref
  return it->second.num;
}

const Widget* Widgets::ref(const Widget& w, int off) const {
  const auto it = w.f.find(off);
  if (it == w.f.end() || it->second.ref < 0) return nullptr;
  const int i = it->second.ref;
  if (i < 0 || i >= static_cast<int>(widgets_.size())) return nullptr;
  return &widgets_[i];
}

int Widgets::paramOf(const Widget& w) const { return w.id >= 1000 ? w.id - 1000 : -1; }

void Widgets::send(int id, int value) {
  log.push_back({id, value});
  if (id >= 1000 && id <= 1098) values[id - 1000] = value;
  else if (id == 1254) values[254] = value;
  else if (id == 1252) values[252] = value;
  if (onCommand) onCommand(id, value);
}

bool Widgets::ledLit(const Widget& l) const {   // FUN_10012170: lit = (LED +0x80 == group value)
  const Widget* owner = ref(l, 0x8c);
  if (!owner) return F(l, 0x74) != 0;
  const int p = paramOf(*owner);
  if (p < 0) return false;
  const auto it = values.find(p);
  return it != values.end() && it->second == s32(F(l, 0x80));
}

int Widgets::lcdIndex(const Widget& w) const {   // FUN_10012380 (list types)
  const int p = paramOf(w);
  const auto vit = p >= 0 ? values.find(p) : values.end();
  for (size_t k = 0; k < w.items.size(); k++) {
    if (vit != values.end() && w.items[k].value == vit->second) return static_cast<int>(k);
  }
  return static_cast<int>(F(w, 0xe8));
}

int Widgets::knobValue(const Widget& w) const {
  const int p = w.id == 1254 ? 254 : paramOf(w);
  const auto it = p >= 0 ? values.find(p) : values.end();
  return it != values.end() ? it->second : static_cast<int>(s32(F(w, 0x78)));
}

int Widgets::knobOrLcd(const Widget& w) const {
  const int p = paramOf(w);
  const auto it = p >= 0 ? values.find(p) : values.end();
  return it != values.end() ? it->second : static_cast<int>(s32(F(w, 0x110)));
}

bool Widgets::hit(const Widget& w, int x, int y) const {
  const int* r = w.rect;
  if (w.cls == "CLED") {   // FUN_1000dd10
    int l = r[0];
    int ri = r[2];
    const int32_t ext = s32(F(w, 0x88));
    if (ext < 1) l += ext;
    else ri += ext;
    return inRect(l, r[1], ri, s32(F(w, 0x28)), x, y);
  }
  if (w.cls == "CLEDMatrix") return false;   // FUN_10016c70: return 0
  return inRect(r[0], r[1], r[2], r[3], x, y);   // FUN_1000dcf0
}

int Widgets::find(int x, int y) const {   // canvas FUN_10011bd0: first child that hits, id != 0
  for (const Widget& w : widgets_) {
    if (hit(w, x, y) && w.id != 0) return w.i;
  }
  return -1;
}

const Widget* firstCledOf(const std::vector<Widget>& W, int id) {
  for (const Widget& w : W) {
    if (w.id == id && w.cls == "CLED") return &w;
  }
  return nullptr;
}

void Widgets::pushDown(const Widget& w) {   // FUN_10010500
  hasPressed = true;
  pressed = Pressed{w.id, w.i, 0};
  const int64_t mode = F(w, 0x78);
  if (mode == 1) {
    const int lo = static_cast<int>(F(w, 0x7c));
    const int hi = static_cast<int>(F(w, 0x80));
    // FUN_1000fe00: first lit LED in id order lo..hi -> next id (wrap)
    int cur = hi;
    for (int id = lo; id <= hi; id++) {
      const Widget* l = firstCledOf(widgets_, id);
      if (l && ledLit(*l)) {
        cur = id;
        break;
      }
    }
    int next = cur + 1;
    if (next > hi) next = lo;
    const Widget* nl = firstCledOf(widgets_, next);
    const int value = nl ? static_cast<int>(s32(F(*nl, 0x80))) : 0;   // FUN_1000fbb0
    send(w.id, value);
  } else if (mode == 2 || mode == 3) {
    const Widget* l = ref(w, 0x84);
    if (l) {
      if (mode == 2) lcdNext(*l);
      else lcdPrev(*l);
    }
  } else {
    send(w.id, 0);
  }
}

void Widgets::pushLed(const Widget& owner, int ledId) {   // FUN_10010fd0
  if (F(owner, 0x78) != 1) return;
  const Widget* l = firstCledOf(widgets_, ledId);
  if (!l) return;
  if (F(owner, 0x88) != 0 && ledLit(*l)) {
    int alt = static_cast<int>(F(owner, 0x7c));
    if (ledId == alt) alt = static_cast<int>(F(owner, 0x80));
    l = firstCledOf(widgets_, alt);
    if (!l) return;
  }
  send(owner.id, static_cast<int>(s32(F(*l, 0x80))));
}

void Widgets::lcdNext(const Widget& w) {   // FUN_1000fd40
  const int64_t t = F(w, 0x98);
  if (t == 1) {
    const int v = knobOrLcd(w);
    const int lo = static_cast<int>(s32(F(w, 0x118)));
    const int hi = static_cast<int>(s32(F(w, 0x11c)));
    const int n = std::max(lo, std::min(hi, v + 1));
    if (n != v) send(w.id, n);
  } else if (t == 2 || t == 3) {
    int k = lcdIndex(w) + 1;
    if (k >= static_cast<int>(w.items.size())) k = 0;
    send(w.id, w.items[k].value);
  }
}

void Widgets::lcdPrev(const Widget& w) {   // FUN_1000df40
  const int64_t t = F(w, 0x98);
  if (t == 1) {
    const int v = knobOrLcd(w);
    const int lo = static_cast<int>(s32(F(w, 0x118)));
    const int hi = static_cast<int>(s32(F(w, 0x11c)));
    const int n = std::max(lo, std::min(hi, v - 1));
    if (n != v) send(w.id, n);
  } else if (t == 2 || t == 3) {
    int k = lcdIndex(w) - 1;
    if (k < 0) k = static_cast<int>(w.items.size()) - 1;
    send(w.id, w.items[k].value);
  }
}

void Widgets::spinDown(const Widget& w, int x, int y) {   // FUN_100106b0
  const int* r = w.rect;
  const Widget* link = ref(w, 0x88);
  bool upv;
  if (F(w, 0x84) == 0) upv = ((r[2] + r[0]) / 2) < x;
  else upv = y < ((r[3] + r[1]) / 2);
  hasPressed = true;
  pressed = Pressed{w.id, w.i, upv ? 1 : 2};
  if (link) {
    if (upv) lcdNext(*link);
    else lcdPrev(*link);
    return;
  }
  const int p = w.id == 1252 ? 252 : paramOf(w);
  const auto it = p >= 0 ? values.find(p) : values.end();
  const int old = it != values.end() ? it->second : static_cast<int>(s32(F(w, 0x78)));
  const int lo = static_cast<int>(s32(F(w, 0x80)));
  const int hi = static_cast<int>(s32(F(w, 0x7c)));
  const int v = std::max(lo, std::min(hi, old + (upv ? 1 : -1)));
  if (v != old) send(w.id, v);
}

void Widgets::toggleDown(const Widget& w) {   // FUN_10010790
  const int p = paramOf(w);
  const auto it = p >= 0 ? values.find(p) : values.end();
  const int cur = it != values.end() ? (it->second ? 1 : 0) : static_cast<int>(F(w, 0x74));
  send(w.id, cur ? 0 : 1);
}

void Widgets::ledDown(const Widget& w) {   // FUN_100104e0
  const Widget* owner = ref(w, 0x8c);
  if (owner && owner->cls == "CPushButton") pushLed(*owner, w.id);
}

void Widgets::knobDown(const Widget& w, int x, int y) {   // FUN_10010580
  (void)x;
  const int* r = w.rect;
  const int max = static_cast<int>(s32(F(w, 0x74)));
  if (slidertype == 1) {
    const int px = F(w, 0x8c) == 0 ? r[2] : r[0] - 0x12;
    openSlider(w, px, r[1] - 0x3c, 0, max, knobValue(w));
  } else {
    drag_ = Drag{&w, y, 0, max, knobValue(w), true};   // FUN_10012520
  }
}

void Widgets::lcdDown(const Widget& w, int x, int y) {   // FUN_10010150
  const int64_t kind = F(w, 0x94);
  if (kind == 0) {
    send(w.id, (x & 0xffff) | (y << 16));
    return;
  }
  if (kind == 1) {
    if (F(w, 0x98) != 1) return;
    const int lo = static_cast<int>(s32(F(w, 0x118)));
    const int hi = static_cast<int>(s32(F(w, 0x11c)));
    const int v = knobOrLcd(w);
    if (slidertype != 1) {
      drag_ = Drag{&w, y, lo, hi, v, true};
      return;
    }
    openSlider(w, w.rect[2], w.rect[1] - 0x3c, lo, hi, v);
    return;
  }
  if (kind == 2) openList(w);
}

/* ---- CVolSubWin popup (FUN_10010ba0 / paint FUN_10011800) ---- */

void Widgets::openSlider(const Widget& owner, int x, int y, int min, int max, int value) {
  Popup pop;
  pop.kind = 1;
  pop.owner = &owner;
  pop.ownerId = owner.id;
  pop.x = x;
  pop.y = y;
  pop.w = 0x12;
  pop.h = 0x96;
  pop.min = min;
  pop.max = max;
  pop.value = value;
  pop.last = value;
  pop.dragging = true;
  pop.cl = x + 1;
  pop.ct = y + 1;
  pop.cw = pop.w - 1 - 1;
  pop.ch = pop.h - 1 - 1;
  pop.px = x + 1;
  pop.py = y + 1;
  // WM_CREATE: cursor warped to (client.right - 2, top + (v-max)*(top-bottom)/(max-min))
  pop.warpX = pop.cl + pop.cw - 2 + 1;
  pop.warpY = pop.ct + static_cast<int>(cdiv(static_cast<int64_t>(value - max) * (0 - pop.ch), (max - min))) + 1;
  popup = pop;
}

void Widgets::sliderMove(Popup& pop, int y) {
  const int top = 0;
  const int bottom = pop.ch;
  const int ly = y - pop.ct;
  int v = static_cast<int>(cdiv(static_cast<int64_t>(bottom - ly - top) * (pop.max - pop.min), (bottom - top))) + pop.min;
  v = std::max(pop.min, std::min(pop.max, v));
  pop.value = v;
  if (v != pop.last) {
    ownerSet(pop.owner, v);
    pop.last = v;
  }
}

void Widgets::ownerSet(const Widget* w, int v) {   // owner vt+0x2c: FUN_100124f0 / FUN_100124a0
  if (w->cls == "CRoteryV") {
    send(w->id, v);
  } else if (w->cls == "CLCD") {
    if (F(*w, 0x98) == 1) {
      send(w->id, std::max(static_cast<int>(s32(F(*w, 0x118))), std::min(static_cast<int>(s32(F(*w, 0x11c))), v)));
    } else {
      send(w->id, v);
    }
  }
}

bool Widgets::sliderMouse(MouseType type, int x, int y) {
  Popup& pop = popup;
  const bool inside = inRect(pop.cl, pop.ct, pop.cl + pop.cw, pop.ct + pop.ch, x, y);
  if (type == MouseType::Move) {
    if (pop.dragging) sliderMove(pop, y);
    return true;
  }
  if (type == MouseType::Down || type == MouseType::DblClick) {
    if (!inside) {
      closePopup();   // ReleaseCapture + forward the click
      return false;
    }
    pop.dragging = true;
    sliderMove(pop, y);
    return true;
  }
  if (type == MouseType::Up) {
    pop.dragging = false;
    return true;
  }
  if (type == MouseType::RDown) {
    closePopup();
    return true;
  }
  return true;
}

/* ---- CLCDSubWin list popup (FUN_10010840 / paint FUN_1000e0f0) ---- */

void Widgets::openList(const Widget& w) {
  const int rows = static_cast<int>(F(w, 0xa4));
  const int n = static_cast<int>(w.items.size());
  const int cols = n / rows + (n % rows ? 1 : 0);
  popup = Popup();
  popup.kind = 2;
  popup.owner = &w;
  popup.ownerId = w.id;
  popup.rows = rows;
  popup.cols = cols;
  popup.count = n;
  popup.current = lcdIndex(w);
  popup.hover = lcdIndex(w);
  popup.geom = listGeomFn ? listGeomFn(w.id) : ListGeom();
}

int Widgets::listCell(const Popup& pop, int x, int y) const {   // FUN_1000fac0
  const ListGeom& g = pop.geom;
  if (!g.ok) return -1;
  const int lx = x - g.cl - g.mx;
  const int ly = y - g.ct - g.my;
  if (lx < 0 || ly < 0) return -1;
  const int c = lx / g.cellw;
  const int r = ly / g.cellh;
  if (c >= pop.cols || r >= pop.rows) return -1;
  return c * pop.rows + r;
}

bool Widgets::listMouse(MouseType type, int x, int y) {
  Popup& pop = popup;
  const ListGeom& g = pop.geom;
  const bool inside = g.ok && inRect(g.cl, g.ct, g.cl + g.cw, g.ct + g.ch, x, y);
  if (type == MouseType::Move) {
    const int k = listCell(pop, x, y);
    pop.hover = k < pop.count ? k : -1;
    return true;
  }
  if (type == MouseType::Down || type == MouseType::DblClick) {
    if (!inside) {
      closePopup();
      return false;
    }
    if (pop.hover != -1) {
      const WidgetItem& it = pop.owner->items[pop.hover];
      pop.current = pop.hover;
      send(pop.owner->id, it.value);
    }
    closePopup();
    return true;
  }
  if (type == MouseType::RDown) {
    closePopup();
    return true;
  }
  return true;
}

/* ---- canvas mouse entry ---- */

void Widgets::mouse(MouseType type, int px, int py) {
  const int x = px - 1;   // panel -> canvas client coords
  const int y = py - 1;
  if (drag_.active) {   // slidertype 0 drag (FUN_10011a80)
    if (type == MouseType::Up) {
      drag_.active = false;
      return;
    }
    if (type == MouseType::Move) {
      const int64_t iv = static_cast<int64_t>(y - drag_.y0) * (drag_.max - drag_.min);
      int v = drag_.start - static_cast<int>(cdiv(iv, 128));
      v = std::max(drag_.min, std::min(drag_.max, v));
      ownerSet(drag_.w, v);
    }
    return;
  }
  if (popup.kind != 0) {
    const bool handled = popup.kind == 1 ? sliderMouse(type, x, y) : listMouse(type, x, y);
    if (handled) return;
  }
  const int i = find(x, y);
  const int prev = hover;
  if (prev != -1 && prev != i) leave(widgets_[prev]);
  hover = i;
  if (i < 0) return;
  const Widget& w = widgets_[i];
  if (type == MouseType::Move) return;   // slot 0x10: no-op for every class
  if (type == MouseType::Up) {
    up(w);
    return;
  }
  if (type == MouseType::Down || type == MouseType::DblClick) down(w, x, y);   // slot 0x14
}

void Widgets::wheel(int delta) {   // only the slider popup reacts (case 0x20a)
  if (popup.kind != 1) return;
  int v = static_cast<int>(cdiv(delta, 0x78)) + popup.value;
  v = std::max(popup.min, std::min(popup.max, v));
  popup.value = v;
  ownerSet(popup.owner, v);
  popup.last = v;
}

bool Widgets::tapSafe(int i) const {
  if (i < 0 || i >= static_cast<int>(widgets_.size())) return false;
  const Widget& w = widgets_[i];
  if (w.cls == "CLED" || w.cls == "CToggleButton") return true;
  if (w.cls == "CLCD" && F(w, 0x94) == 0) return true;
  return false;
}

void Widgets::tapDown(int px, int py) {
  const int x = px - 1;
  const int y = py - 1;
  const int i = find(x, y);
  if (!tapSafe(i)) return;
  const Widget& w = widgets_[i];
  if (w.cls == "CLED") ledDown(w);
  else if (w.cls == "CToggleButton") toggleDown(w);
  else lcdDown(w, x, y);   // CLCD kind 0: send the coords (instantaneous)
}

bool Widgets::listCellHit(int px, int py) const {
  if (popup.kind != 2 || !popup.geom.ok) return false;
  const ListGeom& g = popup.geom;
  return inRect(g.cl, g.ct, g.cl + g.cw, g.ct + g.ch, px - 1, py - 1);
}

void Widgets::leave(const Widget& w) {   // slot 0xc: push button / spin release (FUN_100107e0)
  if ((w.cls == "CPushButton" || w.cls == "CSpin") && hasPressed && pressed.i == w.i) {
    hasPressed = false;
    const std::string id = std::to_string(w.id);
    const auto k = std::find(damage.begin(), damage.end(), id);   // its repaint now lies on top
    if (k != damage.end()) damage.erase(k);
    damage.push_back(id);
  }
}

void Widgets::up(const Widget& w) {   // slot 0x18: FUN_10010810 -> slot 0xc
  if (w.cls == "CPushButton" || w.cls == "CSpin") leave(w);
}

void Widgets::down(const Widget& w, int x, int y) {
  if (w.cls == "CPushButton") pushDown(w);
  else if (w.cls == "CToggleButton") toggleDown(w);
  else if (w.cls == "CLED") ledDown(w);
  else if (w.cls == "CSpin") spinDown(w, x, y);
  else if (w.cls == "CRoteryV") knobDown(w, x, y);
  else if (w.cls == "CLCD") lcdDown(w, x, y);
}

}   // namespace panel
}   // namespace synth1
