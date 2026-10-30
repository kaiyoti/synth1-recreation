/* Synth1 Options dialog (COptionDlg, dialog 0xa6 + pages 0xa7 General / 0xa8 MIDI) — C++ port of
 * tasks/m4-panel/web/panel_opt.js (M4 W2). Header-only so the Panel facade hooks link without a
 * new translation unit (the U2 gate Makefile lists the panel sources explicitly).
 *
 * The dialog client (502x438) is composed from parts cropped out of captures of the original
 * dialog under the reference harness (tasks/m4-panel/opt/cap, built by tools/build_opt.js); the
 * assets are DATA: <skinDir>/opt/opt.json + opt.png. Behaviour follows those captures (Wine 9.0
 * comctl32/user32):
 *  - checkbox: click toggles; the focus rectangle is shown only on a 2nd consecutive click on the
 *    same checkbox; ticking Panel/Text enables its colour edit and moves the focus there;
 *  - push buttons (- + Reset) and tab headers always show the focus rectangle when focused;
 *    OK shows the thick default border only while it has the focus (initial state);
 *  - Master Tuning trackbar range -100..+100 cents (TBM_SETRANGE 0x64ff9c, decomp 25006),
 *    A4 = 440 * 2^(c/1200) Hz;
 *  - Panel Size combo 80..250 % (step 10).
 * Settings keys = the original's ini keys (slidertype, transparencywin, hostselectprogram,
 * mastertune, panelcolor, textcolor (bit 31 = disabled), coordinatesscale; the MIDI-tab receive
 * checkboxes map to the ini keys enableBankSelect / enableProgramChange).
 *
 * Part state keys must match tools/build_opt.js (the asset builder / the gate replay).
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "panel_util.hpp"
#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

/* The dialog's settings (the original's ini keys; colours are BGR with bit 31 = disabled). */
struct OptSettings {
  int slidertype = 1;
  int transparencywin = 1;
  int hostselectprogram = 0;
  int mastertune = 0;
  std::uint32_t panelcolor = 0x803c2828u;
  std::uint32_t textcolor = 0x80c8c8c8u;
  int coordinatesscale = 150;
  int receivebank = 1;
  int receiveprog = 1;
};

class OptDialog {
 public:
  static constexpr int W = 502;
  static constexpr int H = 438;

  static const std::vector<int>& sizes() {
    static const std::vector<int> s = [] {
      std::vector<int> v;
      for (int s = 80; s <= 250; s += 10) v.push_back(s);
      return v;
    }();
    return s;
  }

  static OptSettings defaults() { return OptSettings(); }

  /* lib = the opt.json + opt.png atlas loaded from the skin dir. */
  void setLib(const Json& json, const Image& atlas) {
    J_ = &json;
    A_ = &atlas;
  }

  /* Open (fresh state from the settings, like the web create()). */
  void open(const OptSettings& s) {
    S_ = s;
    st_.tab = "general";
    st_.tabFocus = false;
    st_.focus = "ok";
    st_.lastChk.clear();
    st_.focusVis = false;
    st_.chkPanel = ((s.panelcolor >> 31) & 1u) == 0;
    st_.chkText = ((s.textcolor >> 31) & 1u) == 0;
    st_.chkSlider = s.slidertype == 1;
    st_.chkTransp = s.transparencywin != 0;
    st_.chkHost = s.hostselectprogram != 0;
    st_.chkBank = s.receivebank != 0;
    st_.chkProg = s.receiveprog != 0;
    st_.tune = std::max(-100, std::min(100, s.mastertune));
    st_.size = indexOfSize(s.coordinatesscale) >= 0 ? s.coordinatesscale : 100;
    st_.panelcolor = s.panelcolor;
    st_.textcolor = s.textcolor;
    st_.comboOpen = false;
    st_.comboHover = -1;
    st_.dragTrack = false;
    open_ = true;
    renderBuffers();
  }

  bool isOpen() const { return open_; }
  const OptSettings& settings() const { return S_; }

  struct State {
    std::string tab = "general";
    bool tabFocus = false;
    std::string focus = "ok";
    std::string lastChk;   // "" = none (JS null)
    bool focusVis = false;
    bool chkPanel = true, chkText = true, chkSlider = true, chkTransp = true, chkHost = false;
    bool chkBank = true, chkProg = true;
    int tune = 0;
    int size = 100;
    std::uint32_t panelcolor = 0x803c2828u, textcolor = 0x80c8c8c8u;
    bool comboOpen = false;
    int comboHover = -1;
    bool dragTrack = false;
  };
  const State& state() const { return st_; }

