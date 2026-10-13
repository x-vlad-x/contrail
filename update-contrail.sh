#!/usr/bin/env bash
# Builds Contrail, an unofficial native GNOME interface (GTK4 + libadwaita) for Telegram Desktop, for
# aarch64 Linux phones (tested on the FuriLabs FLX1s with FuriOS) and installs it over SSH.
#
#   ./update-contrail.sh                 build the latest Telegram Desktop release, install it if the phone has an older one
#   ./update-contrail.sh --force         rebuild and reinstall even if the version is the same
#   ./update-contrail.sh --tag <tag>     build a specific tag (e.g. v7.3.1)
#   ./update-contrail.sh --build-only    only build, don't install on the phone
#   ./update-contrail.sh --ui-only       quickly update only the program on the phone (after changes in gtk-ui/)
#   ./update-contrail.sh --check         only check what is installed on the phone and what's released
#   ./update-contrail.sh --uninstall     remove from the phone (the account and the cache are kept;
#                                        --uninstall --purge removes them too)
#
#   PHONE=user@host ./update-contrail.sh  SSH destination (default: "furios", e.g. from ~/.ssh/config)
#
# Telegram API credentials: get your own api_id and api_hash at https://my.telegram.org (API development
# tools) and put them into api-credentials.env next to this script:
#   TELEGRAM_API_ID=12345
#   TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef
# (or pass them in the environment). For development only, CONTRAIL_TEST_API=1 builds with Telegram Desktop's
# public test credentials instead (very limited by Telegram, not for everyday use).
#
# How it works: the Telegram Desktop core (MTProto, data, storage, media) comes from the official repository
# unchanged, except for a small patch (patches/) that hooks the interface in. The interface is our own, built
# on GTK4/libadwaita (gtk-ui/); Telegram Desktop's own Qt window is never shown. UI strings come from Telegram's
# translations. The build cross-compiles for aarch64 in a Debian testing container (same base as FuriOS 14)
# with podman. On the phone the program lives in ~/.local/share/contrail, the account and the cache in
# ~/.local/share/contrail-data.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/tdesktop"
BUILD_DIR="$SCRIPT_DIR/build-aarch64"
OUT_DIR="$SCRIPT_DIR/out"
PHONE="${PHONE:-furios}"                     # SSH host, e.g. from ~/.ssh/config
REMOTE_DIR=".local/share/contrail"           # relative to $HOME on the phone
REMOTE_DATA=".local/share/contrail-data"
REPO_URL="https://github.com/telegramdesktop/tdesktop.git"
IMAGE="localhost/tdesktop-cross"
APP_ID="io.github.x_vlad_x.Contrail"

FORCE=0; BUILD_ONLY=0; TAG=""; CHECK=0; UNINSTALL=0; PURGE=0; UI_ONLY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) FORCE=1 ;;
    --build-only) BUILD_ONLY=1 ;;
    --tag) TAG="$2"; shift ;;
    --ui-only) UI_ONLY=1 ;;
    --check) CHECK=1 ;;
    --uninstall) UNINSTALL=1 ;;
    --purge) PURGE=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
in_container() { podman run --rm --userns=keep-id -v "$SCRIPT_DIR":/src:z -w /src "$IMAGE" "$@"; }

