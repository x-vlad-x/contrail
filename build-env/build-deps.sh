#!/usr/bin/env bash
# Runs inside the build container. Builds the libraries Telegram Desktop needs that the container's Debian
# doesn't package (or packages too old): rnnoise, tg_owt (WebRTC), tde2e (from TDLib), tdesktop_rust
# (tlottie + wallet engine) and, where Debian has no package, ada. The commits are the ones the official
# Linux build uses, taken from tdesktop's Telegram/build/docker/centos_env/Dockerfile. Each library is rebuilt
# only when its commit changes.
#
#   build-deps.sh [aarch64|x86_64]       (default: aarch64, cross-compiled)
#
# The exact sources of every library (with submodules, downloaded data, the Rust lockfile and the vendored
# crates) are kept in deps-<arch>/sources/ as the corresponding source of the binaries they end up in;
# deps-<arch>/sources/manifest.json lists them. The container can choose another directory with CONTRAIL_DEPS.
set -euo pipefail

ARCH=${1:-aarch64}
case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-gnu; RUST_TARGET=aarch64-unknown-linux-gnu; CROSS=1 ;;
  x86_64) TRIPLE=x86_64-linux-gnu; RUST_TARGET=x86_64-unknown-linux-gnu; CROSS=0 ;;
  *) echo "Unknown architecture: $ARCH" >&2; exit 1 ;;
esac
SRC=/src/tdesktop
DOCKERFILE=$SRC/Telegram/build/docker/centos_env/Dockerfile
DEPS=${CONTRAIL_DEPS:-/src/deps-$ARCH}
PREFIX=$DEPS/prefix
SOURCES=$DEPS/sources
TOOLCHAIN=/src/build-env/$ARCH-toolchain.cmake
JOBS=${CONTRAIL_JOBS:-$(nproc)}
mkdir -p "$DEPS" "$PREFIX" "$SOURCES"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$SRC" log -1 --format=%ct)}

# commit_of <repo url fragment>: the commit fetched after "git remote add origin <url>".
commit_of() {
  awk -v repo="$1" '$0 ~ "remote add origin .*" repo {found=1; next}
                    found && /git fetch --depth=1 origin/ {print $NF; exit}' "$DOCKERFILE"
}
# tag_of <repo url fragment>: the tag of "git clone -b <tag> ... <url>".
tag_of() {
  awk -v repo="$1" '$0 ~ "git clone -b .*" repo {for (i = 1; i < NF; i++) if ($i == "-b") print $(i + 1); exit}' "$DOCKERFILE"
}

