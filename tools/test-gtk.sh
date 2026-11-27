#!/usr/bin/env bash
# Focused regressions of the interface's own code on the build host (GTK lifetimes, layouts, geometry, parsing);
# no Telegram account needed. The tests built with the program are in gtk-ui/CMakeLists.txt (build-td.sh test).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The application ID header the build generates (gtk-ui/CMakeLists.txt).
printf "#pragma once\nnamespace Contrail {\ninline constexpr char kAppId[] = \"io.github.x_vlad_x.Contrail.Test\";\ninline constexpr char kAppName[] = \"Contrail\";\ninline constexpr char kDataName[] = \"contrail-test\";\n}\n" > "$work/contrail_app.h"
san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
${CXX:-g++} -std=c++20 -g -fPIC "${san[@]}" -Igtk-ui -I"$work" tests/gtk-signals.cpp $(pkg-config --cflags --libs gtk4 libadwaita-1) \
  -o "$work/gtk-signals"
ASAN_OPTIONS=detect_leaks=0 "$work/gtk-signals"
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/css-split.cpp -o "$work/css-split"
"$work/css-split" gtk-ui/style.css
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/zoom-geometry.cpp -o "$work/zoom-geometry"
"$work/zoom-geometry"
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/geo.cpp $(pkg-config --cflags --libs glib-2.0) -o "$work/geo"
"$work/geo"
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/media-progress.cpp -o "$work/media-progress"
"$work/media-progress"
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/memory-pressure.cpp -o "$work/memory-pressure"
"$work/memory-pressure"
${CXX:-g++} -std=c++20 -g "${san[@]}" -Igtk-ui tests/album-layout.cpp gtk-ui/album_layout.cpp -o "$work/album-layout"
"$work/album-layout"
echo "All passed"
