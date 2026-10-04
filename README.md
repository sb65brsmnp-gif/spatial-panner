# spatial-panner

A spatial audio tool: many audio layers placed in a 3D scene, a listener that
moves through it along drawn paths (head yaw/pitch, speed curve), rendered to
binaural (HRTF) or multichannel loudspeaker output. Target: Logic Pro on macOS
(AUv2 + standalone). The architecture spec lives in the project's
`spec/architecture.md`.

This repository currently contains **the engine** (thread 2 of the plan): plain
C++17, no JUCE, with a command-line renderer and listenable test renders. The
3D editor / standalone app and the plugin come next and build on the
interfaces described in [docs/engine.md](docs/engine.md).

## Layout

```
engine/            the DSP engine (library sp::engine)
  include/sp/      public headers: Scene, Pose, Renderer, SceneJson, SpeakerLayout
  src/             implementation, src/dsp/ holds the signal processing blocks
  tests/           Catch2 unit tests
tools/render/      sp-render: scene.json + audio -> WAV (binaural, speakers, ambiX)
tools/gensignals/  sp-gensignals: synthesises the demo sources (no third-party audio)
scenes/            demo scenes (lshape.obj is a room mesh)
scripts/           render_demos.sh renders every demo
cmake/             dependency fetching (SAF, libmysofa, nlohmann/json, dr_libs, Catch2,
                   Steam Audio with FlatBuffers and pffft, built from source)
```

## Building

Requirements: CMake 3.22+, a C++17 compiler, git, network access on first
configure (dependencies and the default HRTF are fetched and pinned).

macOS (Apple Accelerate is used for BLAS/LAPACK, nothing to install):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

Linux (Debian/Ubuntu):

```sh
sudo apt install build-essential cmake ninja-build libopenblas-dev liblapacke-dev zlib1g-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The default HRTF (SADIE II subject D1, a Neumann KU100, 8802 directions,
Apache 2.0) is downloaded at configure time into `build/data/hrtf/`. Pass
`-DSP_DEFAULT_HRTF=/path/to/other.sofa` to use another SOFA file, or
`--hrtf` on the renderer.

Steam Audio (ray-traced reflections and occlusion against room meshes and
objects) is cloned and built from source as a static library on the first
configure: about 40 MB of download and 3 to 5 minutes of extra build. It
needs nothing beyond the requirements above. `-DSP_WITH_STEAM_AUDIO=OFF`
leaves it out; the engine then renders every scene with the built-in
image-source model and ignores meshes and objects. Nothing has to be bundled
with the plugin later: the library is linked in.

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
| `08_lshape_binaural.wav` | Ray traced: a walk round the corner of an L-shaped room loaded from `lshape.obj`. Layers in the other leg are heard through the opening and go indirect as the wall comes between. |
| `09_occluder_binaural.wav` | Ray traced: a box room with a brick partition, a concrete pillar and a sofa. Layers dull and drop as they pass behind the partition; the room's own reflections are traced against the objects too. |
| `10_room_walk_traced_binaural.wav` | The room walk of 03 with `--reflections steam`: the same box room, reflections and reverb from the ray tracer instead of the image-source model and FDN. A/B with 03. |
| `11_occluder_no_objects_binaural.wav` | The occluder scene with `--reflections builtin`: the objects are ignored, so nothing is occluded. A/B with 09. |

Renderer usage:

```
sp-render scene.json -o out.wav [--mode binaural|speakers|ambix] [--layout 7.1.4]
          [--hrtf file.sofa] [--order 3] [--duration s] [--normalize] [--bench]
          [--reflections auto|builtin|steam] [--steam-rays N] [--steam-bounces N]
          [--steam-interval s] [--steam-block N] [--steam-reverb convolution|hybrid|parametric]
```

`--reflections auto` (the default) uses the ray tracer whenever the scene has
a mesh room or objects, and the image-source model for a plain box or
outdoors.

## Scene files

Scenes are JSON (see `scenes/`): layers (audio, position, level, Doppler
amount, spread, directivity, distance model, occlusion radius), a room (box
with per-wall materials, outdoor with a ground material, a mesh from an OBJ
file or inline, or none, plus box-shaped objects such as walls, pillars and
furniture), the listener (paths made
of line / Bezier / Catmull-Rom / arc segments, a speed curve, head mode: along
path, look-at, or keyframed yaw/pitch/roll, plus offsets) and the environment
(temperature, humidity, speed of sound). `docs/engine.md` documents the format.

## Licences of what is pulled in

Spatial Audio Framework (ISC), libmysofa (BSD-3), nlohmann/json (MIT),
dr_libs (MIT-0 / public domain), Catch2 (BSL-1.0), SADIE II HRTF data
(Apache 2.0), Steam Audio (Apache 2.0), FlatBuffers (Apache 2.0), pffft
(BSD-like, FFTPACK licence).
