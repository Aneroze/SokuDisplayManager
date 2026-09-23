#!/bin/sh
# Build DisplayManager.dll (32-bit) on Linux (or any host) by cross-compiling with mingw-w64.
# No Visual Studio or Windows SDK needed - the Windows headers/libs come from mingw-w64.
#
# Install the toolchain first, e.g.:
#   Debian/Ubuntu : sudo apt install g++-mingw-w64-i686
#   Arch          : sudo pacman -S mingw-w64-gcc
#   Fedora        : sudo dnf install mingw32-gcc-c++
set -e

CXX="${CXX:-i686-w64-mingw32-g++}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/build"
mkdir -p "$OUT"

"$CXX" -std=c++17 -O2 -shared \
  -D_CRT_SECURE_NO_WARNINGS -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
  -I"$ROOT/src" \
  "$ROOT/src/DisplayManager.cpp" \
  -o "$OUT/DisplayManager.dll" \
  -static -static-libgcc -static-libstdc++ \
  -lshlwapi -luser32

echo "built: $OUT/DisplayManager.dll"
