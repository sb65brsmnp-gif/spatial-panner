# The Logic plugin

Spatial Panner as an Audio Unit (AUv2, universal binary) for Logic Pro on
macOS 26. You insert it on every track you want in the scene. Each track
renders its own layer for the listener, and Logic's mix sums the tracks. One
of the instances also holds the scene: the room, the layer positions, the
listener's paths, and the listener automation. The listener follows Logic's
playhead, so playback, scrubbing, cycle and bounces all hear the same path.

## Build and install on your Mac

You need:

* Xcode, or the Command Line Tools (`xcode-select --install`).
* CMake 3.22 or later, Ninja and Node.js 20 or later: `brew install cmake ninja node`.
* Network access the first time you configure. JUCE, Steam Audio, the HRTF
  and the other dependencies are downloaded and pinned.

```sh
git clone https://github.com/sb65brsmnp-gif/spatial-panner.git
cd spatial-panner
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                  # first build: about 10 minutes (JUCE, Steam Audio)
ctest --test-dir build               # optional: engine, app and plugin tests
scripts/install_au.sh                # copy, sign, refresh macOS's plug-in list, run auval
```

`install_au.sh` does the following:

* Copies `build/plugin/SpatialPannerPlugin_artefacts/Release/AU/Spatial Panner.component`
  to `~/Library/Audio/Plug-Ins/Components/`.
* Signs it ad hoc. A plug-in you build and run on the same Mac needs no
  Developer ID and no notarization.
* Restarts `AudioComponentRegistrar` so macOS notices the new component.
* Runs `auval -v aufx Spnr SpPn`. The last line must read
  `AU VALIDATION SUCCEEDED`.

The build is universal (arm64 and x86_64) by default. Pass
`-DCMAKE_OSX_ARCHITECTURES=arm64` to build for Apple Silicon only, which
builds faster.

Next, start Logic. The plug-in appears under **Audio Units > Spatial Panner >
Spatial Panner**. If Logic ran while you installed it, open **Logic Pro >
Settings > Plug-in Manager**, select Spatial Panner and click **Reset &
Rescan Selection**.

To update the plug-in later:

```sh
git pull
cmake --build build
scripts/install_au.sh
```

Then quit and reopen Logic, or reopen the project.

