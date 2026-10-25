#!/usr/bin/env bash
# Screenshots with made-up data (gtk-ui/demo.cpp), on the phone over SSH (PHONE, default "furios"):
#   tools/demo.sh tiles          OpenStreetMap's tiles around the demo's places, for its maps (once)
#   tools/demo.sh run [dark]     a second, offline Contrail in English with its own empty data directory
#   tools/demo.sh cmd 'chat 0'   a debug command (see gtk-ui/debug.cpp); "demo" fills in the chats
#   tools/demo.sh shot FILE [call]  a 3x screenshot of the window (or of the call window)
#   tools/demo.sh stop
# The installed Contrail and its account are not touched: the demo runs with another application ID,
# from ~/.local/share/contrail-demo (wiped on every run), in a network namespace without network,
# so the fictional account never reaches Telegram's servers. PROGRAM is the program's directory on the phone
# (an SSH install, default .local/share/contrail).
set -euo pipefail
PHONE="${PHONE:-furios}"
PROGRAM="${PROGRAM:-.local/share/contrail}"
APP_ID=io.github.x_vlad_x.Contrail.Demo
DATA='$HOME/.local/share/contrail-demo'
# libshumate's cache for the demo: its maps can't download anything.
CACHE='$HOME/.cache/contrail-demo'
TILES=https___tile_openstreetmap_org__z___x___y__png
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
ENV='export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'
# The pattern doesn't match its own ssh command line.
stop() { rsh "pkill -f 'contrail-dem[o]/'; true"; }
case "${1:-}" in
  tiles)
    # The places of gtk-ui/demo.cpp and a few tiles around them at every zoom the maps show them at, downloaded
    # here once (kept in ~/.cache) with a User-Agent, as OpenStreetMap's tile policy asks.
    tiles="${XDG_CACHE_HOME:-$HOME/.cache}/contrail-demo-tiles/$TILES"
    python3 -c '
import math, os, sys, time, urllib.request
places = [(46.5072, 6.6262), (46.5101, 6.6318), (46.5088, 6.6405)]
def tile(lat, lon, zoom):
    n = 2 ** zoom
    return int((lon + 180) / 360 * n), int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
for zoom in range(12, 17):  # The maps zoom in to 16 to show a place.
    xs, ys = zip(*(tile(lat, lon, zoom) for lat, lon in places))
    for x in range(min(xs) - 1, max(xs) + 2):
        for y in range(min(ys) - 2, max(ys) + 3):
            path = os.path.join(sys.argv[1], str(zoom), str(x), f"{y}.png")
            if os.path.exists(path):
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            request = urllib.request.Request(f"https://tile.openstreetmap.org/{zoom}/{x}/{y}.png",
                headers={"User-Agent": "Contrail-demo (https://github.com/x-vlad-x/contrail)"})
            with urllib.request.urlopen(request, timeout=30) as response, open(path + ".part", "wb") as out:
                out.write(response.read())
            os.rename(path + ".part", path)
            time.sleep(0.2)
' "$tiles"
    rsh "rm -rf $CACHE/shumate && mkdir -p $CACHE/shumate"
    scp -rq "$tiles" "$PHONE:.cache/contrail-demo/shumate/" ;;
  run)
    stop
    sleep 1
    scheme=$([[ "${2:-}" == dark ]] && echo prefer-dark || echo prefer-light)
    rsh "$ENV; rm -rf $DATA; mkdir -p $DATA/tdata
      echo '{\"skip-url-scheme-register\": true}' > $DATA/tdata/experimental_options.json
      cd ~/$PROGRAM
      LANG=en_US.UTF-8 LANGUAGE=en LC_ALL=en_US.UTF-8 ADW_DEBUG_COLOR_SCHEME=$scheme CONTRAIL_DEBUG=1 XDG_CACHE_HOME=$CACHE \
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
  *) sed -n '2,11p' "$0"; exit 1 ;;
esac