  std::function<void(const OptSettings*)> onClose;   // nullptr = cancel

  /* result(): the settings as they would be applied on OK. */
  OptSettings result() const {
    OptSettings r = S_;
    r.slidertype = st_.chkSlider ? 1 : 0;
    r.transparencywin = st_.chkTransp ? 1 : 0;
    r.hostselectprogram = st_.chkHost ? 1 : 0;
    r.mastertune = st_.tune;
    r.coordinatesscale = st_.size;
    r.panelcolor = flag(st_.chkPanel, st_.panelcolor);
    r.textcolor = flag(st_.chkText, st_.textcolor);
    r.receivebank = st_.chkBank ? 1 : 0;
    r.receiveprog = st_.chkProg ? 1 : 0;
    return r;
  }

  const char* tab() const { return st_.tab.c_str(); }
  bool comboOpen() const { return st_.comboOpen; }
  bool dragTrack() const { return st_.dragTrack; }

  const std::vector<uint8_t>& client() const { return client_; }   // W*H*4 RGBA
  const std::vector<uint8_t>& list() const { return list_; }      // LIST.w*LIST.h*4 or empty
  std::array<int, 4> listPos() const {                            // in client coords {x,y,w,h}
    return {kHitCombo[0] + kListDx, kHitCombo[1] + kListDy, kListW, kListH};
  }

  const char* hitTest(int x, int y) const {
    const std::vector<const char*>& hits = st_.tab == "general" ? generalHits() : midiHits();
    for (const char* n : hits) {
      const std::array<int, 4>& r = hitRect(n);
      if (inR(r, x, y)) return n;
    }
    return nullptr;
  }

  void pointerDown(int x, int y) {
    if (st_.comboOpen) {
      const int k = listHit(x, y);
      if (k >= 0) st_.size = sizes()[static_cast<std::size_t>(k)];
      if (k != -1) st_.comboOpen = false;   // outside or on a row closes
      renderBuffers();
      return;
    }
    const char* n = hitTest(x, y);
    if (!n) return;
    const std::string name(n);
    if (name == "tabGeneral" || name == "tabMidi") {
      st_.tab = name == "tabGeneral" ? "general" : "midi";
      st_.tabFocus = true;
      st_.focus = "tab";
      st_.lastChk.clear();
      renderBuffers();
      return;
    }
    st_.tabFocus = false;
    const char* chk = chkKey(name);
    if (chk) {
      bool& c = chkRef(chk);
      c = !c;
      if ((std::strcmp(chk, "panel") == 0 || std::strcmp(chk, "text") == 0) && c) {
        st_.focus = std::strcmp(chk, "panel") == 0 ? "editPanel" : "editText";
        st_.lastChk = name;
        renderBuffers();
        return;
      }
      st_.focusVis = st_.lastChk == name;
      st_.focus = name;
      st_.lastChk = name;
      renderBuffers();
      return;
    }
    st_.lastChk.clear();
    if (name == "plus") {
      st_.tune = std::min(100, st_.tune + 1);
      st_.focus = name;
    } else if (name == "minus") {
      st_.tune = std::max(-100, st_.tune - 1);
      st_.focus = name;
    } else if (name == "reset") {
      st_.tune = 0;
      st_.focus = name;
    } else if (name == "track") {
      st_.tune = trackValue(x);
      st_.focus = "track";
      st_.dragTrack = true;
    } else if (name == "combo") {
      st_.comboOpen = true;
      st_.comboHover = indexOfSize(st_.size);
      st_.focus = "combo";
    } else if (name == "ok") {
      close(true);
      return;
    } else if (name == "cancel") {
      close(false);
      return;
    } else if (name == "editPanel" && st_.chkPanel) {
      st_.focus = name;
    } else if (name == "editText" && st_.chkText) {
      st_.focus = name;
    } else if (name == "browseBank" || name == "browseZip" || name == "update" || name == "load" ||
               name == "save" || name == "ccList" || name == "ccEdit") {
      st_.focus = name;
    }
    renderBuffers();
  }

  void pointerMove(int x, int y) {
    if (st_.dragTrack) {
      st_.tune = trackValue(x);
      renderBuffers();
      return;
    }
    if (st_.comboOpen) {
      const int k = listHit(x, y);
      if (k >= 0) {
        st_.comboHover = k;
        renderBuffers();
      }
    }
  }

