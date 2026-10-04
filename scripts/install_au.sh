#!/usr/bin/env bash
# Installs the built Audio Unit for the current user and validates it.
#
#   scripts/install_au.sh [build-dir]      (default: build)
#
# Copies "Spatial Panner.component" to ~/Library/Audio/Plug-Ins/Components,
# signs it ad hoc (enough for a plug-in built on this Mac), makes macOS
# re-scan Audio Units, and runs auval. Logic picks it up on its next start
# (or Logic Pro > Settings > Plug-in Manager > Reset & Rescan Selection).
set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
  echo "Audio Units install on macOS only." >&2
  exit 1
fi

BUILD_DIR="${1:-build}"
SRC="$BUILD_DIR/plugin/SpatialPannerPlugin_artefacts/Release/AU/Spatial Panner.component"
DEST_DIR="$HOME/Library/Audio/Plug-Ins/Components"
DEST="$DEST_DIR/Spatial Panner.component"

if [[ ! -d "$SRC" ]]; then
  echo "Not built yet: $SRC" >&2
  echo "Build first: cmake -S . -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR" >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
rm -rf "$DEST"
ditto "$SRC" "$DEST"
# A copy downloaded from CI carries the quarantine flag; a local build does not.
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
codesign --force --deep --sign - --timestamp=none "$DEST"
lipo -info "$DEST/Contents/MacOS/Spatial Panner" || true

# Make the Audio Unit registry notice the new or replaced component.
killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 2

echo "Validating with auval (type aufx, subtype Spnr, manufacturer SpPn)..."
auval -v aufx Spnr SpPn
echo "Installed: $DEST"
