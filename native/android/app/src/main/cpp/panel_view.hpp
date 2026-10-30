// panel_view.hpp — portable presentation + input of the Android shell (no Android types).
// Landscape:  [ WHEEL column | Synth1 panel | VIEW column ]   columns stretch to fill the width
//             [        on-screen keyboard (full width, no wheel area)        ]
// Portrait:   [ Synth1 panel ] / [ WHEEL | VIEW strip ] / [ keyboard ]
// Everything is drawn in C++ in the classic theme and pixel-replicated at an integer scale k (1x/2x
// skin, no resampling); nothing on the surface is black clear colour (frame colour instead).
// VIEW column: KBD (keyboard on/off), INFO (status overlay), PANIC (all notes off), OCT -/+ with a
// teal LCD readout, MIDI-in lamp. Wheels and the INFO text are drawn at device resolution
// (anti-aliased, procedural); the rest is pixel-replicated. Host-testable (panel_view_test, input_router_test).
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
  static constexpr int kMinWheelCol = 60, kMinViewCol = 64;   // minimum column widths (logical px)
  static constexpr int kStripH = 96;      // portrait strip height (logical px)

  // skinRoot holds 100/ and 200/ (classic assets / assets200). dpi = physical pixels per inch of the
  // display (white key target 9 mm).
  bool init(const std::string& skinRoot, int surfaceW, int surfaceH, std::string& err,
            bool keyboardVisible = true, float dpi = 420.f);
  // App startup state: factory bank 00 program 1 loaded as the web edition does.
  void loadInitialProgram() { panel_->loadProgram(0, 0); }
  // Relayout (KBD toggle / surface change): the panel is rebuilt, edited values + program restored.
  bool rebuild(std::string& err);
  bool resize(int w, int h, std::string& err) { surfW_ = w; surfH_ = h; return rebuild(err); }
  std::function<void()> onRebuilt;       // re-attach Panel callbacks (onParam / onProgram)

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
  bool infoVisible() const { return info_; }
  int whiteKeys() const { return (int)kb_->whites.size(); }
  int octaves() const { return octaves_; }

  Rect panelRect() const { return panelR_; }
  Rect keyboardRect() const { return kbVisible_ ? belowR_ : Rect{0, 0, 0, 0}; }
  Rect belowRect() const { return belowR_; }          // keyboard area (also the INFO overlay area)
  Rect wheelBoxRect() const { return wheelBox_.rect(); }
  Rect viewBoxRect() const { return viewBox_.rect(); }
  Rect wheelRect() const { return wheelR_; }          // both wheels (surface px)
  Rect wheelRect(int i) const { return wheelRc_[i]; } // 0 pitch, 1 mod (device-resolution, procedural)
  Rect itemRect(Item i) const;                        // surface px
  int selectScale() const { return selScale_; }
  Rect selectRect() const;

  // Engine-facing callbacks (octave shift already applied to notes).
  std::function<void(int note, int vel)> onNoteOn;
  std::function<void(int note)> onNoteOff;
  std::function<void(int signed14)> onPitchBend;
  std::function<void(int value)> onModWheel;
  std::function<void()> onAllNotesOff;                // PANIC: shell sends note-off 0..127

  // status text (INFO overlay), '|' separates lines
  void setInfoText(const std::string& s) { infoText_ = s; }
  void setInfoVisible(bool on) { info_ = on; }
  void setMidiLamp(bool on) { midiLamp_ = on; }

  void composePanel(std::vector<uint8_t>& rgba);
  void blit(uint8_t* dst, int dstW, int dstH, int rowBytes);   // fills every pixel (frame colour)
  bool toPanel(float sx, float sy, int& px, int& py) const;

  // Pointer events in surface px. action: 0 down, 1 move, 2 up, 3 cancel (all up).
  void pointer(int action, int id, float sx, float sy, long long timeMs = -1);
  void pointer(int action, float sx, float sy) { pointer(action, 0, sx, sy); }

  void setModWheel(int v) { mod_ = v; }
  void setPitchBend(int s14) { pitch_ = s14; }

 private:
  enum Region { kNone, kPanel, kSelect, kKeys, kWheel, kButton };
  struct Box {
    int lw = 0, lh = 0, px = 0, py = 0;      // logical size, surface px of its top-left (may be < 0)
    std::vector<uint8_t> buf;
    Rect rect() const;
    int scale = 1;
  };
  struct Cell { Item item; int x, y, w, h; };   // logical, in the view box
  std::unique_ptr<synth1::panel::Panel> panel_;
  std::unique_ptr<synth1::keyboard::Keyboard> kb_;
  OctaveShift oct_;
  std::string root_, infoText_;
  int skin_ = 1, mult_ = 1, surfW_ = 0, surfH_ = 0, selScale_ = 1, octaves_ = 2, kbLow_ = 48;
  bool landscape_ = true, kbVisible_ = true, info_ = false, midiLamp_ = false, needRebuild_ = false;
  float dpi_ = 420.f;
  Rect panelR_{}, belowR_{}, wheelR_{}, wheelRc_[2]{};
  int pitch_ = 0, mod_ = 0;
  std::map<int, int> heldWheel_;           // pointer id -> wheel (0 pitch, 1 mod)
  Box wheelBox_, viewBox_;
  std::vector<Cell> cells_;
  int kbLogicalW_ = 0, kbLogicalH_ = 0;
  std::vector<uint8_t> frame_, belowFrame_;
  std::map<int, Region> region_;
  std::map<int, int> btn_;                  // pointer id -> pressed item
  int activeWheel_ = -1, keysDown_ = 0;
  void layoutCells();
  void paintBoxes();
  void paintBelow();
  void drawWheels(uint8_t* dst, int dw, int dh, int rowBytes);
  void drawInfo(uint8_t* dst, int dw, int dh, int rowBytes);
  void setWheelFromY(int wheel, float sy);
  void drawKeycap(Box& b, const Cell& c, const char* label, bool lit, bool pressed);
  void releaseAll();
  void wireKeyboards();
  void doItem(Item it);
};

}  // namespace synth1_android
