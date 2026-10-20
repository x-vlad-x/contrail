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

# --- FFmpeg (release packages) ----------------------------------------------------
# The release packages carry a small FFmpeg instead of Debian's, which brings some 90 MB of libraries for codecs,
# filters and protocols the program never uses. It is Debian 13's own source package (upstream 7.1.5 with
# Debian's changes), the version and library ABI of Debian's libavcodec61 the program is built against, configured
# for what Telegram Desktop does with FFmpeg: playing and sending the usual audio and video formats, recording
# voice messages (Opus in Ogg), changing the speed of audio (atempo), VA-API and VDPAU decoding as Debian's build
# has. Rare formats are still sent and opened as files. CONTRAIL_FFMPEG=1 (the release containers) builds it; the
# development build for the phone uses the phone's own FFmpeg.
FFMPEG_VERSION=7.1.5
FFMPEG_DEBIAN=7:7.1.5-0+deb13u1
FFMPEG_URL=https://snapshot.debian.org/archive/debian-security/20261005T000000Z/pool/updates/main/f/ffmpeg
FFMPEG_FILES=(
  "ffmpeg_7.1.5-0+deb13u1.dsc 9ed2ed34cbe7f056eeebbe9045c5e2d15e41b5b053fe7c8ba6979a0b6fb081ce"
  "ffmpeg_7.1.5.orig.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
  "ffmpeg_7.1.5-0+deb13u1.debian.tar.xz a1be51d8a10744952fe94fa318bf71bbc8074bed0951382c079ab7ef227f74ef"
)
FFMPEG_CONFIG=(
  --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-postproc --disable-network --enable-shared --disable-static --enable-pic
  --toolchain=hardened --extra-version=contrail
  --enable-zlib --enable-libdav1d --enable-libopus --enable-libvpx --enable-vaapi --enable-vdpau
  # Video: phones' and cameras' H.264 and HEVC, the web's VP8, VP9 and AV1 (with dav1d), MPEG-4, GIFs; transparent
  # VP9 video stickers need libvpx's decoder. Cover art of music files is JPEG or PNG.
  --enable-decoder=h264,hevc,vp8,vp9,av1,libdav1d,libvpx_vp8,libvpx_vp9,mpeg4,gif,mjpeg,png,webp
  # Audio: AAC, Opus, MP3, Vorbis, FLAC and PCM.
  --enable-decoder=aac,aac_latm,opus,mp3,mp3float,vorbis,flac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le
  --enable-decoder=pcm_f64le,pcm_u8,pcm_alaw,pcm_mulaw
  --enable-encoder=libopus,pcm_s16le
  --enable-demuxer=mov,matroska,ogg,wav,mp3,aac,flac,gif
  --enable-muxer=opus,ogg,wav
  --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,aac,aac_latm,mpegaudio,opus,vorbis,flac,mjpeg,png,webp
  --enable-filter=abuffer,abuffersink,atempo,aformat,aresample,anull,volume,buffer,buffersink,format,scale,null
  --enable-protocol=file
  --enable-hwaccel=h264_vaapi,hevc_vaapi,vp8_vaapi,vp9_vaapi,av1_vaapi,mpeg4_vaapi
  --enable-hwaccel=h264_vdpau,hevc_vdpau,vp9_vdpau,av1_vdpau,mpeg4_vdpau
)
if [[ ${CONTRAIL_FFMPEG:-0} == 1 ]]; then
  FFMPEG_ID="$FFMPEG_DEBIAN-$(printf '%s\n' "${FFMPEG_CONFIG[@]}" | sha256sum | cut -c1-12)"
  FFMPEG_ID=${FFMPEG_ID//[:+]/_}
  if [[ ! -e $(done_marker ffmpeg "$FFMPEG_ID") ]]; then
    echo "==> FFmpeg $FFMPEG_DEBIAN (Debian's source, Contrail's configuration)"
    W=$DEPS/ffmpeg
    rm -rf "$W"; mkdir -p "$W/download"
    for entry in "${FFMPEG_FILES[@]}"; do
      read -r name sum <<< "$entry"
      curl -fsSL -o "$W/download/$name" "$FFMPEG_URL/${name//+/%2B}"
      echo "$sum  $W/download/$name" | sha256sum -c --quiet
    done
    tar -C "$W" -xJf "$W/download/ffmpeg_$FFMPEG_VERSION.orig.tar.xz"
    mv "$W/ffmpeg-$FFMPEG_VERSION" "$W/source"
    tar -C "$W/source" -xJf "$W/download/ffmpeg_7.1.5-0+deb13u1.debian.tar.xz"
    if [[ -f $W/source/debian/patches/series ]]; then
      while read -r patch _; do
        [[ -z $patch || $patch == \#* ]] && continue
        patch -d "$W/source" -p1 -s < "$W/source/debian/patches/$patch"
      done < "$W/source/debian/patches/series"
    fi
    CROSS_ARGS=(--arch="${TRIPLE%%-*}" --target-os=linux)
    if [[ $CROSS -eq 1 ]]; then
      CROSS_ARGS+=(--enable-cross-compile --cross-prefix="$TRIPLE-" --pkg-config="$TRIPLE-pkg-config")
    fi
    (cd "$W/source" && ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --incdir="$PREFIX/include" \
       "${CROSS_ARGS[@]}" "${FFMPEG_CONFIG[@]}" > "$W/configure.log" 2>&1 || { tail -30 "$W/configure.log"; exit 1; })
    make -C "$W/source" -s -j"$JOBS" >/dev/null
    make -C "$W/source" -s install >/dev/null
    # What the packaging needs to know about these libraries: where their source and copyright come from.
    mkdir -p "$PREFIX/share/contrail"
    cp "$W/source/debian/copyright" "$PREFIX/share/contrail/ffmpeg.copyright"
    python3 - "$PREFIX" "$FFMPEG_DEBIAN" "$W/source/config.h" <<'PYEOF'
import json, re, sys
from pathlib import Path
prefix, version, config = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]).read_text()
configuration = re.search(r'#define FFMPEG_CONFIGURATION "(.*)"', config).group(1)
own = prefix / "share/contrail/own-libraries.json"
data = json.loads(own.read_text()) if own.exists() else {}
for path in sorted((prefix / "lib").glob("lib*.so.*")):
    name = re.match(r"(lib\w+\.so\.\d+)", path.name).group(1)
    if name.startswith(("libav", "libsw")):
        data[name] = dict(source_package="ffmpeg", source_version=version, copyright="ffmpeg.copyright",
                          built_by="build-env/build-deps.sh", configuration=configuration)
own.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PYEOF
    rm -rf "$W" "$DEPS"/.ffmpeg-*
    touch "$(done_marker ffmpeg "$FFMPEG_ID")"
  fi
fi

# --- tg_owt ------------------------------------------------------------------
# With Contrail's fixes (patches/tg_owt/), part of the kept source; changing them rebuilds it.
TG_OWT=$(commit_of desktop-app/tg_owt.git)
TG_OWT_PATCHES=$(cat /src/patches/tg_owt/*.patch 2>/dev/null | sha256sum | cut -c1-12)
if ! built tg_owt "$TG_OWT-$TG_OWT_PATCHES" tg_owt; then
  echo "==> tg_owt $TG_OWT"
  fetch "$DEPS/tg_owt" https://github.com/desktop-app/tg_owt.git "$TG_OWT"
  git -C "$DEPS/tg_owt" submodule -q update --init --recursive --depth=1
  for patch in /src/patches/tg_owt/*.patch; do
    [[ -e $patch ]] && git -C "$DEPS/tg_owt" apply "$patch"
  done
  keep_source tg_owt "$DEPS/tg_owt" https://github.com/desktop-app/tg_owt.git "$TG_OWT"
  cmake -S "$DEPS/tg_owt" -B "$DEPS/tg_owt/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_INSTALL_PREFIX=$PREFIX -DTG_OWT_DLOPEN_PIPEWIRE=ON -DBUILD_SHARED_LIBS=OFF >/dev/null
  ninja -C "$DEPS/tg_owt/build" -j"$JOBS" >/dev/null
  ninja -C "$DEPS/tg_owt/build" install >/dev/null
  rm -rf "$DEPS/tg_owt" "$DEPS"/.tg_owt-*
  touch "$(done_marker tg_owt "$TG_OWT-$TG_OWT_PATCHES")"
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
