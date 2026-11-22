#!/bin/sh
# Builds the shareable apps into dist/:
#   dist/Replay-Trader-macOS.zip    universal .app (Apple Silicon + Intel), ad-hoc signed
#   dist/Replay-Trader-Windows.zip  ReplayTrader.exe (x64, static), cross-compiled with mingw-w64
# Each zip also holds READ-ME-FIRST.txt, LICENSE.txt and THIRD_PARTY_NOTICES.txt (the font license must ship with it).
# Needs: cmake, Xcode command line tools, and for Windows `brew install mingw-w64`.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DIST="$ROOT/dist"
rm -rf "$DIST" && mkdir -p "$DIST/stage"

# license files that ship in every zip
NOTICES="$DIST/stage/notices"
mkdir -p "$NOTICES"
cp LICENSE "$NOTICES/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$NOTICES/THIRD_PARTY_NOTICES.txt"

# macOS
cmake -S . -B build-mac -DRT_DIST=ON >/dev/null
cmake --build build-mac -j >/dev/null
APP="build-mac/Replay Trader.app"
codesign --force --deep -s - "$APP"
M="$DIST/stage/mac"
mkdir -p "$M"
ditto "$APP" "$M/Replay Trader.app"
cp packaging/READ-ME-FIRST.txt "$NOTICES"/*.txt "$M/"
(cd "$M" && zip -qry "$DIST/Replay-Trader-macOS.zip" .)  # -y keeps the bundle's symlinks

# Windows
if command -v x86_64-w64-mingw32-g++ >/dev/null; then
  cmake -S . -B build-win -DRT_DIST=ON -DCMAKE_TOOLCHAIN_FILE=packaging/mingw-w64-x86_64.cmake >/dev/null
  cmake --build build-win -j >/dev/null
  W="$DIST/stage/win"
  mkdir -p "$W"
  cp build-win/ReplayTrader.exe packaging/READ-ME-FIRST.txt "$NOTICES"/*.txt "$W/"
  (cd "$W" && zip -qr "$DIST/Replay-Trader-Windows.zip" .)
else
  echo "mingw-w64 not found: skipped the Windows build (brew install mingw-w64)"
fi

rm -rf "$DIST/stage"
ls -la "$DIST"
