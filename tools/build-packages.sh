#!/usr/bin/env bash
# Builds Contrail's installable packages locally, for FuriOS on the FuriPhone FLX1s:
#
#   tools/build-packages.sh [aarch64]  ARM64, cross-compiled ("all" means the same)
#
# The program is built once, against Debian 13, and packaged as a DEB and a Flatpak (tools/package.py;
# CONTRAIL_FORMATS=deb or CONTRAIL_FORMATS=flatpak picks one). The packages land in out/packages/.
# Nothing is installed, copied to a phone, pushed or published. There are no x86_64 packages: the x86_64 build
# for development and tests is build-env/build-td.sh.
# CONTRAIL_FLAVOUR=devel (default) makes the test version on TDLib, which installs next to the stable Contrail
# ("Contrail TDLib", io.github.x_vlad_x.Contrail.Devel, the package contrail-td); CONTRAIL_FLAVOUR=stable makes it
# under Contrail's own ID and names.
# TDLib is pinned in build-env/build-tdlib.sh; its sources are in deps-td/src (cloned there when missing; TD_DIR
# names another deps-td, for example the main checkout's from a git worktree). It is built in the package
# container, into deps-td-pkg/.
#
# Calls carry sound and video, as in 0.3.1: tgcalls is compiled from Telegram Desktop's sources (tdesktop/, the
# release tag) against WebRTC (tg_owt) and rnnoise, which build-env/build-deps.sh builds in the same Debian 13
# container, with the small FFmpeg of the packages, into deps-aarch64-trixie (deps-aarch64 is the phone's FuriOS
# build and doesn't fit these packages). They are built there when missing; DEPS_ROOT names the checkout that has
# them (by default this one, or the main checkout of a git worktree). CONTRAIL_CALLS=OFF builds without sound and
# video in calls.
#
# Telegram API credentials: TELEGRAM_API_ID and TELEGRAM_API_HASH from the environment or from
# api-credentials.env (see README.md). CONTRAIL_TEST_API=1 builds with Telegram Desktop's public credentials
# instead: such packages are for checking the packaging only and can't be released.
#
# Needs git, python3 and podman (or docker). CONTRAIL_JOBS limits parallel compile jobs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TARGET="${1:-aarch64}"
case "$TARGET" in aarch64|all) ;; *) sed -n '2,28p' "$0" >&2; exit 2 ;; esac

FLAVOUR="${CONTRAIL_FLAVOUR:-devel}"
case "$FLAVOUR" in
  devel) APP_ID=io.github.x_vlad_x.Contrail.Devel APP_NAME="Contrail TDLib" DATA_NAME=contrail-td/app COMMAND=contrail-td ;;
  # The account of the version on TDLib lives beside 0.3's Telegram Desktop data (contrail-data/tdata), which
  # stays for going back.
  stable) APP_ID=io.github.x_vlad_x.Contrail APP_NAME=Contrail DATA_NAME=contrail-data/td COMMAND=contrail ;;
  *) echo "CONTRAIL_FLAVOUR is devel or stable" >&2; exit 2 ;;
esac
TDLIB_COMMIT=$(sed -n 's/^COMMIT=\([0-9a-f]*\).*/\1/p' build-env/build-tdlib.sh)
TD_DIR="$(realpath -m "${TD_DIR:-$ROOT/deps-td}")"
JOBS="${CONTRAIL_JOBS:-$(nproc)}"

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

for tool in git python3; do
  command -v "$tool" >/dev/null || die "required tool is missing: $tool"
done
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
[[ -n "$ENGINE" ]] || die "podman or docker is required"
ENGINE_NAME=$(basename "$ENGINE")
if [[ $ENGINE_NAME == podman ]]; then USER_ARGS=(--userns=keep-id); else USER_ARGS=(--user "$(id -u):$(id -g)"); fi
export CONTAINER_ENGINE="$ENGINE"
# deps-td from elsewhere is mounted in its place.
TD_MOUNT=()
if [[ $TD_DIR != "$ROOT/deps-td" ]]; then
  [[ -d $TD_DIR ]] || die "TD_DIR is not a directory: $TD_DIR"
  TD_MOUNT=(-v "$TD_DIR:/src/deps-td:z")
  export CONTRAIL_EXTRA_MOUNTS="$TD_DIR:/src/deps-td"
fi
mkdir -p deps-td

# WebRTC and the small FFmpeg (build-deps.sh), and tgcalls' sources in tdesktop/: mounted where the containers look
# for them. A worktree's tdesktop is a link to the main checkout's, mounted at the path the link names.
CALLS="${CONTRAIL_CALLS:-ON}"
# (The containers leave empty mount points behind in this checkout: only a finished prefix counts.)
DEPS_ROOT="$(realpath -m "${DEPS_ROOT:-$(ls "$ROOT"/deps-aarch64-trixie/prefix/lib/libtg_owt.a >/dev/null 2>&1 &&
  echo "$ROOT" || dirname "$(git rev-parse --path-format=absolute --git-common-dir)")}")"
TDESKTOP="$(realpath -m "$ROOT/tdesktop")"
EXTRA_MOUNTS=()
add_mount() {  # add_mount <host> <container>
  EXTRA_MOUNTS+=(-v "$1:$2:z")
  CONTRAIL_EXTRA_MOUNTS="${CONTRAIL_EXTRA_MOUNTS:+$CONTRAIL_EXTRA_MOUNTS;}$1:$2"
}
TGCALLS=/src/tdesktop/Telegram/ThirdParty/tgcalls
if [[ $TDESKTOP != "$ROOT/tdesktop" ]]; then
  add_mount "$TDESKTOP" "$TDESKTOP"
  TGCALLS=$TDESKTOP/Telegram/ThirdParty/tgcalls
