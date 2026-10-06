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
listener can walk between or through them. Drag the white handle at the
centre (or the bar) to move the pair, drag the L or R ball to widen, narrow
or turn it (Option-drag an end keeps the centre fixed), or use the *Stereo field* sliders: width (metres), rotation,
elevation and *Mono*, which sums both channels at the centre. *Play as* turns
a stereo file into a single summed source instead. A 4-, 9- or 16-channel
file is an **Ambisonic recording** (first to third order) and becomes a
translucent **sphere**: the recording's sounds sit on its surface, the ball
at its centre is where the recording was made, and the listener can walk
into it, through it and out again (inside, the near side comes closer and
the far side recedes; at the centre it is the recording as it was; outside it
narrows into the sphere's direction and falls off with distance). Drag the
ball to move the sphere and the small cube on its surface to resize it; the
arrow is the recording's front. The *Ambisonic sphere* section sets the
format (ambiX, the ACN/SN3D layout most tools write, or FuMa for first-order
W X Y Z files), the radius, the recording's yaw / pitch / roll, and whether
its W channel feeds the room's reverb (off by default: a recording carries
its own room). *Add Ambisonic from separate files…* builds one such layer
from 4, 9 or 16 mono files, taken in name order (W X Y Z sorts right for
FuMa; 0 1 2 3 … for ambiX). Each layer also has mute
and solo, level, start time and loop, Doppler amount, spread, directivity and
facing, distance rolloff, room send and reflections. Drag a layer in the view
to move it; Shift+drag changes its height. Audio files can also be **dragged
from the Finder** onto the window: mono and stereo files become layers where
they are dropped (in a row when there are several; dropped on the panels,
they go on the ring around the start like *Add audio files…*). Multichannel
files go through *Add audio files…*. Any number of layers can be added;
CPU is the limit (see docs/engine.md for figures; a stereo pair costs two
layers, an Ambisonic sphere about four).

**Path tools** (toolbar, key in brackets). Drawing on an empty scene creates
the path; drawing again appends to the current path's end. The tools draw
the listener's path, or the selected layer's own (see *Layer paths* below;
*for* in the toolbar shows which).

| Tool | Use |
| --- | --- |
| Select (V) | Move layers, path points, Bézier handles and the listener's start. Alt+click on a path inserts a point, Delete removes the selected point or layer. |
| Freehand (F) | Drag to draw; the stroke is simplified and fitted with smooth curves. |
| Point to point (L) | Click points joined by straight lines; double-click or Enter finishes. |
| Curve (C) | Click points for a smooth curve through them; double-click or Enter finishes. |
| Pen (P) | Bézier pen: click for a corner, drag for a smooth point; Enter finishes. |
| Shape (S) | Drag out a circle, ellipse, figure 8, spiral or helix from its centre. |

Paths are drawn on a plane at the height set under **Path & head** (default
ear height, 1.6 m); draw in the Front or Side view to draw vertically. Grid
snap is on the same tab.

