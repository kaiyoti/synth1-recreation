// android_main.cpp — JNI + Oboe + ANativeWindow glue of the Android shell (a1a-android-shell).
// All DSP/GUI logic lives in core/ (Engine via SynthHost) and panel/ (Panel via PanelView).
#include <android/log.h>
#include <amidi/AMidi.h>
#include <dlfcn.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <oboe/Oboe.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "input_router.hpp"
#include "panel_view.hpp"
#include "synth_host.hpp"

#define TAG "Synth1"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr int kEngineBlock = 512;

struct App {
  std::string filesDir;
  synth1_android::SynthHost host;
  synth1_android::PanelView view;
  bool viewOk = false;
  ANativeWindow* window = nullptr;
  std::mutex uiMutex;   // UI thread only (surface + panel); audio never touches the panel
  float sampleRate = 48000.f;
  std::atomic<long> callbacks{0};
  std::atomic<int> xruns{0};
  std::string audioInfo = "audio: not started";
  std::shared_ptr<oboe::AudioStream> stream;
  std::vector<float> L, R;
  std::string skin = "classic";
  int curBank = 0;
};
App g;

// ---- MIDI in (android.media.midi devices opened in Java; AMidi read on the audio thread) ----
// AMidi is API 29+; the shell is minSdk 26, so the library is dlopen'ed and everything degrades to
// "no hardware MIDI" below 29.
struct AMidiApi {
  media_status_t (*deviceFromJava)(JNIEnv*, jobject, AMidiDevice**) = nullptr;
  media_status_t (*deviceRelease)(const AMidiDevice*) = nullptr;
  media_status_t (*openOutputPort)(const AMidiDevice*, int32_t, AMidiOutputPort**) = nullptr;
  void (*closeOutputPort)(const AMidiOutputPort*) = nullptr;
  ssize_t (*receive)(const AMidiOutputPort*, int32_t*, uint8_t*, size_t, size_t*, int64_t*) = nullptr;
  bool ok = false;
} amidi;

void loadAMidi() {
  if (amidi.ok) return;
  void* h = dlopen("libamidi.so", RTLD_NOW);
  if (!h) { LOGI("libamidi.so not available (API < 29): hardware MIDI disabled"); return; }
  amidi.deviceFromJava = (decltype(amidi.deviceFromJava))dlsym(h, "AMidiDevice_fromJava");
  amidi.deviceRelease = (decltype(amidi.deviceRelease))dlsym(h, "AMidiDevice_release");
  amidi.openOutputPort = (decltype(amidi.openOutputPort))dlsym(h, "AMidiOutputPort_open");
  amidi.closeOutputPort = (decltype(amidi.closeOutputPort))dlsym(h, "AMidiOutputPort_close");
  amidi.receive = (decltype(amidi.receive))dlsym(h, "AMidiOutputPort_receive");
  amidi.ok = amidi.deviceFromJava && amidi.deviceRelease && amidi.openOutputPort &&
             amidi.closeOutputPort && amidi.receive;
}

// UI-bound MIDI events (audio thread -> UI thread, SPSC)
struct UiEv { int type; int a; int b; };   // 0 mod wheel, 1 pitch bend, 2 program select
struct UiRing {
  static constexpr unsigned N = 256;
  UiEv ev[N];
  std::atomic<unsigned> head{0}, tail{0};
  void push(UiEv e) {
    unsigned h = head.load(std::memory_order_relaxed);
    if (h - tail.load(std::memory_order_acquire) >= N) return;
    ev[h % N] = e; head.store(h + 1, std::memory_order_release);
  }
  bool pop(UiEv& e) {
    unsigned t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) return false;
    e = ev[t % N]; tail.store(t + 1, std::memory_order_release); return true;
  }
} uiRing;

// Router sink for the audio thread: engine calls go straight to the callback's staging queue.
struct AudioSink : synth1_android::MidiSink {
  void noteOn(int n, int v) override { g.host.audioNoteOn(n, v); }
  void noteOff(int n) override { g.host.audioNoteOff(n); }
  void cc(int c, int v) override { g.host.audioCc(c, v); }
  void pitchBend(int s) override { g.host.audioPitchBend(s); }
  void aftertouch(int v) override { g.host.audioAftertouch(v); }
  void modWheelUi(int v) override { uiRing.push({0, v, 0}); }
  void pitchBendUi(int s) override { uiRing.push({1, s, 0}); }
  void programSelect(int bank, int prog) override { uiRing.push({2, bank, prog}); }
} audioSink;

