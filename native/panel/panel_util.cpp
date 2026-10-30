#include "panel_util.hpp"

#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

namespace synth1 {
namespace panel {

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) {
    std::fclose(f);
    err = "cannot size " + path;
    return false;
  }
  out.resize(static_cast<size_t>(n));
  size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  if (got != out.size()) {
    err = "short read " + path;
    return false;
  }
  return true;
}

bool loadPng(const std::string& path, Image& img, std::string& err) {
  int comp = 0;
  int w = 0;
  int h = 0;
  unsigned char* d = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (!d) {
    err = "png load failed: " + path + " (" + std::string(stbi_failure_reason()) + ")";
    return false;
  }
  img.w = w;
  img.h = h;
  img.rgba.assign(d, d + static_cast<size_t>(w) * h * 4);
  stbi_image_free(d);
  return true;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
  static signed char T[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) T[i] = -1;
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(a[i])] = static_cast<signed char>(i);
    init = true;
  }
  std::vector<uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  uint32_t val = 0;
  int valb = -8;
  for (unsigned char c : s) {
    if (T[c] == -1) continue;
    val = (val << 6) | static_cast<uint32_t>(T[c]);
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

}   // namespace panel
}   // namespace synth1
