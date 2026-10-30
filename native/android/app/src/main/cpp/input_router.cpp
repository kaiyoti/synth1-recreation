#include "input_router.hpp"

#include <algorithm>

namespace synth1_android {

void MidiRouter::message(int status, int a, int b) {
  const int t = status & 0xF0;
  if (t == 0x90 && b > 0) sink_->noteOn(a, b);
  else if (t == 0x80 || (t == 0x90 && b == 0)) sink_->noteOff(a);
  else if (t == 0xB0 && a == 0) {            // bank select (Receive Bank Select)
    if (recvBank_)
      for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i] == b) { bankSel_ = (int)i; break; }
  } else if (t == 0xB0) {
    sink_->cc(a, b);
    if (a == 1) sink_->modWheelUi(b);
  } else if (t == 0xC0) {                    // program change (Receive Program Change)
    if (recvProg_) sink_->programSelect(bankSel_, a);
  } else if (t == 0xE0) {
    const int pb = ((b << 7) | a) - 8192;
    sink_->pitchBend(pb);
    sink_->pitchBendUi(pb);
  } else if (t == 0xD0) {
    sink_->aftertouch(a);
  }
}

void MidiRouter::feed(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const int c = p[i];
    if (c >= 0xF8) continue;                 // realtime: ignored, never disturbs running status
    if (c & 0x80) {
      if (c == 0xF0) { inSysex_ = true; running_ = 0; have_ = 0; continue; }
      if (c == 0xF7) { inSysex_ = false; continue; }
      inSysex_ = false;
      if (c >= 0xF0) {                       // system common: skipped (F1/F3 one data byte, F2 two)
        running_ = 0; have_ = 0;
        need_ = 0;
        if (c == 0xF1 || c == 0xF3) { running_ = -1; need_ = 1; }
        else if (c == 0xF2) { running_ = -1; need_ = 2; }
        continue;
      }
      running_ = c;
      need_ = ((c & 0xF0) == 0xC0 || (c & 0xF0) == 0xD0) ? 1 : 2;
      have_ = 0;
      continue;
    }
    if (inSysex_ || running_ == 0) continue;
    d_[have_++] = c;
    if (have_ == need_) {
      if (running_ > 0) message(running_, d_[0], need_ == 2 ? d_[1] : 0);
      have_ = 0;                             // running status stays (system common: drop)
      if (running_ < 0) running_ = 0;
    }
  }
}

bool OctaveShift::set(int o) {
  o = std::max(kMin, std::min(kMax, o));
  if (o == oct_) return false;
  oct_ = o;
  return true;
}

int OctaveShift::noteOn(int kb) {
  const int e = std::max(0, std::min(127, kb + 12 * oct_));
  sounding_[kb] = e;
  return e;
}

int OctaveShift::noteOff(int kb) {
  auto it = sounding_.find(kb);
  if (it == sounding_.end()) return std::max(0, std::min(127, kb + 12 * oct_));
  const int e = it->second;
  sounding_.erase(it);
  return e;
}

std::vector<int> OctaveShift::releaseAll() {
  std::vector<int> v;
  for (auto& kv : sounding_) v.push_back(kv.second);
  sounding_.clear();
  return v;
}

}  // namespace synth1_android
