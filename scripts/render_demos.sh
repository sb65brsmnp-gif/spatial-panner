#!/usr/bin/env bash
# Builds (if needed), synthesises the test signals and renders the demo scenes.
#   scripts/render_demos.sh [BUILD_DIR] [OUT_DIR]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
OUT="${2:-$ROOT/renders}"

if [ ! -f "$BUILD/build.ninja" ] && [ ! -f "$BUILD/Makefile" ]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD" --parallel

RENDER="$BUILD/tools/render/sp-render"
GEN="$BUILD/tools/gensignals/sp-gensignals"
mkdir -p "$OUT" "$ROOT/signals"
[ -f "$ROOT/signals/voice.wav" ] || "$GEN" "$ROOT/signals" --seconds 32

"$RENDER" "$ROOT/scenes/flyby.json"           -o "$OUT/01_flyby_binaural.wav" --bench --normalize
"$RENDER" "$ROOT/scenes/flyby_nodoppler.json" -o "$OUT/02_flyby_no_doppler_binaural.wav" --normalize
"$RENDER" "$ROOT/scenes/room_walk.json"       -o "$OUT/03_room_walk_binaural.wav" --bench --normalize
"$RENDER" "$ROOT/scenes/room_walk.json"       -o "$OUT/04_room_walk_dry_binaural.wav" --no-reflections --no-reverb --normalize
"$RENDER" "$ROOT/scenes/head_turn.json"       -o "$OUT/05_head_turn_binaural.wav" --normalize
"$RENDER" "$ROOT/scenes/room_walk.json"       -o "$OUT/06_room_walk_7.1.4.wav" --mode speakers --layout 7.1.4 --bench --normalize
"$RENDER" "$ROOT/scenes/room_walk.json"       -o "$OUT/07_room_walk_ambix_o3.wav" --mode ambix --order 3 --normalize
# Ray-traced scenes (Steam Audio); in a build without it they fall back to the box model with a note.
"$RENDER" "$ROOT/scenes/lshape.json"          -o "$OUT/08_lshape_binaural.wav" --bench --normalize
"$RENDER" "$ROOT/scenes/occluder.json"        -o "$OUT/09_occluder_binaural.wav" --bench --normalize
"$RENDER" "$ROOT/scenes/room_walk.json"       -o "$OUT/10_room_walk_traced_binaural.wav" --reflections steam --bench --normalize
"$RENDER" "$ROOT/scenes/occluder.json"        -o "$OUT/11_occluder_no_objects_binaural.wav" --reflections builtin --normalize
echo "renders in $OUT"
