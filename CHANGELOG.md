# Changelog

## Android 1.0.0 — 2026-10-30 (pre-release)
First Android release (`com.kaiyoti.synth1`, Android 8.0+, arm64-v8a and x86_64).

- Same sound engine as the web edition (C++ core); output via Oboe in low-latency mode at the
  device's native sample rate.
- Classic skin by default, s80 skin selectable from the bottom bar (remembered). The panel is drawn
  at the largest whole-number scale that fits the screen, so it stays sharp.
- All 12 banks (00 Factory + 01–11), program select window with the bank list, bank and program
  change from the panel.
- On-screen keyboard with pitch and mod wheels, multitouch (play keys while turning a knob), octave
  shift buttons.
- USB MIDI input (Android 10+): notes, CC (CC1 moves the mod wheel), pitch bend, channel
  aftertouch, bank select, program change.

Known limitations:
- Not yet tested on a real device: built and checked on desktop only (engine renders
  byte-identical to the reference, panel pixel checks, MIDI routing tests).
- No Bluetooth MIDI yet; incoming MIDI notes do not light the on-screen keys.
- The OPT dialog is not available (as on the web edition).
- Held notes stop when the program changes.
- The program select window needs a screen of at least 987×623 pixels to show uncut.

## Web edition — 2026-10-24
- First public release: browser editor with artwork drawn in code (classic skin by default,
  `?skin=s80` for the Nord-coloured skin), engine compiled to WebAssembly, AudioWorklet audio with
  a lower-latency fallback, Web MIDI, on-screen keyboard with pitch/mod wheels and octave shift
  (buttons or `-` / `=`), default panel scale 2.
- Banks reorganised into 11 family banks (1007 patches, duplicates removed, cleaned-up names,
  colour tags by family); Synth1's stock bank added as bank 00.
- MIT licence (bank 00 excluded).
