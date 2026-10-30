#include "panel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace synth1 {
namespace panel {

namespace {

/* panel_app.js parseSy1: first line = name (trailing whitespace stripped), then "id,value" rows. */
struct Sy1Parsed {
  std::string name;
  std::map<int, int> values;
};

Sy1Parsed parseSy1(const std::string& text) {
  Sy1Parsed s;
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    std::string line = text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
    start = nl + 1;
  }
  if (!lines.empty()) {
    std::string n = lines[0];
    size_t e = n.size();
    while (e > 0 && (unsigned char)n[e - 1] <= ' ') e--;
    s.name = n.substr(0, e);
  }
  for (size_t i = 1; i < lines.size(); i++) {
    const std::string& l = lines[i];
    size_t j = 0;
    while (j < l.size() && isdigit((unsigned char)l[j])) j++;
    if (j == 0 || j >= l.size() || l[j] != ',') continue;
    const int id = std::stoi(l.substr(0, j));
    size_t k = j + 1;
    int sign = 1;
    if (k < l.size() && l[k] == '-') {
      sign = -1;
      k++;
    }
    size_t d0 = k;
    while (k < l.size() && isdigit((unsigned char)l[k])) k++;
    if (k == d0) continue;
    const int value = sign * std::stoi(l.substr(d0, k - d0));
    size_t rest = k;
    while (rest < l.size() && (unsigned char)l[rest] <= ' ') rest++;
    if (rest != l.size()) continue;
    s.values[id] = value;
  }
  return s;
}

const char* halfSuffix(int half) {
  if (half == 1) return "u";
  if (half == 2) return "d";
  return "";
}

}   // namespace

bool Panel::load(const std::string& skinDir, int scale, std::string& err) {
  scale_ = scale >= 2 ? 2 : 1;
  // A re-load (host scale change 1x <-> 2x) must start from empty asset objects: PanelCore,
  // PopupAssets and SelectWin append while parsing, so a second load kept the first skin's
  // control table and drew 1x-geometry tiles into the 2x buffer (u3b, 2026-10-22). Patch
  // state (values, LCD text, current bank/program) is kept.
  core_ = PanelCore();
  skin_ = Skin();
  banks_.clear();
  pops_ = PopupAssets();
  sel_ = SelectWin();
  opt_ = OptDialog();
  popComp_ = Composed();
  popShift_[0] = popShift_[1] = 0;
  popVisible_ = false;
  warp_[0] = warp_[1] = 0;
  popOpenKey_ = -1;
  lastXY_[0] = lastXY_[1] = 0;
  ptrs_.clear();
  owner_ = -1;
  ownerWidget_ = -1;
  tapWidget_ = -1;
  if (!loadSkin(skinDir, skin_, err)) return false;
  if (!core_.load(skin_.panelData, skin_.base.rgba, skin_.atlas.rgba, skin_.atlas.w, err)) return false;
  std::vector<Widget> ws;
  if (!parseWidgets(skin_.widgets, ws, err)) return false;
  emu_ = Widgets(ws, 1);
  if (skin_.hasPopups) {
    if (!parsePopupAssets(skin_.popups, pops_, err)) return false;
    emu_.listGeomFn = [this](int ownerId) { return listGeom(pops_, ownerId); };
  }
  if (skin_.hasBanks) parseBanks(skin_.banks, banks_, err);
  if (skin_.hasSelect) {
    if (!sel_.load(skin_.select, skin_.selectPng.rgba, banks_, err)) return false;
    sel_.onLoad = [this](int b, int k) { loadProgram(b, k); };
  }
  emu_.values[254] = 64;   // master volume: not per program (boot value 64)
  emu_.onCommand = [this](int id, int value) {
    if (id == 3 || id == 5) {
      openSelect();
      return;
    }
    if (id == 1252) {
      loadProgram(curBank_, value);
      return;
    }
    if (id == 94) {   // 'write': the host serializes the current panel state (web: download)
      if (onCommand) onCommand(94, value);
      return;
    }
    if (id >= 1000 && id <= 1098) {
      if (onParam) onParam(id - 1000, value);
    } else if (id == 1254) {
      if (onParam) onParam(254, value);
    }
    if (onCommand) onCommand(id, value);
  };
  return true;
}

void Panel::openSelect() {
  if (!skin_.hasSelect) return;
  sel_.open(curBank_, curProgram_);
}

