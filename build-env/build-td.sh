#!/usr/bin/env bash
# Builds Contrail on TDLib. Needs build-env/build-tdlib.sh first and api-credentials.env.
#   build-env/run.sh build-env/build-td.sh aarch64             for the phone (cross container)
#   podman run --rm --userns=keep-id -v "$PWD":/src:z -w /src localhost/contrail-desktop \
#     build-env/build-td.sh x86_64 [test]                      here, with the tests when "test" is given
set -euo pipefail
cd /src
ARCH=${1:?usage: $0 aarch64|x86_64 [test]}
# shellcheck disable=SC1091
source api-credentials.env
BUILD=${BUILD_DIR:-build-td/$ARCH}  # Another directory lets two builds run side by side.
ARGS=()
if [[ $ARCH == aarch64 ]]; then
  ARGS+=(-DCMAKE_TOOLCHAIN_FILE=/src/build-env/aarch64-toolchain.cmake)
fi
if [[ ${2:-} == test ]]; then
  ARGS+=(-DCONTRAIL_TESTS=ON)
fi
cmake -S gtk-ui -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONTRAIL_TD_DIR=/src/deps-td \
  -DTELEGRAM_API_ID="$TELEGRAM_API_ID" -DTELEGRAM_API_HASH="$TELEGRAM_API_HASH" "${ARGS[@]}" >/dev/null
ninja -C "$BUILD"
# What tools/phone.sh td-install puts on the phone.
if [[ $ARCH == aarch64 ]]; then
  aarch64-linux-gnu-strip -o "$BUILD/contrail.stripped" "$BUILD/contrail"
fi
