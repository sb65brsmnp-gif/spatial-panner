# The editor and the standalone app

`Spatial Panner.app` is a JUCE window hosting a Three.js editor (a WebView,
WKWebView on macOS). You place audio layers in a 3D room, draw the listener's
path, shape speed and head movement on a timeline, and hear the result live
through the engine, binaural on headphones or on a speaker layout up to 9.1.6.

## Building and running

The normal CMake build (see the README) also builds the editor UI with npm
and the app:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
open "build/app/SpatialPannerApp_artefacts/Release/Spatial Panner.app"
```

The app is ad-hoc signed by the build, which is all a locally built app needs
to run on the Mac that built it. A scene file can be passed on the command
line or dropped on the Dock icon. Audio device and output channels are set
with **Output → Audio device…** and remembered.

### Working on the UI

```sh
cd ui && npm ci
npm run dev          # http://localhost:5173, browser-only (uses build/tools/scene/sp-scene for analysis)
npm test             # geometry unit tests (vitest)
npm run e2e          # drives the editor in headless Chromium against the dev server
```

In the browser the transport is simulated (no audio). To develop against the
real audio engine, start `npm run dev` and launch the app with
`SP_UI_DEV_URL=http://localhost:5173` in its environment; it then loads the
dev server instead of the built-in page, with hot reload.

## Using it

**Layers** (sidebar): *Add audio files…* creates one layer per file (WAV,
AIFF, FLAC, MP3, M4A, Ogg, CAF; kept in memory). A mono file is one source.
A stereo file becomes a **stereo pair**: two balls, L and R, joined by a bar,
playing the file's left and right channels from their own places, so the
listener can walk between or through them. Drag the bar to move the pair,
drag an end to widen, narrow or turn it (Option-drag an end keeps the centre
fixed), or use the *Stereo field* sliders: width (metres), rotation,
elevation and *Mono*, which sums both channels at the centre. *Play as* turns
a stereo file into a single summed source instead. Each layer also has mute
and solo, level, start time and loop, Doppler amount, spread, directivity and
facing, distance rolloff, room send and reflections. Drag a layer in the view
to move it; Shift+drag changes its height. Any number of layers can be added;
CPU is the limit (see docs/engine.md for figures; a stereo pair costs two
layers).

**Path tools** (toolbar, key in brackets). Drawing on an empty scene creates
the path; drawing again appends to the current path's end.

| Tool | Use |
| --- | --- |
| Select (V) | Move layers, path points and Bézier handles. Alt+click on a path inserts a point, Delete removes the selected point or layer. |
| Freehand (F) | Drag to draw; the stroke is simplified and fitted with smooth curves. |
| Point to point (L) | Click points joined by straight lines; double-click or Enter finishes. |
| Curve (C) | Click points for a smooth curve through them; double-click or Enter finishes. |
| Pen (P) | Bézier pen: click for a corner, drag for a smooth point; Enter finishes. |
| Shape (S) | Drag out a circle, ellipse, figure 8, spiral or helix from its centre. |

Paths are drawn on a plane at the height set under **Path & head** (default
ear height, 1.6 m); draw in the Front or Side view to draw vertically. Grid
snap is on the same tab.

**Views**: 3D orbit (1), Top (2), Front (3), Side (4), and Ears (5), the
listener's own view while playing. *Follow* keeps the listener in view.

**Timeline** (bottom): transport buttons for beginning of path (Enter), back
and forward (a click jumps 5 s, hold to scrub at 4x), stop, play/pause
(Space) and end of path; `,` and `.` step the playhead 1 s back or forward,
0.1 s with Shift. Click the ruler to seek. Three
lanes hold keys for speed (m/s), head yaw and head pitch. Double-click a lane
to add a key, drag to move it, Delete to remove it; the bar above the lanes
edits the selected key's value and easing. Mouse wheel zooms. In the default
*along path* head mode the yaw and pitch keys turn and tilt the head on top
of the direction of travel; in *keyframed* mode they are absolute; *look at*
keeps the head on a point.

