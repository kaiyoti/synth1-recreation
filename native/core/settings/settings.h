// settings.h — host settings persistence (synth1.ini [global]) + .ccm CC-map files.
//
// Spec: out/spec_host_tools.md §2 (initsettings writer), §4 (DLL INI keys/defaults),
//       out/spec_midi_settings.md §1 (CC map), §4 (CSV), §5a (INI path + keys).
// Ghidra cites:
//   FUN_180004660 (2796)  path builder → %APPDATA%\Daichi\Synth1\synth1.ini
//   FUN_180004620/750     GetPrivateProfileIntA / StringA wrappers (section "global")
//   FUN_180004960/9c0     WritePrivateProfileStringA ("%u" / str)
//   FUN_180037420 (34240) startup loader: CCMap CSV, empty/malformed ⇒ all -1
//   FUN_180024af0 (25651) .ccm dialog: load skips '#' / empty lines
//   FUN_1800279b0/a70     CSV write / read (128 ints, values < 0x100)

#pragma once

#include <cstdint>
#include <string>

#include "midi/cc_map.h"

namespace synth1 {

// bankfolder0..99 (spec_host_tools §4: DLL reads d=0..99; initsettings sets 0..9).
inline constexpr int BANKFOLDER_COUNT = 100;

// coordinatesscale: default 0x96=150, clamped 0x50..0xfa (24611-24618).
inline constexpr int COORDINATES_SCALE_MIN = 0x50;  // 80
inline constexpr int COORDINATES_SCALE_MAX = 0xfa;  // 250

// mastertune: default 0, range -100..+100 (24644/38077, 24716).
inline constexpr int MASTERTUNE_MIN = -100;
inline constexpr int MASTERTUNE_MAX = 100;

struct Settings {
  std::string bankfolder[BANKFOLDER_COUNT];  // default soundbank00..09, rest ""
  std::string installfolder;                 // default "c:\\"
  std::string zipfolder;                     // default "c:\\zipbank"
  int enableBankSelect;                      // default 1
  int enableProgramChange;                   // default 1
  int hostselectprogram;                     // default 0
  int slidertype;                            // default 1
  int coordinatesscale;                      // default 150, clamped 80..250
  std::uint32_t panelcolor;                  // default 0x87c8c8
  std::uint32_t textcolor;                   // default 0x3c3c3c
  int transparencywin;                       // default 1
  int mastertune;                            // default 0, clamped -100..+100
  midi::CcMap cc_map;                        // INI key "CCMap" (CSV), default all -1

  Settings();  // spec defaults (spec_host_tools §4 key table)

  // Clamp coordinatesscale to 0x50..0xfa and mastertune to -100..+100.
  void clamp();
};

// INI persistence ([global] section). load_ini resets out_settings to defaults
// first; returns false if the file cannot be opened (defaults kept in out).
bool load_ini(const std::string& filepath, Settings& out_settings);
bool save_ini(const std::string& filepath, const Settings& settings);

// %APPDATA%\Daichi\Synth1\synth1.ini on Windows (SHGetFolderPath CSIDL 0x801a),
// $HOME/.config/synth1/synth1.ini elsewhere.
std::string default_ini_path();

// .ccm file (spec_midi_settings §3 id 0x3ef/0x405): line-based, skips lines
// starting with '#' and blank lines, CSV must parse to exactly 128 ints.
// out_map is left untouched on failure.
bool load_ccm(const std::string& filepath, midi::CcMap& out_map);
bool save_ccm(const std::string& filepath, const midi::CcMap& in_map);

}  // namespace synth1
