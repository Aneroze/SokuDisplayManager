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

# Dear ImGui (the settings menu), vendored in third_party/imgui - see its README for the IMGUI_* defines.
IMGUI="$ROOT/third_party/imgui"

# Embeds the shipped DisplayManager.ini (the template older inis are upgraded from).
"${WINDRES:-i686-w64-mingw32-windres}" -I"$ROOT" "$ROOT/src/DisplayManager.rc" -O coff -o "$OUT/DisplayManager.res.o"

"$CXX" -std=c++17 -O2 -shared \
  -D_CRT_SECURE_NO_WARNINGS -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DNDEBUG \
  -DIMGUI_DISABLE_WIN32_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS \
  -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS \
  -I"$ROOT/src" -I"$IMGUI" \
  "$ROOT/src/DisplayManager.cpp" "$OUT/DisplayManager.res.o" \
  "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp" \
  "$IMGUI/backends/imgui_impl_dx9.cpp" \
  -o "$OUT/DisplayManager.dll" \
  -static -static-libgcc -static-libstdc++ \
  -lshlwapi -luser32 -limm32

echo "built: $OUT/DisplayManager.dll"