void Panel::paintPopShift() {
  // The web keeps the popup inside the page; with the panel at the page origin (PAD 0) that is
  // dx = max(c.x, 0), dy = max(c.y, 0) and the content shifted by (dx-c.x, dy-c.y).
  popComp_ = Composed();
  if (skin_.hasPopups && emu_.popup.kind) {
    Composed c;
    if (composePopup(emu_.popup, pops_, skin_.popupsPng, c)) {
      popComp_ = std::move(c);
      const int dx = std::max(popComp_.x, 0);
      const int dy = std::max(popComp_.y, 0);
      popShift_[0] = dx - popComp_.x;
      popShift_[1] = dy - popComp_.y;
      popRect_[0] = dx;
      popRect_[1] = dy;
      popRect_[2] = popComp_.w;
      popRect_[3] = popComp_.h;
      popVisible_ = true;
      return;
    }
  }
  popShift_[0] = popShift_[1] = 0;
  popRect_[0] = popRect_[1] = popRect_[2] = popRect_[3] = 0;
  popVisible_ = false;
}

void Panel::syncWarp() {
  // The original warps the cursor (SetCursorPos) when a popup opens: onto the slider value or the
  // current list cell. A host without pointer warping sees a virtual cursor = pointer + warp.
  const int key = emu_.popup.kind ? emu_.popup.ownerId * 4 + emu_.popup.kind : -1;
  if (emu_.popup.kind && key != popOpenKey_) {
    int t[2] = {0, 0};
    bool have = false;
    if (emu_.popup.kind == 1) {
      t[0] = emu_.popup.warpX;
      t[1] = emu_.popup.warpY;
      have = true;
    } else if (emu_.popup.kind == 2 && emu_.popup.geom.ok) {
      const int k = emu_.popup.current;
      const int c = k / emu_.popup.rows;
      const int r = k % emu_.popup.rows;
      const ListGeom& g = emu_.popup.geom;
      t[0] = g.cl + g.mx + c * g.cellw + (g.cellw >> 1) + 1;
      t[1] = g.ct + g.my + r * g.cellh + (g.cellh >> 1) + 1;
      have = true;
    }
    if (have) {
      warp_[0] = t[0] - lastXY_[0];
      warp_[1] = t[1] - lastXY_[1];
    }
  } else if (!emu_.popup.kind && popOpenKey_ != -1) {
    warp_[0] = 0;
    warp_[1] = 0;
  }
  popOpenKey_ = emu_.popup.kind ? key : -1;
}

void Panel::panelMouse(MouseType type, int px, int py) {
  int x = px;
  int y = py;
  if (popVisible_ && x >= popRect_[0] && x < popRect_[0] + popRect_[2] && y >= popRect_[1] &&
      y < popRect_[1] + popRect_[3]) {
    x -= popShift_[0];
    y -= popShift_[1];
  }
  lastXY_[0] = x;
  lastXY_[1] = y;
  emu_.mouse(type, x + warp_[0], y + warp_[1]);
  syncWarp();
}

void Panel::pointerDown(int px, int py) { panelMouse(MouseType::Down, px, py); }
void Panel::pointerMove(int px, int py) { panelMouse(MouseType::Move, px, py); }
void Panel::pointerUp(int px, int py) { panelMouse(MouseType::Up, px, py); }
void Panel::dblClick(int px, int py) { panelMouse(MouseType::DblClick, px, py); }
void Panel::rightDown(int px, int py) { panelMouse(MouseType::RDown, px, py); }

void Panel::wheel(int delta) {
  emu_.wheel(delta);
}

// ---------------------------------------------------------------------------
// Multi-pointer + touch semantics (P1)

long long Panel::nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

namespace {
inline bool nearPx(int x0, int y0, int x1, int y1, int r) {
  const long long dx = x0 - x1;
  const long long dy = y0 - y1;
  return dx * dx + dy * dy <= static_cast<long long>(r) * r;
}
}   // namespace

