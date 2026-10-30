/* Synth1 on-screen keyboard, C++ port of tasks/m4-keyboard/keyboard.js (p1-panel-touch, mobile
 * plan P1). Pure model (no DOM): playable piano + pitch/mod wheels, multitouch (each pointer its
 * own note, glissando on slide), velocity from touch y, reference-counted notes per source.
 * render() is a procedural RGBA rasterizer at any size (no skin); C-label text is skipped (the
 * portable layer has no font; the gate checks colours/geometry, not glyphs).
 * Coordinates: keyboard-local pixels, (0,0) top-left, y grows down. */
#ifndef SYNTH1_KEYBOARD_HPP
#define SYNTH1_KEYBOARD_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace synth1 {
namespace keyboard {

struct Event {
  std::string type;   // "on" | "off" | "pitch" | "mod"
  int note = 0;
  int vel = 0;
  int value = 0;
};

class Keyboard {
 public:
  struct Options {
    int low = 24;
    int high = 108;
    double height = 120;
    int baseNote = 60;
    int octaveRange = 4;
    bool wheels = true;
  };

  explicit Keyboard(const Options& o);

  /* geometry */
  void layout(double width, double height);
  int noteAt(double x, double y) const;            // -1 outside the keys
  int velocityFromY(double y) const;               // 30 top .. 127 bottom
  int wheelAt(double x, double y) const;           // 0 pitch, 1 mod, -1 none
  int pitchFromY(double y) const;
  int modFromY(double y) const;

  /* pointer streams (multitouch: one id per finger) */
  std::vector<Event> press(int id, double x, double y);
  std::vector<Event> move(int id, double x, double y);
  std::vector<Event> release(int id);

  /* computer keyboard (tracker layout) */
  std::vector<Event> keyDown(const std::string& code, bool repeat);
  std::vector<Event> keyUp(const std::string& code);
  int setOctave(int o);
  std::vector<Event> allNotesOff();

  /* wheels */
  int setPitchBend(double v);   // external setters: no callback
  int setModWheel(double v);
  std::vector<Event> nudgeWheel(int wheel, int dir);   // 0 pitch, 1 mod

  std::vector<int> activeNotes() const;
  std::vector<int> heldPointers() const;

  /* callbacks (may be empty) */
  std::function<void(int note, int vel)> onNoteOn;
  std::function<void(int note)> onNoteOff;
  std::function<void(int value)> onPitchBend;
  std::function<void(int value)> onModWheel;

  /* procedural RGBA render (w*h*4 bytes, out preallocated); lit = active notes;
   * activeWheel: 0 pitch, 1 mod, -1 none (cap highlight). */
  void render(int w, int h, const std::set<int>& lit, int activeWheel, uint8_t* out) const;

  int pitchBend = 0;
  int modWheel = 0;
  int octave = 0;
  int keyVelocity = 100;
  double width = 0;
  double height = 120;
  double whiteW = 0;
  double blackW = 0;
  double blackH = 0;
  bool wheels = true;
  double wheelWidth = 56;
  std::vector<int> whites;
  std::vector<int> blacks;
  std::map<int, int> blackBoundary;   // black note -> white slot it sits on
  std::vector<Event> log;

 private:
  Options opt_;
  std::map<int, int> active_;        // note -> refcount
  std::map<int, int> heldPointer_;   // pointer id -> note (-1 = down off the keys)
  std::map<std::string, int> heldKey_;
  std::map<int, int> heldWheel_;     // pointer id -> wheel (0/1)
  std::map<int, int> whiteOf_;

  Event emit(const Event& e);
  std::vector<Event> pressNote(const std::string& src, int note, int vel);
  std::vector<Event> releaseNote(const std::string& src, int note);
  std::vector<Event> setPitch(double v, bool emitCb);
  std::vector<Event> setMod(double v, bool emitCb);
};

bool isBlackNote(int n);
int jsRound(double x);   // JS Math.round: floor(x + 0.5)

}   // namespace keyboard
}   // namespace synth1

#endif
