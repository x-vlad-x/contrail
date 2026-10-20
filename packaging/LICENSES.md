# Licenses and sources of the Contrail packages

Contrail is distributed under the GNU General Public License, version 3, with the OpenSSL exception — the
same license as Telegram Desktop, which it is built on (`LICENSE`, `NOTICE`). That license covers Contrail
and Telegram Desktop; the libraries listed below keep their own licenses.

Contrail is an unofficial client that uses the Telegram API. It is not affiliated with or endorsed by
Telegram. Telegram is a trademark of Telegram FZ-LLC.

## What the program is made of

The program (`libexec/contrail/contrail` in each package: under `/usr/lib/contrail` in the DEB and RPM, `usr` in
the AppImage, `/app` in the Flatpak) is Telegram Desktop at a pinned release tag with the patches in `patches/`
and Contrail's GTK interface (`gtk-ui/`) compiled in. Inside the packages, `share/doc/contrail/build-info.json`
names the exact Contrail source commit, the Telegram Desktop tag and commit, and a checksum of the patches.

Compiled into the program from source:

- **Telegram Desktop** and its submodules and bundled third-party code (lib_base, lib_ui, tgcalls, cld3,
  hunspell, libprisma, MicroTeX and others): their license texts are in `share/doc/contrail/upstream`.
- **Libraries built by `build-env/build-deps.sh`** at the commits the official Telegram Desktop build uses:
  tg_owt (WebRTC, with Contrail's fix in `patches/tg_owt/`), TDLib's tde2e, rnnoise (with its neural network
  model), ada, and the Rust part (tlottie, wallet engine and the Rust crates they use). Their license texts are in `share/doc/contrail/static-libraries`;
  `sources.json` there lists the source archives with their SHA-256, and `tdesktop_rust/crates.txt` lists the
  Rust crates that are linked in, with their licenses.
- **Debian packages** used at build time (Debian 13 for both architectures):
  `share/doc/contrail/build-inputs.json` lists every Debian package whose headers, static archives or shared
  libraries the build used, with the source package and version.

## The DEB, RPM and AppImage

These packages carry the shared libraries, plugins, icons and schemas the program needs, from Debian 13 (GTK,
libadwaita, Qt, OpenSSL and others). The libraries are Debian's files, unmodified, except that a run-time
library search path pointing into the system is replaced with the library's own folder.

FFmpeg is the exception: `build-env/build-deps.sh` builds its libraries (libavcodec, libavformat, libavfilter,
libavutil, libswscale, libswresample) from Debian 13's own source package, `ffmpeg` 7:7.1.5-0+deb13u1, with fewer
components than Debian's build: the decoders, demuxers, filters and hardware decoding Telegram Desktop uses, no
network protocols and no programs. They are LGPL-2.1-or-later (no GPL parts are enabled), have the same version
and library ABI as Debian's, and are listed in `bundled-packages.json` as built by Contrail from that source
package, with the exact `configure` options; their copyright file is `bundled/ffmpeg.copyright`. They keep their own
licenses — including the LGPL and the GPL — and are not relicensed by Contrail. `share/doc/contrail/bundled` holds
each package's Debian copyright file, the common license texts, `bundled-packages.json`, which maps every bundled
file to its Debian binary and source package and version, and `origins.json`, where each bundled library came from.
`host-libraries.txt` lists what is taken from the system instead (glibc, graphics drivers, fontconfig, the X11
and Wayland client libraries, ALSA).

The AppImage runtime (the small program at the start of the file that mounts it) is AppImage's type2-runtime
under the MIT license, with statically linked libfuse (LGPL-2.1), squashfuse, musl, zstd, zlib and mimalloc;
see `share/doc/contrail/appimage-runtime`.

Replacing a bundled library: in the DEB and RPM, replace it in `/usr/lib/contrail/lib`; for the AppImage, extract
it with `--appimage-extract`, replace the library in `squashfs-root/usr/lib` and start `squashfs-root/AppRun`.
Contrail adds no restriction on modifying the libraries or on the reverse engineering needed to debug such
modifications.

## The Flatpak

The Flatpak runs on the GNOME runtime from Flathub (`share/doc/contrail/flatpak-runtime.txt` names the version it
was made for), which Flatpak installs from there; the runtime is not part of Contrail's files. The Flatpak carries
the libraries the runtime doesn't have (Qt and others) and PulseAudio's `pactl`, from Debian 13, and the same
FFmpeg 7.1 libraries as the other packages (never the runtime's own FFmpeg), documented the same way in
`share/doc/contrail/bundled`. Inside the installed Flatpak they are in
`/app/lib`; `flatpak run --command=sh io.github.x_vlad_x.Contrail` opens a shell in the sandbox.

## Corresponding sources

Each release offers, next to the packages:

- `Contrail-<version>-sources.tar.gz`: Contrail's own sources at the release commit (with
  `SOURCE-REVISION.json`, a checksum manifest of the files);
- `Contrail-<version>-corresponding-sources.tar` (in numbered parts, see `SOURCES.md`): the Telegram Desktop
  source tree at the pinned tag with all submodules, the source archives of the libraries built by
  `build-deps.sh` (including the vendored Rust crates and the Cargo lockfile; tg_owt with its patch applied),
  the exact Debian source packages of everything the packages carry (FFmpeg's included, with the build options in
  `build-env/build-deps.sh`) and of the Debian libraries compiled into the programs (verified
  against the signed APT indexes and each `.dsc`), and the AppImage runtime sources.

To build the packages yourself, follow "Building" in `README.md`: `tools/build-packages.sh` needs podman (or
docker) and your own Telegram API credentials (`api_id`/`api_hash` from <https://my.telegram.org>). The build
containers install packages from mutable repositories, so byte-for-byte identical results are not claimed;
`build-environment-*.txt` in each release lists the exact package versions the released builds used.