void Panel::pointerDown(int id, int px, int py, long long timeMs) {
  if (timeMs < 0) timeMs = nowMs();
  int x = px;
  int y = py;
  const bool inPop = popVisible_ && px >= popRect_[0] && px < popRect_[0] + popRect_[2] &&
                     py >= popRect_[1] && py < popRect_[1] + popRect_[3];
  if (inPop) {
    x -= popShift_[0];
    y -= popShift_[1];
  }
  const bool excl = exclusiveActive();
  PointerTrack& pt = ptrs_[id];
  if (excl && owner_ != -1 && owner_ != id) {
    // Secondary pointer while another pointer owns the exclusive state.
    if (inPop) return;   // the popup is the owner's single-capture overlay
    const int w = emu_.find(x - 1, y - 1);
    if (w < 0 || w == ownerWidget_ || !emu_.tapSafe(w)) return;   // same widget / not tap-safe
    pt.down = true;
    pt.x = px;
    pt.y = py;
    pt.widget = w;
    pt.tDown = timeMs;
    pt.longPressFired = false;
    pt.previewList = false;
    emu_.tapDown(x, y);   // instantaneous action; hover/pressed/popup untouched
    return;
  }
  lastXY_[0] = x;
  lastXY_[1] = y;
  MouseType type = MouseType::Down;
  pt.previewList = false;
  if (touchMode_) {
    if (emu_.popup.kind == 2 && emu_.listCellHit(x + warp_[0], y + warp_[1])) {
      pt.previewList = true;   // touch-down previews the hover state; the lift selects
      type = MouseType::Move;
    } else if (tapWidget_ != -1 && timeMs - tapT_ <= kDoubleTapMs && nearPx(tapX_, tapY_, px, py, kDoubleTapPx) &&
               emu_.find(x + warp_[0] - 1, y + warp_[1] - 1) == tapWidget_) {
      type = MouseType::DblClick;   // double-tap
      tapWidget_ = -1;
    }
  }
  emu_.mouse(type, x + warp_[0], y + warp_[1]);
  syncWarp();
  pt.down = true;
  pt.x = px;
  pt.y = py;
  pt.tDown = timeMs;
  pt.longPressFired = false;
  pt.widget = emu_.find(x + warp_[0] - 1, y + warp_[1] - 1);
  if (owner_ == -1 || owner_ == id) {
    owner_ = id;
    ownerWidget_ = pt.widget;
  }
}

void Panel::pointerMove(int id, int px, int py, long long timeMs) {
  if (timeMs < 0) timeMs = nowMs();
  const auto it = ptrs_.find(id);
  const bool excl = exclusiveActive();
  if (excl && owner_ != -1 && owner_ != id) return;   // single virtual cursor: no secondary hover
  PointerTrack& pt = ptrs_[id];
  int x = px;
  int y = py;
  if (popVisible_ && px >= popRect_[0] && px < popRect_[0] + popRect_[2] && py >= popRect_[1] &&
      py < popRect_[1] + popRect_[3]) {
    x -= popShift_[0];
    y -= popShift_[1];
  }
  if (touchMode_ && pt.down && !pt.longPressFired && timeMs - pt.tDown >= kLongPressMs &&
      nearPx(pt.x, pt.y, px, py, kDoubleTapPx)) {
    pt.longPressFired = true;   // long-press = right-click equivalent
    lastXY_[0] = x;
    lastXY_[1] = y;
    emu_.mouse(MouseType::RDown, x + warp_[0], y + warp_[1]);
    syncWarp();
  }
  lastXY_[0] = x;
  lastXY_[1] = y;
  emu_.mouse(MouseType::Move, x + warp_[0], y + warp_[1]);
  syncWarp();
}

void Panel::pointerUp(int id, int px, int py, long long timeMs) {
  if (timeMs < 0) timeMs = nowMs();
  const auto it = ptrs_.find(id);
  const bool excl = exclusiveActive();
  if (it != ptrs_.end() && it->second.down && excl && owner_ != -1 && owner_ != id) {
    ptrs_.erase(it);   // secondary tap-safe action fired on down; its lift is a no-op
    return;
  }
  if (excl && owner_ != -1 && owner_ != id) return;   // this pointer never acted
  PointerTrack& pt = ptrs_[id];
  int x = px;
  int y = py;
  if (popVisible_ && px >= popRect_[0] && px < popRect_[0] + popRect_[2] && py >= popRect_[1] &&
      py < popRect_[1] + popRect_[3]) {
    x -= popShift_[0];
    y -= popShift_[1];
  }
  if (touchMode_ && pt.down && !pt.longPressFired && timeMs - pt.tDown >= kLongPressMs &&
      nearPx(pt.x, pt.y, px, py, kDoubleTapPx)) {
    pt.longPressFired = true;   // long-press before the lift: right-click equivalent
    lastXY_[0] = x;
    lastXY_[1] = y;
    emu_.mouse(MouseType::RDown, x + warp_[0], y + warp_[1]);
    syncWarp();
  }
  MouseType type = MouseType::Up;
  if (touchMode_ && pt.previewList && emu_.popup.kind == 2) {
    type = MouseType::Down;   // the lift selects the previewed cell
  }
  pt.previewList = false;
  lastXY_[0] = x;
  lastXY_[1] = y;
  emu_.mouse(type, x + warp_[0], y + warp_[1]);
  syncWarp();
  if (touchMode_ && type != MouseType::RDown) {
    tapWidget_ = emu_.find(x + warp_[0] - 1, y + warp_[1] - 1);
    tapX_ = px;
    tapY_ = py;
    tapT_ = timeMs;
  }
  if (owner_ == id) {
    pt.down = false;
    if (!exclusiveActive()) {
      owner_ = -1;
      ownerWidget_ = -1;
    }
  }
  ptrs_.erase(id);
}

