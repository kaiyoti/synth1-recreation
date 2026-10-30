// settings.cpp — synth1.ini [global] persistence + .ccm CC-map loader/writer.
//
// Spec: out/spec_host_tools.md §2/§4, out/spec_midi_settings.md §4/§5a.

#include "settings/settings.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace synth1 {
namespace {

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Decimal or 0x-hex integer, whole string must parse (GetPrivateProfileIntA-like).
bool parse_long(const std::string& s, long& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

std::string unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    s = s.substr(1, s.size() - 2);
  }
  return s;
}

std::string strip_cr(std::string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

}  // namespace

Settings::Settings()
    : installfolder("c:\\"),
      zipfolder("c:\\zipbank"),
      enableBankSelect(1),
      enableProgramChange(1),
      hostselectprogram(0),
      slidertype(1),
      coordinatesscale(150),
      panelcolor(0x87c8c8),
      textcolor(0x3c3c3c),
      transparencywin(1),
      mastertune(0) {
  // initsettings.exe writer 0x4015f0: "soundbank%02d" for bankfolder0..9.
  for (int i = 0; i < 10; ++i) bankfolder[i] = "soundbank0" + std::to_string(i);
}

void Settings::clamp() {
  if (coordinatesscale < COORDINATES_SCALE_MIN) coordinatesscale = COORDINATES_SCALE_MIN;
  if (coordinatesscale > COORDINATES_SCALE_MAX) coordinatesscale = COORDINATES_SCALE_MAX;
  if (mastertune < MASTERTUNE_MIN) mastertune = MASTERTUNE_MIN;
  if (mastertune > MASTERTUNE_MAX) mastertune = MASTERTUNE_MAX;
}

bool load_ini(const std::string& filepath, Settings& out_settings) {
  out_settings = Settings();
  std::ifstream in(filepath);
  if (!in) return false;

  std::string line;
  bool in_global = false;
  while (std::getline(in, line)) {
    const std::string t = trim(strip_cr(std::move(line)));
    if (t.empty() || t[0] == ';' || t[0] == '#') continue;
    if (t[0] == '[') {
      const std::size_t close = t.find(']');
      const std::string name = (close == std::string::npos) ? t.substr(1) : t.substr(1, close - 1);
      in_global = ieq(trim(name), "global");
      continue;
    }
    if (!in_global) continue;
    const std::size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(t.substr(0, eq));
    const std::string val = unquote(trim(t.substr(eq + 1)));
    long iv = 0;

    if (ieq(key, "installfolder")) {
      out_settings.installfolder = val;
    } else if (ieq(key, "zipfolder")) {
      out_settings.zipfolder = val;
    } else if (ieq(key, "enableBankSelect") && parse_long(val, iv)) {
      out_settings.enableBankSelect = static_cast<int>(iv);
    } else if (ieq(key, "enableProgramChange") && parse_long(val, iv)) {
      out_settings.enableProgramChange = static_cast<int>(iv);
    } else if (ieq(key, "hostselectprogram") && parse_long(val, iv)) {
      out_settings.hostselectprogram = static_cast<int>(iv);
    } else if (ieq(key, "slidertype") && parse_long(val, iv)) {
      out_settings.slidertype = static_cast<int>(iv);
    } else if (ieq(key, "coordinatesscale") && parse_long(val, iv)) {
      out_settings.coordinatesscale = static_cast<int>(iv);
    } else if (ieq(key, "panelcolor") && parse_long(val, iv)) {
      out_settings.panelcolor = static_cast<std::uint32_t>(iv);
    } else if (ieq(key, "textcolor") && parse_long(val, iv)) {
      out_settings.textcolor = static_cast<std::uint32_t>(iv);
    } else if (ieq(key, "transparencywin") && parse_long(val, iv)) {
      out_settings.transparencywin = static_cast<int>(iv);
    } else if (ieq(key, "mastertune") && parse_long(val, iv)) {
      out_settings.mastertune = static_cast<int>(iv);
    } else if (ieq(key, "CCMap")) {
      // FUN_180037420: empty or malformed ⇒ default map (all -1).
      if (!out_settings.cc_map.from_csv(val)) out_settings.cc_map = midi::CcMap();
    } else if (key.size() > 10 && ieq(key.substr(0, 10), "bankfolder")) {
      const std::string num = key.substr(10);
      bool digits = !num.empty();
      for (char c : num) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          digits = false;
          break;
        }
      }
      if (digits) {
        const long idx = std::strtol(num.c_str(), nullptr, 10);
        if (idx >= 0 && idx < BANKFOLDER_COUNT) out_settings.bankfolder[idx] = val;
      }
    }
  }
  out_settings.clamp();
  return true;
}

bool save_ini(const std::string& filepath, const Settings& settings) {
  std::ofstream out(filepath, std::ios::binary);
  if (!out) return false;

  Settings s = settings;
  s.clamp();  // DLL clamps coordinatesscale at the write site (24675)

  out << "[global]\r\n";
  for (int i = 0; i < BANKFOLDER_COUNT; ++i) {
    out << "bankfolder" << i << "=" << s.bankfolder[i] << "\r\n";
  }
  out << "installfolder=" << s.installfolder << "\r\n";
  out << "zipfolder=" << s.zipfolder << "\r\n";
  out << "enableBankSelect=" << s.enableBankSelect << "\r\n";
  out << "enableProgramChange=" << s.enableProgramChange << "\r\n";
  out << "hostselectprogram=" << s.hostselectprogram << "\r\n";
  out << "slidertype=" << s.slidertype << "\r\n";
  out << "coordinatesscale=" << s.coordinatesscale << "\r\n";
  out << "panelcolor=" << s.panelcolor << "\r\n";  // FUN_180004960 "%u" decimal
  out << "textcolor=" << s.textcolor << "\r\n";
  out << "transparencywin=" << s.transparencywin << "\r\n";
  out << "mastertune=" << s.mastertune << "\r\n";
  out << "CCMap=" << s.cc_map.to_csv() << "\r\n";  // FUN_1800049c0 (34158/34441)
  return out.good();
}

std::string default_ini_path() {
#ifdef _WIN32
  const char* appdata = std::getenv("APPDATA");
  if (appdata && *appdata) return std::string(appdata) + "\\Daichi\\Synth1\\synth1.ini";
  return "synth1.ini";
#else
  const char* home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.config/synth1/synth1.ini";
  return ".config/synth1/synth1.ini";
#endif
}

bool load_ccm(const std::string& filepath, midi::CcMap& out_map) {
  std::ifstream in(filepath);
  if (!in) return false;

  // FUN_180024af0 load path (24944-24966): skip '#' / empty lines, then CSV parse.
  std::string line, joined;
  while (std::getline(in, line)) {
    const std::string t = trim(strip_cr(std::move(line)));
    if (t.empty() || t[0] == '#') continue;
    joined += t;
    joined += '\n';
  }

  midi::CcMap m;
  if (!m.from_csv(joined)) return false;  // out_map untouched on failure
  out_map = m;
  return true;
}

bool save_ccm(const std::string& filepath, const midi::CcMap& in_map) {
  std::ofstream out(filepath, std::ios::binary);
  if (!out) return false;
  out << in_map.to_csv() << "\n";  // FUN_1800279b0 + "\n" (id 0x405)
  return out.good();
}

}  // namespace synth1
