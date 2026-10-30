// event.h — bounded event contract for the Synth1 engine.
//
// The engine's inputs are a stream of bounded events, each carrying an
// absolute sample offset (not a block-relative index) so the engine can
// split at event boundaries and control-rate ticks.
//
// Design notes (plan §5 "no realtime allocations"; Claude's advice):
// - POD-variant, no virtual dispatch in the hot path.
// - All events carry an absolute sample offset.
// - The engine splits each block at event offsets and control-rate ticks.
// - prepare()/process(block)/reset() on the engine; NO allocations in process().

#pragma once

#include <cstdint>

namespace synth1::events {

// ---------------------------------------------------------------------------
// Event type tag
// ---------------------------------------------------------------------------
enum class EventType : std::uint8_t {
  kNoteOn = 0,
  kNoteOff = 1,
  kCC = 2,
  kProgramChange = 3,
  kParameterSet = 4,
  kSetSampleRate = 5,
  kSetBlockSize = 6,
  // m3-engine-events: 32-bit dispatcher FUN_10012730 (:13716) routes
  // 0xd0 (channel aftertouch) and 0xe0 (pitch bend) to the generic slot
  // handler FUN_10031700 (:35669); 0xe0 additionally to the bend setter
  // FUN_10025900 (:29072).
  kAftertouch = 7,   // 0xd0: value 0..127
  kPitchBend = 8,    // 0xe0: signed14 = (MSB-0x40)*0x80 + LSB (-8192..8191)
  // m3-u4c-land: one program load (effSetProgram law, FUN_10024640).
  // rows = flat (id,val) pairs owned by the event producer.
  kProgramLoad = 9,
};

// ---------------------------------------------------------------------------
// Bounded event struct (POD-variant, no virtual dispatch)
// ---------------------------------------------------------------------------
struct Event {
  EventType type;
  std::int64_t sample_offset;  // absolute sample offset within the block

  // Union of payloads (POD, no virtual dispatch).
  union {
    struct {  // NoteOn / NoteOff
      std::uint8_t note;        // midi note 0..127
      std::uint8_t velocity;    // velocity 0..127 (NoteOff: may be 0)
    } note;
    struct {  // CC
      std::uint8_t controller;  // controller 0..127
      std::uint8_t value;      // value 0..127
    } cc;
    struct {  // Aftertouch (0xd0) / PitchBend (0xe0)
      std::int32_t value;      // AT: 0..127; bend: signed14 -8192..8191
    } perf;
    struct {  // ProgramChange
      std::uint8_t bank;        // bank 0..127
      std::uint8_t program;     // program 0..127
    } program;
    struct {  // ParameterSet
      std::uint16_t param_id;   // parameter id 0..254 (255 slots)
      // Dispatcher band value (FUN_180028fe0 param_3). Signed and wider than
      // 8 bits: key shift (9) is −24..+24 and the MIDI-ctrl sources (86/88)
      // carry (status<<8)|cc, e.g. 0xB001 = 45057 for CC1.
      std::int32_t raw_value;
      // #134: 1 = the value comes from the boot/init-sound injection
      // (upgrade_v105 fills slots a ver<108 file omits), NOT the file chunk
      // or a host set. The device applies init values through a
      // store+setter path that does NOT run the FUN_180028fe0 range clamp
      // (evidence: 083 p74=32 injected -> ALWAYS-glide, #112; boot 73=32 ->
      // unison OFF; boot 93=9 -> N=2, #57). 0 = normal (clamp applies).
      std::uint8_t init_inject;
    } param;
    // SetSampleRate / SetBlockSize: single-member structs would prevent
    // direct assignment, so we use a tagged struct with explicit fields.
    struct {
      float sample_rate;
      std::int32_t block_size;
    } io;
    struct {  // ProgramLoad (m3-u4c-land): flat (id,val) rows + program no.
      const std::int32_t* rows;   // [id0,val0,id1,val1,...], producer-owned
      std::int32_t count;         // number of (id,val) pairs
      std::int32_t program;       // effSetProgram number (-1 = force replay)
    } load;
  } payload;

  // ---- Constructors (convenience) ----
  static Event ProgramLoad(std::int64_t sample, const std::int32_t* rows,
                           std::int32_t count, std::int32_t program) {
    Event e{};
    e.type = EventType::kProgramLoad;
    e.sample_offset = sample;
    e.payload.load.rows = rows;
    e.payload.load.count = count;
    e.payload.load.program = program;
    return e;
  }
  static Event NoteOn(std::int64_t sample, std::uint8_t note, std::uint8_t velocity) {
    Event e{};
    e.type = EventType::kNoteOn;
    e.sample_offset = sample;
    e.payload.note.note = note;
    e.payload.note.velocity = velocity;
    return e;
  }
  static Event NoteOff(std::int64_t sample, std::uint8_t note, std::uint8_t velocity = 0) {
    Event e{};
    e.type = EventType::kNoteOff;
    e.sample_offset = sample;
    e.payload.note.note = note;
    e.payload.note.velocity = velocity;
    return e;
  }
  static Event CC(std::int64_t sample, std::uint8_t controller, std::uint8_t value) {
    Event e{};
    e.type = EventType::kCC;
    e.sample_offset = sample;
    e.payload.cc.controller = controller;
    e.payload.cc.value = value;
    return e;
  }
  static Event Aftertouch(std::int64_t sample, int value) {
    Event e{};
    e.type = EventType::kAftertouch;
    e.sample_offset = sample;
    e.payload.perf.value = value;
    return e;
  }
  static Event PitchBend(std::int64_t sample, int signed14) {
    Event e{};
    e.type = EventType::kPitchBend;
    e.sample_offset = sample;
    e.payload.perf.value = signed14;
    return e;
  }
  static Event ProgramChange(std::int64_t sample, std::uint8_t bank, std::uint8_t program) {
    Event e{};
    e.type = EventType::kProgramChange;
    e.sample_offset = sample;
    e.payload.program.bank = bank;
    e.payload.program.program = program;
    return e;
  }
  static Event ParameterSet(std::int64_t sample, std::uint16_t param_id,
                            std::int32_t raw_value, bool init_inject = false) {
    Event e{};
    e.type = EventType::kParameterSet;
    e.sample_offset = sample;
    e.payload.param.param_id = param_id;
    e.payload.param.raw_value = raw_value;
    e.payload.param.init_inject = init_inject ? 1 : 0;
    return e;
  }
  static Event SetSampleRate(std::int64_t sample, float sample_rate) {
    Event e{};
    e.type = EventType::kSetSampleRate;
    e.sample_offset = sample;
    e.payload.io.sample_rate = sample_rate;
    return e;
  }
  static Event SetBlockSize(std::int64_t sample, std::int32_t block_size) {
    Event e{};
    e.type = EventType::kSetBlockSize;
    e.sample_offset = sample;
    e.payload.io.block_size = block_size;
    return e;
  }
};

// ---------------------------------------------------------------------------
// Bounded event buffer (no virtual dispatch; fixed capacity)
// ---------------------------------------------------------------------------
template <std::size_t kCapacity = 1024>
class EventBuffer {
 public:
  void push(const Event& e) {
    if (count_ < kCapacity) {
      data_[count_++] = e;
    }
  }
  void clear() { count_ = 0; }
  std::size_t size() const { return count_; }
  const Event& at(std::size_t i) const { return data_[i]; }
  bool full() const { return count_ >= kCapacity; }
  void reset() { count_ = 0; }

 private:
  Event data_[kCapacity]{};
  std::size_t count_ = 0;
};

}  // namespace synth1::events
