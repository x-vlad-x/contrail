<p align="center"><img src="https://raw.githubusercontent.com/x-vlad-x/contrail/main/packaging/io.github.x_vlad_x.Contrail.svg" width="96" alt=""></p>

# Contrail 0.2.1

A bugfix release of **Contrail**, a native GNOME (GTK4 + libadwaita) interface for Telegram on Linux phones and
desktops: a chat with a link preview could freeze the app on a phone. It is built on the Telegram Desktop 7.3.3
core. Contrail is an unofficial client that uses the Telegram API; it is not affiliated with or endorsed by
Telegram.

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
| Debian, Ubuntu, Linux Mint, FuriOS, Mobian | `contrail_0.2.1-1_amd64.deb` | `contrail_0.2.1-1_arm64.deb` |
| Fedora, openSUSE | `contrail-0.2.1-1.x86_64.rpm` | `contrail-0.2.1-1.aarch64.rpm` |
| Any distribution with Flatpak | `Contrail-0.2.1-x86_64.flatpak` | `Contrail-0.2.1-aarch64.flatpak` |
| Any distribution, without installing | `Contrail-0.2.1-x86_64.AppImage` | `Contrail-0.2.1-aarch64.AppImage` |

```sh
sudo apt install ./contrail_0.2.1-1_arm64.deb              # DEB
sudo dnf install ./contrail-0.2.1-1.x86_64.rpm             # RPM (openSUSE: zypper install --allow-unsigned-rpm)
flatpak install --user ./Contrail-0.2.1-aarch64.flatpak    # Flatpak
chmod +x Contrail-0.2.1-x86_64.AppImage && ./Contrail-0.2.1-x86_64.AppImage
```

No building and no Telegram API keys are needed. `README.md` below is the installation guide: what each package
needs, where your account is kept, and the move from the earlier SSH install (`contrail-migrate-launcher`, which
keeps your account and chats). To update from 0.2.0, 0.1.1 or 0.1.0, install the new DEB, RPM or Flatpak the same
way, or run the new AppImage: the account and chats stay. The DEB also installs over the 0.1.0-preview1 package.

## What's new since 0.2.0

- **No more freezes on link previews**: a chat with a link preview that has a picture could freeze Contrail for
  good as soon as it was opened, on a phone-sized screen. GTK went round in circles laying out the message,
  because the preview's picture gets taller as the bubble gets wider. The bubble is now laid out without that
  question.
- **Telegram Desktop 7.3.3** core instead of 7.3.1.

## Earlier, in 0.2.0

- **Albums**: photos and videos sent together appear as one message in Telegram's own grid, files and music sent
  together one under the other, every caption kept. Each photo or video of an album opens, has its menu (reply,
  save, delete) and shows its own upload or download progress; a reply or a link to it lights up that one.
  Albums that arrive page by page, lose a message or get their ids from the server keep their place.