void Panel::key(int code) {
  if (!sel_.isOpen) return;
  if (code == KeyEscape) sel_.key("Escape");
  else if (code == KeyArrowDown) sel_.key("ArrowDown");
  else if (code == KeyArrowUp) sel_.key("ArrowUp");
}

int Panel::getParam(int id) const {
  const auto it = emu_.values.find(id);
  return it == emu_.values.end() ? 0 : it->second;
}

bool Panel::hasParam(int id) const { return emu_.values.count(id) != 0; }

void Panel::setParam(int id, int value) { emu_.values[id] = value; }

void Panel::setValues(const std::map<int, int>& v) {
  for (const auto& kv : v) emu_.values[kv.first] = kv.second;
}

void Panel::setLcd(const std::string& program, const std::string& bank) {
  lcd_["program"] = program;
  lcd_["bank"] = bank;
}

void Panel::loadProgram(int bank, int prog) {
  if (!skin_.hasBanks || bank < 0 || bank >= static_cast<int>(banks_.size())) return;
  const SelectBank& b = banks_[bank];
  curBank_ = bank;
  curProgram_ = prog;
  curName_.clear();
  emu_.values[252] = prog;
  const SelectProgram* p =
      prog >= 0 && prog < static_cast<int>(b.programs.size()) && b.programs[prog].present ? &b.programs[prog] : nullptr;
  if (!p && skin_.hasInitSound) {   // empty slot: the device loads its initial sound
    for (const auto& kv : skin_.initSound.at("init_sound").items()) {
      emu_.values[std::stoi(kv.key())] = kv.value().get<int>();
    }
    setLcd(PanelCore::programText(prog, "initial sound"), PanelCore::bankText(b.slot, b.folder));
  }
  if (p) {
    std::map<int, int> parsed;
    const std::map<int, int>* v = &p->values;
    if (!p->hasValues) {
      parsed = parseSy1(p->sy1).values;
      v = &parsed;
    }
    for (const auto& kv : *v) emu_.values[kv.first] = kv.second;
    std::string hname;
    std::string hcolor;
    sy1Head(p->sy1, hname, hcolor);
    setLcd(PanelCore::programText(prog, hname), PanelCore::bankText(b.slot, b.folder));
  }
  if (onProgram) onProgram(bank, prog);
}

void Panel::loadSy1(const std::string& text, int programIndex, const std::string& bankLabel) {
  const Sy1Parsed s = parseSy1(text);
  setValues(s.values);
  curName_ = s.name;
  setLcd(PanelCore::programText(programIndex, s.name),
          bankLabel.empty() ? PanelCore::bankText(0, "bank0") : bankLabel);
}

std::string Panel::sy1Text() const {
  const SelectBank* b = skin_.hasBanks && curBank_ >= 0 && curBank_ < static_cast<int>(banks_.size())
                            ? &banks_[curBank_]
                            : nullptr;
  const SelectProgram* p = b && curProgram_ >= 0 && curProgram_ < static_cast<int>(b->programs.size()) &&
                                   b->programs[curProgram_].present
                               ? &b->programs[curProgram_]
                               : nullptr;
  std::string headName;
  std::string headColor;
  if (p) {
    sy1Head(p->sy1, headName, headColor);
  } else {
    headName = curName_.empty() ? "initial sound" : curName_;
    headColor = "default";
  }
  const std::string name = curName_.empty() ? headName : curName_;
  std::vector<int> order;
  if (p) {
    // the program's own row order: p.sy1 lines 3.. /^(\d+),/
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
      size_t nl = p->sy1.find('\n', start);
      if (nl == std::string::npos) {
        lines.push_back(p->sy1.substr(start));
        break;
      }
      std::string line = p->sy1.substr(start, nl - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
      start = nl + 1;
    }
    for (size_t i = 3; i < lines.size(); i++) {
      const std::string& l = lines[i];
      size_t j = 0;
      while (j < l.size() && isdigit((unsigned char)l[j])) j++;
      if (j == 0 || j >= l.size() || l[j] != ',') continue;
      const int id = std::stoi(l.substr(0, j));
      if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    }
  }
  for (int id = 0; id <= 98; id++) {
    if (std::find(order.begin(), order.end(), id) == order.end() && emu_.values.count(id)) order.push_back(id);
  }
  std::string out = name + "\r\n" + "color=" + headColor + "\r\n" + "ver=113";
  for (const int id : order) {
    const auto it = emu_.values.find(id);
    if (it == emu_.values.end()) continue;
    out += "\r\n" + std::to_string(id) + ',' + std::to_string(it->second);
  }
  out += "\r\n";
  return out;
}

