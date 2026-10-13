# Contrail

An unofficial native GNOME interface (GTK4 + libadwaita) for [Telegram](https://telegram.org), built on the
[Telegram Desktop](https://github.com/telegramdesktop/tdesktop) core and made for Linux phones. Developed and
tested on the FuriLabs FLX1s with FuriOS (Phosh), but it should work on any aarch64 phone or tablet with
GTK 4.12+ and libadwaita 1.6+.

The core (MTProto, the chats and messages database, local storage, media downloads and playback,
notifications) is the official Telegram Desktop code with a two-line patch. Its own Qt window is never
shown: the interface is new and adapts to the window — one column on a phone, the chat list and the chat
side by side on a wider screen.

- sign-in with a phone number, the code and the cloud password (two-step verification);
- chat list in Telegram Desktop's order: pinned chats, unread and mention badges, previews, drafts, read
  ticks; archive; search by name plus global search by username; pin, mute, archive, mark as read, delete
  or leave from a long press;
- chats: messages loaded page by page, date separators, grouped senders with avatars, replies (tap to
  jump), forwards, formatted text with links, mentions and hidden spoilers, photos (full-screen viewer),
  stickers, files (download, open), videos and GIFs (open in the system player), voice messages and music
  (played by the core), link previews, polls, contacts; read on screen;
- sending text, replies (long press or swipe left), editing (also Up in an empty field), deleting (for
  everyone too), files and pictures from the file chooser; drafts are kept per chat;
- chat info, contacts, Saved Messages, settings (notifications, log out);
- notifications through the core's native Linux notifications; tapping one opens the chat here;
- `tg://` and `t.me` links.

Not there yet: calls, stories, reactions, message forwarding and selection, stickers and emoji panels,
voice recording, secret chats (Telegram Desktop doesn't have them either), folders, multiple accounts.

The interface is in every language Telegram is translated into: the strings are Telegram Desktop's own
(`tr::lng_*`), and on the first start the language follows the system's.

## Building and installing

Requirements on the build machine (x86_64 Linux): git, patch, podman, ssh. The phone must be reachable over SSH
with key authentication.

You need your own Telegram API credentials: get `api_id` and `api_hash` at <https://my.telegram.org>
(API development tools) and put them into `api-credentials.env` next to the script (it's in `.gitignore`):

```bash
TELEGRAM_API_ID=12345
TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef
```

Then:

```bash
./update-contrail.sh            # build the latest Telegram Desktop release and install it on the phone
./update-contrail.sh --ui-only  # quickly update only the program (after changes in gtk-ui/)
./update-contrail.sh --check    # what is installed on the phone, is there an update
./update-contrail.sh --build-only
./update-contrail.sh --uninstall [--purge]
```

The SSH destination is taken from `PHONE` (default `furios`), e.g. `PHONE=furios@192.168.1.10`.

The script clones Telegram Desktop at the latest release tag into `tdesktop/`, applies the patch and
cross-compiles for aarch64 in a container (`build-env/`). The container uses the same Debian snapshot as
FuriOS (its mirror, `debian.furios.io`), so the program runs with the phone's own Qt, FFmpeg and the rest;
the few libraries the phone doesn't have are put next to it. Libraries Debian doesn't package (tg_owt,
tde2e, tdesktop_rust) are built from source at the versions the official build uses. Code generators
that Telegram Desktop builds and runs during the build go through qemu-user, so nothing has to be
installed on the build machine. The first build takes about an hour, later ones a minute or two.

On the phone the program is installed to `~/.local/share/contrail`, the account and the cache are kept in
`~/.local/share/contrail-data` (separate from the official Telegram Desktop). No root access is needed.

## Project layout

- `gtk-ui/` — the interface, C++ on the plain GTK4/libadwaita C API, compiled into Telegram Desktop's
  executable: `app` (start, helpers), `main_window` (adaptive layout, following the core's own window),
  `login`, `chat_list`, `chat_view` (history loading, scrolling, message field), `message` (one message),
  `text` (Telegram entities → Pango markup, links), `userpics`, `settings`, `dialogs` (chat info, photo
  viewer, contacts, about), `style.css`, `icons/`;
- `patches/` — the patch for Telegram Desktop: adds `gtk-ui/` to its CMake target and calls
  `Contrail::Start()` at the end of `Core::Application::run()`;
- `build-env/` — the container, the CMake toolchain, the build of the extra libraries, the library
  bundling, wrappers to run aarch64 tools through qemu;
- `packaging/` — launcher, desktop entry and icon.

## Debugging

The core writes its log to `~/.local/share/contrail-data/log.txt`. With `CONTRAIL_DEBUG=1` the app accepts
commands over D-Bus, which is handy for testing on the phone without touching it (see `gtk-ui/debug.cpp`
for the list):

```bash
gdbus call --session --dest io.github.x_vlad_x.Contrail --object-path /io/github/x_vlad_x/Contrail/Debug \
  --method org.gtk.Actions.Activate debug "[<'chat 0'>]" "{}"
```

## How it works

Telegram Desktop's data layer (`Data::Session`, `ApiWrap`, `History`) is tied to its Qt widgets, so the
whole program is built. Qt runs with the `offscreen` platform and uses GLib's event loop, the same loop
GTK runs on, so both live on the main thread. Once the core has started, `Contrail::Start()` creates the
GTK window and from then on talks to `Main::Session` directly: it reads the chat list and the history,
subscribes to the core's change streams (`Data::Changes`), sends requests through `ApiWrap` and
`Data::Histories`. When the core opens a chat in its hidden window (a notification, a link), the GTK
window follows.

## License

GNU General Public License v3.0 with the OpenSSL exception, the same as Telegram Desktop; see
[LICENSE](LICENSE) and [NOTICE](NOTICE).

Contrail is an unofficial client, not affiliated with or endorsed by Telegram. Telegram is a trademark of
Telegram FZ-LLC.