- **Files and pictures are shown before they are sent**, from the file chooser or pasted with Ctrl+V,
  Shift+Insert or Paste in the field's menu: a screenshot, an image copied in a browser, files copied in a file
  manager (also through Flatpak's portal). Previews, removing single files, a caption (the text already typed),
  "Send as documents" and "Group items"; pictures go as photos, photos and videos as albums of up to ten, larger
  sets as several albums in order. Folders and unreadable files are refused with the reason, remote links are
  pasted as text, plain text stays text. Cancelling keeps the text and the reply.
- **The photo viewer zooms properly**: pinching keeps the point between the fingers under them (also while they
  move), a zoomed photo can be dragged up to its edges, a double tap zooms to 2.5x around the tap and back, up
  to 6x; Ctrl+wheel with a mouse. Turning the phone keeps the place, the full photo replacing its thumbnail
  doesn't move it.
- **Less memory and fewer wakeups in the background**: with the window closed Contrail releases its interface
  (the chats, the lists, the viewers, their textures) and keeps only where it was. On the FLX1s, after a chat
  with photos was open, it now takes 340 MiB instead of 1148 MiB (PSS) in the background, and wakes up about
  70% less often; photos also take far less memory while the chat is open. Opening the window again comes back
  to the same chat, draft and scroll position, or to the chat a notification or a link points to. Notifications,
  calls, downloads and a voice message being recorded go on meanwhile. See [the measurements](https://github.com/x-vlad-x/contrail/blob/main/docs/measurements-0.2.0.md).
- **Online status**: the account shows as online while Contrail's window is in front, offline once it is
  closed, as in the official apps.
- **Smaller packages**: FFmpeg is built from Debian's source with only what Contrail uses, which leaves out some
  90 MB of codec libraries: the DEB is 97–106 MB instead of 140–153 MB, the Flatpak 72–79 MB.
- Calls no longer leave two of the phone's video devices open after each call.

## Earlier, in 0.1.1

- **Groups with topics**: a group with topics opens its list of topics, with unread badges, previews and pinned
  topics; a topic opens as a chat of its own, where you can read, reply, send text, files and voice messages,
  and keep a draft. Notifications and links to a message in a topic open that topic.

## Earlier, in 0.1.0

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
- Groups with topics: the list of topics, and each topic as a chat of its own.
- Chats with history loaded while scrolling, replies, forwards, formatted text, photos and albums, stickers,
  files, videos, voice messages (hold to record), link previews, polls, editing and deleting; sending files and
  pasted pictures with a caption; a photo viewer with pinch zoom.
- One-to-one audio and video calls; notifications; `tg://` and `t.me` links; every Telegram language.
- Sponsored messages in channels, as the Telegram API terms require of clients that show channels.
- An adaptive layout: one column on a phone, the chat list and the chat side by side on a wider window, with a
  normal header and window buttons on desktops.

## Known limitations

- Rare media formats (MPEG-2, AVI, WMA and others outside the common ones) are sent and opened as files, without
  a preview.
- Ogg Opus audio files chosen together with other music are sent each on its own: Telegram doesn't take them in
  an album of music.
- The energy use in the background was not measured on battery yet: the wakeups and CPU time went down, the
  battery saving is not confirmed (see [the measurements](https://github.com/x-vlad-x/contrail/blob/main/docs/measurements-0.2.0.md)).
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
- Each x86_64 package was installed the way users do and started in a clean container with an offline, made-up
  demo account (`tools/smoke-test.sh`, on X11), including pasting a picture from the X11 clipboard (and files,
  outside the Flatpak) into the attach dialog: the DEB with `apt` on Debian 13 and Ubuntu 24.04, the RPM with
  `dnf` on Fedora 44 and `zypper` on openSUSE Tumbleweed, the AppImage on Debian 13, Ubuntu 24.04, Fedora 44 and
  Arch Linux, the Flatpak with `flatpak` on Fedora 44.
- On the FuriLabs FLX1s (FuriOS 14, Phosh), the ARM64 AppImage with a real account: the chat that froze 0.2.0
  (a link preview with a picture) opens and scrolls, older pages load; in Saved Messages, 19 rounds of sending a
  picture and deleting it.
- The ARM64 DEB, RPM and Flatpak weren't installed in containers for this release; they are built the same way
  as the AppImage, from the same program. 0.2.0's packages were, and nothing in the packaging changed since.
- Not checked with these files: calls with another person, notifications and calls on a locked phone, writing
  in a group with topics, pasting files through Flatpak's portal, Wayland desktops. Battery use in the
  background wasn't measured.

## Sources and licenses

GNU General Public License, version 3, with the OpenSSL exception, the same as Telegram Desktop. The packages
contain libraries under their own licenses; see `LICENSES.md`. `Contrail-0.2.1-sources.tar.gz` holds Contrail's
sources; the `Contrail-0.2.1-corresponding-sources.tar.part*` files (see `SOURCES.md`) hold everything else the
packages are built from. Checksums: `SHA256SUMS`.

Telegram is a trademark of Telegram FZ-LLC.
