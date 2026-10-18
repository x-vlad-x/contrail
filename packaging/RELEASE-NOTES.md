<p align="center"><img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/packaging/io.github.x_vlad_x.Contrail.svg" width="96" alt=""></p>

# Contrail 0.1.0

The first release of **Contrail**, a native GNOME (GTK4 + libadwaita) interface for Telegram on Linux phones
and desktops, built on the Telegram Desktop 7.3.1 core. Contrail is an unofficial client that uses the Telegram
API; it is not affiliated with or endorsed by Telegram.

<p align="center">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chats.png" width="180" alt="Chat list">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chat.png" width="180" alt="A conversation">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/call.png" width="180" alt="An incoming call">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chats-dark.png" width="180" alt="Chat list, dark style">
</p>
<p align="center"><sub>Screenshots of a made-up account.</sub></p>

## Downloads

| Your system | x86_64 (most computers) | ARM64 (Linux phones, ARM computers) |
| --- | --- | --- |
| Debian, Ubuntu, Linux Mint, FuriOS, Mobian | `contrail_0.1.0-1_amd64.deb` | `contrail_0.1.0-1_arm64.deb` |
| Fedora, openSUSE | `contrail-0.1.0-1.x86_64.rpm` | `contrail-0.1.0-1.aarch64.rpm` |
| Any distribution with Flatpak | `Contrail-0.1.0-x86_64.flatpak` | `Contrail-0.1.0-aarch64.flatpak` |
| Any distribution, without installing | `Contrail-0.1.0-x86_64.AppImage` | `Contrail-0.1.0-aarch64.AppImage` |

```sh
sudo apt install ./contrail_0.1.0-1_arm64.deb              # DEB
sudo dnf install ./contrail-0.1.0-1.x86_64.rpm             # RPM (openSUSE: zypper install --allow-unsigned-rpm)
flatpak install --user ./Contrail-0.1.0-aarch64.flatpak    # Flatpak
chmod +x Contrail-0.1.0-x86_64.AppImage && ./Contrail-0.1.0-x86_64.AppImage
```

No building and no Telegram API keys are needed. `README.md` below is the installation guide: what each package
needs, where your account is kept, and the move from the earlier SSH install (`contrail-migrate-launcher`, which
keeps your account and chats). The DEB installs over the 0.1.0-preview1 package.

## What's new since 0.1.0-preview1

- **Packages for every common system**: DEB, RPM, AppImage and Flatpak, each for x86_64 and ARM64. The DEB, RPM
  and AppImage carry the libraries Contrail needs, so one file installs on Debian 13, Ubuntu 24.04, Fedora,
  openSUSE, FuriOS and other systems with glibc 2.39 or newer; the Flatpak runs on the GNOME runtime.
- **Running in the background** (Settings): closing the window keeps messages, notifications and calls coming;
  an icon in the notification area on desktops, the app's launcher on a phone brings the window back.
- **Notifications through the shell**, also while the window is closed, with the phone's own message sound.
- **Incoming calls like the phone's own**: the phone's ringtone (feedbackd), Accept and Decline from the
  notification, and on a locked phone the call fills the lock screen with Mute, the loudspeaker and End Call,
  without unlocking.
- **Where a call is heard**: headphones or Bluetooth when connected, otherwise the earpiece, switchable to the
  loudspeaker or another device; each output keeps its own call volume. The screen goes dark at the ear and
  doesn't lock during a call.
- **Read marks**: opening a chat marks the messages seen as read.

## Everything it does

- Sign-in (code, cloud password), the chat list with pinned chats, archive, search and long-press actions.
- Chats with history loaded while scrolling, replies, forwards, formatted text, photos, stickers, files,
  videos, voice messages (hold to record), link previews, polls, editing and deleting.
- One-to-one audio and video calls; notifications; `tg://` and `t.me` links; every Telegram language.
- Sponsored messages in channels, as the Telegram API terms require of clients that show channels.
- An adaptive layout: one column on a phone, the chat list and the chat side by side on a wider window, with a
  normal header and window buttons on desktops.

## Known limitations

- The packages are large (110–190 MB): they carry Debian's full FFmpeg with all its codec libraries.
- The Flatpak keeps its own account and chats, apart from the other packages (see the installation guide), and
  takes `flatpak update` only from a newer file.
- The RPM isn't signed.
- Emoji in messages need a color emoji font on the system (the DEB recommends Noto Color Emoji).

## Not there yet

Group calls, screen sharing, stories, reactions, voting in polls, forwarding and selecting messages, sticker
and emoji panels, recording video messages, folders, multiple accounts. Secret chats aren't planned: Telegram
Desktop doesn't have them either.

## Tested

<!-- Keep this true for the published files: re-run tools/smoke-test.sh on them and update the lists. -->
- Each x86_64 package was installed the way users do and started in a clean container with an offline,
  made-up demo account (`tools/smoke-test.sh`): the DEB with `apt` on Debian 13 and Ubuntu 24.04, the RPM with
  `dnf` on Fedora 44 and `zypper` on openSUSE Tumbleweed, the AppImage on Debian 13, Ubuntu 24.04, Fedora 44 and
  Arch Linux, the Flatpak with `flatpak` on Fedora 44 (all on X11).
- On the FuriLabs FLX1s (FuriOS 14, Phosh), the ARM64 AppImage and Flatpak ran as a second, offline instance
  with the demo account.
- Not checked with these files: signing in and chatting with a real account, calls, Wayland desktops.

## Sources and licenses

GNU General Public License, version 3, with the OpenSSL exception, the same as Telegram Desktop. The packages
contain libraries under their own licenses; see `LICENSES.md`. `Contrail-0.1.0-sources.tar.gz` holds Contrail's
sources; the `Contrail-0.1.0-corresponding-sources.tar.part*` files (see `SOURCES.md`) hold everything else the
packages are built from. Checksums: `SHA256SUMS`.

Telegram is a trademark of Telegram FZ-LLC.
