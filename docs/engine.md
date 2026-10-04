# The engine

What `sp::engine` does, how it is meant to be driven by the standalone app
(thread 3) and the plugin (thread 4), and what is and is not there yet.

## Signal flow

For every layer, per 32-sample sub-block:

```
layer audio ─► delay line (one write, many fractional reads)
                 ├─ direct path:  distance gain ▸ directivity ▸ air absorption (ISO 9613-1 fit)
                 │                 ▸ binaural: HRTF pair (SADIE II KU100, interpolated, per-ear parallax
                 │                             and level inside 1.5 m, spread by blending directions,
                 │                             crossfaded partitioned convolution)
                 │                 ▸ speakers: VBAP gains (SAF)
                 │                 ▸ ambiX:    SH encode
                 ├─ image sources (box room, order 2 = 24 images; outdoor = ground only):
                 │                 distance gain ▸ directivity ▸ wall reflectance x air (3 bands)
                 │                 ▸ encoded into a 3rd-order Ambisonics bus
                 └─ reverb send ──► FDN (16 lines, 3-band Eyring RT60, Householder feedback)
                                    ▸ 16 plane waves into the Ambisonics bus

   or, with the ray-traced back-end (mesh rooms, objects, or --reflections steam):
                 ├─ direct path: as above, plus Steam Audio occlusion / transmission
                 │               (volumetric, per layer radius, 3-band) before the HRTF / VBAP / SH
                 └─ direct tap ──► Steam Audio reflection IR (Ambisonics, traced from the
                                   listener against the geometry) ▸ convolution ▸ Ambisonics bus
                                   (image sources and the FDN are off)

Ambisonics bus ─► binaural: 32 MagLS decoding filters designed from the loaded HRTF
                ▸ speakers: AllRAD matrix (EPAD/SAD for planar or tiny layouts)
                ▸ ambiX:    N3D -> SN3D
```

Every read of the delay line is a fractional (4th-order Lagrange) tap whose
length is `distance / c`. Because the listener moves, the length changes and
the pitch shifts: that is the Doppler effect, and it applies to each
reflection with its own geometry. A layer's `doppler` (0..1) scales the
amount by holding part of the delay at the path's starting distance.

Gains, delays and spherical-harmonic weights are interpolated per sample
inside a sub-block; filter coefficients are swapped per sub-block; HRTF pairs
crossfade over the sub-block in which they change (and are re-derived at
most every 4 sub-blocks, and only if the direction moved by more than
0.15 degrees).

Levels follow physics with one convention: a layer at its `reference_distance`
(default 1 m) plays at `level_db`. Direct level falls with
`(reference / distance) ^ rolloff` (clamped inside `min_distance`). The late
reverb's level comes from the room constant (Hopkins-Stryker, 16 pi / R minus
the energy already carried by the modelled images), so direct-to-reverberant
ratio is the distance cue it should be. `reverb_level_db`,
`reflections_level_db` and per-layer `reverb_send_db` are trims on top.

### Two reflection back-ends

`RenderConfig::reflections` picks `Builtin` (image sources + FDN, the
default for box rooms and outdoors), `SteamAudio` (ray tracing against the
room geometry and objects) or `Auto` (Steam whenever the scene has a mesh
room or objects, else Builtin). The two are interchangeable behind the same
`Renderer`; `stats().backend` says which one is running and `stats().note`
why, if it is not the one asked for (not compiled in, empty mesh).

Steam Audio supplies two things per layer and nothing else: the fraction of
the direct sound that gets through the geometry (occlusion, with
transmission through the material of whatever is in the way), and the
reflected sound field as an Ambisonics impulse response, re-traced every
`steam.updateInterval` seconds from the listener's position. The IR is
convolved with the layer's signal at the direct tap (so it carries the direct
path's propagation delay and Doppler; Steam's IR is relative to the direct
arrival), in blocks of `steam.frameSize` samples (256 by default: the
reflections arrive `frameSize - 32` samples, 4.7 ms, after the direct sound,
which is below the shortest real reflection delay in any room larger than a
cupboard; `stats().reflectionLatency`). Everything else, from the delay
lines and distance model to the HRTF and the decoders, is the same code in
both modes.

