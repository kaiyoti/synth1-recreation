// render_cli.cpp — the shell's native render entry as a host CLI (gate 3b / on-device parity).
// Same SynthHost::renderProtocol the Android app links; writes a stereo float32 WAV that must be
// byte-identical to portable-synth/build/offline_render for the U1 protocol.
//   synth1_render_cli --preset X.sy1 [--notes 60] [--secs 0.4] [--gap 0.15] [--sr 44100]
//                     [--bs 512] -o out.wav
//   synth1_render_cli --x87-selftest
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "synth_host.hpp"

static int program_from_path(const std::string& path) {
  size_t s = path.find_last_of('/');
  std::string b = s == std::string::npos ? path : path.substr(s + 1);
  size_t i = 0;
  while (i < b.size() && !isdigit((unsigned char)b[i])) ++i;
  if (i >= b.size()) return -1;
  int n = 0;
  while (i < b.size() && isdigit((unsigned char)b[i])) n = n * 10 + (b[i++] - '0');
  return n - 1;
}

int main(int argc, char** argv) {
  std::string preset, out;
  std::vector<int> notes{60};
  double secs = 0.4, gap = 0.15, sr = 44100;
  int bs = 512;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() { return i + 1 < argc ? argv[++i] : (fprintf(stderr, "missing arg\n"), exit(2), ""); };
    if (a == "--x87-selftest") {
      int bad = 0, n = 0;
      for (double v : {0.05, 0.1, 0.15, 0.25, 0.4, 1.2, 3.0, 0.3, 0.7, 1.1, 2.2, 0.35})
        for (double r : {44100.0, 48000.0, 96000.0, 22050.0, 32000.0}) {
          long long a1 = synth1_android::x87_frames(v, r);
          long long b1 = (long long)((long double)v * (long double)r);
          ++n; if (a1 != b1) { ++bad; printf("MISMATCH %g*%g: %lld vs %lld\n", v, r, a1, b1); }
        }
      printf("x87_frames selftest: %d/%d match (0.15@44.1k=%ld 1.2@44.1k=%ld)\n", n - bad, n,
             synth1_android::x87_frames(0.15, 44100), synth1_android::x87_frames(1.2, 44100));
      return bad != 0;
    }
    if (a == "--preset") preset = need();
    else if (a == "--notes") { notes.clear(); std::stringstream ss(need()); std::string t; while (std::getline(ss, t, ',')) notes.push_back(atoi(t.c_str())); }
    else if (a == "--secs") secs = atof(need());
    else if (a == "--gap") gap = atof(need());
    else if (a == "--sr") sr = atof(need());
    else if (a == "--bs") bs = atoi(need());
    else if (a == "-o" || a == "--out") out = need();
  }
  std::string text;
  if (!preset.empty()) {
    std::ifstream f(preset, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", preset.c_str()); return 1; }
    std::stringstream ss; ss << f.rdbuf(); text = ss.str();
  }
  std::vector<float> pcm; int frames = 0;
  if (!synth1_android::SynthHost::renderProtocol(text, program_from_path(preset), notes, secs, gap,
                                                 sr, bs, pcm, frames)) { fprintf(stderr, "bad preset\n"); return 1; }
  FILE* f = fopen(out.c_str(), "wb");
  if (!f) return 1;
  // same 44-byte IEEE-float stereo header as offline_render's wav::write_header
  struct H { char riff[4]; uint32_t rs; char wave[4]; char fmt[4]; uint32_t fs; uint16_t fmtc, ch;
             uint32_t sr, br; uint16_t ba, bps; char data[4]; uint32_t ds; } h =
    {{'R','I','F','F'}, 0, {'W','A','V','E'}, {'f','m','t',' '}, 16, 3, 2, (uint32_t)sr, (uint32_t)sr * 8, 8, 32, {'d','a','t','a'}, (uint32_t)frames * 8};
  h.rs = 36 + h.ds;
  fwrite(&h, 1, sizeof h, f);
  fwrite(pcm.data(), sizeof(float), pcm.size(), f);
  fclose(f);
  fprintf(stderr, "wrote %s (%d frames)\n", out.c_str(), frames);
  return 0;
}
