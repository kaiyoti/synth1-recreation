/* Skin loader: the panel skin is DATA (never compiled in) — the captured files of
 * tasks/m4-panel/web/assets (100 %) or assets200 (native 200 %):
 *   panel_data.json widgets.json popups.json select.json
 *   base.png atlas.png popups.png select.png banks.json init_sound.json
 *   opt/opt.json opt/opt.png        (Options dialog assets, 1x only; optional)
 * popups/select/banks/init_sound/opt are optional (a minimal skin may ship only the panel).
 */
#pragma once

#include <string>

#include "panel_popups.hpp"
#include "panel_select.hpp"
#include "panel_util.hpp"
#include "panel_widgets.hpp"
#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

struct Skin {
  std::string dir;
  Json panelData;
  Json widgets;
  Json popups;
  Json select;
  Json banks;
  Json initSound;
  Json opt;
  Image base;
  Image atlas;
  Image popupsPng;
  Image selectPng;
  Image optPng;
  bool hasPopups = false;
  bool hasSelect = false;
  bool hasBanks = false;
  bool hasInitSound = false;
  bool hasOpt = false;
};

bool loadSkin(const std::string& dir, Skin& skin, std::string& err);

}   // namespace panel
}   // namespace synth1
