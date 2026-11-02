#!/usr/bin/env bash
# Testing helpers on the phone over SSH (PHONE, default "furios"):
#   tools/phone.sh run            start Contrail with CONTRAIL_DEBUG=1 (log: /tmp/contrail.log on the phone)
#   tools/phone.sh stop
#   tools/phone.sh cmd 'chat 0'   a debug command (see gtk-ui/debug.cpp)
#   tools/phone.sh shot [file]    screenshot (needs grim: tools/phone.sh get-grim once)
#   tools/phone.sh log            the end of the log
#   tools/phone.sh td-install     put the TDLib version (build-td/aarch64, or TD_BINARY=file) into ~/.local/share/contrail-td-app,
#                                 with an icon among the apps ("Contrail TDLib", log: /tmp/contrail-td.log)
#   tools/phone.sh td-run         start it next to the stable one (log: /tmp/contrail-td.log on the phone)
#   tools/phone.sh td-stop
#   tools/phone.sh td-log
#   tools/phone.sh td-cmd 'tree'  a debug command (see gtk-ui/debug.cpp)
#   tools/phone.sh td-mem         its memory, threads and CPU, the same way 0.3.1 was measured (PSS, anonymous + swap)
#   tools/phone.sh td-mem-run [ENV=VALUE...]  restart it with that environment and measure: start, Saved Messages
#                                 scrolled to the top, closed, a minute later (only Saved Messages: nothing is marked read).
#                                 The values go into a shell on the phone as they are: no spaces or quotes in them.
set -euo pipefail
PHONE="${PHONE:-furios}"
APP_ID=io.github.x_vlad_x.Contrail
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
ENV='export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'
case "${1:-}" in
  run) rsh "$ENV; pkill -f '[b]in/contrail'; sleep 1; CONTRAIL_DEBUG=1 nohup ~/.local/share/contrail/contrail > /tmp/contrail.log 2>&1 &" ;;
  stop) rsh "pkill -f '[b]in/contrail'; true" ;;
  cmd) rsh "$ENV; gdbus call --session --dest $APP_ID --object-path /io/github/x_vlad_x/Contrail/Debug --method org.gtk.Actions.Activate debug \"[<'$2'>]\" '{}'" ;;
  shot) rsh "$ENV; ~/.cache/contrail-tools/root/usr/bin/grim -s 1 /tmp/contrail-shot.png" && scp -q "$PHONE:/tmp/contrail-shot.png" "${2:-shot.png}" ;;
  get-grim) rsh 'set -e; mkdir -p ~/.cache/contrail-tools && cd ~/.cache/contrail-tools && apt-get download grim >/dev/null && dpkg-deb -x grim_*.deb root' ;;
  log) rsh "tail -n ${2:-40} /tmp/contrail.log" ;;
  td-install)
    rsh 'mkdir -p ~/.local/share/contrail-td-app'
    scp -q "${TD_BINARY:-$(dirname "$0")/../build-td/aarch64/contrail.stripped}" "$PHONE:.local/share/contrail-td-app/contrail.new"
    rsh 'mv ~/.local/share/contrail-td-app/contrail.new ~/.local/share/contrail-td-app/contrail'
    scp -q "$(dirname "$0")/contrail-td-icon.svg" "$PHONE:.local/share/contrail-td-app/icon.svg"
    rsh 'cat > ~/.local/share/contrail-td-app/start' <<'SCRIPT'
#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/contrail" "$@" >> /tmp/contrail-td.log 2>&1
SCRIPT
    # Named after the application ID: that is how the shell ties the window to this icon.
    rsh 'chmod +x ~/.local/share/contrail-td-app/start
      cat > ~/.local/share/applications/io.github.x_vlad_x.Contrail.Devel.desktop <<DESKTOP