Settings (`SteamAudioSettings`): `rays` (4096) and `bounces` (0 = enough
to travel the IR length in the room, from the mean free path 4V/S, capped
at 96) set the ray tracing effort; `irSeconds` (0 = from Eyring's RT60 of
the room, times 1.1, plus 0.15 s, 0.3 to 6 s); `updateInterval`; `threads`
(0 = cores minus one); `reverb` = `Convolution` (true to the IR),
`Hybrid` (convolution for the first `hybridTransitionSeconds`, then a
parametric tail) or `Parametric` (cheapest, a 3-band reverb matched to the
IR's decay); `asyncSimulation` moves the tracing to a worker thread (for
the plugin; `sp-render` traces inline so renders are deterministic);
`occlusion` and `occlusionSamples`.

Levels: Steam Audio reconstructs pressure IRs from its energy histograms with
a normalisation that lands below the diffuse-field level a box room should
have. Measured against the Hopkins-Stryker reverberant-to-direct ratio in
two rooms with fully scattering walls, the shortfall was stable at about
2.5 dB (below 800 Hz), 7.7 dB (800 Hz to 8 kHz) and 4.3 dB (above 8 kHz), so
the feed into the convolution is shelved by those amounts. After that the
two back-ends agree within 2 dB per band on reverberant level in a box, and
within 1 dB on total tail energy (the A/B unit test).

Decay: the ray tracer is a geometric model. With mirror-like walls
(`scattering` near 0) a box room's near-horizontal paths between reflective
walls outlive the Eyring prediction that the FDN follows (RT60 about 2x
longer in a 10 x 3.2 x 12 m room with an absorbent ceiling). Material
scattering sets how fast the traced field becomes diffuse; the built-in
table uses 0.1 to 0.5 (surfaces in a room with ordinary contents) and
scenes can override it per material. With `scattering` 1 the two models
agree on decay within 10 %.

## Conventions

Metres, right-handed, **Y up, -Z forward, +X right** (Three.js). Yaw is
positive to the left (about +Y), pitch positive up, roll positive right ear
down. The engine converts to SOFA / SAF spherical coordinates (azimuth
counter-clockwise from the front, elevation up) at its boundary
(`sp::toSpherical`).

## Public API (engine/include/sp)

* `Scene.h` : the whole scene as plain data. `Layer`, `Room` (+ `Material`,
  `materials::byName`, `MeshGeometry`, `SceneObject`, `roomGeometry`,
  `loadObjMesh`), `Path` / `PathSegment`, `SpeedCurve`, `HeadTrack`,
  `Listener`, `Environment`, `Scene`.
* `SceneJson.h` : `sceneFromJson`, `sceneToJson`, `loadSceneFile`,
  `saveSceneFile`.
* `Pose.h` : `PoseEvaluator(scene, duration).evaluate(t, controls)` returns
  the listener `Pose` (position, orientation, velocity) as a pure function of
  timeline time. `ListenerControls` carries the live/automatable parameters
  (speed multiplier, yaw/pitch/roll offsets, position along path, active
  path). `SampledPath` and `SampledSpeed` are the arc-length and
  speed-integral tables behind it.
* `SpeakerLayout.h` : presets `stereo quad 5.1 7.1 5.1.4 7.1.4 9.1.6`, or a
  custom list of speakers (azimuth, elevation, distance, LFE flag).
* `Renderer.h` : the renderer, `ReflectionsBackend`, `SteamAudioSettings`,
  `Renderer::steamAudioAvailable()`.