ui_rev() { cat "$SCRIPT_DIR"/gtk-ui/*.cpp "$SCRIPT_DIR"/gtk-ui/*.hpp "$SCRIPT_DIR"/gtk-ui/*.css "$SCRIPT_DIR"/gtk-ui/icons/*.svg \
  "$SCRIPT_DIR"/patches/*.patch | md5sum | cut -c1-8; }

# --- uninstall ---------------------------------------------------------------
if [[ $UNINSTALL -eq 1 ]]; then
  [[ $PURGE -eq 1 ]] && log "Removing Contrail from the phone, including the account and the cache" \
                     || log "Removing Contrail from the phone (the account and the cache are kept)"
  rsh "pkill -f '[b]in/contrail' ; true"
  cmd="rm -rf $REMOTE_DIR .local/share/applications/$APP_ID.desktop;"
  [[ $PURGE -eq 1 ]] && cmd+="rm -rf $REMOTE_DATA;"
  cmd+="update-desktop-database .local/share/applications 2>/dev/null; true"
  rsh "$cmd"
  rsh "ls -d $REMOTE_DATA 2>/dev/null | sed 's/^/  kept: ~\//'" || true
  log "Done"
  exit 0
fi

# --- which version ----------------------------------------------------------
if [[ -z "$TAG" ]]; then
  TAG=$(git ls-remote --tags --refs "$REPO_URL" 'v*' 2>/dev/null | awk -F/ '{print $3}' | grep -E '^v[0-9.]+$' | sort -V | tail -1)
fi
[[ -n "$TAG" ]] || die "could not find the latest Telegram Desktop release"
VERSION="${TAG#v}"
FULL_VERSION="$VERSION+contrail.$(ui_rev)"
log "Telegram Desktop: $VERSION, interface: $(ui_rev)"

INSTALLED=""
if [[ $BUILD_ONLY -eq 0 ]]; then
  INSTALLED=$(rsh "cat $REMOTE_DIR/VERSION 2>/dev/null" || true)
  echo "On the phone: ${INSTALLED:-not installed}"
fi
if [[ $CHECK -eq 1 ]]; then
  if [[ "$INSTALLED" == "$FULL_VERSION" ]]; then echo "Up to date."
  elif [[ "${INSTALLED%%+*}" == "$VERSION" ]]; then echo "Same core, the interface changed. Update: $0 --ui-only"
  else echo "Update available: ${INSTALLED:-none} -> $FULL_VERSION. Install: $0"; fi
  exit 0
fi
if [[ $BUILD_ONLY -eq 0 && $UI_ONLY -eq 0 && "$INSTALLED" == "$FULL_VERSION" && $FORCE -eq 0 ]]; then
  echo "Already the latest version. To reinstall: --force"
  exit 0
fi

# --- API credentials ---------------------------------------------------------
[[ -f "$SCRIPT_DIR/api-credentials.env" ]] && source "$SCRIPT_DIR/api-credentials.env"
if [[ -n "${TELEGRAM_API_ID:-}" && -n "${TELEGRAM_API_HASH:-}" ]]; then
  API_ARGS=(-DTDESKTOP_API_TEST=OFF -DTDESKTOP_API_ID="$TELEGRAM_API_ID" -DTDESKTOP_API_HASH="$TELEGRAM_API_HASH")
elif [[ "${CONTRAIL_TEST_API:-}" == 1 ]]; then
  echo "Using Telegram Desktop's test API credentials (CONTRAIL_TEST_API=1)"
  API_ARGS=(-DTDESKTOP_API_TEST=ON)
else
  die "no Telegram API credentials: put TELEGRAM_API_ID and TELEGRAM_API_HASH into api-credentials.env (see --help)"
fi

command -v podman >/dev/null || die "podman is required"
command -v patch >/dev/null || die "patch is required"

# --- sources -----------------------------------------------------------------
if [[ ! -d "$SRC_DIR/.git" ]]; then
  log "Cloning Telegram Desktop $TAG"
  git clone --depth 1 --branch "$TAG" "$REPO_URL" "$SRC_DIR"
fi
CURRENT=$(git -C "$SRC_DIR" describe --tags --exact-match 2>/dev/null || true)
if [[ "$CURRENT" != "$TAG" ]]; then
  log "Switching the sources to $TAG"
  git -C "$SRC_DIR" checkout -- .
  git -C "$SRC_DIR" fetch --depth 1 origin tag "$TAG"
  git -C "$SRC_DIR" checkout -q "$TAG"
fi
log "Submodules"
git -C "$SRC_DIR" submodule update --init --recursive --depth 1 >/dev/null

log "Patches"
# Back to the pristine sources (the patches touch submodules too), then patch.
git -C "$SRC_DIR" checkout -q -- .
git -C "$SRC_DIR" submodule foreach --quiet --recursive 'git checkout -q -- .'
for p in "$SCRIPT_DIR"/patches/*.patch; do
  echo "  $(basename "$p")"
  patch -p1 -s -N -d "$SRC_DIR" < "$p" || die "patch $(basename "$p") does not apply, regenerate it for the new version"
done

# --- build container ---------------------------------------------------------
if ! podman image exists "$IMAGE"; then
  log "Building the cross-compilation container (Debian testing + arm64)"
  podman build -t "$IMAGE" -f "$SCRIPT_DIR/build-env/Containerfile" "$SCRIPT_DIR/build-env"
fi

# --- build -------------------------------------------------------------------
log "Libraries Debian doesn't have (tg_owt, tde2e, tdesktop_rust)"
in_container build-env/build-deps.sh
# The interface sources are listed with a glob: pick up added or removed files.
[[ -f "$BUILD_DIR/build.ninja" ]] && touch "$SCRIPT_DIR/gtk-ui/CMakeLists.txt"

log "Building (aarch64)"
in_container cmake -S tdesktop -B build-aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/src/build-env/aarch64-toolchain.cmake -DDESKTOP_APP_USE_PACKAGED=ON \
  -DDESKTOP_APP_DISABLE_AUTOUPDATE=ON -DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
  "${API_ARGS[@]}" \
  -DCONTRAIL_GTK_DIR=/src/gtk-ui >/dev/null
in_container ninja -C build-aarch64 -j"$(nproc)" Telegram
in_container aarch64-linux-gnu-strip -o build-aarch64/contrail.stripped build-aarch64/bin/Telegram

# --- package -----------------------------------------------------------------
DIST="$OUT_DIR/contrail-$VERSION"
log "Packaging: $DIST"
rm -rf "$DIST"
mkdir -p "$DIST/bin" "$DIST/icons"
cp "$BUILD_DIR/contrail.stripped" "$DIST/bin/contrail"
cp "$SCRIPT_DIR"/gtk-ui/icons/*.svg "$SCRIPT_DIR/packaging/$APP_ID.svg" "$DIST/icons/"
cp "$SCRIPT_DIR/packaging/contrail" "$SCRIPT_DIR/packaging/$APP_ID.desktop" "$SCRIPT_DIR/packaging/$APP_ID.svg" "$DIST/"
# Libraries the phone doesn't have (built against FuriOS's own packages, the rest comes from the phone).
PHONE_LIBS=""
if [[ $BUILD_ONLY -eq 0 ]] && rsh "/sbin/ldconfig -p" 2>/dev/null | awk 'NR > 1 && /aarch64/ {print $1}' | sort -u > "$OUT_DIR/phone-libs.txt"; then
  PHONE_LIBS="out/phone-libs.txt"
fi
in_container build-env/bundle-libs.sh "out/contrail-$VERSION" $PHONE_LIBS
echo "$FULL_VERSION" > "$DIST/VERSION"
echo "Done: $DIST ($(du -sh "$DIST" | cut -f1))"

[[ $BUILD_ONLY -eq 1 ]] && exit 0

# --- install -----------------------------------------------------------------
log "Closing Contrail on the phone"
rsh "pkill -f '[b]in/contrail' ; true"

install_desktop_entry() {
  rsh "set -e; mkdir -p .local/share/applications
       sed \"s|@HOME@|\$HOME|g\" $REMOTE_DIR/$APP_ID.desktop > .local/share/applications/$APP_ID.desktop
       update-desktop-database .local/share/applications 2>/dev/null || true"
}

if [[ $UI_ONLY -eq 1 ]] && rsh "test -d $REMOTE_DIR/lib"; then
  log "Updating the program on the phone"
  tar -C "$DIST" -cf - bin icons contrail "$APP_ID.desktop" "$APP_ID.svg" VERSION \
    | gzip -1 | rsh "mkdir -p $REMOTE_DIR && tar -C $REMOTE_DIR -xzf -"
  install_desktop_entry
  log "Done: $FULL_VERSION (restart the app)"
  exit 0
fi

log "Copying to the phone"
rsh "rm -rf $REMOTE_DIR.new && mkdir -p $REMOTE_DIR.new"
tar -C "$DIST" -cf - . | gzip -1 | rsh "tar -C $REMOTE_DIR.new -xzf -"
rsh "set -e; rm -rf $REMOTE_DIR.old; [ -d $REMOTE_DIR ] && mv $REMOTE_DIR $REMOTE_DIR.old; mv $REMOTE_DIR.new $REMOTE_DIR; rm -rf $REMOTE_DIR.old"
install_desktop_entry

log "Checking that the program starts on the phone"
rsh "LD_BIND_NOW=1 $REMOTE_DIR/contrail --check-libs >/dev/null 2>&1 && echo '  ok' || echo '  does NOT start, check: ~/$REMOTE_DIR/contrail' >&2"

log "Installed: $FULL_VERSION, look for \"Contrail\" in the app list"
