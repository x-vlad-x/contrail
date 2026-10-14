#!/usr/bin/env bash
# Checks the built packages in clean containers, the way users install them. Nothing touches the host's
# system or any Telegram account; the program runs with a fictional local demo account and no network.
#
#   tools/smoke-test.sh deb        install the ARM64 .deb with apt in an arm64 FuriOS userland (needs
#                                  qemu-user binfmt on the host for arm64 containers) and run it
#   tools/smoke-test.sh appimage   run the AppImage on Debian 13, Ubuntu 24.04, Fedora 42 and Arch Linux
#
# Screenshots and logs go to out/smoke/<target>/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker)}"
VERSION=$(cat VERSION)
DEB_VERSION=$(python3 -c "import sys; sys.path.insert(0, 'tools'); import package; print(package.deb_version('$VERSION'))")
DEB="out/packages/contrail_${DEB_VERSION//\~/-}_arm64.deb"
APPIMAGE="out/packages/Contrail-$VERSION-x86_64.AppImage"
TEST_TOOLS_DEBIAN="xvfb dbus dbus-x11 libglib2.0-bin x11-apps netpbm"
# What a desktop system has and the AppImage takes from it (its usr/share/doc/contrail/host-libraries.txt):
# fonts, graphics, X11/Wayland client libraries, ALSA. Minimal container images lack them.
HOST_DEBIAN="libfontconfig1 fonts-dejavu-core libharfbuzz0b libegl1 libgl1 libopengl0 libgl1-mesa-dri libx11-xcb1"
HOST_DEBIAN+=" libxcb-dri3-0 libasound2t64 libwayland-client0 libgraphite2-3 libfribidi0 libatomic1 libcom-err2"
HOST_DEBIAN+=" libgmp10 libbrotli1 libpng16-16t64 libexpat1 libbz2-1.0 libdrm2 libgles2"
# Fedora installs by file: the sonames of host-libraries.txt.
HOST_FEDORA=$(printf '/usr/lib64/%s ' libEGL.so.1 libGLX.so.0 libOpenGL.so.0 libX11-xcb.so.1 libasound.so.2 \
  libatomic.so.1 libbrotlidec.so.1 libbz2.so.1 libcom_err.so.2 libdrm.so.2 libexpat.so.1 libfontconfig.so.1 \
  libfreetype.so.6 libfribidi.so.0 libGLESv2.so.2 libgmp.so.10 libgraphite2.so.3 libharfbuzz.so.0 libpng16.so.16 \
  libwayland-client.so.0 libxcb-dri3.so.0)

# image <tag> <context> <Dockerfile text>: a test image (built with network; the tests run without).
image() {
  printf '%s\n' "$3" | "$ENGINE" build -q --platform "${PLATFORM:-linux/amd64}" -t "$1" -f - "$2" >/dev/null
}
# run <image> <script>: no network, the sources read-only, results in out/smoke.
run() {
  "$ENGINE" run --rm --network=none --platform "${PLATFORM:-linux/amd64}" -v "$ROOT:/src:ro" \
    -v "$ROOT/out/smoke:/out" -w /src "$1" sh -c "$2"
}

case "${1:-}" in
  deb)
    [[ -f $DEB ]] || { echo "No $DEB: run tools/build-packages.sh phone" >&2; exit 1; }
    mkdir -p out/smoke/deb
    export PLATFORM=linux/arm64
    # FuriOS 14's userland: the Debian snapshot the package was built against. apt installs the package
    # with the dependencies it declares, as on the phone.
    "$ENGINE" build -q --platform linux/arm64 -t localhost/contrail-smoke-arm64 \
      ${SMOKE_BASE:+--build-arg BASE="$SMOKE_BASE"} -f build-env/Containerfile.smoke build-env >/dev/null
    image localhost/contrail-smoke-deb out/packages "FROM localhost/contrail-smoke-arm64
COPY $(basename "$DEB") /tmp/
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      /tmp/$(basename "$DEB") > /tmp/apt.log 2>&1 || { tail -n 30 /tmp/apt.log; exit 1; }"
    run localhost/contrail-smoke-deb "
      set -e
      dpkg -s contrail | sed -n '/^Version/p;/^Depends/p'
      contrail --check-libs
      desktop-file-validate /usr/share/applications/io.github.x_vlad_x.Contrail.desktop
      SMOKE_SETTLE=30 SMOKE_TIMEOUT=900 tests/smoke/run-demo.sh /out/deb contrail"
    ;;
  appimage)
    [[ -f $APPIMAGE ]] || { echo "No $APPIMAGE: run tools/build-packages.sh desktop" >&2; exit 1; }
    declare -A systems=(
      [debian-13]="docker.io/library/debian:trixie|apt-get update -qq \
        && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $TEST_TOOLS_DEBIAN \
        $HOST_DEBIAN"
      [ubuntu-24.04]="docker.io/library/ubuntu:24.04|apt-get update -qq \
        && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $TEST_TOOLS_DEBIAN \
        $HOST_DEBIAN"
      [fedora-42]="registry.fedoraproject.org/fedora:42|dnf install -q -y xorg-x11-server-Xvfb dbus-daemon \
        dbus-tools glib2 xwd netpbm-progs mesa-dri-drivers dejavu-sans-fonts $HOST_FEDORA"
      [archlinux]="docker.io/library/archlinux:latest|pacman -Sy --noconfirm --quiet xorg-server-xvfb dbus glib2 xorg-xwd netpbm \
        mesa libglvnd fontconfig ttf-dejavu harfbuzz libx11 libxcb alsa-lib wayland graphite fribidi gcc-libs \
        e2fsprogs gmp brotli libpng expat bzip2 libdrm"
    )
    for name in ${SMOKE_SYSTEMS:-debian-13 ubuntu-24.04 fedora-42 archlinux}; do
      IFS='|' read -r base setup <<< "${systems[$name]}"
      echo "==> $name"
      mkdir -p "out/smoke/appimage-$name"
      image "localhost/contrail-smoke-$name" build-env "FROM $base
RUN $setup"
      run "localhost/contrail-smoke-$name" "
        set -e
        cp $APPIMAGE /tmp/Contrail.AppImage
        tests/smoke/run-demo.sh /out/appimage-$name /tmp/Contrail.AppImage --appimage-extract-and-run"
      [[ -n ${SMOKE_KEEP:-} ]] || "$ENGINE" rmi -f "localhost/contrail-smoke-$name" >/dev/null
    done
    ;;
  *) sed -n '2,10p' "$0"; exit 2 ;;
esac
echo "Screenshots and logs: out/smoke/"