The macOS CI job also builds the plug-in, runs auval on it, and uploads it as
the `spatial-panner-au` artifact (a zip of the component). A downloaded copy
carries the quarantine flag. To use it without building, unzip it into
`~/Library/Audio/Plug-Ins/Components/`, then run:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Spatial Panner.component"
killall -9 AudioComponentRegistrar; auval -v aufx Spnr SpPn
```

## Setting up a session

1. **Choose the format.** Insert Spatial Panner on each mono or stereo
   source track as an effect:
   * Pick **Mono → Stereo** or **Stereo → Stereo** for binaural on
     headphones.
   * Pick **Mono → 7.1.4** (or 5.1, 7.1, 5.1.4 and so on) on a track whose
     output is a surround bus or the Atmos bed.

   A mono track is one source. A stereo track plays its layer as a **stereo
   pair**: left and right from the two ends of a bar in the scene, which the
   editor's Stereo field section (or dragging the ends) places, widens and
   turns; *Mono* sums the two at the centre. The Spread parameter widens any
   source.
2. **Roles.** The first instance becomes the **scene** track. Every later
   instance becomes a **layer**, and appears in the scene at a free spot
   around the listener. The header at the top of the plug-in window shows
   the role and lets you change it:
   * **Layer**: this track is a sound in the scene.
   * **Scene**: this track holds the scene and is also a layer.
   * **Scene only**: this track holds the scene and passes its own audio
     through. Use this on an empty or aux track if you want the listener
     controls on a track of their own.

   One scene per session. Choosing Scene on another track takes the scene
   over: the document moves with it, and the old scene track becomes a
   layer.
3. **Edit the scene.** Open the plug-in window on the scene track. The editor
   is the same one the standalone app has (see [editor.md](editor.md)):
   * Drag layers.
   * Draw paths with the freehand, point-to-point, curve, pen and shape tools.
   * Edit the speed curve and head keys on the timeline.
   * Change the room.

   The differences in the plug-in:
   * Each track is a layer. The layer is named after the track, and the
     **Track** menu on the Layers tab shows which track plays which layer.
     Duplicating a track in Logic puts the copy at the same position.
   * There is no transport. The editor's playhead follows Logic's, and you
     play, stop, scrub and cycle in Logic.
   * **Import…** and **Export…** read and write the same `.json` scene files
     as the app. On import, layers whose names match track names are bound
     to those tracks.
   * The scene is saved inside the Logic project, with the scene track's
     state.

   On layer tracks, the window shows a small map of the scene with that
   track's layer highlighted.
4. **Output.** Binaural or speakers is decided by each track's output format:
   * A stereo track renders binaural. The header's output menu can switch it
     to plain stereo speakers instead.
   * A surround track renders its own layout: quad, 5.0/5.1, 6.x, 7.x, and
     the height formats up to 7.1.4, 7.1.6, 9.1.4 and 9.1.6.
   * The LFE channel stays silent.
5. **Bounce** with **File > Bounce**. Every track renders its own layer
   offline, and the result matches playback.

## Automation

The plug-in's parameters appear in Logic's automation lanes under two groups.

**Listener (scene track):** these only have an effect on the scene track.

| Parameter | Range | Effect |
|---|---|---|
| Listener Speed | 0 to 4 ×, default 1 | Multiplies the scene's speed curve. 0 stops the listener where it is. |
| Listener Path Position | 0 to 100 % | Where the listener is along the path, when the scene's movement is set to "position along path" (Path & head tab). Draw this lane to move the listener directly. |
| Listener Head Turn | ±180° | Added to the head direction from the scene. Positive turns left. |
| Listener Head Tilt | ±90° | Positive looks up. |
| Listener Head Roll | ±90° | |
| Listener Path | Scene setting, Path 1 to 8 | Which drawn path the listener follows. |

**Layer (this track):** every track has its own set.

| Parameter | Range | Effect |
|---|---|---|
| Layer Level | −60 to +12 dB | Gain before spatialisation. The bottom of the range mutes the layer. |
| Layer Mute | on, off | |
| Layer Doppler | 0 to 100 % | Scales the layer's Doppler setting. |
| Layer Spread | 0 to 180° | Added to the layer's spread (apparent source size). |
| Layer Offset X / Y / Z | ±20 m | Moves the layer from its place in the scene. |
| Layer Stereo Width | 0 to 400 %, default 100 | Stereo tracks: multiplies the pair's width in the scene. 0 brings both ends to the centre. |
| Layer Stereo Rotation | ±180° | Stereo tracks: added to the pair's rotation. |
| Layer Mono | on, off | Stereo tracks: on sums left and right at the centre. Off leaves the scene's Mono setting. |

Automation is evaluated every 32 samples (0.7 ms at 48 kHz), and the
plug-in reports a latency of 32 samples, which Logic compensates.

### How listener automation reaches the other tracks

Logic gives each plug-in instance only its own automation. The layer tracks
cannot see the scene track's lanes. To give them the same listener
movement, the scene track records the listener values it plays into a
timeline at 10 ms resolution, and every track evaluates the listener from
that timeline at its own playhead position. This gives three consequences:

* **Changing the speed automation:** the listener's position at any time
  integrates the speed over everything before it. The integration uses the
  speed values the scene track has played, and falls back to the current
  value where nothing has been played yet. After you change the speed
  lane, play the section from its start once, or bounce, and every later
  playback is exact. The other listener lanes (position, head, path) affect
  only the moment they are at, so they need no replay.
* **Clear recorded automation**, in the scene track's header, forgets the
  recorded values. Use it after deleting speed automation you no longer want
  heard.
* The recording is saved in the project with the scene track, so a project
  reopens and plays as it did.

Without any speed automation (the multiplier stays at 1), the listener
follows the scene's speed curve exactly, from any start point, with nothing
to replay.

## What each track does

Each layer instance builds a renderer for its one layer from the shared
scene. It reads Logic's playhead each block, evaluates the listener pose at
that time, and renders its track's audio for that pose: delay with Doppler,
distance, air absorption, HRTF or speaker panning, reflections and reverb.

No audio passes between tracks. Because the pose is a function of the
timeline time, tracks agree on it whatever order Logic processes them in.
Scene edits reach the other tracks within about 0.1 s. Moving a layer or
editing a path updates live with a 40 ms glide. Changing the room or the
output format builds a new renderer and crossfades to it over 40 ms.

### Rooms with Steam Audio

The engine branch added a second reflection back-end: Steam Audio ray tracing
for mesh rooms and for scenes with objects (occluders). It is selected
automatically when the scene has a mesh or objects, and box rooms keep the
built-in image-source model.

In the plug-in, the ray tracing runs on a worker thread in each track's
instance (one thread per track), never on Logic's audio thread. The audio
thread only convolves. During a bounce the simulation instead runs in line
with the audio, so every block uses up-to-date reflections and the bounce is
repeatable.

Cost per track at the engine's defaults (measured on Linux; see
[engine.md](engine.md#performance-measured-on-one-28-ghz-xeon-core-linux-gcc-13--o3)):
* Ray tracing: about half a core, on its own thread.
* Convolution: about 6 % of a core, on the audio thread.

With many tracks in a mesh room, the ray tracing adds up. Box rooms with the
built-in model don't have this cost.

## Limits and deviations from the spec

* **Sharing between instances** uses a memory-mapped file,
  `~/Library/Application Support/Spatial Panner/live-session.bin`. The spec
  proposed an in-process registry with a socket fallback. The mapped file
  covers both cases, because Logic can host plug-ins in a separate process
  (AUHostingService). If the file cannot be created, the header shows
  "(this process only)" and only instances in the same process share the
  scene.
* **One Logic project at a time.** Two projects open at once would share the
  file. Logic opens one project at a time, so this does not come up there.
* **Not automatable:** room size and reverb level. The spec listed them, but
  a room change rebuilds the renderer, which takes too long for automation.
  Edit them in the Room tab.
* **Output mode** is chosen by the track's format, not by an automatable
  parameter.
* **Tempo:** paths follow time in seconds, not bars and beats. If you change
  the project tempo, the listener keeps its timing in seconds.
* **Memory:** each binaural track loads its own copy of the HRTF, about 18 MB
  and 0.4 s on insert. Sharing the HRTF between instances needs an engine
  change.
* **Mesh rooms** refer to their `.obj` file by its full path in the
  session. If you move the project to another Mac, export the scene with
  the OBJ next to it and import it there.
* **Layer without a scene track:** a layer instance stores a copy of the
  scene with its own state. If you remove the scene track, the layers keep
  playing that copy, and their header says so.
* **Logic's Atmos object panner** is not used. On the Atmos bed the plug-in
  renders bed channels (up to 7.1.4). Steve asked for routing to multichannel
  outputs, and the bed is that.
