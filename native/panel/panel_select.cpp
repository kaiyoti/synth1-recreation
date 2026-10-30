#include "panel_select.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "panel_util.hpp"

namespace synth1 {
namespace panel {

namespace {

std::string jsTrim(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  const char* ws = " \t\n\r\f\v";
  while (a < b && std::strchr(ws, s[a]) != nullptr) a++;
  while (b > a && std::strchr(ws, s[b - 1]) != nullptr) b--;
  return s.substr(a, b - a);
}

}   // namespace

void sy1Head(const std::string& text, std::string& name, std::string& color) {
  // fgets(33): at most 32 characters, the newline included.
  auto fgets33 = [&text](size_t pos, std::string& line, size_t& next) {
    size_t e = pos;
    while (e < text.size() && e - pos < 32) {
      e++;
      if (text[e - 1] == '\n') break;
    }
    line = text.substr(pos, e - pos);
    next = e;
  };
  std::string l1, l2;
  size_t p = 0;
  size_t dummy = 0;
  fgets33(0, l1, p);
  fgets33(p, l2, dummy);
  size_t e = l1.size();
  while (e > 0 && (l1[e - 1] == '\r' || l1[e - 1] == '\n')) e--;
  name = l1.substr(0, e);
  const std::string c = jsTrim(l2);
  color = (c.rfind("color=", 0) == 0) ? c.substr(6) : "default";
}

bool parseBanks(const Json& B, std::vector<SelectBank>& out, std::string& err) {
  out.clear();
  for (const Json& b : B) {
    SelectBank sb;
    sb.slot = b.at("slot").get<int>();
    sb.folder = b.at("folder").get<std::string>();
    sb.count = b.at("count").get<int>();
    for (const Json& p : b.at("programs")) {
      SelectProgram sp;
      if (!p.is_null()) {
        sp.present = true;
        sp.name = p.value("name", std::string());
        sp.color = p.value("color", std::string("default"));
        sp.sy1 = p.value("sy1", std::string());
        if (p.contains("values") && p.at("values").is_object()) {
          sp.hasValues = true;
          for (const auto& kv : p.at("values").items()) sp.values[std::stoi(kv.key())] = kv.value().get<int>();
        }
      }
      sb.programs.push_back(std::move(sp));
    }
    out.push_back(std::move(sb));
  }
  return true;
}

bool SelectWin::load(const Json& S, const std::vector<uint8_t>& chromeRGBA,
                    const std::vector<SelectBank>& banksIn, std::string& err) {
  CW_ = S.at("client")[0].get<int>();
  CH_ = S.at("client")[1].get<int>();
  g_.x = S.at("grid").at("x").get<int>();
  g_.y = S.at("grid").at("y").get<int>();
  g_.cw = S.at("grid").at("cw").get<int>();
  g_.ch = S.at("grid").at("ch").get<int>();
  g_.cols = S.at("grid").at("cols").get<int>();
  g_.rows = S.at("grid").at("rows").get<int>();
  g_.wrap = S.at("grid").at("wrap").get<int>();
  for (int k = 0; k < 3; k++) frame_[k] = S.at("frame")[k].get<int>();
  for (const auto& kv : S.at("glyphs").items()) {
    const Json& e = kv.value();
    Glyph gh;
    gh.adv = e.at("adv").get<int>();
    gh.x = e.contains("x") ? e.at("x").get<int>() : 0;
    gh.y = e.contains("y") ? e.at("y").get<int>() : 0;
    gh.w = e.contains("w") ? e.at("w").get<int>() : 0;
    gh.h = e.contains("h") ? e.at("h").get<int>() : 0;
    if (e.contains("rgb") && !e.at("rgb").is_null()) gh.rgb = base64Decode(e.at("rgb").get<std::string>());
    glyph_.emplace(std::stoi(kv.key()), gh);
  }
  for (const auto& kv : S.at("fills").at("normal").items()) {
    std::array<int, 3> a;
    for (int k = 0; k < 3; k++) a[k] = kv.value()[k].get<int>();
    fillNormal_[kv.key()] = a;
  }
  for (const auto& kv : S.at("fills").at("high").items()) {
    std::array<int, 3> a;
    for (int k = 0; k < 3; k++) a[k] = kv.value()[k].get<int>();
    fillHigh_[kv.key()] = a;
  }
  for (const Json& it : S.at("tree_items")) {
    TreeItem ti;
    for (int k = 0; k < 4; k++) ti.rect[k] = it.at("rect")[k].get<int>();
    ti.chrome = it.at("chrome").get<int>();
    ti.bank = it.at("bank").is_null() ? -1 : it.at("bank").get<int>();
    tree_.push_back(ti);
  }
  banks_ = banksIn;
  // one tree row per bank (panel_select.js: the captured tree had 11 rows, the reorganised set
  // has more): extra rows continue the row pitch and reuse the last captured chrome.
  if (!tree_.empty() && banks_.size() != tree_.size()) {
    const TreeItem t0 = tree_[0];
    const int last = tree_.back().chrome;
    // row pitch of the captured rows (16 px); the web's panel_select.js uses the row height (15 px)
    // here, which makes the 12th row overlap the 11th by 1 px - the true pitch is used instead.
    const int rh = tree_.size() > 1 ? tree_[1].rect[1] - tree_[0].rect[1] : t0.rect[3] - t0.rect[1];
    std::vector<TreeItem> nt;
    for (size_t i = 0; i < banks_.size(); i++) {
      if (i < tree_.size()) { nt.push_back(tree_[i]); continue; }
      TreeItem ti;
      ti.rect[0] = t0.rect[0];
      ti.rect[1] = t0.rect[1] + rh * static_cast<int>(i);
      ti.rect[2] = t0.rect[2] + 20;
      ti.rect[3] = t0.rect[3] + rh * static_cast<int>(i);
      ti.chrome = last;
      ti.bank = static_cast<int>(i);
      nt.push_back(ti);
    }
    tree_ = nt;
  }
  chrome_ = chromeRGBA;
  client.assign(static_cast<size_t>(CW_) * CH_ * 4, 0);
  return true;
}

int SelectWin::itemOfBank(int b) const {
  for (size_t i = 0; i < tree_.size(); i++) {
    if (tree_[i].bank == b) return static_cast<int>(i);
  }
  return -1;
}

int SelectWin::adv(int c) const {
  const auto it = glyph_.find(c);
  return it == glyph_.end() ? 0 : it->second.adv;
}

/* Wine gdip_format_string word wrap (line 1). */
std::string SelectWin::firstLine(const std::string& s, int maxw) const {
  int ext = 0;
  int fit = 0;
  const int n = static_cast<int>(s.size());
  for (int i = 0; i < n; i++) {
    const int a = adv(static_cast<unsigned char>(s[i]));
    if (ext + a > maxw) break;
    ext += a;
    fit++;
  }
  if (fit == 0) return "";
  for (int i = 0; i < fit; i++) {
    if (s[i] == '\n') return s.substr(0, i);
  }
  if (fit < n) {
    const int fitcpy = fit;
    if (s[fit] == ' ') {
      while (fit < n && s[fit] == ' ') fit++;
    } else {
      while (s[fit - 1] != ' ') {
        fit--;
        if (s[fit] == '\t') break;
        if (fit == 0) {
          fit = fitcpy;
          break;
        }
      }
    }
    int end = fit;
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    return s.substr(0, end);
  }
  return s.substr(0, fit);
}

void SelectWin::px(int x, int y, int r, int gg, int b) {
  if (x < 0 || y < 0 || x >= CW_ || y >= CH_) return;
  const size_t p = (static_cast<size_t>(y) * CW_ + x) * 4;
  client[p] = static_cast<uint8_t>(r);
  client[p + 1] = static_cast<uint8_t>(gg);
  client[p + 2] = static_cast<uint8_t>(b);
  client[p + 3] = 255;
}

void SelectWin::text(const std::string& s, int ox, int oy, const int fg[3]) {
  int pen = 0;
  for (const unsigned char ch : s) {
    const auto it = glyph_.find(ch);
    if (it != glyph_.end() && !it->second.rgb.empty()) {
      const Glyph& e = it->second;
      for (int yy = 0; yy < e.h; yy++) {
        for (int xx = 0; xx < e.w; xx++) {
          const size_t q = (static_cast<size_t>(yy) * e.w + xx) * 3;
          const int ar = e.rgb[q];
          const int ag = e.rgb[q + 1];
          const int ab = e.rgb[q + 2];
          if (!(ar | ag | ab)) continue;
          const int X = ox + pen + e.x + xx;
          const int Y = oy + e.y + yy;
          if (X < 0 || Y < 0 || X >= CW_ || Y >= CH_) continue;
          const size_t p = (static_cast<size_t>(Y) * CW_ + X) * 4;
          client[p] = static_cast<uint8_t>((fg[0] * ar + client[p] * (255 - ar) + 127) / 255);
          client[p + 1] = static_cast<uint8_t>((fg[1] * ag + client[p + 1] * (255 - ag) + 127) / 255);
          client[p + 2] = static_cast<uint8_t>((fg[2] * ab + client[p + 2] * (255 - ab) + 127) / 255);
        }
      }
    }
    pen += adv(ch);
  }
}

std::array<int, 2> SelectWin::cellRect(int k) const {
  return {g_.x + (k / g_.rows) * g_.cw, g_.y + (k % g_.rows) * g_.ch};
}

void SelectWin::paintCell(int k, bool high) {
  const std::array<int, 2> xy = cellRect(k);
  const int x = xy[0];
  const int y = xy[1];
  const SelectProgram* p = nullptr;
  if (shown_ >= 0 && shown_ < static_cast<int>(banks_.size()) &&
      k >= 0 && k < static_cast<int>(banks_[shown_].programs.size())) {
    p = banks_[shown_].programs[k].present ? &banks_[shown_].programs[k] : nullptr;
  }
  const std::string color = p ? p->color : "default";
  const auto& fills = high ? fillHigh_ : fillNormal_;
  auto fit = fills.find(color);
  if (fit == fills.end()) fit = fills.find("default");
  const std::array<int, 3> fill = fit->second;
  for (int yy = 0; yy < g_.ch + 1; yy++) {
    for (int xx = 0; xx < g_.cw + 1; xx++) {
      const bool edge = yy == 0 || yy == g_.ch || xx == 0 || xx == g_.cw;
      if (edge) px(x + xx, y + yy, 0, 0, 0);
      else px(x + xx, y + yy, fill[0], fill[1], fill[2]);
    }
  }
  int fg[3] = {0, 0, 0};
  if (shown_ == curBank_ && k == curProg_) {
    if (fill[0] == 0 && fill[1] == 0 && fill[2] == 255) {
      fg[0] = 255;
      fg[1] = 255;
      fg[2] = 255;
    } else {
      fg[0] = 0;
      fg[1] = 0;
      fg[2] = 255;
    }
  }
  const std::string name = p ? p->name : "initial sound";
  text(firstLine(std::to_string(k + 1) + ':' + name, g_.wrap), x, y, fg);
}

void SelectWin::fullPaint() {
  const int c = tree_[item_].chrome;
  const size_t off = static_cast<size_t>(c) * CW_ * CH_ * 4;
  std::memcpy(client.data(), chrome_.data() + off, static_cast<size_t>(CW_) * CH_ * 4);
  if (hasGrid()) {
    for (int k = 0; k < g_.cols * g_.rows; k++) paintCell(k, k == hover_);
  }
  if (labels_) paintLabels();
}

void SelectWin::paintLabels() {
  auto fillRect = [this](int x0, int y0, int w, int h, int r, int g, int b) {
    for (int y = y0; y < y0 + h; y++)
      for (int x = x0; x < x0 + w; x++) px(x, y, r, g, b);
  };
  const UiFont& reg = uiFontRegular();
  const UiFont& bold = uiFontBold();
  const uint8_t black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
  const int nb = static_cast<int>(banks_.size());
  fillRect(6, 41, 247, std::max(0, 16 * nb + 4), 255, 255, 255);
  for (int i = 0; i < nb; i++) {
    char num[8];
    std::snprintf(num, sizeof num, "%02d", banks_[i].slot);
    const std::string label = std::string(num) + ":" + banks_[i].folder + " (" + std::to_string(banks_[i].count) + ")";
    const int* r = i < static_cast<int>(tree_.size()) ? tree_[i].rect : nullptr;
    const int r0 = r ? r[0] : 25, r1 = r ? r[1] : 41 + 16 * i, r3 = r ? r[3] : 56 + 16 * i;
    const int w = uiTextWidth(reg, label) + 4;
    const bool sel = i == item_;
    if (sel) fillRect(r0 - 2, r1, w, r3 - r1, 0x33, 0x99, 0xff);
    uiDrawText(client.data(), CW_, CH_, r0, r1 + 12, label, sel ? white : black, reg);
  }
  if (shown_ >= 0 && shown_ < nb) {
    const SelectBank& bk = banks_[shown_];
    fillRect(345, 9, 360, 22, 255, 255, 255);
    fillRect(820, 9, 140, 22, 255, 255, 255);
    char num[8];
    std::snprintf(num, sizeof num, "%02d", bk.slot);
    uiDrawText(client.data(), CW_, CH_, 351, 24, std::string("Bank") + num + ":" + bk.folder, black, bold);
    const std::string np = std::to_string(bk.count) + " Programs";
    uiDrawText(client.data(), CW_, CH_, 951 - uiTextWidth(bold, np), 24, np, black, bold);
  }
}

void SelectWin::selectItem(int i) {   // TVN_SELCHANGED
  if (i < 0 || i >= static_cast<int>(tree_.size()) || i == item_) return;
  item_ = i;
  if (hasGrid()) shown_ = tree_[item_].bank;
  fullPaint();
}

int SelectWin::cellAt(int x, int y) const {   // FUN_1001ebb0
  const int lx = x - g_.x;
  const int ly = y - g_.y;
  if (lx < 0 || lx >= g_.cw * g_.cols || ly < 0 || ly >= g_.ch * g_.rows) return -1;
  return (ly / g_.ch) + (lx / g_.cw) * g_.rows;
}

void SelectWin::close() {
  isOpen = false;
  if (onClose) onClose();
}

void SelectWin::open(int bank, int prog) {
  curBank_ = bank;
  curProg_ = prog;
  shown_ = bank;
  hover_ = prog;   // +0x23c = current program
  item_ = itemOfBank(bank);
  isOpen = true;
  fullPaint();
}

void SelectWin::mouse(const std::string& type, int x, int y) {
  if (!isOpen) return;
  if (type == "move") {   // WM_MOUSEMOVE: repaint the old and the new hover cell
    if (!hasGrid()) return;
    const int k = cellAt(x, y);
    if (k != hover_) {
      if (hover_ != -1) paintCell(hover_, false);
      hover_ = k;
      if (k != -1) paintCell(k, true);
    }
    return;
  }
  if (type == "down") {
    for (size_t i = 0; i < tree_.size(); i++) {   // the TreeView child
      const int* r = tree_[i].rect;
      if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) {
        selectItem(static_cast<int>(i));
        return;
      }
    }
    if (x >= 6 && x < 257 && y >= 41 && y < 617) return;   // other clicks inside the tree
    if (hasGrid() && hover_ != -1) {   // empty slots load too ('initial sound')
      const int b = shown_;
      const int k = hover_;
      close();
      if (onLoad) onLoad(b, k);
      return;
    }
    close();
    return;
  }
  if (type == "rdown") close();
}

void SelectWin::key(const std::string& k) {
  if (!isOpen) return;
  if (k == "Escape") close();
  else if (k == "ArrowDown") selectItem(item_ + 1);
  else if (k == "ArrowUp") selectItem(item_ - 1);
}

void SelectWin::deactivate() {
  if (isOpen) close();
}

}   // namespace panel
}   // namespace synth1
