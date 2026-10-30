/* Synth1 editor popups — C++ port of tasks/m4-panel/web/panel_popups.js (M4 B3):
 * the slider popup (CVolSubWin) and the list popup (CLCDSubWin), drawn from window frames
 * captured from the original editor. Popup windows are separate top-level windows in the
 * original; here they compose to RGBA at their panel position (the host / the Panel facade
 * blits them into the panel buffer exactly where the web overlay canvas draws them).
 *
 *   PopupAssets pops; parsePopupAssets(popupsJson, pops, err);
 *   Composed c; composePopup(emu.popup, pops, popupsAtlas, c);   // panel coords, RGBA
 *   ListGeom g = listGeom(pops, ownerId);                          // for the widget emulator
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "panel_util.hpp"
#include "panel_widgets.hpp"
#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

struct SliderEntry {
  int win[4] = {0, 0, 0, 0};
  std::map<int, int> values;   // GUI value -> popups atlas tile index
};

struct ListEntry {
  int win[4] = {0, 0, 0, 0};
  int client[4] = {0, 0, 0, 0};
  int rows = 0;
  int cols = 0;
  int cellw = 0;
  int cellh = 0;
  int count = 0;
  std::vector<int> values;
  int base = 0;
  std::vector<std::array<int, 4>> cells;   // per cell: plain / hover / cur / cur+hover tile ids
};

struct PopupAssets {
  std::map<int, SliderEntry> slider;
  std::map<int, ListEntry> list;
  std::vector<std::array<int, 4>> tiles;   // [x, y, w, h] in popups.png
  int nslider = 0;
  int sliderAtlasH = 0;
};

bool parsePopupAssets(const Json& p, PopupAssets& out, std::string& err);

struct Composed {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  std::vector<uint8_t> data;   // w*h*4 RGBA
};

bool composePopup(const Popup& p, const PopupAssets& pops, const Image& atlas, Composed& out);

ListGeom listGeom(const PopupAssets& pops, int ownerId);

}   // namespace panel
}   // namespace synth1
