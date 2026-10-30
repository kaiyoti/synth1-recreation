/* Synth1 universal panel — the C++ counterpart of the M4 web panel app (tasks/m4-panel/web/
 * panel_app.js) for the U2 universal build: portable C++17 with NO OS and NO GUI-toolkit code.
 * It renders the editor into an RGBA buffer and consumes pointer/key events; a plugin UI (U3)
 * only blits the buffer and forwards events.
 *
 *   synth1::panel::Panel panel;
 *   panel.load("tasks/m4-panel/web/assets", 1);         // the skin dir (assets200 for 200 %)
 *   panel.loadProgram(0, 0);
 *   panel.render(rgba);                                 // width()*height()*4 bytes
 *   panel.pointerDown(x, y); panel.pointerUp(x, y);     // panel/buffer coords
 *   panel.key(synth1::panel::KeyArrowDown);             // select-window keys
 *
 * Popups (CVolSubWin / CLCDSubWin) are drawn into the SAME buffer where the web overlay canvas
 * draws them (clipped at the panel edge). The program/bank select window is a top-level window
 * in the original: its client bitmap is exposed via selectClient()/selectPosition() for the host
 * to blit; its events go through the selectPointer* methods.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "panel_core.hpp"
#include "panel_opt.hpp"
#include "panel_popups.hpp"
#include "panel_select.hpp"
#include "panel_widgets.hpp"
#include "skin.hpp"

namespace synth1 {
namespace panel {

enum PanelKey {
  KeyEnter = 13,
  KeyEscape = 27,
  KeyArrowUp = 38,
  KeyArrowDown = 40,
};

class Panel {
 public:
  Panel() = default;

  /* scale = 1 for a 100 % skin dir, 2 for the native 200 % skin dir (assets200). */
  bool load(const std::string& skinDir, int scale, std::string& err);

  int width() const { return core_.width(); }
  int height() const { return core_.height(); }
  int scale() const { return scale_; }

  /* Panel bitmap + popups (the web overlay) into rgba (width()*height()*4).
     withPopups=false renders the panel WITHOUT the popup overlay (the host composites the popup
     itself with the transparencywin alpha — see popupComp()/popupKind()). */
  void render(uint8_t* rgba, bool withPopups = true);

  void pointerDown(int x, int y);
  void pointerMove(int x, int y);
  void pointerUp(int x, int y);
  void dblClick(int x, int y);
  void rightDown(int x, int y);
  void wheel(int delta);
  void key(int code);   // KeyEscape / KeyArrowUp / KeyArrowDown while the select window is open

  /* ---- Multi-pointer (P1 touch, m3 mobile plan) ----
     pointerDown/Move/Up(id, x, y) carry a pointer id; the panel keeps per-pointer state.
     The single-pointer methods above are pointer id 0 with identical behaviour (they delegate
     here). timeMs is the event timestamp in milliseconds; -1 reads the steady clock (only used
     while touch mode is on — the mouse path never reads it).

     The original editor is single-capture: exactly one pointer may hold the exclusive emulator
     state (an open slider/list popup, an active knob/lcd drag, or a held push button / spin).
     The rule this API enforces:
       - the FIRST pointer down that engages exclusive state OWNS that widget and the state;
       - a second pointer inside the open popup rect is ignored (the popup is the owner's
         single-capture overlay);
       - a second pointer on the SAME widget as the owner is ignored (a second finger on the
         same knob does nothing);
       - a second pointer may act on a DIFFERENT tap-safe widget (CLED, CToggleButton, CLCD
         kind-0: instantaneous, non-exclusive actions — e.g. tapping an LED group while another
         finger drags a knob's slider popup);
       - a second pointer on any other widget is ignored (no second popup / drag / held button).
     Pointer id 0 carries no privilege: it is only the default id used by the legacy methods. */
  void pointerDown(int id, int x, int y, long long timeMs = -1);
  void pointerMove(int id, int x, int y, long long timeMs = -1);
  void pointerUp(int id, int x, int y, long long timeMs = -1);
  int activePointerCount() const { return static_cast<int>(ptrs_.size()); }

  /* ---- Touch semantics (opt-in; the mouse path is unchanged when off) ----
     - hover-dependent behaviours (list popups, select-window cells): touch-down PREVIEWS the
       hover state (a Move into the cell), the LIFT selects (a Down);
     - double-click -> double-tap: a second tap within 300 ms and 10 px of the previous tap on
       the same widget emits DblClick (then Up);
     - right-click equivalents -> long-press: a pointer held >= 500 ms with movement <= 10 px
       emits one RDown at the press point (evaluated on the next move/up carrying timeMs). */
  static constexpr long long kDoubleTapMs = 300;
  static constexpr int kDoubleTapPx = 10;
  static constexpr long long kLongPressMs = 500;
  void setTouchMode(bool on) { touchMode_ = on; }
  bool touchMode() const { return touchMode_; }

  /* GUI int domain: params 0..98, 252 program, 254 master volume. */
  int getParam(int id) const;
  bool hasParam(int id) const;
  void setParam(int id, int value);
  void setValues(const std::map<int, int>& v);

  void setLcd(const std::string& program, const std::string& bank);
  const std::map<std::string, std::string>& lcd() const { return lcd_; }

  /* Load factory program prog of bank b (indices into the skin's banks.json) as the editor does. */
  void loadProgram(int bank, int prog);
  /* Factory bank table (banks.json order): slot number of bank index i (false past the end). */
  bool bankSlot(int i, int& slot) const {
    if (i < 0 || i >= static_cast<int>(banks_.size())) return false;
    slot = banks_[i].slot;
    return true;
  }
  int currentBank() const { return curBank_; }
  int currentProgram() const { return curProgram_; }
  int bankCount() const { return static_cast<int>(banks_.size()); }

  /* .sy1 text in/out (the web panel_app parseSy1 / sy1Text). */
  void loadSy1(const std::string& text, int programIndex, const std::string& bankLabel);
  std::string sy1Text() const;

  /* Program/bank select window (a top-level window in the original). */
  bool selectOpen() const { return sel_.isOpen; }
  const std::vector<uint8_t>& selectClient() const { return sel_.client; }
  int selectClientWidth() const { return sel_.clientWidth(); }
  int selectClientHeight() const { return sel_.clientHeight(); }
  const std::array<int, 3>& selectFrameColor() const { return sel_.frame(); }
  /* The window rect centred on the panel (panel coords; x/y may be negative). */
  std::array<int, 4> selectPosition() const;
  void selectPointerMove(int x, int y);   // select client coords
  void selectPointerDown(int x, int y);
  /* Touch semantics for the select window (touch mode only): selectPointerDown PREIEWS the cell
     under the finger (hover repaint), selectPointerUp on the same cell selects it. In mouse mode
     selectPointerDown stays the plain down (bit-identical). */
  void selectPointerUp(int x, int y);
  void selectRightDown(int x, int y);
  void selectDeactivate();
  /* Android/web label overlay for the select window (bank tree text from the bank data). */
  void setSelectLabels(bool on) { sel_.setLabelOverlay(on); }

  /* Options dialog (OPT button, 0x709 command 15; COptionDlg 0xa6). Assets are DATA:
     <skinDir>/opt/opt.{json,png}; missing -> optAvailable() false and OPT does nothing.
     The dialog client (502x438) is a top-level window in the original: the host blits
     optClient() over the panel at optPosition() (panel-buffer coords, already scaled:
     client + 1 px border per side, centred on the panel, clamped to the panel origin —
     the select-window law) and optList() (the open combo drop list) at optListPos()
     (dialog client coords). Events go through the optPointerDown/Move/Up and optKey methods
     (dialog client coords). While it is open the host blocks panel input (modal). */
  bool optAvailable() const { return skin_.hasOpt; }
  bool optOpen() const { return opt_.isOpen(); }
  void openOpt(const OptSettings& s);
  void closeOpt();   // host-side close (no result)
  const std::vector<uint8_t>& optClient() const { return opt_.client(); }
  const std::vector<uint8_t>& optList() const { return opt_.list(); }   // empty = closed
  std::array<int, 4> optListPos() const { return opt_.listPos(); }
  std::array<int, 4> optPosition() const;
  void optPointerDown(int x, int y);   // dialog client coords
  void optPointerMove(int x, int y);
  void optPointerUp();
  void optKey(int code);               // KeyEscape / KeyEnter
  OptDialog& optDialog() { return opt_; }

  /* opt slidertype -> the widget emulator's slider mode (1 popup / 0 drag). */
  void setSlidertype(int v) { emu_.slidertype = v == 0 ? 0 : 1; }

  /* The last render()'s popup composition (for the host's transparencywin blend):
     kind 1 = slider popup (alpha 191), 2 = list popup (alpha 204), 0 = none. */
  const Composed& popupComp() const { return popComp_; }
  int popupKind() const { return emu_.popup.kind; }

  /* Callbacks (all optional). */
  std::function<void(int id, int value)> onParam;     // 0..98 params (+ 254 master volume)
  std::function<void(int bank, int prog)> onProgram;  // a program was loaded
  std::function<void(int id, int value)> onCommand;   // raw 0x709 ids: 3/5 select, 15 opt, 94 write
  std::function<void(const OptSettings&)> onOptResult;  // Options dialog OK

  Widgets& emu() { return emu_; }
  const Widgets& emu() const { return emu_; }
  PanelCore& core() { return core_; }
  const PanelCore& core() const { return core_; }

  /* The pointer-transform state (virtual cursor warp + the popup shift/rect, as the web overlay
     keeps them). Exposed for hosts that must feed panel-space coordinates through the facade and
     for the gate harnesses. */
  struct PointerState {
    int warpX = 0;
    int warpY = 0;
    int shiftX = 0;
    int shiftY = 0;
    int rect[4] = {0, 0, 0, 0};
    bool visible = false;
  };
  PointerState pointerState() const {
    PointerState s;
    s.warpX = warp_[0];
    s.warpY = warp_[1];
    s.shiftX = popShift_[0];
    s.shiftY = popShift_[1];
    s.rect[0] = popRect_[0];
    s.rect[1] = popRect_[1];
    s.rect[2] = popRect_[2];
    s.rect[3] = popRect_[3];
    s.visible = popVisible_;
    return s;
  }

 private:
  PanelCore core_;
  Skin skin_;
  std::vector<SelectBank> banks_;
  Widgets emu_{std::vector<Widget>{}};
  PopupAssets pops_;
  SelectWin sel_;
  OptDialog opt_;
  Composed popComp_;
  int scale_ = 1;
  int curBank_ = 0;
  int curProgram_ = 0;
  std::string curName_;
  std::map<std::string, std::string> lcd_;   // "program" / "bank"
  int warp_[2] = {0, 0};
  int popOpenKey_ = -1;
  int lastXY_[2] = {0, 0};
  int popShift_[2] = {0, 0};
  int popRect_[4] = {0, 0, 0, 0};
  bool popVisible_ = false;

  /* Multi-pointer / touch state (P1). */
  struct PointerTrack {
    bool down = false;
    int x = 0;
    int y = 0;              // raw panel coords at down
    int widget = -1;        // widget index owned at down (emu find, canvas coords)
    long long tDown = 0;
    bool longPressFired = false;
    bool previewList = false;   // touch: the down previewed a list-popup cell
  };
  std::map<int, PointerTrack> ptrs_;
  int owner_ = -1;              // pointer holding the exclusive emulator state (-1 = none)
  int ownerWidget_ = -1;
  bool touchMode_ = false;
  int tapWidget_ = -1;          // last completed tap (double-tap detection)
  int tapX_ = 0;
  int tapY_ = 0;
  long long tapT_ = 0;

  void panelMouse(MouseType type, int px, int py);   // the legacy single-pointer path
  bool exclusiveActive() const {
    return emu_.popup.kind != 0 || emu_.dragging() || emu_.hasPressed;
  }
  static long long nowMs();

  void openSelect();
  void syncWarp();
  void paintPopShift();
};

}   // namespace panel
}   // namespace synth1
