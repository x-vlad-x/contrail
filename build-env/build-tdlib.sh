#!/usr/bin/env bash
# Builds TDLib at the pinned commit as static libraries.
# Runs inside a build container: tdesktop-cross for aarch64, contrail-desktop for x86_64 (it has the native libraries):
#   build-env/run.sh build-env/build-tdlib.sh aarch64
# The sources go to deps-td/src, the libraries to deps-td/<architecture> (or TDLIB_OUT: the packages build them
# in their own containers, tools/build-packages.sh). Nothing is rebuilt when they are current.
set -euo pipefail
cd /src
ARCH=${1:?usage: $0 aarch64|x86_64}
COMMIT=4c6377a  # TDLib 1.8.69. Updating it is a step of its own: td_api changes between commits.
JOBS=${JOBS:-$(nproc)}
ROOT=/src/deps-td
SRC=$ROOT/src
OUT=${TDLIB_OUT:-$ROOT/$ARCH}
FLAGS="-ffunction-sections -fdata-sections"

if [[ ! -d $SRC/.git ]]; then
  git clone --filter=blob:none https://github.com/tdlib/td.git "$SRC"
fi
if [[ $(git -C "$SRC" rev-parse --short=7 HEAD) != "$COMMIT" ]]; then
  git -C "$SRC" fetch origin
  git -C "$SRC" checkout -q "$COMMIT"
fi

# TDLib generates sources with programs it builds itself: those run on the build machine.
if [[ ! -f $ROOT/host/.done-$COMMIT ]]; then
  cmake -S "$SRC" -B "$ROOT/host" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C "$ROOT/host" -j"$JOBS" prepare_cross_compiling
  touch "$ROOT/host/.done-$COMMIT"
fi

TOOLCHAIN=()
if [[ $ARCH == aarch64 ]]; then
  TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE=/src/build-env/aarch64-toolchain.cmake)
fi
cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel "${TOOLCHAIN[@]}" \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" >/dev/null
ninja -C "$OUT" -j"$JOBS" tdclient memprof
echo "TDLib $COMMIT for $ARCH is in $OUT"