**Room**: box size and centre, wall materials (absorption per band), air
temperature and humidity; or no room (free field). *Early reflections* has
the on/off, the reflection order and a level slider; *Late reverb* has the
on/off, a level slider, and the choice between the built-in reverb (with its
decay multiplier) and an impulse response. Both levels are trims on a
calibrated model: at 0 dB the room is as loud as a room of that size and
those materials should be, so a scene with "too much room" is turned down
here rather than by moving layers. Scenes can also carry a mesh room (an OBJ
file, as in `scenes/lshape.json`) and box objects such as partitions and
pillars (as in `scenes/occluder.json`); the editor draws them, lists them on
the Room tab and plays them through the engine's ray-traced back-end (Steam
Audio), but they are set in the scene file, not placed in the editor yet.

**Impulse response reverb**: choose *Impulse response (WAV)* under Late
reverb and press *Load IR…* to pick a WAV (any sample rate and bit depth;
8/16/24/32-bit PCM or float). The tab shows the file's channel count,
length and rate and how it is played: a mono file becomes a diffuse field
around the listener, a stereo file two broad sources left and right of the
head, a 4-channel file first-order ambiX (ACN/SN3D) fixed to the room, so it
stays put when the head turns. *Channels* overrides that reading (a stereo
file can be summed to mono, for instance). The IR replaces only the late
tail: the engine's own early reflections of each layer keep running on top,
and they move with the layers; if the recording already contains the room's
first reflections, switch *Early reflections* off. The IR is normalised to
the room's calibrated reverb level (so the Level slider still works as a
trim) and *IR gain* trims it further. The tail arrives 4.7 ms (224 samples)
later than the built-in reverb would, which is inaudible in a reverb but is
reported by the engine. Changing or reloading the IR restarts the room
model, like any other room change. Two synthetic test IRs come with the
signals (`sp-gensignals` writes `hall_ir.wav`, stereo 2.5 s, and
`plate_ir.wav`, mono 1.4 s); real room recordings in WAV form, mono, stereo
or first-order ambiX, work the same way. The scene file stores the IR path
relative to the scene, like audio.

**Output**: binaural (SADIE II KU100 HRTF), speakers (stereo, quad, 5.1, 7.1,
5.1.4, 7.1.4, 9.1.6, mapped onto the device's first outputs) or ambiX. Changes
apply while playing. *Bounce to WAV…* renders the scene to a 24-bit WAV in the
current output format, faster than real time.

Undo/redo (⌘Z / ⇧⌘Z), open (⌘O), save (⌘S / ⇧⌘S). Scenes are the engine's
scene JSON with a few editor-only keys (layer colour, solo, draw height,
output); audio paths are stored relative to the scene file.

## How it fits together

```
ui/ (TypeScript, Three.js)           app/Source (JUCE)
  model/store.ts  scene + undo   ─┐
  view/*          3D view, tools  │  setScene, transport, setOutput, analyze,
  panels/*        sidebar,        ├─ open/save, bounce … (native functions)  → Bridge.cpp
                  timeline        │  tick (30 Hz: time, pose, meters),        ← Session.cpp
  bridge/backend.ts              ─┘  openFile (events)                          AudioLibrary.cpp
```

* Every edit sends the whole scene (throttled to 50 ms). `Session` tries
  `Renderer::prepareUpdate` first, which covers moving layers, levels, paths,
  speed and head keys without interrupting playback. Changes that need a new
  renderer (adding or removing layers, room, output format) are built on a
  background thread and crossfaded in over 40 ms.
* The audio thread never allocates or locks: programs and patches arrive
  through single-producer queues and are handed back to the message thread to
  be freed.
* Path and pose drawing comes from the engine itself (`analyze`), so what the
  editor draws is what the engine plays.

## Not in this version

* DAW integration: the AU plugin, host transport sync and automation are the
  next step.
* Placing and editing walls, objects and mesh rooms in the editor (they load,
  show and play, but are edited in the scene file).
* Audio files are held in memory, about 0.2 GB per hour of mono audio at
  48 kHz. Streaming from disk is a later addition if needed.