struct MidiIn {
  AMidiDevice* dev = nullptr;
  AMidiOutputPort* port = nullptr;
  std::unique_ptr<synth1_android::MidiRouter> router;
};
std::mutex midiMutex;          // UI: lock; audio: try_lock only
std::vector<MidiIn> midiIns;   // guarded by midiMutex
std::vector<int> bankSlots;    // guarded by midiMutex
std::atomic<long> midiMsgs{0};
std::atomic<int> midiCount{0};

void midiPump() {
  std::unique_lock<std::mutex> lk(midiMutex, std::try_to_lock);
  if (!lk.owns_lock() || !amidi.ok) return;
  uint8_t buf[512];
  for (auto& m : midiIns) {
    for (int guard = 0; guard < 64; ++guard) {
      int32_t op = 0; size_t nb = 0; int64_t ts = 0;
      const ssize_t r = amidi.receive(m.port, &op, buf, sizeof buf, &nb, &ts);
      if (r <= 0) break;
      if (op == AMIDI_OPCODE_DATA && nb > 0) { m.router->feed(buf, nb); ++midiMsgs; }
    }
  }
}

class Callback : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
 public:
  oboe::DataCallbackResult onAudioReady(oboe::AudioStream* s, void* data, int32_t n) override {
    auto* out = static_cast<float*>(data);
    const int ch = s->getChannelCount();
    if (n > (int)g.L.size()) n = (int)g.L.size();
    midiPump();
    g.host.process(g.L.data(), g.R.data(), n);
    for (int i = 0; i < n; ++i) {
      out[i * ch] = g.L[i];
      if (ch > 1) out[i * ch + 1] = g.R[i];
    }
    const long c = ++g.callbacks;
    if (c == 1 || c % 500 == 0) {
      auto xr = s->getXRunCount();
      LOGI("oboe callback #%ld frames=%d rate=%d burst=%d xruns=%d", c, n, s->getSampleRate(),
           s->getFramesPerBurst(), xr ? xr.value() : -1);
    }
    return oboe::DataCallbackResult::Continue;
  }
  void onErrorAfterClose(oboe::AudioStream*, oboe::Result r) override {
    LOGE("oboe stream closed: %s", oboe::convertToText(r));
  }
};
Callback g_cb;

std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

std::string jstr(JNIEnv* e, jstring s) {
  const char* c = e->GetStringUTFChars(s, nullptr);
  std::string r(c);
  e->ReleaseStringUTFChars(s, c);
  return r;
}

void drawLocked() {
  if (!g.window || !g.viewOk) return;
  ANativeWindow_Buffer b;
  if (ANativeWindow_lock(g.window, &b, nullptr) != 0) return;
  g.view.blit(static_cast<uint8_t*>(b.bits), b.width, b.height, b.stride * 4);
  ANativeWindow_unlockAndPost(g.window);
}

