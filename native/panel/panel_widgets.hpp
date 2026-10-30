/* Synth1 v1.13 editor behaviour — C++ port of tasks/m4-panel/web/panel_widgets.js (M4 B3).
 *
 * The widget list (assets/widgets.json) is the editor's own canvas child array, dumped from the
 * live process; the handlers are ports of the decompiled 32-bit code (addresses cited in the
 * .js / NOTES.md). State the original keeps in widget fields is derived from the GUI parameter
 * values.
 *
 *   Widgets emu(widgetsJson, 1);
 *   emu.values[p] = GUI int;
 *   emu.mouse(MouseType::Down, x, y);   // PANEL coords (the editor bitmap)
 *   emu.wheel(delta);                   // to the slider popup
 *
 * The editor's canvas window (CCWindow0, style 0x52800000) has WS_BORDER: canvas client
 * coords = panel coords - (1, 1). Widget rects and popup positions are canvas coords; popup
 * rects are reported in panel coords (popup.px, popup.py).
 *   emu.onCommand = [](int id, int value) {}   // the 0x709 commands (FUN_1002fa40):
 *                                              // 1000..1098 params, 1252 program, 1254 volume,
 *                                              // 3/5 select window, 15 opt, 94 write
 *   emu.pressed  -> held push button / spin {id, i, half}, half: 0 none, 1 up, 2 down
 *   emu.popup    -> kind 0 none | 1 slider | 2 list
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

struct WidgetField {
  int64_t num = 0;
  int ref = -1;   // >= 0 => the field holds a widget index (a {ref: n} JSON object)
};

struct WidgetItem {
  int value = 0;
  std::string text;
};

struct Widget {
  int i = 0;
  std::string cls;
  int id = 0;
  int rect[4] = {0, 0, 0, 0};   // left, top, right, bottom
  std::string label;
  std::map<int, WidgetField> f;
  std::vector<WidgetItem> items;
  bool hasItems = false;
};

/* Geometry the emulator needs for list popups (cell hit-testing in canvas coords). */
struct ListGeom {
  int cl = 0;
  int ct = 0;
  int cw = 0;
  int ch = 0;
  int mx = 0;
  int my = 0;
  int cellw = 0;
  int cellh = 0;
  bool ok = false;
};

struct Popup {
  int kind = 0;   // 0 none, 1 slider (CVolSubWin), 2 list (CLCDSubWin)
  const Widget* owner = nullptr;
  int ownerId = 0;
  // slider
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int min = 0;
  int max = 0;
  int value = 0;
  int last = 0;
  int cl = 0;
  int ct = 0;
  int cw = 0;
  int ch = 0;
  int px = 0;   // window rect in panel coords
  int py = 0;
  int warpX = 0;   // cursor warp target (panel coords)
  int warpY = 0;
  bool dragging = false;
  // list
  int rows = 0;
  int cols = 0;
  int count = 0;
  int current = 0;
  int hover = -1;
  ListGeom geom;
};

struct Pressed {
  int id = 0;
  int i = 0;
  int half = 0;   // 0 none, 1 up half, 2 down half
};

enum class MouseType { Down, Up, Move, DblClick, RDown };

class Widgets {
 public:
  Widgets(const std::vector<Widget>& widgets, int slidertype = 1);

  std::vector<Widget> widgets_;   // owned copy (canvas child array, hit-test order)
  std::map<int, int> values;      // GUI int domain: params 0..98, 254 volume, 252 program
  bool hasPressed = false;
  Pressed pressed;
  Popup popup;
  int hover = -1;
  std::vector<std::string> damage;   // released widget ids, oldest first
  std::function<void(int id, int value)> onCommand;
  std::function<ListGeom(int ownerId)> listGeomFn;
  int slidertype = 1;
  std::vector<std::pair<int, int>> log;   // every sent (id, value)

  void mouse(MouseType type, int px, int py);
  void wheel(int delta);

  int find(int x, int y) const;
  bool ledLit(const Widget& l) const;

  /* Multi-pointer tap path (P1): a second pointer acting on a DIFFERENT widget while another
     pointer holds the exclusive state (popup / drag / held button). tapSafe(w) = the widget's
     down action is instantaneous and non-exclusive (CLED, CToggleButton, CLCD kind-0); tapDown
     performs it at panel coords WITHOUT touching hover / pressed / drag / popup / damage. */
  bool tapSafe(int widgetIndex) const;
  void tapDown(int px, int py);
  bool dragging() const { return drag_.active; }
  /* True when an open list popup has a cell under these panel coords (touch preview hit-test). */
  bool listCellHit(int px, int py) const;

 private:
  int64_t F(const Widget& w, int off) const;
  const Widget* ref(const Widget& w, int off) const;
  void send(int id, int value);
  int paramOf(const Widget& w) const;
  bool hit(const Widget& w, int x, int y) const;
  void pushDown(const Widget& w);
  void pushLed(const Widget& owner, int ledId);
  void lcdNext(const Widget& w);
  void lcdPrev(const Widget& w);
  int lcdIndex(const Widget& w) const;
  int knobOrLcd(const Widget& w) const;
  int knobValue(const Widget& w) const;
  void spinDown(const Widget& w, int x, int y);
  void toggleDown(const Widget& w);
  void ledDown(const Widget& w);
  void knobDown(const Widget& w, int x, int y);
  void lcdDown(const Widget& w, int x, int y);
  void openSlider(const Widget& owner, int x, int y, int min, int max, int value);
  void sliderMove(Popup& pop, int y);
  void ownerSet(const Widget* w, int v);
  bool sliderMouse(MouseType type, int x, int y);
  void openList(const Widget& w);
  int listCell(const Popup& pop, int x, int y) const;
  bool listMouse(MouseType type, int x, int y);
  void leave(const Widget& w);
  void up(const Widget& w);
  void down(const Widget& w, int x, int y);
  void closePopup() { popup = Popup(); }

  std::map<int, std::vector<int>> byId_;
  struct Drag {
    const Widget* w = nullptr;
    int y0 = 0;
    int min = 0;
    int max = 0;
    int start = 0;
    bool active = false;
  } drag_;
};

/* Parse widgets.json into the canvas child array. */
bool parseWidgets(const Json& W, std::vector<Widget>& out, std::string& err);

}   // namespace panel
}   // namespace synth1
