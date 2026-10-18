# Installing Contrail 0.1.0

Contrail is an unofficial Telegram client that uses the Telegram API. It is not affiliated with or endorsed
by Telegram. You don't need to build anything or get Telegram API keys to use these packages.

## Which file

| Your system | x86_64 (most computers) | ARM64 (Linux phones, ARM computers) |
| --- | --- | --- |
| Debian, Ubuntu, Linux Mint, FuriOS, Mobian and other DEB systems | `contrail_0.1.0-1_amd64.deb` | `contrail_0.1.0-1_arm64.deb` |
| Fedora, openSUSE and other RPM systems | `contrail-0.1.0-1.x86_64.rpm` | `contrail-0.1.0-1.aarch64.rpm` |
| Any distribution with Flatpak | `Contrail-0.1.0-x86_64.flatpak` | `Contrail-0.1.0-aarch64.flatpak` |
| Any distribution, without installing | `Contrail-0.1.0-x86_64.AppImage` | `Contrail-0.1.0-aarch64.AppImage` |

`uname -m` prints `x86_64` or `aarch64`. The DEB, RPM and AppImage carry the libraries Contrail needs (GTK 4.18,
libadwaita 1.7, Qt 6.8, FFmpeg 7.1 from Debian 13) and take only the base system from yours: they need glibc 2.39
or newer (Debian 13, Ubuntu 24.04, Fedora 40, openSUSE Leap 16 or newer) and the usual graphics, font and sound
libraries. The Flatpak runs on the GNOME 51 runtime, which Flatpak downloads from Flathub.

## DEB

Open a terminal in the folder with the file and run:

```sh
sudo apt install ./contrail_0.1.0-1_arm64.deb     # or ..._amd64.deb
```

`apt` installs what the package needs from your system's repositories. If it reports dependencies that can't be
installed, don't force the installation: please report the message instead. A newer `.deb` installs the same
way, also over the 0.1.0-preview1 package. `sudo apt remove contrail` removes the program.

## RPM

```sh
sudo dnf install ./contrail-0.1.0-1.x86_64.rpm                       # Fedora (or ...aarch64.rpm)
sudo zypper install --allow-unsigned-rpm ./contrail-0.1.0-1.x86_64.rpm  # openSUSE
```

The package isn't signed: the checksums below tell whether the download is intact. `sudo dnf remove contrail`
(or `sudo zypper remove contrail`) removes it.

## Flatpak

```sh
flatpak install --user ./Contrail-0.1.0-aarch64.flatpak    # or ...x86_64.flatpak
flatpak run io.github.x_vlad_x.Contrail
```

Flatpak asks to add Flathub for the runtime if your system doesn't have it yet. The app then appears in the app
list. `flatpak uninstall --user io.github.x_vlad_x.Contrail` removes it, `flatpak update` doesn't update it:
install a newer file the same way.

The sandbox allows what Contrail uses: the network, sound, the camera and microphone (all devices), the Downloads
and Pictures folders, notifications, the tray icon, and on phones feedbackd (the ringtone), the session's lock
state and the proximity sensor. Files you attach are picked through the system's file chooser.

## AppImage

```sh
chmod +x Contrail-0.1.0-x86_64.AppImage    # or ...aarch64.AppImage
./Contrail-0.1.0-x86_64.AppImage
```

If your system has no FUSE, run it with `--appimage-extract-and-run`.

## Your account and chats

Contrail keeps the account, the chats and the cache in `~/.local/share/contrail-data` (the DEB, RPM and
AppImage share it), separate from the official Telegram Desktop. The Flatpak keeps its own in
`~/.var/app/io.github.x_vlad_x.Contrail/data/contrail-data`. Removing a package doesn't delete them. To move an
account between the two, close Contrail and copy the folder.

`contrail --version` (in the Flatpak: `flatpak run io.github.x_vlad_x.Contrail --version`) shows the version
without starting the app.

### If you used the earlier SSH install on the phone

The earlier install (`update-contrail.sh`) put a launcher into your home folder that hides the package's. After
installing the DEB or RPM, run this once, as yourself (**without** `sudo`):

```sh
contrail-migrate-launcher
```

It moves only that old launcher into a backup folder. Your account, chats, cache and settings
(`~/.local/share/contrail-data`) are kept and the package uses them as they are. The old program files in
`~/.local/share/contrail` stay too; when the package works for you, you can delete that folder (not
`contrail-data`).

## On a phone

Contrail is developed on the FuriLabs FLX1s with FuriOS 14 (Phosh). Choosing where a call is heard needs `pactl`
(PulseAudio, or PipeWire's PulseAudio server; the DEB and RPM recommend it, the Flatpak carries it). The dark
screen at the ear needs a proximity sensor behind iio-sensor-proxy, the phone's ringtone comes from feedbackd,
and a call over the lock screen needs a shell with the layer shell, as Phosh. For emoji in messages, install a
color emoji font (for example Noto Color Emoji) if your system has none.

## Checking the download

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

## Problems

Please open an issue at <https://github.com/x-vlad-x/contrail/issues> with the file you installed, what you did
and any error message. `contrail --version` tells which build you have.
