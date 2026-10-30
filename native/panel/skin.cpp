#include "skin.hpp"

namespace synth1 {
namespace panel {

namespace {

bool loadJson(const std::string& path, Json& out, std::string& err) {
  std::vector<uint8_t> bytes;
  if (!readFile(path, bytes, err)) return false;
  Json j = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (j.is_discarded()) {
    err = "json parse failed: " + path;
    return false;
  }
  out = std::move(j);
  return true;
}

}   // namespace

bool loadSkin(const std::string& dir, Skin& skin, std::string& err) {
  skin = Skin();
  skin.dir = dir;
  const std::string slash = dir.empty() || dir.back() == '/' ? "" : "/";
  const std::string D = dir + slash;
  if (!loadJson(D + "panel_data.json", skin.panelData, err)) return false;
  if (!loadJson(D + "widgets.json", skin.widgets, err)) return false;
  if (!loadPng(D + "base.png", skin.base, err)) return false;
  if (!loadPng(D + "atlas.png", skin.atlas, err)) return false;

  {   // optional popups
    Json j;
    std::string e;
    if (loadJson(D + "popups.json", j, e)) {
      skin.popups = std::move(j);
      if (!loadPng(D + "popups.png", skin.popupsPng, err)) return false;
      skin.hasPopups = true;
    }
  }
  {   // optional select window
    Json j;
    std::string e;
    if (loadJson(D + "select.json", j, e)) {
      skin.select = std::move(j);
      if (!loadPng(D + "select.png", skin.selectPng, err)) return false;
      skin.hasSelect = true;
    }
  }
  {
    Json j;
    std::string e;
    if (loadJson(D + "banks.json", j, e)) {
      skin.banks = std::move(j);
      skin.hasBanks = true;
    }
  }
  {
    Json j;
    std::string e;
    if (loadJson(D + "init_sound.json", j, e)) {
      skin.initSound = std::move(j);
      skin.hasInitSound = true;
    }
  }
  {   // optional Options dialog assets (1x only; the UI draws them 2x2 at 200 %)
    Json j;
    std::string e;
    if (loadJson(D + "opt/opt.json", j, e)) {
      if (loadPng(D + "opt/opt.png", skin.optPng, e)) {
        skin.opt = std::move(j);
        skin.hasOpt = true;
      }
    }
  }
  return true;
}

}   // namespace panel
}   // namespace synth1
