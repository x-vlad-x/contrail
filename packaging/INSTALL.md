# Installing Contrail 0.1.0-preview1

Contrail is an unofficial Telegram client that uses the Telegram API. It is not affiliated with or endorsed
by Telegram. This is a preview: see the release notes for what works and what doesn't yet.

| Device | File |
| --- | --- |
| Linux phone, ARM64 (FuriOS, Mobian and other Debian-based systems) | `contrail_0.1.0-preview1-1_arm64.deb` |
| Linux computer, x86_64 (glibc 2.39 or newer) | `Contrail-0.1.0-preview1-x86_64.AppImage` |

You don't need to build anything or get Telegram API keys to use these packages.

## Phone (Debian package)

Download the `.deb` on the phone, open a terminal in the folder with the file and run:

```sh
sudo apt install ./contrail_0.1.0-preview1-1_arm64.deb
```

`apt` installs what the package needs from the system's repositories. If it reports dependencies that
can't be installed, don't force the installation: please report the message instead. The package was built
for FuriOS 14 (Debian testing as of June 2026); other systems need GTK 4.12+, libadwaita 1.6+ and Qt 6.10.

Open **Contrail** from the app list, or run `contrail` in a terminal. `contrail --version` shows the
version without starting the app.

### If you used the earlier SSH install

The earlier install (`update-contrail.sh`) put a launcher into your home folder that hides the package's.
Run this once, as yourself (**without** `sudo`):

```sh
contrail-migrate-launcher
```

It moves only that old launcher into a backup folder. Your account, chats, cache and settings
(`~/.local/share/contrail-data`) are kept and the package uses them as they are. The old program files in
`~/.local/share/contrail` stay too; when the package works for you, you can delete that folder
(not `contrail-data`).

### Updating and removing

Install a newer `.deb` the same way. `sudo apt remove contrail` removes the program; your account and chats
stay in `~/.local/share/contrail-data` until you delete that folder yourself.

## Computer (AppImage)

```sh
chmod +x Contrail-0.1.0-preview1-x86_64.AppImage
./Contrail-0.1.0-preview1-x86_64.AppImage
```

If your system has no FUSE, run it with `--appimage-extract-and-run`. The AppImage carries GTK 4.18,
libadwaita 1.7 and Qt 6.8 itself and needs a Linux system with glibc 2.39 or newer and the usual graphics,
font and sound libraries. It was started on Ubuntu 24.04, Debian 13, Fedora 42 and Arch Linux (on X11; GTK runs
on Wayland too, but that wasn't checked for this preview). For emoji in messages, install a color emoji font
(for example Noto Color Emoji) if your system has none. The AppImage keeps your account in
`~/.local/share/contrail-data`, like the phone package, separate from the official Telegram Desktop.

## Checking the download

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

## Problems

Please open an issue at <https://github.com/x-vlad-x/contrail/issues> with the package name, what you did
and any error message. `contrail --version` tells which build you have.