  void pointerUp() { st_.dragTrack = false; }

  void key(const char* k) {
    if (std::strcmp(k, "Escape") == 0) {
      if (st_.comboOpen) st_.comboOpen = false;
      else
        close(false);
    } else if (std::strcmp(k, "Enter") == 0) {
      if (st_.comboOpen) {
        if (st_.comboHover >= 0) st_.size = sizes()[static_cast<std::size_t>(st_.comboHover)];
        st_.comboOpen = false;
      } else {
        close(true);
      }
    }
    renderBuffers();
  }

  /* edit fields (#RRGGBB) -> BGR int (web setColor). */
  bool setColor(const char* which, const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return false;
    unsigned v = 0;
    for (char c : h) {
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      v = v * 16 + static_cast<unsigned>(d);
    }
    const std::uint32_t bgr = ((v & 0xffu) << 16) | (v & 0xff00u) | ((v >> 16) & 0xffu);
    if (std::strcmp(which, "panel") == 0) st_.panelcolor = bgr;
    else st_.textcolor = bgr;
    return true;
  }

  std::string colorHex(const char* which) const {
    const std::uint32_t c = std::strcmp(which, "panel") == 0 ? st_.panelcolor : st_.textcolor;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", static_cast<unsigned>(c & 0xffu),
                  static_cast<unsigned>((c >> 8) & 0xffu), static_cast<unsigned>((c >> 16) & 0xffu));
    return std::string(buf);
  }

  /* Part state keys (must match tools/build_opt.js). */
  std::string partKey(const char* tab, const char* name) const {
    (void)tab;   // the state's tab is authoritative (web partKey uses st.tab)
    const std::string f = st_.focus;
    const std::string n(name);
    if (n == "tabs") return st_.tab + (st_.tabFocus ? ":f" : "");
    if (n == "ok") return f == "ok" ? "f" : "";
    if (n == "cancel") return "";
    if (n == "minus" || n == "plus" || n == "reset") return f == n ? "f" : "";
    if (n == "track" || n == "cents" || n == "hz") return std::to_string(st_.tune);
    if (n == "combo") return std::to_string(st_.size);
    if (n == "editPanel") return st_.chkPanel ? "on" : "off";
    if (n == "editText") return st_.chkText ? "on" : "off";
    const char* chk = chkKey(n);
    if (chk) {
      const bool on = chkRef(chk);
      return (on ? "1" : "0") + std::string(f == n && st_.focusVis ? "f" : "");
    }
    return "";
  }

  /* Repaint the client + drop-list buffers from the current state. */
  void renderBuffers() {
    client_.assign(static_cast<std::size_t>(W) * H * 4, 0);
    if (!J_ || !A_) return;
    const std::string tab = st_.tab;
    const Json& base = (*J_)["base"][tab];
    blit(client_, W, base, 0, 0);
    for (const char* name : partNames(tab)) {
      const std::array<int, 4> r = partRect(tab.c_str(), name);
      const std::string key = tab + "." + name + "." + partKey(tab.c_str(), name);
      const Json& parts = (*J_)["parts"];
      if (parts.contains(key)) blit(client_, W, parts[key], r[0], r[1]);
    }
    if (st_.comboOpen) {
      list_.assign(static_cast<std::size_t>(kListW) * kListH * 4, 0);
      blit(list_, kListW, (*J_)["list"]["base"], 0, 0);
      const int cur = st_.comboHover;
      if (cur >= 0 && cur < static_cast<int>((*J_)["list"]["hi"].size())) {
        blit(list_, kListW, (*J_)["list"]["hi"][static_cast<std::size_t>(cur)], 1, kListTop + cur * kListRowH);
      }
    } else {
      list_.clear();
    }
  }

 private:
  /* Part rectangles [x0, y0, x1, y1) in client coords (screen - (549, 394)), from the dialog's
     window dump (opt/cap/gen_winfo.txt) widened to cover focus rectangles; disjoint. */
  static const std::map<std::string, std::array<int, 4>>& partsGeneral() {
    static const std::map<std::string, std::array<int, 4>> m = {
        {"tabs", {0, 0, 130, 28}},        {"ok", {144, 402, 234, 429}},   {"cancel", {269, 402, 359, 429}},
        {"track", {22, 262, 216, 302}},  {"minus", {40, 303, 68, 329}},   {"plus", {68, 303, 97, 329}},
        {"reset", {108, 303, 153, 329}}, {"cents", {25, 330, 107, 354}},  {"hz", {108, 330, 201, 354}},
        {"chkPanel", {327, 264, 384, 287}}, {"editPanel", {391, 262, 457, 288}},
        {"chkText", {327, 290, 384, 317}},  {"editText", {391, 289, 457, 317}},
        {"chkSlider", {232, 327, 327, 353}}, {"chkTransp", {341, 327, 500, 353}},
        {"chkHost", {232, 353, 411, 379}}, {"combo", {234, 280, 304, 305}},
    };
    return m;
  }
  static const std::map<std::string, std::array<int, 4>>& partsMidi() {
    static const std::map<std::string, std::array<int, 4>> m = {
        {"tabs", {0, 0, 130, 28}},      {"ok", {144, 402, 234, 429}}, {"cancel", {269, 402, 359, 429}},
        {"chkBank", {288, 73, 450, 101}}, {"chkProg", {288, 101, 470, 129}},
    };
    return m;
  }
  static std::vector<const char*> partNames(const std::string& tab) {
    // the JS object-literal order of PARTS (tiles are disjoint; order is cosmetic)
    if (tab == "general")
      return {"tabs", "ok", "cancel", "track", "minus", "plus", "reset", "cents", "hz", "chkPanel",
              "editPanel", "chkText", "editText", "chkSlider", "chkTransp", "chkHost", "combo"};
    return {"tabs", "ok", "cancel", "chkBank", "chkProg"};
  }

  /* Hit rectangles (client coords) of the controls. */
  static const std::map<std::string, std::array<int, 4>>& hits() {
    static const std::map<std::string, std::array<int, 4>> m = {
        {"tabGeneral", {2, 2, 62, 24}},   {"tabMidi", {62, 2, 118, 24}},
        {"ok", {145, 403, 233, 428}},     {"cancel", {270, 403, 358, 428}},
        {"browseBank", {408, 36, 478, 61}}, {"browseZip", {408, 213, 478, 238}},
        {"track", {23, 263, 214, 300}},  {"minus", {41, 304, 67, 327}},   {"plus", {69, 304, 95, 327}},
        {"reset", {109, 304, 151, 327}}, {"combo", {235, 281, 302, 303}},
        {"chkPanel", {328, 265, 381, 284}}, {"editPanel", {392, 263, 455, 286}},
        {"chkText", {328, 293, 382, 314}},  {"editText", {392, 291, 455, 314}},
        {"chkSlider", {233, 328, 324, 351}}, {"chkTransp", {342, 328, 500, 351}},
        {"chkHost", {233, 354, 408, 377}},
        {"chkBank", {289, 74, 448, 99}},  {"chkProg", {289, 102, 468, 127}},
        {"ccList", {21, 60, 259, 282}},  {"update", {135, 288, 191, 316}}, {"ccEdit", {198, 290, 230, 316}},
        {"load", {319, 204, 368, 237}},  {"save", {380, 204, 431, 237}},
    };
    return m;
  }
  static const std::vector<const char*>& generalHits() {
    static const std::vector<const char*> v = {"tabGeneral", "tabMidi", "ok", "cancel", "browseBank",
                                               "browseZip", "minus", "plus", "reset", "track", "combo",
                                               "chkPanel", "editPanel", "chkText", "editText", "chkSlider",
                                               "chkTransp", "chkHost"};
    return v;
  }
  static const std::vector<const char*>& midiHits() {
    static const std::vector<const char*> v = {"tabGeneral", "tabMidi", "ok", "cancel", "chkBank",
                                               "chkProg", "ccList", "update", "ccEdit", "load", "save"};
    return v;
  }

  /* Trackbar geometry (Wine trackbar: channel inset, thumb 11 px): value -> thumb, x -> value. */
  static constexpr int kTrackX0 = 23 + 13;
  static constexpr int kTrackX1 = 23 + 178;
  static constexpr int kTrackVmin = -100;
  static constexpr int kTrackVmax = 100;
  /* Combo drop list (ComboLBox) relative to the combo's top-left: 65x252 client + 1 px border. */
  static constexpr int kListDx = 0;
  static constexpr int kListDy = 22;
  static constexpr int kListW = 67;
  static constexpr int kListH = 254;
  static constexpr int kListRowH = 14;
  static constexpr int kListTop = 1;
  static constexpr std::array<int, 4> kHitCombo = {235, 281, 302, 303};

  static const std::map<std::string, std::string>& chkMap() {
    static const std::map<std::string, std::string> m = {
        {"chkPanel", "panel"}, {"chkText", "text"},   {"chkSlider", "slider"},
        {"chkTransp", "transp"}, {"chkHost", "host"}, {"chkBank", "bank"},      {"chkProg", "prog"},
    };
    return m;
  }
  static const char* chkKey(const std::string& name) {
    const auto it = chkMap().find(name);
    return it == chkMap().end() ? nullptr : it->second.c_str();
  }
  bool& chkRef(const char* k) {
    if (!std::strcmp(k, "panel")) return st_.chkPanel;
    if (!std::strcmp(k, "text")) return st_.chkText;
    if (!std::strcmp(k, "slider")) return st_.chkSlider;
    if (!std::strcmp(k, "transp")) return st_.chkTransp;
    if (!std::strcmp(k, "host")) return st_.chkHost;
    if (!std::strcmp(k, "bank")) return st_.chkBank;
    return st_.chkProg;
  }
  bool chkRef(const char* k) const {
    if (!std::strcmp(k, "panel")) return st_.chkPanel;
    if (!std::strcmp(k, "text")) return st_.chkText;
    if (!std::strcmp(k, "slider")) return st_.chkSlider;
    if (!std::strcmp(k, "transp")) return st_.chkTransp;
    if (!std::strcmp(k, "host")) return st_.chkHost;
    if (!std::strcmp(k, "bank")) return st_.chkBank;
    return st_.chkProg;
  }

  static std::uint32_t flag(bool on, std::uint32_t c) {
    return on ? (c & 0xffffffu) : ((c & 0xffffffu) | 0x80000000u);
  }

  static bool inR(const std::array<int, 4>& r, int x, int y) {
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
  }
  static const std::array<int, 4>& hitRect(const char* name) { return hits().at(name); }
  static std::array<int, 4> partRect(const char* tab, const char* name) {
    return (std::strcmp(tab, "general") == 0 ? partsGeneral() : partsMidi()).at(name);
  }

  static int indexOfSize(int v) {
    const std::vector<int>& s = sizes();
    for (std::size_t i = 0; i < s.size(); ++i)
      if (s[i] == v) return static_cast<int>(i);
    return -1;
  }

  /* JS Math.round = floor(x + 0.5) (std::round is half-away-from-zero: differs for negatives). */
  static int jsRound(double v) { return static_cast<int>(std::floor(v + 0.5)); }

  int trackValue(int x) const {
    const double v = static_cast<double>(kTrackVmin) +
                     (x - kTrackX0) * static_cast<double>(kTrackVmax - kTrackVmin) /
                         static_cast<double>(kTrackX1 - kTrackX0);
    return std::max(kTrackVmin, std::min(kTrackVmax, jsRound(v)));
  }

  int listHit(int x, int y) const {
    const int lx = kHitCombo[0] + kListDx;
    const int ly = kHitCombo[1] + kListDy;
    if (x < lx || x >= lx + kListW || y < ly || y >= ly + kListH) return -2;
    const int k = static_cast<int>(std::floor(static_cast<double>(y - ly - kListTop) / kListRowH));
    return k >= 0 && k < static_cast<int>(sizes().size()) ? k : -1;
  }

  void close(bool ok) {
    open_ = false;
    if (onClose) {
      if (ok) {
        const OptSettings r = result();
        onClose(&r);
      } else {
        onClose(nullptr);
      }
    }
  }

  /* blit: t = json [ax, ay, w, h] from the atlas; RGB copied, alpha forced 255 (web law). */
  void blit(std::vector<uint8_t>& dst, int dw, const Json& t, int dx, int dy) const {
    const int ax = t[0].get<int>(), ay = t[1].get<int>(), tw = t[2].get<int>(), th = t[3].get<int>();
    for (int y = 0; y < th; y++) {
      for (int x = 0; x < tw; x++) {
        const std::size_t s = (static_cast<std::size_t>(ay + y) * A_->w + ax + x) * 4;
        const std::size_t d = (static_cast<std::size_t>(dy + y) * dw + dx + x) * 4;
        dst[d] = A_->rgba[s];
        dst[d + 1] = A_->rgba[s + 1];
        dst[d + 2] = A_->rgba[s + 2];
        dst[d + 3] = 255;
      }
    }
  }

  const Json* J_ = nullptr;
  const Image* A_ = nullptr;
  OptSettings S_;
  State st_;
  bool open_ = false;
  std::vector<uint8_t> client_;
  std::vector<uint8_t> list_;
};

}   // namespace panel
}   // namespace synth1
