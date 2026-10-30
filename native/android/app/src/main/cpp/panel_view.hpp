// panel_view.hpp — portable presentation + input of the Android shell (no Android types).
// The synth panel uses the largest integer scale k that fits the WHOLE screen (the keyboard never
// reserves height). Showing/hiding the keyboard never changes the panel scale or position.
//  Landscape: [ KEYS column | panel | VIEW column ] (columns fill the leftover width, classic look);
//             the keyboard (with the pitch/mod wheels at its left end) is an opaque OVERLAY on the
//             bottom of the screen. All buttons live in the columns ABOVE the overlay's top edge.
//  Portrait:  panel, a button strip under it, the keyboard below (overlay at the bottom if it does
//             not fit without overlapping).
// Wheels and INFO text are drawn at device resolution (anti-aliased); everything else is
// pixel-replicated at k. Host-testable (panel_view_test, input_router_test).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "input_router.hpp"
#include "keyboard.hpp"
#include "panel.hpp"

namespace synth1_android {

class PanelView {
 public:
  struct Rect { int x, y, w, h; };
  enum Item { kKbd, kInfo, kPanic, kOctDown, kOctLcd, kOctUp, kMidi, kItemCount };
  static constexpr int kPanelW = 466, kPanelH = 340;
  static constexpr int kHeaderH = 13;     // dark header bar of a box (logical px)
  static constexpr int kBtnH = 20;
  static constexpr int kMinColW = 60;     // minimum column width (logical px), both columns
  static constexpr int kStripH = 72;      // portrait button strip height (logical px)

  // skinRoot holds 100/ and 200/ (classic assets / assets200). dpi = physical pixels per inch.
  bool init(const std::string& skinRoot, int surfaceW, int surfaceH, std::string& err,
            bool keyboardVisible = true, float dpi = 420.f);
  void loadInitialProgram() { panel_->loadProgram(0, 0); }   // bank 00 program 1 (web startup state)
  bool rebuild(std::string& err);          // relayout (surface change): panel state restored
  bool resize(int w, int h, std::string& err) { surfW_ = w; surfH_ = h; return rebuild(err); }
  std::function<void()> onRebuilt;         // re-attach Panel callbacks (onParam / onProgram)

  synth1::panel::Panel& panel() { return *panel_; }
  synth1::keyboard::Keyboard& keyboard() { return *kb_; }
  OctaveShift& octave() { return oct_; }
  int skinScale() const { return skin_; }
  int mult() const { return mult_; }
  int nativeScale() const { return skin_ * mult_; }
  int panelW() const { return panel_->width(); }
  int panelH() const { return panel_->height(); }
  bool landscape() const { return landscape_; }
  bool keyboardVisible() const { return kbVisible_; }
  bool keyboardOverlaysPanel() const { return overlay_; }
  bool infoVisible() const { return info_; }
  int whiteKeys() const { return (int)kb_->whites.size(); }
  int octaves() const { return octaves_; }

  Rect panelRect() const { return panelR_; }
  Rect overlayRect() const { return ovR_; }               // keyboard area (always computed)
  Rect keyboardRect() const { return kbVisible_ ? ovR_ : Rect{0, 0, 0, 0}; }
  Rect keysRect() const { return keysR_; }                // the piano keys (surface px)
  Rect leftBoxRect() const { return boxL_.rect(); }
  Rect rightBoxRect() const { return boxR_.rect(); }
  Rect wheelRect(int i) const { return wheelRc_[i]; }     // 0 pitch, 1 mod
  Rect itemRect(Item i) const;                            // surface px
  int selectScale() const { return selScale_; }
  Rect selectRect() const;

  // Engine-facing callbacks (octave shift already applied to notes).
  std::function<void(int note, int vel)> onNoteOn;
  std::function<void(int note)> onNoteOff;
  std::function<void(int signed14)> onPitchBend;
  std::function<void(int value)> onModWheel;
  std::function<void()> onAllNotesOff;                    // PANIC: shell sends note-off 0..127

  void setInfoText(const std::string& s) { infoText_ = s; }
  void setInfoVisible(bool on) { info_ = on; }
  void setMidiLamp(bool on) { midiLamp_ = on; }
  void setModWheel(int v) { mod_ = v; }
  void setPitchBend(int s14) { pitch_ = s14; }

  void composePanel(std::vector<uint8_t>& rgba);
  void blit(uint8_t* dst, int dstW, int dstH, int rowBytes);   // fills every pixel (frame colour)
  bool toPanel(float sx, float sy, int& px, int& py) const;

  // Pointer events in surface px. action: 0 down, 1 move, 2 up, 3 cancel (all up).
  void pointer(int action, int id, float sx, float sy, long long timeMs = -1);
  void pointer(int action, float sx, float sy) { pointer(action, 0, sx, sy); }

 private:
  enum Region { kNone, kPanel, kSelect, kKeys, kWheel, kButton, kSink };
  struct Box {
    int lw = 0, lh = 0, px = 0, py = 0, scale = 1;   // logical size, surface px of the top-left
    std::string title;
    std::vector<uint8_t> buf;
    Rect rect() const { return {px, py, lw * scale, lh * scale}; }
  };
  struct Cell { Item item; int box, x, y, w, h; };   // logical, inside boxes_[box]
  std::unique_ptr<synth1::panel::Panel> panel_;
  std::unique_ptr<synth1::keyboard::Keyboard> kb_;
  OctaveShift oct_;
  std::string root_, infoText_;
  int skin_ = 1, mult_ = 1, surfW_ = 0, surfH_ = 0, selScale_ = 1, octaves_ = 3, kbLow_ = 48;
  bool landscape_ = true, kbVisible_ = true, overlay_ = true, info_ = false, midiLamp_ = false;
  float dpi_ = 420.f;
  Rect panelR_{}, ovR_{}, keysR_{}, wheelRc_[2]{};
  Box boxL_, boxR_;
  int pitch_ = 0, mod_ = 0, ovBorder_ = 3;
  std::vector<Cell> cells_;
  int kbLogicalW_ = 0, kbLogicalH_ = 0;
  std::vector<uint8_t> frame_, keysFrame_;
  std::map<int, Region> region_;
  std::map<int, int> btn_, heldWheel_;      // pointer id -> pressed item / wheel
  int activeWheel_ = -1;
  void layoutCells();
  void paintBoxes();
  void drawOverlay(uint8_t* dst, int dw, int dh, int rowBytes);
  void drawWheels(uint8_t* dst, int dw, int dh, int rowBytes);
  void drawInfo(uint8_t* dst, int dw, int dh, int rowBytes);
  void setWheelFromY(int wheel, float sy);
  void drawKeycap(Box& b, const Cell& c, const char* label, bool lit, bool pressed);
  void releaseAll();
  void wireKeyboard();
  void doItem(Item it);
};

}  // namespace synth1_android
