# Installing Contrail 0.3.1

Contrail is an unofficial Telegram client that uses the Telegram API. It is not affiliated with or endorsed
by Telegram. You don't need to build anything or get Telegram API keys to use these packages.

Contrail is made for FuriOS on the FuriPhone FLX1s, and tested there before each release. The packages are for
ARM64 only: a Flatpak and a DEB. They may also work on Mobian, PureOS and other Debian-based Phosh systems; this
isn't verified. There are no packages for x86_64 computers: Contrail 0.3.1 remains the last release for them, and
the last one as an RPM or an AppImage.

## With updates: the Flatpak repository

Contrail has its own signed Flatpak repository, [flatpak.xvlad.com](https://flatpak.xvlad.com), with every release
for ARM64. Open [Contrail.flatpakref](https://flatpak.xvlad.com/io.github.x_vlad_x.Contrail.flatpakref)
in GNOME Software, Discover or another app store, or add it in a terminal:

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --user --if-not-exists xvlad https://flatpak.xvlad.com/xvlad.flatpakrepo
flatpak install --user xvlad io.github.x_vlad_x.Contrail
```

New versions then come through the app store or `flatpak update`, like any other app. The GNOME runtime comes
from Flathub (the first command adds it if it isn't there yet). The files below are for installing without the
repository.

## Which file

| Your system | File (ARM64) |
| --- | --- |
| FuriOS, Mobian, PureOS and other Debian-based systems | `contrail_0.3.1-1_arm64.deb` |
| Any Phosh system with Flatpak | `Contrail-0.3.1-aarch64.flatpak` |

`uname -m` prints `aarch64` on these phones. The DEB carries the libraries Contrail needs (GTK 4.18, libadwaita
1.7 from Debian 13, and FFmpeg 7.1 built from Debian 13's source with what Contrail uses) and takes only the base
system from yours: it needs glibc 2.39 or newer (FuriOS 14, Debian 13 or newer) and the usual graphics, font and
sound libraries. The Flatpak runs on the GNOME 51 runtime, which Flatpak downloads from Flathub.

## DEB

Open a terminal in the folder with the file and run:

```sh
sudo apt install ./contrail_0.3.1-1_arm64.deb
```

`apt` installs what the package needs from your system's repositories. If it reports dependencies that can't be
installed, don't force the installation: please report the message instead. A newer `.deb` installs the same
way, also over the 0.1.0-preview1 package. `sudo apt remove contrail` removes the program.

## Flatpak

```sh
flatpak install --user ./Contrail-0.3.1-aarch64.flatpak
flatpak run io.github.x_vlad_x.Contrail
```

Flatpak asks to add Flathub for the runtime if your system doesn't have it yet. The app then appears in the app
list. `flatpak uninstall --user io.github.x_vlad_x.Contrail` removes it, `flatpak update` doesn't update it:
install a newer file the same way. With updates, install it from Contrail's Flatpak repository instead (the account
stays):

```sh
flatpak remote-add --user --if-not-exists xvlad https://flatpak.xvlad.com/xvlad.flatpakrepo
flatpak install --user --reinstall xvlad io.github.x_vlad_x.Contrail
```

The sandbox allows what Contrail uses: the network, sound, the camera and microphone (all devices), the Downloads
and Pictures folders, notifications, the tray icon, and on phones feedbackd (the ringtone), the session's lock
state and the proximity sensor. Your location goes through the desktop portal, which asks you first. Files you attach are picked through the system's file chooser.

## Your account and chats

Contrail keeps the account, the chats and the cache in `~/.local/share/contrail-data/td` (the DEB), separate
from the official Telegram Desktop. The Flatpak keeps its own in
`~/.var/app/io.github.x_vlad_x.Contrail/data/contrail-data/td`. Removing a package doesn't delete them.

Contrail 0.4.0 is the first version on TDLib. After the update from 0.3, sign in once. Contrail carries over the
language, notification and background settings and retrieves your cloud chats again. The data of 0.3 stays in
`contrail-data/tdata` next to it.

`contrail --version` (in the Flatpak: `flatpak run io.github.x_vlad_x.Contrail --version`) shows the version
without starting the app.

### If you used the earlier SSH install on the phone

The earlier install (`update-contrail.sh`) put a launcher into your home folder that hides the package's. After
installing the DEB, run this once, as yourself (**without** `sudo`):

```sh
contrail-migrate-launcher
```

It moves only that old launcher into a backup folder. Your account, chats, cache and settings
(`~/.local/share/contrail-data`) are kept and the package uses them as they are. The old program files in
`~/.local/share/contrail` stay too; when the package works for you, you can delete that folder (not
`contrail-data`).

## On a phone

Contrail is developed on the FuriPhone FLX1s with FuriOS 14 (Phosh). Choosing where a call is heard needs `pactl`
(PulseAudio, or PipeWire's PulseAudio server; the DEB recommends it, the Flatpak carries it). The dark
screen at the ear needs a proximity sensor behind iio-sensor-proxy. The phone's ringtone comes from feedbackd.
Phosh shows call notification actions on the lock screen. Full-screen controls need Wayland layer shell,
which FuriOS offers to the native packages. For emoji in messages, install a color emoji font such as Noto Color
Emoji if your system has none.

## Checking the download

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

## Problems

Please open an issue at <https://github.com/x-vlad-x/contrail/issues> with the file you installed, what you did
and any error message. `contrail --version` tells which build you have.
