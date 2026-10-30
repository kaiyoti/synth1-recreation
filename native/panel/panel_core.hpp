/* Synth1 v1.13 panel compositor — C++ port of tasks/m4-panel/web/panel_core.js (M4 B3).
 *
 * Renders the 466x340 editor bitmap (932x680 at native 200 % skin) into an RGBA buffer,
 * pixel-for-pixel as the original editor draws it under the reference host.
 *
 *   PanelCore core;  core.load(panelDataJson, baseRGBA, atlasRGBA, atlasWidth, err);
 *   PanelCore::State st;  st.values[param] = GUI int;  st.lcd["program"] = "...";
 *   st.pressed = "<widget id>" | "<id>u" | "<id>d" (held);  st.damage = released ids, oldest first;
 *   core.render(st, outRGBA);
 *
 * Controls: every parameter's widget pixels are a masked tile captured per value (base.png holds
 * every pixel no control ever changes). LCD text: FUN_10008ba0 (0x10008ba0) replayed exactly:
 * the background under the text box is pixel-replicated to 2x, the Wine "Lucida Console"
 * (weight 600, 2x height h2) glyphs are blended with Wine's subpixel blend_color
 * ((src*a + dst*(255-a) + 127) / 255 per channel, gamma 1000), and the 2x box is averaged back
 * 2x2 with >> 2. The box is the DrawText DT_CALCRECT width rounded up to even, clipped to the
 * LCD, and glyph pixels are clipped to the calc rect.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "third_party/json.hpp"

namespace synth1 {
namespace panel {

using Json = nlohmann::ordered_json;

struct LcdDef {
  std::string id;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int h2 = 0;
  std::string font;
};

struct Glyph {
  int adv = 0;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  std::vector<uint8_t> rgb;   // empty => no pixel data (space)
};

struct ControlDef {
  int param = 0;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  std::vector<int> idx;                  // mask pixel indices, row-major within w
  std::vector<int> tileX;                // tiles[v] = atlas tile origin
  std::vector<int> tileY;
  std::vector<int> domain;               // GUI int domain (when present)
  std::unordered_map<int, int> dmap;     // GUI int -> tile index (last set wins, as JS Map.set)
  bool hasDomain = false;
};

struct FootprintTile {   // pressed / released repaint footprint (rect + 3 px shadow)
  int x = 0;
  int y = 0;
  int w = 0;
  std::vector<int> idx;
  std::vector<uint8_t> rgb;
};

class PanelCore {
 public:
  struct State {
    std::map<int, int> values;                 // param -> GUI int (only present params are drawn)
    std::map<std::string, std::string> lcd;    // lcd id -> display text
    std::string pressed;                       // '' = none held
    std::vector<std::string> damage;           // released widget ids, oldest first
  };

  PanelCore() = default;

  bool load(const Json& data, const std::vector<uint8_t>& baseRGBA,
            const std::vector<uint8_t>& atlasRGBA, int atlasWidth, std::string& err);

  int width() const { return W_; }
  int height() const { return H_; }
  int atlasWidth() const { return atlasW_; }

  /* out = W*H*4 RGBA buffer. */
  void render(const State& st, uint8_t* out) const;

  int textWidth(const std::string& fontName, const std::string& s) const;

  /* Display strings the editor builds for the two LCD lines (FUN @ 0x1002f... wsprintfA
     "%d:%s  " / "%02d:%s  "). */
  static std::string programText(int index0, const std::string& name) {
    return std::to_string(index0 + 1) + ':' + name + "  ";
  }
  static std::string bankText(int bank, const std::string& name) {
    return (bank < 10 ? "0" : "") + std::to_string(bank) + ':' + name + "  ";
  }

  const std::vector<LcdDef>& lcds() const { return lcds_; }

 private:
  void blitTile(const FootprintTile& pk, uint8_t* out) const;
  void drawLcd(uint8_t* out, const LcdDef& L, const std::string& s) const;

  int W_ = 0;
  int H_ = 0;
  std::vector<ControlDef> controls_;
  std::map<std::string, std::map<int, Glyph>> fonts_;
  std::vector<LcdDef> lcds_;
  int fg_[3] = {0, 0, 0};
  std::map<std::string, FootprintTile> pressed_;
  std::map<std::string, FootprintTile> released_;
  std::vector<uint8_t> base_;
  std::vector<uint8_t> atlas_;
  int atlasW_ = 0;
};

}   // namespace panel
}   // namespace synth1
