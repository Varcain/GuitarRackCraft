#!/usr/bin/env bash
# Build the desktop (x86-64 Linux) test/dev harness for the Android in-process
# X11 server. Renders a wine VST plugin's UI into an SDL2 window via the same
# X11NativeDisplay code that runs in-process on Android, and forwards mouse.
#
# Stub headers in stubinc/ must win over any real Android NDK / EGL headers,
# so -I"$XTEST/stubinc" comes FIRST. SDL2 is the only real external dep.
set -euo pipefail

XTEST=/home/varcain/projects/private/GuitarRackCraft/vsthost_lib/tools/xtest
X11SRC=/home/varcain/projects/private/GuitarRackCraft/vsthost_lib/src/main/cpp/x11

g++ -std=c++17 -O0 -g -pthread \
    -I"$XTEST/stubinc" \
    $(pkg-config --cflags sdl2) \
    -I"$X11SRC" \
    "$XTEST/main.cpp" \
    "$XTEST/stubs.cpp" \
    "$X11SRC/X11NativeDisplay.cpp" \
    "$X11SRC/X11ConnectionHandler.cpp" \
    "$X11SRC/X11Framebuffer.cpp" \
    "$X11SRC/X11PixmapStore.cpp" \
    "$X11SRC/X11WindowManager.cpp" \
    "$X11SRC/X11AtomStore.cpp" \
    "$X11SRC/X11PropertyStore.cpp" \
    "$X11SRC/X11Worker.cpp" \
    $(pkg-config --libs sdl2) \
    -o "$XTEST/xtest"

echo "Built $XTEST/xtest"