std::array<int, 4> Panel::selectPosition() const {
  const double cw = sel_.clientWidth() + 2;
  const double ch = sel_.clientHeight() + 2;
  const int x = std::max(0, static_cast<int>(std::round(width() / 2.0 - cw / 2.0)));
  const int y = std::max(0, static_cast<int>(std::round(height() / 2.0 - ch / 2.0)));
  return {x, y, static_cast<int>(cw), static_cast<int>(ch)};
}

void Panel::selectPointerMove(int x, int y) { sel_.mouse("move", x, y); }
void Panel::selectPointerDown(int x, int y) {
  if (touchMode_) {
    sel_.mouse("move", x, y);   // touch-down previews the cell hover
    return;
  }
  sel_.mouse("down", x, y);
}
void Panel::selectPointerUp(int x, int y) {
  if (!touchMode_) return;   // the original select window has no button-up handling
  sel_.mouse("move", x, y);  // follow the finger to the lift point...
  sel_.mouse("down", x, y);  // ...and select there on lift
}
void Panel::selectRightDown(int x, int y) { sel_.mouse("rdown", x, y); }
void Panel::selectDeactivate() { sel_.deactivate(); }

void Panel::render(uint8_t* rgba, bool withPopups) {
  PanelCore::State st;
  st.values = emu_.values;
  st.lcd = lcd_;
  st.pressed = emu_.hasPressed ? std::to_string(emu_.pressed.id) + halfSuffix(emu_.pressed.half) : "";
  st.damage = emu_.damage;
  core_.render(st, rgba);
  paintPopShift();
  if (withPopups && skin_.hasPopups && emu_.popup.kind) {
    const Composed& c = popComp_;
    const int W = core_.width();
    const int H = core_.height();
    for (int y = 0; y < c.h; y++) {
      const int Y = c.y + y;
      if (Y < 0 || Y >= H) continue;
      for (int x = 0; x < c.w; x++) {
        const int X = c.x + x;
        if (X < 0 || X >= W) continue;
        const size_t s = (static_cast<size_t>(y) * c.w + x) * 4;
        const size_t d = (static_cast<size_t>(Y) * W + X) * 4;
        rgba[d] = c.data[s];
        rgba[d + 1] = c.data[s + 1];
        rgba[d + 2] = c.data[s + 2];
        rgba[d + 3] = 255;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Options dialog (OPT, command 15)

void Panel::openOpt(const OptSettings& s) {
  if (!skin_.hasOpt) return;
  opt_.setLib(skin_.opt, skin_.optPng);
  opt_.onClose = [this](const OptSettings* r) {
    if (r && onOptResult) onOptResult(*r);
  };
  opt_.open(s);
}

void Panel::closeOpt() {
  if (!opt_.isOpen()) return;
  opt_.key("Escape");   // cancel path: onClose(nullptr), no result
}

std::array<int, 4> Panel::optPosition() const {
  // client + 1 px window border per side (the web's B = 1), at the panel scale; centred on the
  // panel, clamped to the panel origin (the select-window law): it overflows right/bottom and the
  // host grows the UI window to the union rect.
  const int dw = (OptDialog::W + 2) * scale_;
  const int dh = (OptDialog::H + 2) * scale_;
  const int x = std::max(0, static_cast<int>(std::round(width() / 2.0 - dw / 2.0)));
  const int y = std::max(0, static_cast<int>(std::round(height() / 2.0 - dh / 2.0)));
  return {x, y, dw, dh};
}

void Panel::optPointerDown(int x, int y) { opt_.pointerDown(x, y); }
void Panel::optPointerMove(int x, int y) { opt_.pointerMove(x, y); }
void Panel::optPointerUp() { opt_.pointerUp(); }

void Panel::optKey(int code) {
  if (!opt_.isOpen()) return;
  if (code == KeyEscape) opt_.key("Escape");
  else if (code == KeyEnter) opt_.key("Enter");
}

}   // namespace panel
}   // namespace synth1
