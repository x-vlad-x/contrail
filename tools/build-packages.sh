#!/usr/bin/env bash
# Builds Contrail's installable packages locally:
#
#   tools/build-packages.sh phone      ARM64 .deb for Linux phones (FuriOS / Debian testing)
#   tools/build-packages.sh desktop    x86_64 AppImage for Linux desktops (glibc 2.39+)
#   tools/build-packages.sh all        both (default)
#
# The packages land in out/packages/. Nothing is installed, copied to a phone, pushed or published.
# The Telegram Desktop core is pinned below; an existing tdesktop/ checkout is used only if it is at that
# commit and either pristine or carrying exactly the patches in patches/ (it is never reset).
#
# Telegram API credentials: TELEGRAM_API_ID and TELEGRAM_API_HASH from the environment or from
# api-credentials.env (see README.md). CONTRAIL_TEST_API=1 builds with Telegram Desktop's public test
# credentials instead: such packages are for checking the packaging only and can't be released.
#
# Needs git, patch, python3 and podman (or docker). CONTRAIL_JOBS limits parallel compile jobs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TARGET="${1:-all}"
case "$TARGET" in phone|desktop|all) ;; *) sed -n '2,16p' "$0" >&2; exit 2 ;; esac

CORE_TAG=v7.3.1
CORE_COMMIT=6fed91ffab9861771a75f29df65031b3c80b6941
CORE_URL=https://github.com/telegramdesktop/tdesktop.git
JOBS="${CONTRAIL_JOBS:-$(nproc)}"

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

for tool in git patch python3; do
  command -v "$tool" >/dev/null || die "required tool is missing: $tool"
done
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
[[ -n "$ENGINE" ]] || die "podman or docker is required"
ENGINE_NAME=$(basename "$ENGINE")
if [[ $ENGINE_NAME == podman ]]; then USER_ARGS=(--userns=keep-id); else USER_ARGS=(--user "$(id -u):$(id -g)"); fi
export CONTAINER_ENGINE="$ENGINE"

# in_container <image> <network: net|none> <command...>
in_container() {
  local image=$1 network=$2; shift 2
  local args=(--rm "${USER_ARGS[@]}" -e HOME=/tmp -e CONTRAIL_JOBS="$JOBS" -v "$ROOT:/src:z" -w /src)
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
  API_ARGS=(-DTDESKTOP_API_TEST=OFF -DTDESKTOP_API_ID="$TELEGRAM_API_ID" -DTDESKTOP_API_HASH="$TELEGRAM_API_HASH")
elif [[ "${CONTRAIL_TEST_API:-}" == 1 ]]; then
  echo "Using Telegram Desktop's public test API credentials: these packages must not be released."
  API_KIND=test
  API_ARGS=(-DTDESKTOP_API_TEST=ON -DTDESKTOP_API_ID=0 -DTDESKTOP_API_HASH=)
else
  die "no Telegram API credentials: put TELEGRAM_API_ID and TELEGRAM_API_HASH into api-credentials.env"
fi

# --- pinned core -------------------------------------------------------------------
if [[ ! -d tdesktop/.git ]]; then
  log "Cloning Telegram Desktop $CORE_TAG"
  git clone --depth 1 --branch "$CORE_TAG" "$CORE_URL" tdesktop
fi
[[ "$(git -C tdesktop rev-parse HEAD)" == "$CORE_COMMIT" ]] \
  || die "tdesktop/ is not at Telegram Desktop $CORE_TAG ($CORE_COMMIT); it was left untouched"
git -C tdesktop submodule update --init --recursive --depth 1 >/dev/null

# Files changed in tdesktop/ (tracked or not, submodules included), relative to its root.
core_changes() {
  git -C tdesktop status --porcelain --untracked-files=all --ignore-submodules=all | cut -c4-
  # shellcheck disable=SC2016
  git -C tdesktop submodule foreach --quiet --recursive \
    'git status --porcelain --untracked-files=all --ignore-submodules=all | cut -c4- | sed "s|^|$displaypath/|"'
}
patched_files() { sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' patches/*.patch | sort -u; }

log "Patches"
for p in patches/*.patch; do
  if patch -p1 -R -s -f --dry-run -d tdesktop < "$p" >/dev/null 2>&1; then
    echo "  $(basename "$p") (already applied)"
  elif patch -p1 -N -s -f --dry-run -d tdesktop < "$p" >/dev/null 2>&1; then
    echo "  $(basename "$p")"
    patch -p1 -N -s -f -d tdesktop < "$p"
  else
    die "$(basename "$p") neither applies to nor is applied in tdesktop/; it was left untouched"
  fi
done
unexpected=$(comm -23 <(core_changes | sort -u) <(patched_files))
[[ -z "$unexpected" ]] || die "tdesktop/ has changes that aren't in patches/:"$'\n'"$unexpected"

# --- build -------------------------------------------------------------------------
# build_target <aarch64|x86_64> <image> <containerfile> <package format>
build_target() {
  local arch=$1 image=$2 recipe=$3 format=$4
  local build=build-$arch
  # The container is rebuilt only when its recipe changes: its packages come from repositories that move on.
  local recipe_sum label
  recipe_sum=$(sha256sum "$recipe" | cut -d' ' -f1)
  label=$("$ENGINE" image inspect --format '{{ index .Config.Labels "contrail.recipe" }}' "$image" 2>/dev/null || true)
  if [[ $label != "$recipe_sum" ]]; then
    log "Build container $image"
    "$ENGINE" build --label "contrail.recipe=$recipe_sum" -t "$image" -f "$recipe" build-env
  fi
  log "Libraries Debian doesn't have ($arch)"
  in_container "$image" net build-env/build-deps.sh "$arch"
  # The interface sources are listed with a glob: pick up added or removed files.
  [[ -f $build/build.ninja ]] && touch gtk-ui/CMakeLists.txt
  log "Telegram Desktop $CORE_TAG with Contrail ($arch)"
  in_container "$image" none cmake -S tdesktop -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="/src/build-env/$arch-toolchain.cmake" -DCMAKE_INSTALL_PREFIX=/usr \
    -DDESKTOP_APP_USE_PACKAGED=ON -DDESKTOP_APP_DISABLE_AUTOUPDATE=ON -DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
    "${API_ARGS[@]}" -DCONTRAIL_GTK_DIR=/src/gtk-ui >/dev/null
  in_container "$image" none ninja -C "$build" -j"$JOBS" Telegram
  log "Package ($format, $arch)"
  python3 tools/package.py "$format" --arch "$arch" --container "$image" --api "$API_KIND" \
    --core-tag "$CORE_TAG" --core-commit "$CORE_COMMIT"
}

if [[ $TARGET == phone || $TARGET == all ]]; then
  build_target aarch64 localhost/tdesktop-cross build-env/Containerfile deb
fi
if [[ $TARGET == desktop || $TARGET == all ]]; then
  build_target x86_64 localhost/contrail-desktop build-env/Containerfile.desktop appimage
fi
