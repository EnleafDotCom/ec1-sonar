# ec–1 Sonar for the M-VAVE FM-1

Firmware that turns the M-VAVE FM-1 into **ec–1 Sonar**: four ambient loops (haze, glint, drone, pulse) running at their own lengths and speeds, each with its own tape machine, all feeding a three-head tape echo and an eleven-second hall, plus a keyboard of pads and angelic tones you play over them.

It is built on [M-VAVE FM1 Open](https://github.com/m-vave/fm1_open) V14 (commit `4c7496d`). The files in `files/` are copied over that tree; GitHub Actions does the build.

## Getting the installer

Every push runs **Build ec-1 Sonar firmware** (Actions tab). When it finishes, download the artifacts:

- `ec1-sonar-installer`: `ec1-sonar-FM1-Installer.html`, the FM1 workbench with this firmware inside
- `ec1-sonar-firmware`: `ec1-sonar.fwsc`, the raw firmware image

To install: open the installer HTML in desktop Chrome or Edge, go to **Device update**, tick **Force flash** (needed when coming from other firmware), connect the FM-1 over USB, allow MIDI access, back up, then flash. Flashing the stock M-VAVE firmware the same way returns the FM-1 to normal.

## Playing it

| Control | Does |
|---|---|
| PLAY | start / stop; hold a second to fade the loops in or out |
| all 27 keys | play KEYS, chromatic from F3, snapped to the mood's scale by default |
| ALGORITHM | the keys' pad (Seraph, Halo Pad, Choir, Cathedral, Aurora, Glass Pad, Strings, Ether) |
| OCT − / + | the keys' octave |
| SELECT | the track (or KEYS) the pages edit |
| PRESETS | the selected track's sound |
| K1 / K2 (main screen) | keys volume / loops volume |
| K3 / K4 (main screen) | space amount / tempo |
| SCL | a new world |
| ARP | drift on / off |
| SEQ | STEPS: white keys are the 16 steps; black keys mute, new loop, clear, remix, undo, shift |
| HOME | WORLD: white keys 1–8 load songs (SAVE + key stores); black keys shuffle, undo, wipe, mood, space, drift |
| EDIT / ENV / LFO | a track's sound, shape and tape; on KEYS: WAVE (drawn), ENVELOPE (drawn), TONE |
| FX / GLO | space / mix |
| REC | the keys write into the selected track |
| hold SAVE + | SCL undo · PLAY restart loops · REC clear track · K1–K4 track levels |
| tap SAVE | save the song (songs autosave too) |

## Files

- `files/app/sonar.h`, `sonar.c`: song, worlds, composer, drift, mutate, undo, saving
- `files/app/sonar_dsp.c`: the sound engine, run in the audio interrupt (integer only, no division, tables in RAM)
- `files/app/sonar_ui.inc`: screen and panel, included by `screen.c`
- `files/app/screen.c`, `audio_output.c`, `fm_factory.c`: FM1 Open files changed to host ec–1 Sonar (stock pages compiled out to make room)
- `files/sdk/Build.ps1`, `manifest.json`: Linux toolchain support and size optimisation for the new files
- `preview/`: the browser preview page (the same firmware compiled to WebAssembly)

Licensed GPL-3.0-only WITH GPL-3.0-linking-exception, like FM1 Open.
