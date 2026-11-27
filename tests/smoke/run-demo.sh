#!/bin/sh
# Runs inside a disposable test container (see tools/smoke-test.sh): starts an installed Contrail on a virtual
# X display without network, on the scripted transport (CONTRAIL_SCENARIO, gtk-ui/td/scenario.cpp: a made-up
# account, nothing reaches Telegram's servers) with a temporary data directory, goes through the chats and the
# settings, and saves what is on the screen.
#   run-demo.sh OUTPUT_DIR APP_ID COMMAND...      e.g. run-demo.sh /out io.github.x_vlad_x.Contrail.Devel contrail-td
set -eu
# A session bus of its own (the program registers its application ID and debug actions on it).
if [ -z "${SMOKE_IN_SESSION:-}" ]; then
  SMOKE_IN_SESSION=1 exec dbus-run-session -- "$0" "$@"
fi
OUT=$1 APP_ID=$2; shift 2
OBJECT=/$(echo "$APP_ID" | tr . /)
mkdir -p "$OUT"
export HOME="$(mktemp -d)" LANG=C.UTF-8 LC_ALL=C.UTF-8 LANGUAGE=en GTK_A11Y=none NO_AT_BRIDGE=1
export XDG_RUNTIME_DIR="$HOME/runtime"
[ -z "${SMOKE_DARK:-}" ] || export ADW_DEBUG_COLOR_SCHEME=prefer-dark
mkdir -m 700 "$XDG_RUNTIME_DIR"

"$@" --version | tee "$OUT/version.txt"
[ -s "$OUT/version.txt" ]

# A free display, told once the server takes connections: on the phone it starts in a few seconds.
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3> "$HOME/display" >/dev/null 2>&1 &
XVFB=$!
i=0
until [ -s "$HOME/display" ]; do
  i=$((i + 1))
  if [ $i -gt 300 ] || ! kill -0 $XVFB 2>/dev/null; then echo "Xvfb did not start" >&2; exit 1; fi
  sleep 0.1
done
export DISPLAY=:$(cat "$HOME/display") GDK_BACKEND=x11
trap 'kill $XVFB 2>/dev/null || true' EXIT

# The X server's view of the screen, when xwd and netpbm are available.
shot() {
  if command -v xwd >/dev/null && command -v xwdtopnm >/dev/null; then
    xwd -root -silent | xwdtopnm 2>/dev/null | pnmtopng > "$OUT/$1.png" 2>/dev/null || true
  fi
}

cmd() {
  gdbus call --session --timeout "${SMOKE_TIMEOUT:-120}" --dest "$APP_ID" --object-path "$OBJECT" \
    --method org.gtk.Actions.Activate debug "[<'$1'>]" '{}' >/dev/null
  sleep "${2:-2}"
}
# The newest "tree" the program logged has TEXT in it.
shown() {
  cmd tree 1
  sed -n '/tree$/,$p' "$OUT/contrail.log" | tail -n 400 | grep -q "$1" || {
    echo "Not on the screen: $1" >&2
    tail -n 60 "$OUT/contrail.log" >&2
    exit 1
  }
}

CONTRAIL_SCENARIO=1 CONTRAIL_DEBUG=1 "$@" > "$OUT/contrail.log" 2>&1 &
APP=$!
# The program is up once its debug actions answer (under emulation this can take minutes).
i=0
until gdbus introspect --session --dest "$APP_ID" --object-path "$OBJECT" 2>/dev/null | grep -q org.gtk.Actions; do
  i=$((i + 1))
  if [ $i -gt 600 ] || ! kill -0 $APP 2>/dev/null; then
    echo "Contrail did not start; log:" >&2
    tail -n 50 "$OUT/contrail.log" >&2
    exit 1
  fi
  sleep 1
done
sleep "${SMOKE_SETTLE:-5}"
shown '"Friend 1"'
shot 1-chats
cmd "chat 0" 4
cmd state 1
grep -q "chat state:" "$OUT/contrail.log"
shot 2-chat
cmd back
cmd settings 2
shown "Notifications"
shot 3-settings
cmd back
cmd about 2
shot 4-about

kill -TERM $APP
wait $APP || true
kill $XVFB 2>/dev/null || true
# The program ran every command and quit cleanly.
grep -q "About: shown" "$OUT/contrail.log"
! grep -E "CRITICAL|Segmentation|Aborted" "$OUT/contrail.log"
