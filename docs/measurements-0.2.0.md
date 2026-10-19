# Contrail 0.2.0 against 0.1.1: memory, background load, energy and package sizes

Measured on the FuriLabs FLX1s (FuriOS 14, Phosh) on 19 October 2026, with the same account, network (Wi-Fi) and
phone state for both versions, one after the other. Both versions are development builds of the same Telegram
Desktop 7.3.1 core installed by `update-contrail.sh` (they use the phone's own Qt 6.10 and FFmpeg 8.1), so the
numbers compare Contrail's own changes; the release packages carry their own libraries, whose shared pages count
somewhat differently. The raw samples are in `out/measure/` (not part of the repository); the tools are
`tools/measure.sh` and `tools/phone-measure.py`.

## The background scenario (`tools/measure.sh background`)

Start the app, wait a minute; open Saved Messages and load its history with the photos (six pages up); close the
window with "Run in the background" on; sample 1, 5 and 15 minutes after closing. Memory is from
`/proc/PID/smaps_rollup`, in MiB.

| Moment | 0.1.1 PSS | 0.2.0 PSS | Change | 0.1.1 RSS | 0.2.0 RSS | 0.1.1 private | 0.2.0 private |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Started (chat list) | 345.4 | 337.3 | −2% | 438.4 | 429.6 | 325.4 | 318.0 |
| Saved Messages with its media loaded | 1153.0 | 484.5 | −58% | 1247.1 | 577.5 | 1131.8 | 464.8 |
| Window closed, 1 min | 1147.7 | 340.1 | **−70%** | 1241.8 | 433.0 | 1126.4 | 320.4 |
| Window closed, 5 min | 1147.7 | 340.1 | **−70%** | 1241.8 | 433.0 | 1126.4 | 320.3 |
| Window closed, 15 min | 1147.7 | 339.9 | **−70%** | 1241.8 | 432.8 | 1126.5 | 320.1 |

Swap stayed at 0.1 MiB in both. 0.1.1 kept everything once the window was closed: its window was only hidden,
and each photo on the screen kept its full decoded image (up to 1280 px, ~6 MiB) besides the texture shown.
0.2.0 frees the decoded image as soon as its texture exists, and with the window closed releases the whole
interface.

Closing and opening the window five times (Saved Messages loaded each time) left the process at 346.8, 350.3,
350.9, 350.3 and 351.0 MiB PSS after each close: no growth after the first reopening warmed the caches.

## Load with the window closed (1 to 15 minutes after closing)

| | 0.1.1 | 0.2.0 | Change |
| --- | ---: | ---: | ---: |
| CPU time (user + system) | 0.57 s | 0.40 s | −30% |
| Wakeups (voluntary context switches, all threads) | 1535 (1.83/s) | 453 (0.54/s) | **−70%** |
| Involuntary context switches | 112 | 133 | |
| Network, sent / received | 9.7 / 41.1 KiB | 11.9 / 62.1 KiB | depends on the updates that came |
| Sleep inhibitors and kernel wake locks held | none | none | |

A two-minute profile of the threads in the background afterwards (`phone-measure.py threads 120`, after the
five open/close cycles): 0.37 wakeups/s in all, 0.19/s on the main thread (the core's timers), 0.13/s on the
network thread (MTProto keeping the connection) and 0.04/s on GDBus. The once-a-second polling of the connection
state, the 30-second status timer of the open chat, GTK's frame clock and the textures are gone with the window.

After a call (a local test call, which opens the camera without calling anyone) 0.2.0 releases the camera, the
session inhibitor and the proximity sensor. The camera search used to leave two descriptors of the phone's
hardware video codecs (`/dev/video1`, `/dev/video2`, mtk-vcodec) open after every call; a fix to WebRTC
(`patches/tg_owt/`) closes them.

## Energy

**Not measured.** The plan needs repeated hour-long runs on battery (a reference hour with the app stopped, then
an hour with each version in the background, screen locked), and the phone was on its charger throughout this
work, so the fuel gauge only shows charging. The 30% energy target is therefore **not verified**. The lower
CPU time and the 70% fewer wakeups point the same way, but short CPU measurements are not evidence of battery
savings. To measure, with the phone off the charger and locked, each run at least an hour, under the same
conditions:

```bash
tools/phone.sh stop && tools/measure.sh energy 60 idle      # the phone without Contrail
tools/measure.sh energy 60 0.2.0                            # after starting 0.2.0 and closing its window
```

and the same for 0.1.1 (install its build, then `energy 60 0.1.1`), repeating each at least twice.

## Package sizes

| Package | 0.1.1 | 0.2.0 | Change |
| --- | ---: | ---: | ---: |
| `contrail_*_amd64.deb` | 153.0 MiB | 105.5 MiB | −31% |
| `contrail_*_arm64.deb` | 140.0 MiB | 97.4 MiB | −30% |
| `contrail-*.x86_64.rpm` | 163.9 MiB | 113.8 MiB | −31% |
| `contrail-*.aarch64.rpm` | 157.1 MiB | 109.8 MiB | −30% |
| `Contrail-*-x86_64.AppImage` | 192.0 MiB | 135.6 MiB | −29% |
| `Contrail-*-aarch64.AppImage` | 184.1 MiB | 130.2 MiB | −29% |
| `Contrail-*-x86_64.flatpak` | 121.6 MiB | 78.7 MiB | −35% |
| `Contrail-*-aarch64.flatpak` | 112.0 MiB | 72.3 MiB | −35% |

The small FFmpeg (6.7 MiB on ARM64, 8.4 MiB on x86_64 for its six libraries) replaces Debian's 30 MiB of FFmpeg
libraries and 87 other libraries (91 MiB) only Debian's FFmpeg needed: the ARM64 bundle went from 215 libraries
to 138.

## What is left, and TDLib

With the window closed, 0.2.0's 340 MiB PSS are roughly: 83 MiB of the program's own code pages (file-backed,
the kernel can drop them under memory pressure), 51 MiB of shared libraries, and about 200 MiB of heap and
anonymous memory of the Telegram Desktop core: the session's data (chats, users, the messages loaded so far),
Qt, the core's hidden window and its caches. The GTK interface no longer adds to it; going lower means a
smaller core. A client on TDLib would keep the messages in its database instead of in memory and would not
need Qt or the core's own window: that, and these numbers, are the basis for evaluating TDLib after 0.2.0.
