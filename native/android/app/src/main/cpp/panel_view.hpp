// panel_view.hpp — portable presentation + input of the Android shell (no Android types):
//   [ Synth1 panel (drawn skin) ]   integer native scale k (1x/2x skin, pixel-replicated, no resampling)
//   [ octave bar                ]   Oct- / Oct+ buttons + label
//   [ on-screen keyboard        ]   portable-synth/keyboard (wheels, multitouch, glissando)
// Compose into a caller buffer, multi-pointer routing (panel P1 API / select window / keyboard / bar).
// Host-testable (panel_view_test, input_router_test).
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
  static constexpr int kBarH = 16;      // octave bar height (logical px)
  static constexpr int kKbMin = 56;     // keyboard height range (logical px)
  static constexpr int kKbMax = 110;

  // skinRoot holds 100/ and 200/ (the skin's assets/ and assets200/ dirs, e.g. skin/classic).
  // Native scale k = floor(min(W/466, H/(340+kBarH+kKbMin))) (>=1): the 200 % skin at 1:1 for k=2,
  // pixel-doubled for k=4, the 100 % skin pixel-replicated for odd k.
  bool init(const std::string& skinRoot, int surfaceW, int surfaceH, std::string& err);

  synth1::panel::Panel& panel() { return *panel_; }
  synth1::keyboard::Keyboard& keyboard() { return *kb_; }
  OctaveShift& octave() { return oct_; }
  int skinScale() const { return skin_; }     // 1 or 2 (which skin dir)
  int mult() const { return mult_; }          // integer replication on top of the skin
  int nativeScale() const { return skin_ * mult_; }
  int panelW() const { return panel_->width(); }
  int panelH() const { return panel_->height(); }
  // layout in surface px
  struct Rect { int x, y, w, h; };
  Rect panelRect() const { return panelR_; }
  Rect barRect() const { return barR_; }
  Rect keyboardRect() const { return kbR_; }
  // The program/bank select window is a top-level window of fixed size (985x621 client, not
  // skin-scaled): drawn over the whole surface at its own integer scale, centred on the panel and
  // clamped into the surface. Valid after init().
  int selectScale() const { return selScale_; }
  Rect selectRect() const;

  // Engine-facing callbacks (octave shift already applied to notes).
  std::function<void(int note, int vel)> onNoteOn;
  std::function<void(int note)> onNoteOff;
  std::function<void(int signed14)> onPitchBend;
  std::function<void(int value)> onModWheel;

  // Panel + popups at panel resolution (panelW*panelH*4 RGBA).
  void composePanel(std::vector<uint8_t>& rgba);
  // Black-filled dst (dstW x dstH, rowBytes stride, RGBA): panel, bar, keyboard, select window.
  void blit(uint8_t* dst, int dstW, int dstH, int rowBytes);
  // Surface px -> panel px; false when outside the panel rect.
  bool toPanel(float sx, float sy, int& px, int& py) const;

  // Pointer events in surface px. action: 0 down, 1 move, 2 up, 3 cancel (id ignored, all up).
  // timeMs: event time for the panel's double-tap / long-press (touch mode).
  void pointer(int action, int id, float sx, float sy, long long timeMs = -1);
  // single-pointer convenience (id 0)
  void pointer(int action, float sx, float sy) { pointer(action, 0, sx, sy); }

  // external controller state shown on the wheels (MIDI in)
  void setModWheel(int v) { kb_->setModWheel(v); }
  void setPitchBend(int s14) { kb_->setPitchBend(s14); }

 private:
  enum Region { kNone, kPanel, kSelect, kKeys, kBar };
  std::unique_ptr<synth1::panel::Panel> panel_;
  std::unique_ptr<synth1::keyboard::Keyboard> kb_;
  OctaveShift oct_;
  int skin_ = 1, mult_ = 1, surfW_ = 0, surfH_ = 0;
  Rect panelR_{}, barR_{}, kbR_{};
  int kbLogicalW_ = 0, kbLogicalH_ = 0, selScale_ = 1;
  std::vector<uint8_t> frame_, kbFrame_, barFrame_, selFrame_;
  std::map<int, Region> region_;
  std::map<int, int> barBtn_;      // pointer id -> 0 (Oct-) / 1 (Oct+)
  int activeWheel_ = -1;
  void wireKeyboard();
  void barRects(int& minusX, int& plusX, int& btnW) const;
  void paintBar();
  void releaseAll();
};

}  // namespace synth1_android
