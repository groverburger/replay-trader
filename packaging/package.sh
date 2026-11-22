#!/bin/sh
# Builds the shareable apps into dist/:
#   dist/Replay-Trader-macOS.zip    universal .app (Apple Silicon + Intel), ad-hoc signed
#   dist/Replay-Trader-Windows.zip  ReplayTrader.exe (x64, static), cross-compiled with mingw-w64
# Each zip also holds READ-ME-FIRST.txt and the source (BUI is GPL-3.0, so the app is too and ships with source).
# Needs: cmake, Xcode command line tools, and for Windows `brew install mingw-w64`.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DIST="$ROOT/dist"
rm -rf "$DIST" && mkdir -p "$DIST/stage"

# source bundle (no build trees, data, or sessions)
SRC="$DIST/stage/replay_trader-source"
mkdir -p "$SRC"
cp -R CMakeLists.txt README.md LICENSE src packaging tools docs "$SRC/"
(cd "$DIST/stage" && zip -qr replay_trader-source.zip replay_trader-source)

# macOS
cmake -S . -B build-mac -DRT_DIST=ON >/dev/null
cmake --build build-mac -j >/dev/null
APP="build-mac/Replay Trader.app"
codesign --force --deep -s - "$APP"
M="$DIST/stage/mac"
mkdir -p "$M"
ditto "$APP" "$M/Replay Trader.app"
cp packaging/READ-ME-FIRST.txt "$DIST/stage/replay_trader-source.zip" "$M/"
(cd "$M" && zip -qry "$DIST/Replay-Trader-macOS.zip" .)  # -y keeps the bundle's symlinks

# Windows
if command -v x86_64-w64-mingw32-g++ >/dev/null; then
  cmake -S . -B build-win -DRT_DIST=ON -DCMAKE_TOOLCHAIN_FILE=packaging/mingw-w64-x86_64.cmake >/dev/null
  cmake --build build-win -j >/dev/null
  W="$DIST/stage/win"
  mkdir -p "$W"
  cp build-win/ReplayTrader.exe packaging/READ-ME-FIRST.txt "$DIST/stage/replay_trader-source.zip" "$W/"
  (cd "$W" && zip -qr "$DIST/Replay-Trader-Windows.zip" .)
else
  echo "mingw-w64 not found: skipped the Windows build (brew install mingw-w64)"
fi

rm -rf "$DIST/stage"
ls -la "$DIST"