**Start**: the green disc on the floor marks where the listener starts (the
active path's first point, or where the listener stands without a path).
Drag the disc, or the listener figure, with the Select tool at any time to
move the start; the active path moves with it and keeps its shape. Shift+drag
moves it up or down.

A newly drawn path starts at the playhead: *Start* on the Path tab is set to
the time the playhead stood at, and the listener waits at the path's first
point until then (the timeline shades the speed lane before the start and
marks it). Draw with the playhead at 0 and the path starts at once.

**Option-click resets**: Option-click any slider, number field, menu or
checkbox to return it to its default (sliders also on double-click).
Option-click a layer's ball or a stereo pair's centre handle to put it back
where it was first placed; an L or R ball to reset the pair's width and angle
(the centre stays); an Ambisonic sphere's cube to reset its radius; the
start disc to put the path back where it was drawn (without a path, the
listener returns to the room centre); a key on the timeline to reset its
value. Option-*drag* on an end of a stereo pair still mirrors it about the
centre: a reset only happens when the pointer does not move.

**Views**: 3D orbit (1), Top (2), Front (3), Side (4), and Ears (5), the
listener's own view while playing. *Home* (H) returns to the default 3D
angle with the scene in frame, *Frame* fits the scene from the current
angle, *Follow* keeps the listener in view.

**Timeline** (bottom): transport buttons for beginning of path (Enter), back
and forward (a click jumps 5 s, hold to scrub at 4x), stop, play/pause
(Space) and end of path; `,` and `.` step the playhead 1 s back or forward,
0.1 s with Shift. Click the ruler to seek. Three
lanes hold keys for speed (m/s), head yaw and head pitch. Double-click a lane
to add a key, drag to move it, Delete to remove it; the bar above the lanes
edits the selected key's value and easing. Speed keys count from the start of
the path (a key at 0 is the speed the listener sets off at, whenever the path
starts); head keys are at scene time. Mouse wheel zooms.

The green **path start** line (when the listener sets off) and the red
**path end** line (when it arrives, as the engine analyses it) drag. Dragging
the start moves the walk in time: the speed curve and the end go with it,
head keys stay. Dragging the end makes the walk faster or slower: the speed
curve is stretched as a whole, so each key keeps its place in the walk and
its speed scales. Cmd-drag over a time range selects the start, end and keys
inside it (the bar above says what); drag the band to move them together,
Delete removes the selected keys, Esc deselects. Moving a band that holds the
start carries the speed curve and the end with it; one with the end but not
the start stretches the curve; speed keys move on their own only when neither
is in the band. In the default
*along path* head mode the yaw and pitch keys turn and tilt the head on top
of the direction of travel; in *keyframed* mode they are absolute; *look at*
keeps the head on a point.

**Layer paths**: the drawing tools draw for whoever is selected. With a
layer selected, picking a tool (toolbar or key) draws that layer's path; with
nothing selected, the listener's. *for* in the toolbar shows which and
changes it (choose *Listener* to draw the listener's path while a layer is
selected), and a hint in the 3D view says whose path is being drawn.
*Draw a path* on the Layers tab does the same with the curve tool (click
points, Enter to finish). The path is drawn at the layer's height and the layer moves to
where it begins; the line drawn is the line it travels. Dragging the layer
moves its path with it; its points edit like the listener's (Option+click
the line adds one, Delete removes one; dragging the first point moves the
layer). *Moves by* picks the timing: *its speed* (a speed curve on the
timeline, setting off at *Sets off at*), *times to be at* (keys on a Path %
lane: where along the path it is at that time) or *a point along the path*
(a slider, and the Path Position parameter in Logic). *At the end* it stops,
starts again or goes back and forth; *Turn* turns it with its direction of
travel (facing, stereo bar, sphere). During playback and scrubbing the 3D
view shows each layer where the engine has it.

With a layer selected the timeline shows that layer's lanes: **Layer speed**
(or **Path %**) with its own green *sets off* and red *arrives* lines, which
drag like the listener's, and **Layer level** for fades (double-click to add
a key; the bottom of the lane is silence). Esc deselects the layer and
brings back the listener's lanes.

**Fit timing** (Path & head tab, shown once a layer has a path): tick the
listener and the layers that should set off and arrive together, set the
times and click *Fit*. Each speed curve is stretched or squeezed as a whole
(the shape of the journey stays); a layer timed by keys has its keys spread
over the same span.

**Room**: a new scene is outdoors (ground only: one reflection off the
ground, no walls, no reverb tail). *Type* on the Room tab switches to a box
room with size and centre, wall materials (absorption per band), air
temperature and humidity; or to no room at all (free field). *Early reflections* has
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

Undo/redo (⌘Z / ⇧⌘Z), open (⌘O), save (⌘S / ⇧⌘S); the same in the File
menu of the menu bar, with **Open Recent** (the last 10 scenes opened or
saved; also *Recent ▾* in the toolbar). Scenes save as `.spscene` files,
which the Finder opens in the app on double-click (the app registers the
type the first time it runs); scenes saved as `.json` before still open with
*Open…*, a drop on the window, or the Finder's *Open With*. Inside, a scene
is the engine's scene JSON with a few editor-only keys (layer colour, solo,
draw height, output, where layers and paths were first placed); audio paths
are stored relative to the scene file. `sp-render` reads either extension.

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
