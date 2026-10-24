# Synth1 Recreation — Kaiyoti Web Edition

A browser recreation of **Synth1**, the classic free virtual-analog synthesizer by **Ichiro Toda
(Daichi)**. This project is not affiliated with or endorsed by the original author.

- `web/` — the playable synth: an editor with artwork drawn in code (no original bitmaps), an on-screen keyboard with pitch/mod wheels, Web MIDI input, and the sound engine
  compiled to WebAssembly.
- `banks/` — the Kaiyoti sound library as Synth1 bank folders (`NNN.sy1`), usable in the original
  plugin too.

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

## Banks
| Bank | Contents |
|---|---|
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
