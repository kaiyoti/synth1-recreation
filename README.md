# Synth1 Recreation — Kaiyoti Edition

A browser and Android recreation of **Synth1**, the classic free virtual-analog synthesizer by **Ichiro Toda
(Daichi)**. This project is not affiliated with or endorsed by the original author.

- `web/` — the playable synth: an editor with artwork drawn in code (no original bitmaps), an on-screen keyboard with pitch/mod wheels, Web MIDI input, and the sound engine
  compiled to WebAssembly.
- `native/` — the Android app (`native/android`) and the C++ code it is built from: the sound
  engine (`core`), the panel renderer (`panel`) and the on-screen keyboard (`keyboard`).
- `banks/` — Synth1 bank folders (`NNN.sy1`), usable in the original plugin too: `00 Factory` is
  Synth1's stock bank, unchanged; 01–11 are the Kaiyoti sound library.

## Run it
Any static web server works:

```sh
cd web && python3 -m http.server 8000
# open http://localhost:8000/
```

`localhost` (or any HTTPS host) enables the low-latency AudioWorklet path and Web MIDI; over plain
http on another host the engine falls back to the main thread.

URL options: `?scale=1|2|3` (panel zoom, default 2), `?skin=classic|s80` (default `classic`),
`?buf=256|512|1024` (main-thread audio buffer, plain-http fallback only).
Computer keyboard: Z..M / Q..P rows play notes, `-` / `=` (or the octave buttons) shift octaves.

## Android
Download the APK from [Releases](https://github.com/kaiyoti/synth1-recreation/releases) and install it (allow installs from unknown
sources). Android 8.0+ (arm64 or x86_64); USB MIDI input needs Android 10+. Classic skin and the same
banks as the web edition: the panel at the largest whole-number scale that fits, side columns
with the pitch/mod wheels and the view buttons (keyboard on/off, octave, panic, MIDI lamp), and a
full-width keyboard sized to your screen (about 3 octaves on a phone).

Build it yourself (JDK 17+, Android SDK with platform 35, build-tools 35.0.0, NDK 27.2.12479018,
CMake 3.22.1):

```sh
cd native/android
./gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
```
The build packs the classic skin from `web/skins/classic` and the banks listed in their
`banks.json`. A release build is signed only when `~/.synth1-android-release/keystore.properties`
(or `$S1_KEYSTORE_PROPS`) exists; otherwise it is unsigned.

## Banks
| Bank | Contents |
|---|---|
| 00 | Factory: Synth1's stock bank (unchanged) |
| 01–03 | Drums & Percussion (kicks, snares, toms, hats, cymbals, kits; taiko/timpani, shakers, tickers, metal, wood) |
| 04 | Bass |
| 05–06 | Brass & Winds (trumpets, French horns, trombones, tuba; flutes, reeds, pipes) |
| 07 | Strings & Choir |
| 08 | Guitars & Plucked |
| 09 | Keys & Bells (pianos, organs, music boxes, mallets, bells) |
| 10 | Synths & SFX (leads, arps/sequences, pads, effects) |
| 11 | Unsorted |

Colour tags (shown in Synth1's program list): red = drums & percussion, blue = bass,
yellow = brass & winds, green = strings, choir & pads, cyan = guitars, plucked, keys & bells,
magenta = synth leads & arps, default = SFX / unsorted.

## License

MIT, see [LICENSE](LICENSE). Synth1 itself is the work of Ichiro Toda; this
licence covers only this recreation (code, artwork and patch banks 01–11). Bank 00 is the stock
Synth1 factory bank, included unchanged; it belongs to its original authors and is not covered
by the MIT licence.

Third-party code and data: Oboe (`native/android/third_party/oboe`, Apache-2.0, its own LICENSE);
DejaVu fonts (Bitstream Vera licence) for the label glyphs drawn into the skins and the built-in
UI bitmap font.

See [CHANGELOG.md](CHANGELOG.md) for release notes.