[Desktop Entry]
Type=Application
Name=Contrail TDLib
Comment=Contrail on TDLib, a test version next to the stable one
Exec=$HOME/.local/share/contrail-td-app/start
Icon=$HOME/.local/share/contrail-td-app/icon.svg
Terminal=false
StartupNotify=true
Categories=Network;InstantMessaging;
X-Purism-FormFactor=Workstation;Mobile;
DESKTOP
      update-desktop-database ~/.local/share/applications' ;;
  td-run)
    rsh "pkill -f '[c]ontrail-td-app/contrail'; true"  # On its own: the command below contains the same path.
    sleep 1
    rsh "$ENV; CONTRAIL_DEBUG=1 nohup ~/.local/share/contrail-td-app/contrail > /tmp/contrail-td.log 2>&1 &" ;;
  td-mem-run)
    shift
    rsh "pkill -f '[c]ontrail-td-app/contrail'; true"
    sleep 1
    rsh "$ENV; CONTRAIL_DEBUG=1 G_MESSAGES_DEBUG=all $* nohup ~/.local/share/contrail-td-app/contrail > /tmp/contrail-td.log 2>&1 &"
    point() {
      echo "## $1"
      "$0" td-mem > /tmp/phone-td-mem.txt || { echo "  not running any more"; rsh 'tail -5 /tmp/contrail-td.log'; exit 1; }
      grep -E '^(Pss|Anonymous|Swap|Threads|CPU|System|VmHWM)' /tmp/phone-td-mem.txt | tr '\n' ' ' | tr -s ' \t' ' '
      echo
      local before
      before=$(rsh 'grep -c "mem:" /tmp/contrail-td.log || true')
      "$0" td-cmd mem > /dev/null
      rsh "for i in \$(seq 1 20); do [ \$(grep -c 'mem:' /tmp/contrail-td.log) -gt $before ] && break; sleep 0.1; done
        grep 'mem:' /tmp/contrail-td.log | tail -1 | sed 's/^.*mem:/  malloc:/'"
      before=$(rsh 'grep -c "mem-arenas:" /tmp/contrail-td.log || true')
      "$0" td-cmd mem-arenas > /dev/null
      rsh "for i in \$(seq 1 20); do [ \$(grep -c 'mem-arenas:' /tmp/contrail-td.log) -gt $before ] && break; sleep 0.1; done
        grep 'mem-arenas:' /tmp/contrail-td.log | tail -1 | sed 's/^.*mem-arenas:/  arenas:/'"
    }
    sleep 25
    point start
    "$0" td-cmd 'list-do 0 saved' > /dev/null
    sleep 3
    for _ in $(seq 1 60); do "$0" td-cmd 'scroll -3000' > /dev/null; sleep 0.6; done
    sleep 2
    point 'Saved Messages scrolled to the top'
    "$0" td-cmd back > /dev/null
    sleep 1
    point 'closed, 1 s'
    sleep 4
    point 'closed, 5 s'
    sleep 55
    point 'closed, 60 s'
    rsh 'grep -E "trimmed in|CRITICAL" /tmp/contrail-td.log | tail -3' ;;
  td-stop) rsh "pkill -f '[c]ontrail-td-app/contrail'; true" ;;
  td-cmd) rsh "$ENV; gdbus call --session --dest $APP_ID.Devel --object-path /io/github/x_vlad_x/Contrail/Devel --method org.gtk.Actions.Activate debug \"[<'$2'>]\" '{}'" ;;
  td-log) rsh "tail -n ${2:-40} /tmp/contrail-td.log" ;;
  td-mem) rsh 'P=$(pgrep -f "[c]ontrail-td-app/contrail" | head -1); [ -n "$P" ] || { echo "not running"; exit 1; }
    awk "/^(Pss|Pss_Anon|Pss_File|Pss_Shmem|Anonymous|Swap|SwapPss):/ {printf \"%s %.1f MiB\n\", \$1, \$2/1024}" /proc/$P/smaps_rollup
    grep -E "^(VmHWM|VmRSS|Threads):" /proc/$P/status
    ps -o times=,etimes= -p $P | awk "{print \"CPU\", \$1, \"s over\", \$2, \"s\"}"
    # All processes since boot, glycin loaders and the compositor included: compare it between two points.
    awk "/^cpu /{printf \"System CPU %.1f s\n\", (\$2+\$3+\$4+\$7+\$8+\$9)/100}" /proc/stat
    du -sh ~/.local/share/contrail-td/app | cut -f1 | sed "s/^/Data /"' ;;
  *) sed -n '2,17p' "$0"; exit 1 ;;
esac
