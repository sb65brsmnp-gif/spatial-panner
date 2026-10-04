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

## Conventions

Metres, right-handed, **Y up, -Z forward, +X right** (Three.js). Yaw is
positive to the left (about +Y), pitch positive up, roll positive right ear
down. The engine converts to SOFA / SAF spherical coordinates (azimuth
counter-clockwise from the front, elevation up) at its boundary
(`sp::toSpherical`).

## Public API (engine/include/sp)

* `Scene.h` : the whole scene as plain data. `Layer`, `Room` (+ `Material`,
  `materials::byName`), `Path` / `PathSegment`, `SpeedCurve`, `HeadTrack`,
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
* `Renderer.h` : the renderer.

```cpp
sp::RenderConfig cfg;
cfg.sampleRate = 48000;
cfg.mode = sp::OutputMode::Binaural;       // or Speakers (cfg.layout) or Ambisonics
cfg.hrtfPath = "sadie_d1.sofa";
sp::Renderer r(scene, cfg, /*duration seconds*/ 60);

// audio thread
r.process(inputs /* one mono float* per layer */, outputs /* numOutputs() channels */,
          numFrames, /*timeline time of the first frame, seconds*/ t);
```

* Any `numFrames` is accepted; output lags by `latencySamples()` (= the
  sub-block, 32 samples). Report that to the host.
* `setListenerControls()` / `setLayerControls()` apply between `process()`
  calls (the plugin's automation lands here).
* `lastPose()` gives the UI the pose used for the latest sub-block.
* `reset()` clears tails for transport jumps.
* The engine renders "whatever audio you hand it now"; file playback, layer
  start times and looping are the caller's job (see `tools/render/main.cpp`).

## For the standalone app and the plugin

* Build the `Scene` from the editor's model; a `Renderer` takes an immutable
  copy at construction. Changing layer positions or the room currently means
  constructing a new `Renderer` (cheap for small scenes; the HRTF load is the
  slow part at ~0.5 s, so cache `HrtfSet` or keep one renderer per output).
  Making layer positions hot-swappable without a rebuild is a small change
  in `Renderer::Impl::computeTargets`, which already recomputes images when a
  layer's position offset changes.
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
    "reverb_send_db": 0, "reflection_order": -1, "start_time": 0, "loop": true
  }],
  "room": {
    "type": "box" | "outdoor" | "none", "size": [w, h, d], "origin": [x, y, z],
    "materials": "plaster" | {"walls": "...", "floor": "...", "ceiling": "...",
                              "left": .., "right": .., "front": .., "back": ..},
    "reflection_order": 2, "reflections_level_db": 0, "reverb_level_db": 0,
    "reverb_time_scale": 1.0, "reflections": true, "reverb": true
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
  },
  "environment": {"speed_of_sound": 343, "temperature_c": 20, "humidity": 50,
                  "pressure_kpa": 101.325, "air_absorption": true}
}
```

Materials (absorption at 125 Hz to 4 kHz): concrete, brick, plaster, glass,
wood_panel, wood_floor, carpet, curtain, acoustic_tile, absorber, grass,
gravel, asphalt, water, snow, audience; or `{"name": "x", "absorption": [6 values]}`.

## Performance (measured on one 2.8 GHz Xeon core, Linux, GCC 13 -O3)

Spec target: 64 layers at 48 kHz, direct HRTF + 2nd-order images + reverb,
under 50 % of one core.

| Scene | Load |
|-------|------|
| 64 layers, free field, binaural | 33 % |
| 64 layers, box room, 1st-order images (6 per layer) | 100 % |
| 64 layers, box room, 2nd-order images (24 per layer) | 290 % |
| Demo room walk, 5 layers, 2nd order | 25 % |

The image taps are the cost (about 100 cycles per tap per sample: a 5-point
fractional read, two first-order sections and a 16-channel encode, all
scalar). Planned levers, in order: process the taps of a layer as a
structure-of-arrays so the filters vectorise across taps; encode 2nd-order
images at 1st Ambisonics order; lower reflection order for quiet or distant
layers (the spec's suggestion); interpolate HRTF spectra instead of
re-transforming interpolated impulse responses. None of this changes the
interfaces.

## Deviations from the spec, and open items

* **HRTF rendering is in-house (libmysofa + own partitioned convolution),
  not Steam Audio.** Steam Audio's source build pulls in FlatBuffers, ISPC,
  Embree and more, and its prebuilt SDK could not be fetched from this
  environment. The HRTF data is the one the spec names (SADIE II KU100),
  interpolation is libmysofa's, and the convolution is ours, so nothing
  audible is lost; Steam Audio stays the plan for geometry occlusion and
  pathing later and would slot in behind the same `Renderer`.
* **Near-field:** per-ear parallax (each ear gets the HRIR for its own
  direction) and per-ear distance level inside 1.5 m, instead of the TH Köln
  near-field HRIR set (not reachable from here; a SOFA loader for it is a
  small addition).
* **Spread** blends HRIRs over a ring of directions around the source;
  speaker mode currently ignores spread (MDAP table per spread value is
  trivial to add).
* **Banking** in along-path head mode is parsed but not applied.
* **Speaker distance compensation** exists for custom layouts with
  distances; presets are equidistant.
* The CPU target is not met for 2nd-order reflections at 64 layers (table
  above).
