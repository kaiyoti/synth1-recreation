// synth_host.hpp — platform-neutral host wrapper around synth1::engine::Engine for the
// Android shell (a1a-android-shell). Mirrors plugin/synth1_dpf.cpp's load law
// (fresh Engine + set_boot_program(boot_prog0) + prepare + ONE ProgramLoad applied
// synchronously) so the shell and the desktop plugin render the same samples.
// No Android / Oboe types here: the same file builds for the Linux-host render entry.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "events/event.h"

namespace synth1_android {

// x87 frame law (m3-harness-x87): trunc(value * sample_rate) with the product rounded to the
// 64-bit x87 mantissa. Portable: uses 80-bit long double when available, an exact integer
// emulation otherwise (arm64 long double is 113-bit and would NOT match).
std::int64_t x87_frames(double value, double sample_rate);

class SynthHost {
 public:
  using engine_t = synth1::engine::Engine;
  SynthHost();
  ~SynthHost();

  // Build a fresh engine (device load law) for sy1 text; program = 0-based slot.
  // Empty text = boot program only. UI/any non-audio thread. The audio thread picks the new
  // engine up at the next block boundary.
  bool load(const std::string& sy1Text, int program, float sampleRate, int blockSize);

  // Control events, any thread (lock-free SPSC; one producer thread at a time).
  void noteOn(int note, int vel);
  void noteOff(int note);
  void paramSet(int id, int raw);
  void cc(int controller, int value);
  void pitchBend(int signed14);
  void aftertouch(int value);

  // AUDIO THREAD ONLY (MIDI read inside the Oboe callback): staged directly, applied by the next
  // process() call of the same callback (before the ring is drained). No locks, no allocation.
  void audioNoteOn(int note, int vel) { pushAudio(0, note, vel); }
  void audioNoteOff(int note) { pushAudio(1, note, 0); }
  void audioCc(int c, int v) { pushAudio(3, c, v); }
  void audioPitchBend(int s14) { pushAudio(4, s14, 0); }
  void audioAftertouch(int v) { pushAudio(5, v, 0); }

  // Audio thread. Drains control events, renders n frames (chunked <= 1024).
  void process(float* left, float* right, int n);

  // Synchronous variants for offline use (no queue): stage via these, then renderBlock.
  engine_t* engine() { return active_.get(); }

  // Offline protocol render (bank-slot note sequence, offline_render semantics):
  // notes on at (fpn+fg)*i vel 100, off at on+fpn, total = (fpn+fg)*n + 3*sr padded to bs.
  // Returns interleaved L,R float32 and sets frames.
  static bool renderProtocol(const std::string& sy1Text, int program,
                             const std::vector<int>& notes, double secs, double gap,
                             double sampleRate, int blockSize,
                             std::vector<float>& interleaved, int& frames);

 private:
  struct Ctl { int type; int a; int b; };
  static constexpr int kRing = 1024;
  Ctl ring_[kRing];
  std::atomic<unsigned> head_{0}, tail_{0};
  std::unique_ptr<engine_t> active_;
  std::atomic<engine_t*> next_{nullptr};
  std::atomic<engine_t*> retired_{nullptr};
  std::vector<std::int32_t> rows_;   // event-referenced flat (id,value) rows
  std::vector<synth1::events::Event> staged_;
  void push(int type, int a, int b);
  static constexpr int kAudioQ = 256;
  Ctl audioQ_[kAudioQ];
  int audioN_ = 0;
  void pushAudio(int type, int a, int b) { if (audioN_ < kAudioQ) audioQ_[audioN_++] = Ctl{type, a, b}; }
  static synth1::events::Event toEvent(const Ctl& c);
};

}  // namespace synth1_android
