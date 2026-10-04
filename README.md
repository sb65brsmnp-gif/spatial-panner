# spatial-panner

A spatial audio tool: many audio layers placed in a 3D scene, a listener that
moves through it along drawn paths (head yaw/pitch, speed curve), rendered to
binaural (HRTF) or multichannel loudspeaker output. Target: Logic Pro on macOS
(AUv2 + standalone). The architecture spec lives in the project's
`spec/architecture.md`.

This repository contains **the engine** (plain C++17, no JUCE, with a
command-line renderer, see [docs/engine.md](docs/engine.md)) and **the
standalone app**: a Three.js scene and path editor in a JUCE window that plays
through the engine live, see [docs/editor.md](docs/editor.md). The AU plugin
and DAW automation come next.

## Layout

```
engine/            the DSP engine (library sp::engine)
  include/sp/      public headers: Scene, Pose, Renderer, SceneJson, SpeakerLayout
  src/             implementation, src/dsp/ holds the signal processing blocks
  tests/           Catch2 unit tests
app/               the standalone JUCE app (audio device, playback, bridge to the editor)
ui/                the editor: TypeScript + Three.js, built into one HTML file
tools/scene/       sp-scene: path/pose analysis and normalisation for the editor
tools/render/      sp-render: scene.json + audio -> WAV (binaural, speakers, ambiX)
tools/gensignals/  sp-gensignals: synthesises the demo sources (no third-party audio)
scenes/            demo scenes
scripts/           render_demos.sh renders every demo
cmake/             dependency fetching (SAF, libmysofa, nlohmann/json, dr_libs, Catch2)
```

## Building

Requirements: CMake 3.22+, a C++17 compiler, git, Node.js 20+ with npm (for
the editor UI), network access on first configure (dependencies, JUCE and the
default HRTF are fetched and pinned). Pass `-DSP_BUILD_APP=OFF` to build only
the engine and tools.

macOS (Apple Accelerate is used for BLAS/LAPACK; only Node is needed, e.g. `brew install node`):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

Linux (Debian/Ubuntu):

```sh
sudo apt install build-essential cmake ninja-build libopenblas-dev liblapacke-dev zlib1g-dev \
  nodejs npm libwebkit2gtk-4.1-dev libgtk-3-dev libasound2-dev libfreetype-dev libfontconfig1-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The default HRTF (SADIE II subject D1, a Neumann KU100, 8802 directions,
Apache 2.0) is downloaded at configure time into `build/data/hrtf/`. Pass
`-DSP_DEFAULT_HRTF=/path/to/other.sofa` to use another SOFA file, or
`--hrtf` on the renderer.

## Rendering the demos

```sh
scripts/render_demos.sh            # -> renders/*.wav
```

| File | What to listen for |
|------|--------------------|
| `01_flyby_binaural.wav` | Listener drives past a running engine at 25 m/s, 3 m to the side: Doppler drop, air absorption at distance, ground reflection. Clicks and a bell further along. |
| `02_flyby_no_doppler_binaural.wav` | Same scene with `doppler: 0` on every layer. |
| `03_room_walk_binaural.wav` | 30 s walk through a 12 x 3.5 x 16 m room past five layers, slowing to pass a talking voice at 0.5 m (near-field). Early reflections and late reverb follow the listener. |
| `04_room_walk_dry_binaural.wav` | The same walk with reflections and reverb off, for comparison. |
| `05_head_turn_binaural.wav` | Standing still: head turns 90 degrees left, 90 right, looks up 45 and down 30. |
| `06_room_walk_7.1.4.wav` | The room walk rendered for a 7.1.4 speaker layout (12 channels, L R C LFE Lss Rss Lrs Rrs Ltf Rtf Ltr Rtr). |
| `07_room_walk_ambix_o3.wav` | The room walk as 3rd-order ambiX (16 channels, ACN/SN3D). |

Renderer usage:

```
sp-render scene.json -o out.wav [--mode binaural|speakers|ambix] [--layout 7.1.4]
          [--hrtf file.sofa] [--order 3] [--duration s] [--normalize] [--bench]
```

## Scene files

Scenes are JSON (see `scenes/`): layers (audio, position, level, Doppler
amount, spread, directivity, distance model), a room (box with per-wall
materials, outdoor with a ground material, or none), the listener (paths made
of line / Bezier / Catmull-Rom / arc segments, a speed curve, head mode: along
path, look-at, or keyframed yaw/pitch/roll, plus offsets) and the environment
(temperature, humidity, speed of sound). `docs/engine.md` documents the format.

## Licences of what is pulled in

Spatial Audio Framework (ISC), libmysofa (BSD-3), nlohmann/json (MIT),
dr_libs (MIT-0 / public domain), Catch2 (BSL-1.0), SADIE II HRTF data
(Apache 2.0).
