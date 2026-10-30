/* synth1::panel — shared tiny utilities: file read, PNG load (stb_image), base64. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace synth1 {
namespace panel {

struct Image {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> rgba;   // w*h*4, straight (non-premultiplied) RGBA
};

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string& err);
bool loadPng(const std::string& path, Image& img, std::string& err);

/* Base64 decode with Buffer.from(s, 'base64') semantics: invalid characters are skipped,
   trailing bits of a partial group are dropped. */
std::vector<uint8_t> base64Decode(const std::string& s);

}   // namespace panel
}   // namespace synth1
