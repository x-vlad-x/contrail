<p align="center"><img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/packaging/io.github.x_vlad_x.Contrail.svg" width="96" alt=""></p>

# Contrail 0.1.0-preview1

The first installable preview of **Contrail**, a native GNOME (GTK4 + libadwaita) interface for Telegram on
Linux phones, built on the Telegram Desktop 7.3.1 core. Contrail is an unofficial client that uses the
Telegram API; it is not affiliated with or endorsed by Telegram.

<p align="center">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chats.png" width="180" alt="Chat list">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chat.png" width="180" alt="A conversation">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/channel.png" width="180" alt="A channel">
  <img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/screenshots/chats-dark.png" width="180" alt="Chat list, dark style">
</p>
<p align="center"><sub>Screenshots of a made-up account.</sub></p>

## Downloads

| Device | Download | Install |
| --- | --- | --- |
| Linux phone, ARM64 (FuriOS, Debian-based) | `contrail_0.1.0-preview1-1_arm64.deb` | `sudo apt install ./contrail_0.1.0-preview1-1_arm64.deb` |
| Linux computer, x86_64 (glibc 2.39+) | `Contrail-0.1.0-preview1-x86_64.AppImage` | `chmod +x` it and run it |

No building and no Telegram API keys are needed. `README.md` below is the installation guide, including the
move from the earlier SSH install (`contrail-migrate-launcher`, which keeps your account and chats).

## What's in it

- Sign-in (code, cloud password), the chat list with pinned chats, archive, search and long-press actions.
- Chats with history loaded while scrolling, replies, forwards, formatted text, photos, stickers, files,
  videos, voice messages (hold to record), link previews, polls, editing and deleting.
- One-to-one audio and video calls; notifications; `tg://` and `t.me` links; every Telegram language.
- Sponsored messages in channels, as the Telegram API terms require of clients that show channels.
- An adaptive layout: one column on a phone, the chat list and the chat side by side on a wider window, with a
  normal draggable header on desktops.
- Packages: system integration (app list entry, icon at all standard sizes, `contrail --version`), its own
  version independent of the core, license notices and the complete corresponding sources.

## Not there yet

Group calls, screen sharing, stories, reactions, forwarding and selecting messages, sticker and emoji panels,
recording video messages, folders, multiple accounts. Secret chats aren't planned: Telegram Desktop doesn't
have them either.

## Tested

<!-- Fill in from the actual checks of these exact files before publishing. -->

## Sources and licenses

GNU General Public License, version 3, with the OpenSSL exception, the same as Telegram Desktop. The packages
contain libraries under their own licenses; see `LICENSES.md`. `Contrail-0.1.0-preview1-sources.tar.gz` holds
Contrail's sources; the `Contrail-0.1.0-preview1-corresponding-sources.tar.part*` files (see `SOURCES.md`) hold
everything else the packages are built from. Checksums: `SHA256SUMS`.

Telegram is a trademark of Telegram FZ-LLC.