fetch() {  # fetch <dir> <url> <commit or tag>
  rm -rf "$1"
  git init -q "$1"
  git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth=1 origin "$3"
  git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

# keep_source <name> <dir> [<url> <ref>]: archives the source tree as it is about to be built (without .git),
# records the commits of the tree and its submodules.
keep_source() {
  local name=$1 dir=$2 url=${3:-} ref=${4:-} archive
  archive=$SOURCES/$name.tar.xz
  local commits=""
  if [[ -e $dir/.git ]]; then
    commits=$(git -C "$dir" rev-parse HEAD)
    commits+=$'\n'$(git -C "$dir" submodule status --recursive 2>/dev/null | awk '{sub(/^[-+ U]/, "", $1); print $1, $2}')
  fi
  tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
      --exclude=.git \
      -C "$(dirname "$dir")" -cJf "$archive" "$(basename "$dir")"
  python3 - "$SOURCES/manifest.json" "$name" "$archive" "$url" "$ref" "$commits" <<'EOF'
import hashlib, json, sys
from pathlib import Path
manifest, name, archive, url, ref, commits = sys.argv[1:]
path = Path(manifest)
data = json.loads(path.read_text()) if path.exists() else {}
data[name] = dict(archive=Path(archive).name, url=url, ref=ref,
                  sha256=hashlib.sha256(Path(archive).read_bytes()).hexdigest(),
                  commits=[line for line in commits.splitlines() if line.strip()])
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
EOF
}

done_marker() { echo "$DEPS/.$1-$2"; }
# built <name> <id> <source archive>: built at this commit, with its sources kept (older builds kept none).
built() { [[ -e $(done_marker "$1" "$2") && -e $SOURCES/$3.tar.xz ]]; }

# --- rnnoise -------------------------------------------------------------------
RNNOISE=$(tag_of xiph/rnnoise.git)
if ! built rnnoise "$RNNOISE" rnnoise; then
  echo "==> rnnoise $RNNOISE"
  fetch "$DEPS/rnnoise" https://github.com/xiph/rnnoise.git "refs/tags/$RNNOISE"
  # autogen.sh downloads the neural network model with wget: curl does the same here. The model is part of
  # the kept source.
  mkdir -p "$DEPS/bin"
  printf '#!/bin/sh\nexec curl -fsSL -O "$@"\n' > "$DEPS/bin/wget"
  chmod +x "$DEPS/bin/wget"
  (cd "$DEPS/rnnoise" && PATH="$DEPS/bin:$PATH" ./download_model.sh >/dev/null)
  # v0.2's NEON code (aarch64) includes Opus's os_support.h, which isn't in the repository.
  printf '#ifndef OS_SUPPORT_H\n#define OS_SUPPORT_H\n#include <string.h>\n#include "common.h"\n#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n)*sizeof(*(dst))))\n#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n)*sizeof(*(dst))))\n#endif\n' \
    > "$DEPS/rnnoise/src/os_support.h"
  keep_source rnnoise "$DEPS/rnnoise" https://github.com/xiph/rnnoise.git "$RNNOISE"
  (cd "$DEPS/rnnoise" && autoreconf -isf >/dev/null 2>&1 \
     && ./configure -q --host=$TRIPLE --prefix="$PREFIX" --enable-static --disable-shared \
          --disable-examples --disable-doc \
     && make -s -j"$JOBS" >/dev/null && make -s install >/dev/null)
  rm -rf "$DEPS/rnnoise" "$DEPS"/.rnnoise-*
  touch "$(done_marker rnnoise "$RNNOISE")"
fi

# --- ada (Debian 13 doesn't package it) -----------------------------------------
if ! pkg-config --exists ada 2>/dev/null && [[ ! -e /usr/lib/$TRIPLE/cmake/ada ]]; then
  ADA=$(tag_of ada-url/ada.git)
  if ! built ada "$ADA" ada; then
    echo "==> ada $ADA"
    fetch "$DEPS/ada" https://github.com/ada-url/ada.git "refs/tags/$ADA"
    keep_source ada "$DEPS/ada" https://github.com/ada-url/ada.git "$ADA"
    cmake -S "$DEPS/ada" -B "$DEPS/ada/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
      -DCMAKE_INSTALL_PREFIX=$PREFIX -DADA_TESTING=OFF -DADA_TOOLS=OFF -DADA_INCLUDE_URL_PATTERN=OFF \
      -DBUILD_SHARED_LIBS=OFF >/dev/null
    ninja -C "$DEPS/ada/build" -j"$JOBS" >/dev/null
    ninja -C "$DEPS/ada/build" install >/dev/null
    rm -rf "$DEPS/ada" "$DEPS"/.ada-*
    touch "$(done_marker ada "$ADA")"
  fi
fi

# --- tg_owt ------------------------------------------------------------------
TG_OWT=$(commit_of desktop-app/tg_owt.git)
if ! built tg_owt "$TG_OWT" tg_owt; then
  echo "==> tg_owt $TG_OWT"
  fetch "$DEPS/tg_owt" https://github.com/desktop-app/tg_owt.git "$TG_OWT"
  git -C "$DEPS/tg_owt" submodule -q update --init --recursive --depth=1
  keep_source tg_owt "$DEPS/tg_owt" https://github.com/desktop-app/tg_owt.git "$TG_OWT"
  cmake -S "$DEPS/tg_owt" -B "$DEPS/tg_owt/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_INSTALL_PREFIX=$PREFIX -DTG_OWT_DLOPEN_PIPEWIRE=ON -DBUILD_SHARED_LIBS=OFF >/dev/null
  ninja -C "$DEPS/tg_owt/build" -j"$JOBS" >/dev/null
  ninja -C "$DEPS/tg_owt/build" install >/dev/null
  rm -rf "$DEPS/tg_owt" "$DEPS"/.tg_owt-*
  touch "$(done_marker tg_owt "$TG_OWT")"
fi

# --- tde2e -------------------------------------------------------------------
TDE2E=$(commit_of tdlib/td.git)
if ! built tde2e "$TDE2E" td; then
  echo "==> tde2e $TDE2E"
  fetch "$DEPS/td" https://github.com/tdlib/td.git "$TDE2E"
  keep_source td "$DEPS/td" https://github.com/tdlib/td.git "$TDE2E"
  if [[ $CROSS -eq 1 ]]; then
    # TDLib generates some sources with programs it builds itself: build those for the host first.
    cmake -S "$DEPS/td" -B "$DEPS/td/build-host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTD_E2E_ONLY=ON >/dev/null
    ninja -C "$DEPS/td/build-host" -j"$JOBS" prepare_cross_compiling >/dev/null 2>&1 || true
  fi
  cmake -S "$DEPS/td" -B "$DEPS/td/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_INSTALL_PREFIX=$PREFIX -DTD_E2E_ONLY=ON >/dev/null
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
if ! built rust "$RUST_ID" tdesktop_rust; then
  echo "==> tdesktop_rust (tlottie $TLOTTIE, wallet-engine $WALLET)"
  R=$DEPS/rust
  rm -rf "$R"; mkdir -p "$R"
  fetch "$R/patches" https://github.com/desktop-app/patches.git "$PATCHES"
  fetch "$R/tlottie" https://github.com/dkaraush/tlottie.git "$TLOTTIE"
  fetch "$R/wallet-engine" https://github.com/i582/wallet-engine.git "$WALLET"
  git -C "$R/wallet-engine" apply "$R/patches/wallet-engine.patch"
  # Only the patch this build applies is kept with the sources, not the whole patches repository.
  mkdir -p "$R/wallet-engine-patch" && cp "$R/patches/wallet-engine.patch" "$R/wallet-engine-patch/"
  rm -rf "$R/patches"
  export CARGO_HOME=$DEPS/cargo-home CARGO_BUILD_JOBS=$JOBS
  (cd "$R" && cargo new -q --lib --vcs none tdesktop_rust)
  cd "$R/tdesktop_rust"
  printf 'pub use ::tlottie;\npub use ::wallet_engine;\n' > src/lib.rs
  cargo add -q --path ../tlottie --features c-api
  cargo add -q --path ../wallet-engine
  # Resolve the crates once, then build offline from vendored copies: the archive below holds exactly the
  # crates compiled into the binary (and the C++ bindings generator).
  cargo generate-lockfile -q
  mkdir -p .cargo
  cargo vendor -q --locked --versioned-dirs \
    -s "$R/wallet-engine/bindgen/cpp/Cargo.toml" vendor > .cargo/config.toml
  keep_source tdesktop_rust "$R" "" "tlottie $TLOTTIE, wallet-engine $WALLET, patches $PATCHES"
  # The crates linked into the program (not build tools or other platforms' crates), with their licenses.
  cargo tree -q --locked --offline -e normal --target $RUST_TARGET --prefix none --format '{p} {l}' \
    | sed 's/ (\*)$//' | sort -u > "$SOURCES/rust-crates.txt"
  cargo rustc -q --lib --release --locked --offline --target $RUST_TARGET \
    --crate-type staticlib --crate-type cdylib \
    --config "profile.release.opt-level=3" --config "profile.release.lto='off'" \
    --config "profile.release.codegen-units=1" --config "profile.release.panic='unwind'"
  OUT=target/$RUST_TARGET/release
  mkdir -p "$PREFIX/lib" "$PREFIX/include/tlottie" "$PREFIX/include/wallet_engine"
  $TRIPLE-objcopy --strip-debug $OUT/libtdesktop_rust.a "$PREFIX/lib/libtdesktop_rust.a"
  cp ../tlottie/include/tlottie.h "$PREFIX/include/tlottie/tlottie.h"
  cargo run -q --manifest-path ../wallet-engine/bindgen/cpp/bindgen/Cargo.toml --locked --offline \
    --config "source.crates-io.replace-with='vendored-sources'" \
    --config "source.vendored-sources.directory='$R/tdesktop_rust/vendor'" -- \
    --library --out-dir "$PREFIX/include/wallet_engine" $OUT/libtdesktop_rust.so
  cd /src
  rm -rf "$R" "$DEPS"/.rust-*
  touch "$(done_marker rust "$RUST_ID")"
fi
rustc --version > "$SOURCES/rustc-version.txt"
echo "==> dependencies are up to date"
