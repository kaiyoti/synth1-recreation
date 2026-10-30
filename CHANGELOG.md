# Changelog

## Android 1.0.2 — 2026-10-30 (pre-release)
- The synth panel now fills the screen: it uses the largest whole-number scale that fits the full
  height (3× on a 2400×1080 phone, 1.5× the size of 1.0.1).
- The keyboard (with the pitch and mod wheels) is an overlay over the bottom of the screen;
  KBD shows or hides it without moving or shrinking the panel. In portrait, where there is room,
  it sits below the panel instead.
- KBD, INFO and PANIC (right) and OCT − / octave / OCT + and the MIDI lamp (left) stay visible
  above the keyboard at all times.

## Android 1.0.1 — 2026-10-30 (pre-release)
Layout rework after the first test on a phone ("poor use of space").

- No black areas: the panel is flanked by two columns in the classic look that fill the screen
  width. Left: pitch and mod wheels, drawn smoothly at the screen's resolution. Right: the VIEW
  pane with KBD (show/hide the keyboard), INFO (audio/MIDI status overlay), PANIC (all notes off),
  OCT − / OCT + with an octave readout, and a MIDI-in lamp.
- The keyboard spans the full width and gets all the height below the panel; its key count follows
  the screen's physical size (about 3 octaves, ~6.5 mm white keys on a phone). In portrait it is a
  single row of normal height under the panel.
- Hiding the keyboard lets the panel grow to the next scale that fits (e.g. 2× → 3×).
- The grey bottom bar (skin button and status text) is gone; the app is classic-skin only, and the
  APK shrank from 15 MB to 10 MB.
- Rotating or resizing no longer resets the patch you are editing.

## Android 1.0.0 — 2026-10-30 (pre-release)
First Android release (`com.kaiyoti.synth1`, Android 8.0+, arm64-v8a and x86_64).

- Same sound engine as the web edition (C++ core); output via Oboe in low-latency mode at the
  device's native sample rate.
- Classic skin by default, s80 skin selectable from the bottom bar. The panel is drawn
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
