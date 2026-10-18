#!/usr/bin/env bash
# Checks the built packages in clean containers, the way users install them. Nothing touches the host's
# system or any Telegram account; the program runs with a fictional local demo account and no network.
#
#   tools/smoke-test.sh FORMAT-ARCH...    deb, rpm, appimage or flatpak; amd64 or arm64; e.g. deb-amd64
#   tools/smoke-test.sh all-amd64         every format of an architecture (all-arm64, all)
#
#   deb       installed with apt on Debian 13, Ubuntu 24.04 and (arm64) FuriOS 14's Debian testing snapshot
#   rpm       installed with dnf on Fedora 44 and with zypper on openSUSE Tumbleweed
#   appimage  run on Debian 13, Ubuntu 24.04, Fedora 44 and (amd64) Arch Linux
#   flatpak   installed with flatpak (user installation, runtime and GL extension from out/flatpak/) on Fedora 44
#
# ARM64 containers need qemu-user binfmt on the host. SMOKE_SYSTEMS limits the systems (e.g. "fedora-44"),
# SMOKE_DARK=1 runs the program with the dark style. Screenshots and logs go to out/smoke/<format-arch-system>/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker)}"
VERSION=$(cat VERSION)
file_of() {
  python3 -c "import sys; sys.path.insert(0, 'tools'); import package; print(package.package_file(*sys.argv[1:]))" \
    "$1" "$2" "$VERSION"
}
TOOLS_DEBIAN="xvfb dbus dbus-x11 libglib2.0-bin x11-apps netpbm"
TOOLS_FEDORA="xorg-x11-server-Xvfb dbus-daemon dbus-tools glib2 xwd netpbm-progs mesa-dri-drivers dejavu-sans-fonts"
TOOLS_SUSE="xorg-x11-server-Xvfb dbus-1-daemon glib2-tools xwd netpbm Mesa-dri dejavu-fonts"
# What a desktop system has and the AppImage takes from it (usr/share/doc/contrail/host-libraries.txt): fonts,
# graphics, X11/Wayland client libraries, ALSA. Minimal container images lack them.
HOST_DEBIAN="libfontconfig1 fonts-dejavu-core libharfbuzz0b libegl1 libgl1 libopengl0 libgl1-mesa-dri libx11-xcb1"
HOST_DEBIAN+=" libxcb-dri3-0 libasound2t64 libwayland-client0 libgraphite2-3 libfribidi0 libatomic1 libcom-err2"
HOST_DEBIAN+=" libgmp10 libbrotli1 libpng16-16t64 libexpat1 libbz2-1.0 libdrm2 libgles2 libgbm1"
HOST_FEDORA="libglvnd-egl libglvnd-glx libglvnd-opengl libX11-xcb alsa-lib libatomic brotli bzip2-libs libcom_err"
HOST_FEDORA+=" libdrm expat fontconfig freetype fribidi libglvnd-gles gmp graphite2 harfbuzz libpng libwayland-client"
HOST_FEDORA+=" libxcb mesa-libgbm"
HOST_ARCH="mesa libglvnd fontconfig ttf-dejavu harfbuzz libx11 libxcb alsa-lib wayland graphite fribidi gcc-libs"
HOST_ARCH+=" e2fsprogs gmp brotli libpng expat bzip2 libdrm"

# image <tag> <platform> <context> <Containerfile text>: a test image (built with network; the tests run without).
image() {
  printf '%s\n' "$4" | "$ENGINE" build -q --platform "$2" -t "$1" -f - "$3" >/dev/null
}
# run <image> <platform> <script> [podman options]: no network, the sources read-only, results in out/smoke.
run() {
  local name=$1 platform=$2 script=$3; shift 3
  "$ENGINE" run --rm --network=none --platform "$platform" -e SMOKE_DARK -e SMOKE_SETTLE -e SMOKE_TIMEOUT "$@" \
    -v "$ROOT:/src:ro" -v "$ROOT/out/smoke:/out" -w /src "$name" sh -c "$script"
}

