/* Synth1 program / bank select window (CSoundSelectWin, WndProc FUN_100209b0, cell paint
 * FUN_1001e800) — C++ port of tasks/m4-panel/web/panel_select.js (M4 B3).
 *
 * The window keeps a persistent client bitmap, painted exactly like the original: a full paint
 * (chrome + cells 0..127 in index order) on open / bank switch, and single-cell repaints on hover
 * changes (so a repainted cell's text descenders land on the cell below, as in the original).
 * Chrome (tree, "List/Info", labels) is the captured client per selected factory bank; the grid
 * is modelled: Rectangle(x, y, x+181, y+19) black pen + fill (colour tag, normal / highlighted),
 * "%d:%s" in GDI+ Arial 18 px (Wine: GDI ExtTextOut subpixel glyphs, GDI advances, layout rect
 * (x+1, y-1, 178, 18) with 3 px margins -> word wrap at 172 px, first line only, no clip).
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "panel_util.hpp"
#include "ui_font.hpp"
#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

struct SelectProgram {
  bool present = false;
  std::string name;
  std::string color;
  std::string sy1;                    // the program's .sy1 text (factory banks)
  std::map<int, int> values;          // the core's loaded GUI ints (precomputed, incl. upgrade chain)
  bool hasValues = false;
};

struct SelectBank {
  int slot = 0;
  std::string folder;
  int count = 0;
  std::vector<SelectProgram> programs;   // padded to 128 (empty slots: present=false)
};

struct TreeItem {
  int rect[4] = {0, 0, 0, 0};
  int chrome = 0;
  int bank = -1;   // -1 = a root node (no bank)
};

class SelectWin {
 public:
  SelectWin() = default;

  bool load(const Json& S, const std::vector<uint8_t>& chromeRGBA, const std::vector<SelectBank>& banksIn,
             std::string& err);

  void open(int bank, int prog);
  void mouse(const std::string& type, int x, int y);   // client coords: 'move' | 'down' | 'rdown'
  void key(const std::string& k);                       // 'Escape' | 'ArrowUp' | 'ArrowDown'
  void deactivate();   // WM_ACTIVATE(0) -> close, no load
  /* Web-edition label overlay (panel_app.js paintSel): the bank tree rows "NN:folder (count)", the
     heading "BankNN:folder" and "N Programs" are drawn from the bank data over the captured chrome
     (white-out first), with the built-in UI font. Off by default (the U2 gates use the captured
     chrome text as is). */
  void setLabelOverlay(bool on) { labels_ = on; if (isOpen) fullPaint(); }
  int treeItem() const { return item_; }
  int shownBank() const { return shown_; }

  bool isOpen = false;
  std::vector<uint8_t> client;   // CW*CH*4 RGBA
  int clientWidth() const { return CW_; }
  int clientHeight() const { return CH_; }
  const std::array<int, 3>& frame() const { return frame_; }

  std::function<void(int bank, int prog)> onLoad;
  std::function<void()> onClose;

 private:
  int CW_ = 0;
  int CH_ = 0;
  struct Grid {
    int x = 0;
    int y = 0;
    int cw = 0;
    int ch = 0;
    int cols = 0;
    int rows = 0;
    int wrap = 0;
  } g_;
  std::array<int, 3> frame_{};
  struct Glyph {
    int adv = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;
  };
  std::map<int, Glyph> glyph_;
  std::map<std::string, std::array<int, 3>> fillNormal_;
  std::map<std::string, std::array<int, 3>> fillHigh_;
  std::vector<TreeItem> tree_;
  std::vector<SelectBank> banks_;
  std::vector<uint8_t> chrome_;
  int item_ = 1;
  int shown_ = 0;
  int curBank_ = 0;
  int curProg_ = 0;
  int hover_ = -1;
  bool labels_ = false;
  void paintLabels();

  int itemOfBank(int b) const;
  bool hasGrid() const { return tree_[item_].bank >= 0; }
  int adv(int c) const;
  std::string firstLine(const std::string& s, int maxw) const;
  void px(int x, int y, int r, int gg, int b);
  void text(const std::string& s, int ox, int oy, const int fg[3]);
  std::array<int, 2> cellRect(int k) const;
  void paintCell(int k, bool high);
  void fullPaint();
  void selectItem(int i);
  int cellAt(int x, int y) const;
  void close();
};

/* Synth1's .sy1 header read (inferred): fgets(33) for the name, colour only from the 2nd read.
   Returns {name, color}. */
void sy1Head(const std::string& text, std::string& name, std::string& color);

/* banks.json -> the factory bank table (programs padded to 128, empty slots present=false). */
bool parseBanks(const Json& B, std::vector<SelectBank>& out, std::string& err);

}   // namespace panel
}   // namespace synth1
