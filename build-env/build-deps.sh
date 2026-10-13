#!/usr/bin/env bash
# Runs inside the build container. Cross-compiles for aarch64 the libraries Telegram Desktop needs
# that Debian doesn't package (or packages too old): rnnoise, tg_owt (WebRTC), tde2e (from TDLib) and
# tdesktop_rust (tlottie + wallet engine). The commits are the ones the official Linux build uses,
# taken from tdesktop's Telegram/build/docker/centos_env/Dockerfile. Each library is rebuilt only
# when its commit changes.
set -euo pipefail

SRC=/src/tdesktop
DOCKERFILE=$SRC/Telegram/build/docker/centos_env/Dockerfile
DEPS=/src/deps-aarch64
PREFIX=$DEPS/prefix
TOOLCHAIN=/src/build-env/aarch64-toolchain.cmake
JOBS=$(nproc)
mkdir -p "$DEPS" "$PREFIX"

# commit_of <repo url fragment>: the commit fetched after "git remote add origin <url>".
commit_of() {
  awk -v repo="$1" '$0 ~ "remote add origin .*" repo {found=1; next}
                    found && /git fetch --depth=1 origin/ {print $NF; exit}' "$DOCKERFILE"
}

fetch() {  # fetch <dir> <url> <commit>
  rm -rf "$1"
  git init -q "$1"
  git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth=1 origin "$3"
  git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

done_marker() { echo "$DEPS/.$1-$2"; }

# --- rnnoise (not in FuriOS's Debian snapshot) ---------------------------------
RNNOISE=$(awk '/AS rnnoise/ {found=1} found && /git clone -b/ {print $4; exit}' "$DOCKERFILE")
if [[ ! -e $(done_marker rnnoise "$RNNOISE") ]]; then
  echo "==> rnnoise $RNNOISE"
  rm -rf "$DEPS/rnnoise"
  git clone -q -c advice.detachedHead=false -b "$RNNOISE" --depth=1 https://github.com/xiph/rnnoise.git "$DEPS/rnnoise"
  # autogen.sh downloads the neural network model with wget: curl does the same here.
  mkdir -p "$DEPS/bin"
  printf '#!/bin/sh\nexec curl -fsSL -O "$@"\n' > "$DEPS/bin/wget"
  chmod +x "$DEPS/bin/wget"
  # v0.2's NEON code (aarch64) includes Opus's os_support.h, which isn't in the repository.
  printf '#ifndef OS_SUPPORT_H\n#define OS_SUPPORT_H\n#include <string.h>\n#include "common.h"\n#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n)*sizeof(*(dst))))\n#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n)*sizeof(*(dst))))\n#endif\n' \
    > "$DEPS/rnnoise/src/os_support.h"
  (cd "$DEPS/rnnoise" && PATH="$DEPS/bin:$PATH" ./autogen.sh >/dev/null 2>&1 \
     && ./configure -q --host=aarch64-linux-gnu --prefix="$PREFIX" --enable-static --disable-shared \
          --disable-examples --disable-doc \
     && make -s -j"$JOBS" >/dev/null && make -s install >/dev/null)
  rm -rf "$DEPS/rnnoise" "$DEPS"/.rnnoise-*
  touch "$(done_marker rnnoise "$RNNOISE")"
fi

# --- tg_owt ------------------------------------------------------------------
TG_OWT=$(commit_of desktop-app/tg_owt.git)
if [[ ! -e $(done_marker tg_owt "$TG_OWT") ]]; then
  echo "==> tg_owt $TG_OWT"
  fetch "$DEPS/tg_owt" https://github.com/desktop-app/tg_owt.git "$TG_OWT"
  git -C "$DEPS/tg_owt" submodule -q update --init --recursive --depth=1
  cmake -S "$DEPS/tg_owt" -B "$DEPS/tg_owt/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DCMAKE_INSTALL_PREFIX=$PREFIX -DTG_OWT_DLOPEN_PIPEWIRE=ON \
    -DBUILD_SHARED_LIBS=OFF >/dev/null
  ninja -C "$DEPS/tg_owt/build" -j"$JOBS" >/dev/null
  ninja -C "$DEPS/tg_owt/build" install >/dev/null
  rm -rf "$DEPS/tg_owt" "$DEPS"/.tg_owt-*
  touch "$(done_marker tg_owt "$TG_OWT")"
fi

# --- tde2e -------------------------------------------------------------------
TDE2E=$(commit_of tdlib/td.git)
if [[ ! -e $(done_marker tde2e "$TDE2E") ]]; then
  echo "==> tde2e $TDE2E"
  fetch "$DEPS/td" https://github.com/tdlib/td.git "$TDE2E"
  # TDLib generates some sources with programs it builds itself: build those for the host first.
  cmake -S "$DEPS/td" -B "$DEPS/td/build-host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTD_E2E_ONLY=ON >/dev/null
  ninja -C "$DEPS/td/build-host" -j"$JOBS" prepare_cross_compiling >/dev/null 2>&1 || true
  cmake -S "$DEPS/td" -B "$DEPS/td/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DCMAKE_INSTALL_PREFIX=$PREFIX -DTD_E2E_ONLY=ON >/dev/null
  ninja -C "$DEPS/td/build" -j"$JOBS" >/dev/null
  ninja -C "$DEPS/td/build" install >/dev/null
  rm -rf "$DEPS/td" "$DEPS"/.tde2e-*
  touch "$(done_marker tde2e "$TDE2E")"
fi

# --- tdesktop_rust (tlottie + wallet engine), one static library like the official build ----
PATCHES=$(commit_of desktop-app/patches.git)
TLOTTIE=$(commit_of dkaraush/tlottie.git)
WALLET=$(commit_of i582/wallet-engine.git)
RUST_ID="$TLOTTIE-$WALLET-$PATCHES"
if [[ ! -e $(done_marker rust "$RUST_ID") ]]; then
  echo "==> tdesktop_rust (tlottie $TLOTTIE, wallet-engine $WALLET)"
  R=$DEPS/rust
  rm -rf "$R"; mkdir -p "$R"
  fetch "$R/patches" https://github.com/desktop-app/patches.git "$PATCHES"
  fetch "$R/tlottie" https://github.com/dkaraush/tlottie.git "$TLOTTIE"
  fetch "$R/wallet-engine" https://github.com/i582/wallet-engine.git "$WALLET"
  git -C "$R/wallet-engine" apply "$R/patches/wallet-engine.patch"
  export CARGO_HOME=$DEPS/cargo-home CARGO_BUILD_JOBS=$JOBS
  (cd "$R" && cargo new -q --lib --vcs none tdesktop_rust)
  cd "$R/tdesktop_rust"
  printf 'pub use ::tlottie;\npub use ::wallet_engine;\n' > src/lib.rs
  cargo add -q --path ../tlottie --features c-api
  cargo add -q --path ../wallet-engine
  cargo rustc -q --lib --release --target aarch64-unknown-linux-gnu \
    --crate-type staticlib --crate-type cdylib \
    --config "profile.release.opt-level=3" --config "profile.release.lto='off'" \
    --config "profile.release.codegen-units=1" --config "profile.release.panic='unwind'"
  OUT=target/aarch64-unknown-linux-gnu/release
  mkdir -p "$PREFIX/lib" "$PREFIX/include/tlottie" "$PREFIX/include/wallet_engine"
  aarch64-linux-gnu-objcopy --strip-debug $OUT/libtdesktop_rust.a "$PREFIX/lib/libtdesktop_rust.a"
  cp ../tlottie/include/tlottie.h "$PREFIX/include/tlottie/tlottie.h"
  cargo run -q --manifest-path ../wallet-engine/bindgen/cpp/bindgen/Cargo.toml --locked -- \
    --library --out-dir "$PREFIX/include/wallet_engine" $OUT/libtdesktop_rust.so
  cd /src
  rm -rf "$R" "$DEPS"/.rust-*
  touch "$(done_marker rust "$RUST_ID")"
fi
echo "==> dependencies are up to date"