fi
[[ -f $TDESKTOP/Telegram/ThirdParty/tgcalls/tgcalls/Instance.cpp || $CALLS == OFF ]] ||
  die "tdesktop/ has no tgcalls: check out Telegram Desktop there (git submodule update --init --recursive)"
DEPS=deps-aarch64-trixie
mkdir -p "$DEPS_ROOT/$DEPS"
[[ $DEPS_ROOT != "$ROOT" ]] && add_mount "$DEPS_ROOT/$DEPS" "/src/$DEPS"
export CONTRAIL_EXTRA_MOUNTS DEPS_ROOT

# in_container <image> <network: net|none> <command...>
in_container() {
  local image=$1 network=$2; shift 2
  local args=(--rm "${USER_ARGS[@]}" -e HOME=/tmp -e JOBS="$JOBS" -v "$ROOT:/src:z" "${TD_MOUNT[@]}" "${EXTRA_MOUNTS[@]}" -w /src)
  [[ $network == none ]] && args+=(--network=none)
  "$ENGINE" run "${args[@]}" "$image" "$@"
}

# --- API credentials (never printed) -------------------------------------------
if [[ -f api-credentials.env ]]; then
  # shellcheck disable=SC1091
  source api-credentials.env
fi
if [[ -n "${TELEGRAM_API_ID:-}" && -n "${TELEGRAM_API_HASH:-}" ]]; then
  [[ $TELEGRAM_API_ID =~ ^[0-9]+$ && $TELEGRAM_API_HASH =~ ^[0-9a-f]{32}$ ]] || die "malformed Telegram API credentials"
  API_KIND=own
elif [[ "${CONTRAIL_TEST_API:-}" == 1 ]]; then
  echo "Using Telegram Desktop's public API credentials: these packages must not be released."
  API_KIND=test
  TELEGRAM_API_ID=17349 TELEGRAM_API_HASH=344583e45741c457fe1862106095a5eb
else
  die "no Telegram API credentials: put TELEGRAM_API_ID and TELEGRAM_API_HASH into api-credentials.env"
fi

# --- build -------------------------------------------------------------------------
# build_target <arch> <image> <containerfile>
build_target() {
  local arch=$1 image=$2 recipe=$3 build=build-td-pkg/$1 td=deps-td-pkg/$1 deps=$DEPS
  # The container is rebuilt only when its recipe changes: its packages come from repositories that move on.
  local recipe_sum label
  recipe_sum=$(sha256sum "$recipe" | cut -d' ' -f1)
  label=$("$ENGINE" image inspect --format '{{ index .Config.Labels "contrail.recipe" }}' "$image" 2>/dev/null || true)
  if [[ $label != "$recipe_sum" ]]; then
    log "Build container $image"
    "$ENGINE" build --label "contrail.recipe=$recipe_sum" -t "$image" -f "$recipe" build-env
  fi
  # Each library is rebuilt only when its commit changes; a finished prefix isn't touched at all.
  if [[ $CALLS != OFF && ! ( -f $DEPS_ROOT/$deps/prefix/lib/libtg_owt.a &&
        -f $DEPS_ROOT/$deps/prefix/share/contrail/own-libraries.json ) ]]; then
    log "WebRTC, rnnoise and FFmpeg for calls ($arch)"
    in_container "$image" net build-env/build-deps.sh "$arch"
  fi
  log "TDLib $TDLIB_COMMIT ($arch)"
  in_container "$image" net env TDLIB_OUT="/src/$td" build-env/build-tdlib.sh "$arch"
  log "Contrail ($arch, $FLAVOUR)"
  in_container "$image" none cmake -S gtk-ui -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/src/build-env/aarch64-toolchain.cmake \
    -DCONTRAIL_TD_DIR=/src/deps-td -DCONTRAIL_TD_BUILD="/src/$td" -DCONTRAIL_VERSION_LIMITS=OFF \
    -DTELEGRAM_API_ID="$TELEGRAM_API_ID" -DTELEGRAM_API_HASH="$TELEGRAM_API_HASH" \
    -DCONTRAIL_APP_ID="$APP_ID" -DCONTRAIL_APP_NAME="$APP_NAME" -DCONTRAIL_DATA_NAME="$DATA_NAME" \
    -DCONTRAIL_COMMAND="$COMMAND" -DCONTRAIL_BUNDLE_RPATH=ON -DCONTRAIL_TESTS=OFF -DCONTRAIL_CALLS:BOOL="$CALLS" \
    -DCONTRAIL_CALL_DEPS="/src/$deps/prefix" -DCONTRAIL_TGCALLS_DIR="$TGCALLS" >/dev/null
  in_container "$image" none ninja -C "$build" -j"$JOBS" contrail
  log "Packages ($arch)"
  python3 tools/package.py --arch "$arch" --container "$image" --api "$API_KIND" --tdlib-commit "$TDLIB_COMMIT" \
    --flavour "$FLAVOUR" ${CONTRAIL_FORMATS:+--formats "$CONTRAIL_FORMATS"}
}

build_target aarch64 localhost/contrail-arm64 build-env/Containerfile.arm64