smoke() {
  local format=$1 arch=$2 machine platform file
  machine=$([[ $arch == arm64 ]] && echo aarch64 || echo x86_64)
  platform=linux/$arch
  file=$(file_of "$format" "$machine")
  [[ -f out/packages/$file ]] || { echo "No out/packages/$file: run tools/build-packages.sh $machine" >&2; return 1; }
  # Under emulation the program takes minutes to start.
  local emulated=
  if [[ $arch != "$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')" ]]; then
    emulated=1
    export SMOKE_SETTLE=${SMOKE_SETTLE:-30} SMOKE_TIMEOUT=${SMOKE_TIMEOUT:-900}
  fi
  local -A systems=()
  case $format in
    deb)
      systems[debian-13]="docker.io/library/debian:trixie|apt-get update -qq && DEBIAN_FRONTEND=noninteractive \
        apt-get install -y -qq --no-install-recommends $TOOLS_DEBIAN"
      systems[ubuntu-24.04]="docker.io/library/ubuntu:24.04|apt-get update -qq && DEBIAN_FRONTEND=noninteractive \
        apt-get install -y -qq --no-install-recommends $TOOLS_DEBIAN"
      [[ $arch == arm64 ]] && systems[furios-14]="localhost/contrail-smoke-furios|true" ;;
    rpm)
      systems[fedora-44]="registry.fedoraproject.org/fedora:44|dnf install -q -y $TOOLS_FEDORA"
      systems[opensuse-tumbleweed]="registry.opensuse.org/opensuse/tumbleweed|zypper -q -n install $TOOLS_SUSE" ;;
    appimage)
      systems[debian-13]="docker.io/library/debian:trixie|apt-get update -qq && DEBIAN_FRONTEND=noninteractive \
        apt-get install -y -qq --no-install-recommends $TOOLS_DEBIAN $HOST_DEBIAN"
      systems[ubuntu-24.04]="docker.io/library/ubuntu:24.04|apt-get update -qq && DEBIAN_FRONTEND=noninteractive \
        apt-get install -y -qq --no-install-recommends $TOOLS_DEBIAN $HOST_DEBIAN"
      systems[fedora-44]="registry.fedoraproject.org/fedora:44|dnf install -q -y $TOOLS_FEDORA $HOST_FEDORA"
      [[ $arch == amd64 ]] && systems[archlinux]="docker.io/library/archlinux:latest|pacman -Sy --noconfirm --quiet \
        xorg-server-xvfb dbus glib2 xorg-xwd netpbm $HOST_ARCH" ;;
    flatpak)
      systems[fedora-44]="registry.fedoraproject.org/fedora:44|dnf install -q -y flatpak $TOOLS_FEDORA" ;;
  esac
  if [[ -n ${systems[furios-14]:-} ]]; then
    "$ENGINE" build -q --platform linux/arm64 -t localhost/contrail-smoke-furios -f build-env/Containerfile.smoke \
      build-env >/dev/null
  fi
  local name base setup out tag install command
  for name in ${SMOKE_SYSTEMS:-$(printf '%s\n' "${!systems[@]}" | sort)}; do
    [[ -n ${systems[$name]:-} ]] || continue
    IFS='|' read -r base setup <<< "${systems[$name]}"
    out=$format-$arch-$name
    tag=localhost/contrail-smoke-$out
    echo "==> $file on $name"
    rm -rf "out/smoke/$out" && mkdir -p "out/smoke/$out"
    case $format in
      deb)
        # apt installs the package with the dependencies it declares, as on a user's system.
        image "$tag" "$platform" out/packages "FROM $base
RUN $setup
COPY $file /tmp/
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /tmp/$file \
      > /tmp/apt.log 2>&1 || { tail -n 30 /tmp/apt.log; exit 1; }"
        run "$tag" "$platform" "set -e
          dpkg -s contrail | sed -n '/^Version/p;/^Depends/p'
          contrail --check-libs
          tests/smoke/run-demo.sh /out/$out contrail" ;;
      rpm)
        if [[ $name == opensuse-* ]]; then
          install="zypper -q -n --no-gpg-checks install --allow-unsigned-rpm /tmp/$file"
        else
          install="dnf install -q -y /tmp/$file"
        fi
        image "$tag" "$platform" out/packages "FROM $base
RUN $setup
COPY $file /tmp/
RUN $install > /tmp/install.log 2>&1 || { tail -n 30 /tmp/install.log; exit 1; }"
        run "$tag" "$platform" "set -e
          rpm -q contrail
          contrail --check-libs
          tests/smoke/run-demo.sh /out/$out contrail" ;;
      appimage)
        if [[ -n $emulated ]]; then
          # Under qemu-user binfmt the AppImage can't be started itself: its "AI" mark in the ELF header doesn't
          # match the emulator's magic. Unpack the squashfs after the runtime (what --appimage-extract-and-run
          # does) and start its AppRun.
          offset=$(python3 -c 'import sys
d = open(sys.argv[1], "rb").read(64)
print(int.from_bytes(d[40:48], "little") + int.from_bytes(d[58:60], "little") * int.from_bytes(d[60:62], "little"))' \
            "out/packages/$file")
          image "$tag" "$platform" build-env "FROM $base
RUN $setup && ( command -v apt-get >/dev/null && apt-get install -y -qq squashfs-tools || dnf install -q -y squashfs-tools )"
          run "$tag" "$platform" "set -e
            unsquashfs -q -o $offset -d /tmp/Contrail out/packages/$file >/dev/null
            tests/smoke/run-demo.sh /out/$out /tmp/Contrail/AppRun"
        else
          image "$tag" "$platform" build-env "FROM $base
RUN $setup"
          run "$tag" "$platform" "set -e
            cp out/packages/$file /tmp/Contrail.AppImage
            tests/smoke/run-demo.sh /out/$out /tmp/Contrail.AppImage --appimage-extract-and-run"
        fi ;;
      flatpak)
        # The runtime comes from the packaging's own download (out/flatpak/), not from the network.
        image "$tag" "$platform" build-env "FROM $base
RUN $setup"
        command="flatpak run --arch=$machine io.github.x_vlad_x.Contrail"
        run "$tag" "$platform" "set -e
          export FLATPAK_USER_DIR=/tmp/flatpak
          cp -a /src/out/flatpak/user /tmp/flatpak
          flatpak install --user -y --noninteractive --no-related --no-deps out/packages/$file
          flatpak info --user io.github.x_vlad_x.Contrail | sed -n '/Version\\|Runtime\\|Arch/p'
          tests/smoke/run-demo.sh /out/$out $command" --privileged ;;
    esac
    [[ -n ${SMOKE_KEEP:-} ]] || "$ENGINE" rmi -f "$tag" >/dev/null 2>&1 || true
  done
}

[[ $# -gt 0 ]] || { sed -n '2,16p' "$0"; exit 2; }
for target in "$@"; do
  case $target in
    all) for t in deb rpm appimage flatpak; do smoke "$t" amd64; smoke "$t" arm64; done ;;
    all-amd64|all-arm64) for t in deb rpm appimage flatpak; do smoke "$t" "${target#all-}"; done ;;
    deb-*|rpm-*|appimage-*|flatpak-*) smoke "${target%-*}" "${target##*-}" ;;
    *) sed -n '2,16p' "$0"; exit 2 ;;
  esac
done
echo "Screenshots and logs: out/smoke/"
