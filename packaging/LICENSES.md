# Licenses and sources of the Contrail packages

Contrail is distributed under the GNU General Public License, version 3, with the OpenSSL exception — the
same license as Telegram Desktop, parts of which it carries: the interface strings, the call tones, the sound
device module for calls (`LICENSE`, `NOTICE`). The libraries listed below keep their own licenses.

Contrail is an unofficial client that uses the Telegram API. It is not affiliated with or endorsed by
Telegram. Telegram is a trademark of Telegram FZ-LLC.

## What the program is made of

The program (`libexec/contrail/contrail` in each package: under `/usr/lib/<package>` in the DEB and RPM, `usr` in
the AppImage, `/app` in the Flatpak) is Contrail's GTK interface (`gtk-ui/`) with TDLib compiled in. Inside the
packages, `share/doc/<package>/build-info.json` names the exact Contrail source commit, the TDLib commit and
whether calls carry sound and video.

Compiled into the program from source:

- **TDLib** (<https://github.com/tdlib/td>), Telegram's library for clients, at the commit pinned in
  `build-env/build-tdlib.sh`, under the Boost Software License 1.0: `share/doc/<package>/tdlib`.
- **For sound and video in calls**: tgcalls from Telegram Desktop's sources at its release tag (LGPL-3.0), and
  the libraries built by `build-env/build-deps.sh` at the commits the official Telegram Desktop build uses:
  tg_owt (WebRTC, with Contrail's fix in `patches/tg_owt/`) and rnnoise (with its neural network model). Their
  license texts are in `share/doc/<package>/static-libraries`; `sources.json` there lists the source archives
  with their SHA-256.
- **Debian packages** used at build time (Debian 13 for both architectures):
  `share/doc/<package>/build-inputs.json` lists every Debian package whose headers, static archives or shared
  libraries the build used, with the source package and version.

## The DEB, RPM and AppImage

These packages carry the shared libraries, plugins, icons and schemas the program needs, from Debian 13 (GTK,
libadwaita, libshumate, OpenSSL and others). The libraries are Debian's files, unmodified, except that a run-time
library search path pointing into the system is replaced with the library's own folder.

FFmpeg is the exception: `build-env/build-deps.sh` builds its libraries (libavcodec, libavformat, libavfilter,
libavutil, libswscale, libswresample) from Debian 13's own source package, `ffmpeg` 7:7.1.5-0+deb13u1, with fewer
components than Debian's build: the decoders, demuxers, filters and hardware decoding Telegram Desktop uses (the
same set serves Contrail), no network protocols and no programs. They are LGPL-2.1-or-later (no GPL parts are enabled), have the same version
and library ABI as Debian's, and are listed in `bundled-packages.json` as built by Contrail from that source
package, with the exact `configure` options; their copyright file is `bundled/ffmpeg.copyright`. They keep their own
licenses — including the LGPL and the GPL — and are not relicensed by Contrail. `share/doc/<package>/bundled` holds
each package's Debian copyright file, the common license texts, `bundled-packages.json`, which maps every bundled
file to its Debian binary and source package and version, and `origins.json`, where each bundled library came from.
`host-libraries.txt` lists what is taken from the system instead (glibc, graphics drivers, fontconfig, the X11
and Wayland client libraries, ALSA).

The AppImage runtime (the small program at the start of the file that mounts it) is AppImage's type2-runtime
under the MIT license, with statically linked libfuse (LGPL-2.1), squashfuse, musl, zstd, zlib and mimalloc;
see `share/doc/<package>/appimage-runtime`.

Replacing a bundled library: in the DEB and RPM, replace it in `/usr/lib/<package>/lib`; for the AppImage, extract
it with `--appimage-extract`, replace the library in `squashfs-root/usr/lib` and start `squashfs-root/AppRun`.
Contrail adds no restriction on modifying the libraries or on the reverse engineering needed to debug such
modifications.

## The Flatpak

The Flatpak runs on the GNOME runtime from Flathub (`share/doc/<package>/flatpak-runtime.txt` names the version it
was made for), which Flatpak installs from there; the runtime is not part of Contrail's files. The Flatpak carries
the libraries the runtime doesn't have (libshumate and others) and PulseAudio's `pactl`, from Debian 13, and the same
FFmpeg 7.1 libraries as the other packages (never the runtime's own FFmpeg), documented the same way in
`share/doc/<package>/bundled`. Inside the installed Flatpak they are in
`/app/lib`; `flatpak run --command=sh io.github.x_vlad_x.Contrail` opens a shell in the sandbox.

## Corresponding sources

Each release offers, next to the packages:

- `Contrail-<version>-sources.tar.gz`: Contrail's own sources at the release commit (with
  `SOURCE-REVISION.json`, a checksum manifest of the files);
- `Contrail-<version>-corresponding-sources.tar` (in numbered parts, see `SOURCES.md`): TDLib at the pinned
  commit, tgcalls at the commit of Telegram Desktop's release tag, the source archives of the libraries built by
  `build-deps.sh` (tg_owt with its patch applied),
  the exact Debian source packages of everything the packages carry (FFmpeg's included, with the build options in
  `build-env/build-deps.sh`) and of the Debian libraries compiled into the programs (verified
  against the signed APT indexes and each `.dsc`), and the AppImage runtime sources.

To build the packages yourself, follow "Building" in `README.md`: `tools/build-packages.sh` needs podman (or
docker) and your own Telegram API credentials (`api_id`/`api_hash` from <https://my.telegram.org>). The build
containers install packages from mutable repositories, so byte-for-byte identical results are not claimed;
`build-environment-*.txt` in each release lists the exact package versions the released builds used.