```cpp
sp::RenderConfig cfg;
cfg.sampleRate = 48000;
cfg.mode = sp::OutputMode::Binaural;       // or Speakers (cfg.layout) or Ambisonics
cfg.hrtfPath = "sadie_d1.sofa";
cfg.reflections = sp::ReflectionsBackend::Auto;   // Builtin | SteamAudio
cfg.steam.asyncSimulation = true;                 // plugin: trace on a worker thread
sp::Renderer r(scene, cfg, /*duration seconds*/ 60);

// audio thread
r.process(inputs /* one mono float* per layer */, outputs /* numOutputs() channels */,
          numFrames, /*timeline time of the first frame, seconds*/ t);
```

* Any `numFrames` is accepted; output lags by `latencySamples()` (= the
  sub-block, 32 samples). Report that to the host. Traced reflections lag a
  further `stats().reflectionLatency` samples behind the direct sound; that
  is part of the room response, not host latency.
* `setListenerControls()` / `setLayerControls()` apply between `process()`
  calls (the plugin's automation lands here).
* `lastPose()` gives the UI the pose used for the latest sub-block.
* `reset()` clears tails for transport jumps.
* The engine renders "whatever audio you hand it now"; file playback, layer
  start times and looping are the caller's job (see `tools/render/main.cpp`).

## For the standalone app and the plugin

* Build the `Scene` from the editor's model; a `Renderer` takes an immutable
  copy at construction. Most edits do not need a new `Renderer`:
  `Renderer::prepareUpdate(scene)` (any thread) returns a `SceneUpdate` when
  only layers' properties, the listener (paths, speed, head) or the name
  changed, and `applyUpdate(update)` (audio thread, allocation free) swaps it
  in. Moved layers glide to their new position over ~40 ms, so dragging a
  source is click free. `prepareUpdate` returns null, meaning "rebuild", when
  the layer count, room or environment changed, or when the new scene could
  exceed the delay lines' `RenderConfig::maxDistance`. The app gives live
  edits headroom by building with a larger `maxDistance`.
* `analyzeScene` / `analysisToJson` (`sp/SceneAnalysis.h`) sample every path
  and the listener pose over time for the editor to draw; `sp-scene analyze`
  exposes the same on stdin/stdout.
* Many plugin instances with N = 1 layer each render exactly what one
  instance with N layers would, except for the reverb tail (each instance
  runs its own FDN; sum is equivalent). `Pose` is a pure function of time, so
  instances only need the scene and the host playhead.
* The 3D editor can read `SampledPath::positionAt(s)` and
  `PoseEvaluator::evaluate(t)` to draw the path and the listener avatar.

## Scene JSON

```jsonc
{
  "name": "...", "duration": 30,
  "layers": [{
    "name": "voice", "audio": "../signals/voice.wav",
    "position": [x, y, z], "level_db": -6, "mute": false,
    "doppler": 1.0, "spread_deg": 0, "directivity": 0.7, "directivity_forward": [1, 0, 0],
    "reference_distance": 1.0, "min_distance": 0.25, "rolloff": 1.0,
    "reverb_send_db": 0, "reflection_order": -1, "start_time": 0, "loop": true,
    "occlusion": true, "occlusion_radius": 0.5          // ray tracer: size of the source for occlusion
  }],
  "room": {
    "type": "box" | "outdoor" | "mesh" | "none", "size": [w, h, d], "origin": [x, y, z],
    "materials": "plaster" | {"walls": "...", "floor": "...", "ceiling": "...",
                              "left": .., "right": .., "front": .., "back": ..},
    "reflection_order": 2, "reflections_level_db": 0, "reverb_level_db": 0,
    "reverb_time_scale": 1.0, "reflections": true, "reverb": true,
    // type "mesh": the room is a triangle mesh (same axes, metres), from an OBJ
    // file relative to the scene file, or inline. OBJ "usemtl" names map to
    // materials via "materials"; a usemtl that is itself a material name needs no entry.
    "mesh": {"file": "lshape.obj", "materials": {"Walls": "plaster", "Floor": "wood_floor"}}
          | {"vertices": [[x, y, z], ...], "triangles": [[a, b, c], ...],
             "material_indices": [0, 1, ...], "materials": ["plaster", {...}]},
    // objects inside any room type: axis-aligned boxes (walls, pillars, furniture).
    // They occlude and reflect in the ray tracer and are ignored by the image-source model.
    "objects": [{"name": "partition", "box": {"min": [x, y, z], "max": [x, y, z]}, "material": "brick"},
                {"name": "pillar", "box": {"center": [x, y, z], "size": [w, h, d]}, "material": "concrete"}]
  },
  "listener": {
    "paths": [{"name": "...", "closed": false, "segments": [
        {"type": "line", "points": [p0, p1]},
        {"type": "bezier", "points": [p0, c0, c1, p1]},
        {"type": "catmull_rom", "points": [p0, p1, p2, ...]},
        {"type": "arc", "points": [centre, start, end], "turns": 0, "clockwise": false}]}],
    "path": {"points": [...]},                     // shorthand: one Catmull-Rom
    "active_path": 0, "position_mode": "speed" | "along_path", "path_fraction": 0,
    "speed": 1.4 | [{"time": 0, "speed": 1.4, "easing": "linear|smooth|ease_in|ease_out|hold"}],
    "path_start_time": 0, "loop_path": false, "static_position": [0, 1.6, 0], "head_radius": 0.0875,
    "head": {"mode": "along_path" | "look_at" | "keyframed",
             "look_at_point": [x, y, z], "look_at_layer": -1,
             "keys": [{"time": 0, "yaw": 0, "pitch": 0, "roll": 0, "easing": "smooth"}],
             "yaw_offset": 0, "pitch_offset": 0, "roll_offset": 0}
    // keyframed: keys are absolute angles. along_path / look_at: keys are
    // offsets on top of the path heading or look-at direction.
  },
  "environment": {"speed_of_sound": 343, "temperature_c": 20, "humidity": 50,
                  "pressure_kpa": 101.325, "air_absorption": true}
}
```

Materials (absorption at 125 Hz to 4 kHz): concrete, brick, plaster, glass,
wood_panel, wood_floor, carpet, curtain, acoustic_tile, absorber, grass,
gravel, asphalt, water, snow, audience; or `{"name": "x", "absorption": [6 values],
"scattering": 0.3, "transmission": [low, mid, high]}`. `scattering` (0 mirror-like,
1 fully diffuse) and `transmission` (energy fraction that passes through, 3 bands:
below 800 Hz, 800 Hz to 8 kHz, above) are used by the ray tracer only.

Every key added for the ray tracer (`occlusion`, `occlusion_radius`, room
`type: mesh`, `mesh`, `objects`, material `scattering` and `transmission`)
is optional; scene files written before them load unchanged, and files that
use them load in a build without Steam Audio (the geometry is then ignored
and `stats().note` says so). `sceneFromJson(text, baseDir)` resolves
`mesh.file` against `baseDir`; `loadSceneFile` passes the scene file's
directory. `sceneToJson` writes the mesh inline unless `room.meshFile` is
set.

## Performance (measured on one 2.8 GHz Xeon core, Linux, GCC 13 -O3)

Spec target: 64 layers at 48 kHz, direct HRTF + 2nd-order images + reverb,
under 50 % of one core.

| Scene | Load |
|-------|------|
| 64 layers, free field, binaural | 33 % |
| 64 layers, box room, 1st-order images (6 per layer) | 100 % |
| 64 layers, box room, 2nd-order images (24 per layer) | 290 % |
| Demo room walk, 5 layers, 2nd order | 25 % |
| Ray traced, per layer: reflection convolution, 0.8 s IR, 256-sample block | 6 % (4 % at 1024) |
| Ray traced, per layer: direct-path occlusion | under 1 % |
| Ray tracing, 10 x 3.2 x 12 m box, 4096 rays x 83 bounces every 0.1 s, 2 threads | 55 % (on those threads) |
| Same, 8192 rays every 0.05 s | 200 % |

The image taps are the cost (about 100 cycles per tap per sample: a 5-point
fractional read, two first-order sections and a 16-channel encode, all
scalar). In Steam mode the taps and the FDN are gone and the audio-thread
cost is the convolution, which scales with the IR length divided by the
block size (Steam's convolution runs at 32-sample blocks at 15x that cost,
which is why the reflections run in their own larger block); the ray
tracing is a fixed cost per scene, not per layer (one set of rays shaded
against every source), and belongs on a worker thread in the plugin
(`asyncSimulation`). Steam Audio is built without Embree and IPP (its SSE
fall-backs), so its tracing and FFTs are slower than the shipped SDK's; both
are optional build switches in `cmake/SteamAudio.cmake` if they are ever
installable. Planned levers, in order: process the taps of a layer as a
structure-of-arrays so the filters vectorise across taps; encode 2nd-order
images at 1st Ambisonics order; lower reflection order for quiet or distant
layers (the spec's suggestion); interpolate HRTF spectra instead of
re-transforming interpolated impulse responses. None of this changes the
interfaces.

## Deviations from the spec, and open items

* **HRTF rendering is in-house (libmysofa + own partitioned convolution),
  not Steam Audio.** The HRTF data is the one the spec names (SADIE II
  KU100), interpolation is libmysofa's, and the convolution is ours (Steam
  Audio's own default HRTF is a CIPIC subject and its interpolation is
  nearest or bilinear, so this is the better binaural path). Steam Audio is
  in as the reflections and occlusion back-end only.
* **Steam Audio is built from source** (master just after 4.8.1, pinned by
  commit, with FlatBuffers 1.12 and pffft) because the prebuilt SDK is not fetchable from
  the build environment. `cmake/PatchSteamAudio.cmake` applies five small
  patches after download: drop its forced macOS architectures and
  deployment target, drop `-fabi-version=6` (GCC 13's `<future>` does not
  compile under it), skip the FMOD copy step, keep its THIRDPARTY.md inside
  the source tree, and change one aligned SSE load in `ArrayMath::
  multiplyAccumulate` to an unaligned one (its accumulator rows are not
  16-byte aligned when the spectrum has an odd bin count; the shipped SDK
  uses IPP for this path, so the SSE path had not been exercised). Only
  needed when Steam's `libphonon` is a dynamic library for the plugin: it is
  not, so nothing is bundled with the AU.
* **Traced reflections carry the direct path's Doppler, not their own.**
  The image-source model shifts every reflection by its own path length
  change; the traced IR is re-computed every `updateInterval` and convolved
  with the direct-tap signal, so moving sources and listeners get the
  direct path's pitch shift on the reflections too. For listener speeds
  below a few m/s this is inaudible; the fly-by demo keeps the built-in
  model.
* **Level calibration of the traced reflections** is the measured 3-band
  shelf described under "Two reflection back-ends", not a derivation from
  Steam Audio's units.
* **Near-field:** per-ear parallax (each ear gets the HRIR for its own
  direction) and per-ear distance level inside 1.5 m, instead of the TH Köln
  near-field HRIR set (not reachable from here; a SOFA loader for it is a
  small addition).
* **Spread** blends HRIRs over a ring of directions around the source;
  speaker mode currently ignores spread (MDAP table per spread value is
  trivial to add).
* **Head keys in along_path and look_at modes are offsets** on top of the
  path heading or the look-at direction (the spec only defined keys for the
  keyframed mode). This lets the timeline's yaw and pitch lanes turn and
  tilt the head while it still follows the path.
* **Banking** in along-path head mode is parsed but not applied.
* **Speaker distance compensation** exists for custom layouts with
  distances; presets are equidistant.
* The CPU target is not met for 2nd-order reflections at 64 layers (table
  above).
