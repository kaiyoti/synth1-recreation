// input_router.hpp — platform-neutral input routing of the Android shell (no Android types):
//   MidiRouter  : raw MIDI byte stream -> engine/UI calls, the same law as the web edition's
//                 Web MIDI hook (public/web/index.html): omni channel, note on/off (vel 0 = off),
//                 CC (CC0 = bank select by bank SLOT, CC1 also moves the mod wheel), pitch bend
//                 (signed14), channel aftertouch, program change (Receive Bank/Program switches).
//   OctaveShift : on-screen keyboard octave shift (note offsets with stuck-note protection).
// Host-testable (input_router_test).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace synth1_android {

struct MidiSink {
  virtual ~MidiSink() {}
  // engine side
  virtual void noteOn(int note, int vel) = 0;
  virtual void noteOff(int note) = 0;
  virtual void cc(int controller, int value) = 0;
  virtual void pitchBend(int signed14) = 0;     // -8192..8191
  virtual void aftertouch(int value) = 0;       // channel pressure 0..127
  // UI side (wheels follow the hardware; program/bank load happens on the UI thread)
  virtual void modWheelUi(int value) = 0;
  virtual void pitchBendUi(int signed14) = 0;
  // bankIndex = index into the panel's bank table chosen by CC0, or -1 = keep the current bank
  virtual void programSelect(int bankIndex, int program) = 0;
};

class MidiRouter {
 public:
  explicit MidiRouter(MidiSink* sink) : sink_(sink) {}
  // bank slot numbers in table order (banks.json "slot"); CC0 value -> index via this table
  void setBankSlots(const std::vector<int>& slots) { slots_ = slots; }
  void setReceiveBank(bool on) { recvBank_ = on; }
  void setReceiveProgram(bool on) { recvProg_ = on; }
  int selectedBank() const { return bankSel_; }
  // Feed any chunk of the byte stream (running status and split messages are kept across calls).
  void feed(const uint8_t* data, size_t n);
  void message(int status, int d1, int d2);   // one complete channel message

 private:
  MidiSink* sink_;
  std::vector<int> slots_;
  bool recvBank_ = true, recvProg_ = true;
  int bankSel_ = -1;
  int running_ = 0, need_ = 0, have_ = 0, d_[2] = {0, 0};
  bool inSysex_ = false;
};

class OctaveShift {
 public:
  static constexpr int kMin = -3, kMax = 3;
  int octave() const { return oct_; }
  bool set(int o);                 // clamped to kMin..kMax; returns true if changed
  bool step(int dir) { return set(oct_ + dir); }
  // keyboard note -> engine note (clamped 0..127). noteOn remembers the mapping so the matching
  // noteOff releases the same engine note even if the shift changed meanwhile.
  int noteOn(int kbNote);
  int noteOff(int kbNote);
  std::vector<int> releaseAll();   // engine notes still sounding (shift changes keep them; panic)
 private:
  int oct_ = 0;
  std::map<int, int> sounding_;    // kb note -> engine note
};

}  // namespace synth1_android
