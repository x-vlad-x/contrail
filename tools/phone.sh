#!/usr/bin/env bash
# Testing helpers on the phone over SSH (PHONE, default "furios"):
#   tools/phone.sh run            start Contrail with CONTRAIL_DEBUG=1 (log: /tmp/contrail.log on the phone)
#   tools/phone.sh stop
#   tools/phone.sh cmd 'chat 0'   a debug command (see gtk-ui/debug.cpp)
#   tools/phone.sh shot [file]    screenshot (needs grim: tools/phone.sh get-grim once)
#   tools/phone.sh log            the end of the log
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
  *) sed -n '2,8p' "$0"; exit 1 ;;
esac