void wirePanel() {
  auto& p = g.view.panel();
  p.loadProgram(0, 0);
  g.curBank = 0;
  p.onParam = [](int id, int v) { g.host.paramSet(id, v); };
  p.onProgram = [](int bank, int prog) {
    g.curBank = bank;
    g.host.load(g.view.panel().sy1Text(), prog, g.sampleRate, kEngineBlock);
  };
  g.host.load(p.sy1Text(), 0, g.sampleRate, kEngineBlock);
  // on-screen keyboard -> engine (UI thread is the only ring producer)
  g.view.onNoteOn = [](int n, int v) { g.host.noteOn(n, v); };
  g.view.onNoteOff = [](int n) { g.host.noteOff(n); };
  g.view.onPitchBend = [](int s) { g.host.pitchBend(s); };
  g.view.onModWheel = [](int v) { g.host.cc(1, v); };
  // bank table for MIDI bank select (CC0 value = bank slot)
  std::vector<int> slots;
  for (int i = 0;; ++i) {
    int slot = 0;
    if (!g.view.panel().bankSlot(i, slot)) break;
    slots.push_back(slot);
  }
  std::lock_guard<std::mutex> lk(midiMutex);
  bankSlots = slots;
  for (auto& m : midiIns) m.router->setBankSlots(bankSlots);
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nInit(JNIEnv* e, jclass, jstring filesDir) {
  g.filesDir = jstr(e, filesDir);
  g.L.assign(8192, 0.f);
  g.R.assign(8192, 0.f);
}

// Opens the Oboe stream (LowLatency, Exclusive, device rate), loads the engine at that rate,
// starts the stream. Returns a one-line status.
JNIEXPORT jstring JNICALL Java_org_synth1_android_Native_nStartAudio(JNIEnv* e, jclass) {
  if (g.stream) return e->NewStringUTF(g.audioInfo.c_str());
  oboe::AudioStreamBuilder b;
  b.setDirection(oboe::Direction::Output)
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Exclusive)
      ->setFormat(oboe::AudioFormat::Float)
      ->setChannelCount(oboe::ChannelCount::Stereo)
      ->setUsage(oboe::Usage::Media)
      ->setDataCallback(&g_cb)
      ->setErrorCallback(&g_cb);
  oboe::Result r = b.openStream(g.stream);
  if (r != oboe::Result::OK) {
    g.audioInfo = std::string("audio: open failed ") + oboe::convertToText(r);
    LOGE("%s", g.audioInfo.c_str());
    return e->NewStringUTF(g.audioInfo.c_str());
  }
  g.sampleRate = (float)g.stream->getSampleRate();
  {
    std::lock_guard<std::mutex> lk(g.uiMutex);
    // (re)build the engine at the DEVICE rate; the panel's current patch if the UI is up
    const std::string sy1 = g.viewOk ? g.view.panel().sy1Text() : readFile(g.filesDir + "/presets/001.sy1");
    g.host.load(sy1, 0, g.sampleRate, kEngineBlock);
  }
  g.stream->setBufferSizeInFrames(g.stream->getFramesPerBurst() * 2);
  r = g.stream->requestStart();
  char buf[256];
  snprintf(buf, sizeof buf, "audio: %s rate=%d burst=%d buf=%d api=%s perf=%s share=%s start=%s",
           "float/stereo", g.stream->getSampleRate(), g.stream->getFramesPerBurst(),
           g.stream->getBufferSizeInFrames(), oboe::convertToText(g.stream->getAudioApi()),
           oboe::convertToText(g.stream->getPerformanceMode()),
           oboe::convertToText(g.stream->getSharingMode()), oboe::convertToText(r));
  g.audioInfo = buf;
  LOGI("%s", buf);
  return e->NewStringUTF(buf);
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nStopAudio(JNIEnv*, jclass) {
  if (g.stream) { g.stream->stop(); g.stream->close(); g.stream.reset(); }
  g.audioInfo = "audio: stopped";
}

JNIEXPORT jstring JNICALL Java_org_synth1_android_Native_nStatus(JNIEnv* e, jclass) {
  char buf[400];
  snprintf(buf, sizeof buf, "%s | callbacks=%ld | %s scale=%d (skin %d x%d) | midi in=%d msgs=%ld",
           g.audioInfo.c_str(), g.callbacks.load(), g.skin.c_str(), g.viewOk ? g.view.nativeScale() : 0,
           g.viewOk ? g.view.skinScale() : 0, g.viewOk ? g.view.mult() : 0, midiCount.load(),
           midiMsgs.load());
  return e->NewStringUTF(buf);
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nSurface(JNIEnv* e, jclass, jobject surface,
                                                               jint w, jint h) {
  std::lock_guard<std::mutex> lk(g.uiMutex);
  if (g.window) { ANativeWindow_release(g.window); g.window = nullptr; }
  g.window = ANativeWindow_fromSurface(e, surface);
  ANativeWindow_setBuffersGeometry(g.window, w, h, WINDOW_FORMAT_RGBA_8888);
  std::string err;
  g.viewOk = g.view.init(g.filesDir + "/skin/" + g.skin, w, h, err);
  if (!g.viewOk) { LOGE("panel init failed: %s", err.c_str()); return; }
  wirePanel();
  LOGI("panel %dx%d native scale %d (skin %d x mult %d) on %dx%d surface", g.view.panelW(),
       g.view.panelH(), g.view.nativeScale(), g.view.skinScale(), g.view.mult(), w, h);
  drawLocked();
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nSurfaceDestroyed(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(g.uiMutex);
  if (g.window) { ANativeWindow_release(g.window); g.window = nullptr; }
}

// action: 0 down, 1 move, 2 up, 3 cancel (all pointers). id = MotionEvent pointer id.
JNIEXPORT void JNICALL Java_org_synth1_android_Native_nTouch(JNIEnv*, jclass, jint action, jint id,
                                                             jfloat x, jfloat y, jlong timeMs) {
  std::lock_guard<std::mutex> lk(g.uiMutex);
  if (!g.viewOk) return;
  g.view.pointer(action, id, x, y, timeMs);
  drawLocked();
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nSetSkin(JNIEnv* e, jclass, jstring name) {
  g.skin = jstr(e, name);
}

// UI-thread tick (~30 Hz): drains audio-thread MIDI events into the panel/keyboard and redraws.
JNIEXPORT void JNICALL Java_org_synth1_android_Native_nPoll(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(g.uiMutex);
  if (!g.viewOk) return;
  UiEv ev;
  bool any = false;
  while (uiRing.pop(ev)) {
    any = true;
    if (ev.type == 0) g.view.setModWheel(ev.a);
    else if (ev.type == 1) g.view.setPitchBend(ev.a);
    else g.view.panel().loadProgram(ev.a >= 0 ? ev.a : g.curBank, ev.b);
  }
  if (any) drawLocked();
}

// ---- hardware MIDI: Java opens the MidiDevice; the native side reads its output port(s) ----
JNIEXPORT jint JNICALL Java_org_synth1_android_Native_nMidiOpen(JNIEnv* e, jclass, jobject device,
                                                                jint port) {
  loadAMidi();
  if (!amidi.ok) return -1;
  AMidiDevice* d = nullptr;
  if (amidi.deviceFromJava(e, device, &d) != AMEDIA_OK) return -2;
  AMidiOutputPort* p = nullptr;
  if (amidi.openOutputPort(d, port, &p) != AMEDIA_OK) { amidi.deviceRelease(d); return -3; }
  MidiIn m;
  m.dev = d; m.port = p;
  m.router.reset(new synth1_android::MidiRouter(&audioSink));
  std::lock_guard<std::mutex> lk(midiMutex);
  m.router->setBankSlots(bankSlots);
  midiIns.push_back(std::move(m));
  midiCount = (int)midiIns.size();
  LOGI("midi input opened (port %d), inputs=%zu", (int)port, midiIns.size());
  return (jint)midiIns.size();
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nMidiCloseAll(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(midiMutex);
  for (auto& m : midiIns) { amidi.closeOutputPort(m.port); amidi.deviceRelease(m.dev); }
  midiIns.clear();
  midiCount = 0;
}

JNIEXPORT jlong JNICALL Java_org_synth1_android_Native_nMidiMessages(JNIEnv*, jclass) {
  return (jlong)midiMsgs.load();
}

JNIEXPORT void JNICALL Java_org_synth1_android_Native_nNote(JNIEnv*, jclass, jboolean on, jint note) {
  if (on) g.host.noteOn(note, 100); else g.host.noteOff(note);
}

// Offline protocol render of a .sy1 (same entry the host CLI gate uses) -> float32 stereo WAV.
JNIEXPORT jstring JNICALL Java_org_synth1_android_Native_nRenderProtocol(JNIEnv* e, jclass,
                                                                         jstring preset, jstring out,
                                                                         jint program) {
  const std::string text = readFile(jstr(e, preset)), outPath = jstr(e, out);
  std::vector<float> pcm; int frames = 0;
  if (!synth1_android::SynthHost::renderProtocol(text, program, {60}, 0.40, 0.15, 44100.0,
                                                 kEngineBlock, pcm, frames))
    return e->NewStringUTF("FAIL parse");
  FILE* f = fopen(outPath.c_str(), "wb");
  if (!f) return e->NewStringUTF("FAIL open out");
  struct H { char riff[4]; uint32_t rs; char wave[4]; char fmt[4]; uint32_t fs; uint16_t fc, ch;
             uint32_t sr, br; uint16_t ba, bps; char data[4]; uint32_t ds; } h =
    {{'R','I','F','F'}, 36u + frames * 8u, {'W','A','V','E'}, {'f','m','t',' '}, 16, 3, 2, 44100,
     44100 * 8, 8, 32, {'d','a','t','a'}, (uint32_t)frames * 8};
  fwrite(&h, 1, sizeof h, f);
  fwrite(pcm.data(), sizeof(float), pcm.size(), f);
  fclose(f);
  return e->NewStringUTF("OK");
}

}  // extern "C"
