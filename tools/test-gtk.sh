#!/usr/bin/env bash
# Runs focused GTK lifetime regressions on the build host; no Telegram account needed.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CXX:-g++} -std=c++20 -g -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Igtk-ui tests/gtk-signals.cpp $(pkg-config --cflags --libs gtk4 libadwaita-1 Qt6Core Qt6Gui) \
  -o "$work/gtk-signals"
ASAN_OPTIONS=detect_leaks=0 "$work/gtk-signals"
${CXX:-g++} -std=c++20 -g -fPIC -Igtk-ui tests/qt-window-lifetime.cpp \
  $(pkg-config --cflags --libs Qt6Widgets) -o "$work/qt-window-lifetime"
QT_QPA_PLATFORM=offscreen "$work/qt-window-lifetime" baseline
QT_QPA_PLATFORM=offscreen "$work/qt-window-lifetime"
${CXX:-g++} -std=c++20 -g -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Igtk-ui tests/video-frame.cpp gtk-ui/video_frame.cpp \
  $(pkg-config --cflags --libs Qt6Core Qt6Gui libswscale) -o "$work/video-frame"
ASAN_OPTIONS=detect_leaks=0 "$work/video-frame"
${CXX:-g++} -std=c++20 -g -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Igtk-ui tests/audio-route.cpp gtk-ui/audio_route.cpp \
  $(pkg-config --cflags --libs Qt6Core) -o "$work/audio-route"
ASAN_OPTIONS=detect_leaks=0 "$work/audio-route"
${CXX:-g++} -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Igtk-ui tests/css-split.cpp \
  -o "$work/css-split"
"$work/css-split" gtk-ui/style.css
