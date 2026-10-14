#!/usr/bin/env bash
# Screenshots with made-up data (gtk-ui/demo.cpp), on the phone over SSH (PHONE, default "furios"):
#   tools/demo.sh run [dark]     a second, offline Contrail in English with its own empty data directory
#   tools/demo.sh cmd 'chat 0'   a debug command (see gtk-ui/debug.cpp); "demo" fills in the chats
#   tools/demo.sh shot FILE [call]  a 3x screenshot of the window (or of the call window)
#   tools/demo.sh stop
# The installed Contrail and its account are not touched: the demo runs with another application ID,
# from ~/.local/share/contrail-demo (wiped on every run), in a network namespace without network,
# so the fictional account never reaches Telegram's servers.
set -euo pipefail
PHONE="${PHONE:-furios}"
APP_ID=io.github.x_vlad_x.Contrail.Demo
DATA='$HOME/.local/share/contrail-demo'
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
ENV='export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'
# The pattern doesn't match its own ssh command line.
stop() { rsh "pkill -f 'contrail-dem[o]/'; true"; }
case "${1:-}" in
  run)
    stop
    sleep 1
    scheme=$([[ "${2:-}" == dark ]] && echo prefer-dark || echo prefer-light)
    rsh "$ENV; rm -rf $DATA; mkdir -p $DATA/tdata
      echo '{\"skip-url-scheme-register\": true}' > $DATA/tdata/experimental_options.json
      cd ~/.local/share/contrail
      LANG=en_US.UTF-8 LANGUAGE=en LC_ALL=en_US.UTF-8 ADW_DEBUG_COLOR_SCHEME=$scheme CONTRAIL_DEBUG=1 \
        CONTRAIL_APP_ID=$APP_ID QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=\$PWD/plugins CONTRAIL_DIR=\$PWD \
        LD_LIBRARY_PATH=\$PWD/lib nohup unshare -rn bin/contrail -workdir $DATA/ > /tmp/contrail-demo.log 2>&1 &" ;;
  stop) stop ;;
  cmd) rsh "$ENV; gdbus call --session --dest $APP_ID --object-path /io/github/x_vlad_x/Contrail/Debug \
    --method org.gtk.Actions.Activate debug \"[<'$2'>]\" '{}'" >/dev/null ;;
  shot)
    # Drawn by the app itself at 3x (works with the screen off, nothing of the phone around it).
    "$0" cmd "screenshot ${3:+$3 }/tmp/contrail-demo.png"
    sleep 1
    scp -q "$PHONE:/tmp/contrail-demo.png" "$2" ;;
  log) rsh "tail -n ${2:-40} /tmp/contrail-demo.log" ;;
  *) sed -n '2,9p' "$0"; exit 1 ;;
esac
