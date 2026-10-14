#!/bin/sh
# Runs inside a disposable test container (see tools/smoke-test.sh): starts an installed Contrail on a virtual
# X display without network, signs the fictional demo account in locally (gtk-ui/demo.cpp) and saves
# screenshots. Nothing reaches Telegram's servers; the data directory is a temporary one.
#   run-demo.sh OUTPUT_DIR COMMAND...      e.g. run-demo.sh /out contrail
set -eu
# A session bus of its own (the program registers its application ID and debug actions on it).
if [ -z "${SMOKE_IN_SESSION:-}" ]; then
  SMOKE_IN_SESSION=1 exec dbus-run-session -- "$0" "$@"
fi
OUT=$1; shift
mkdir -p "$OUT"
export HOME="$(mktemp -d)" LANG=C.UTF-8 LC_ALL=C.UTF-8 LANGUAGE=en GTK_A11Y=none NO_AT_BRIDGE=1
export XDG_RUNTIME_DIR="$HOME/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"

"$@" --version | tee "$OUT/version.txt"
[ -s "$OUT/version.txt" ]

Xvfb :77 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:77 GDK_BACKEND=x11

# The app draws its own window into a PNG (the "screenshot" debug command); the X server's view of the screen
# is saved too when xwd and netpbm are available.
shot() {
  cmd "screenshot $OUT/$1.png"
  if command -v xwd >/dev/null && command -v xwdtopnm >/dev/null; then
    xwd -root -silent | xwdtopnm 2>/dev/null | pnmtopng > "$OUT/$1-screen.png" 2>/dev/null || true
  fi
}

cmd() {
  gdbus call --session --timeout "${SMOKE_TIMEOUT:-120}" --dest io.github.x_vlad_x.Contrail \
    --object-path /io/github/x_vlad_x/Contrail/Debug \
    --method org.gtk.Actions.Activate debug "[<'$1'>]" '{}' >/dev/null
  sleep "${2:-3}"
}

CONTRAIL_DEBUG=1 "$@" > "$OUT/contrail.log" 2>&1 &
APP=$!
# The program is up once its debug actions answer (under emulation this can take minutes).
i=0
until gdbus introspect --session --dest io.github.x_vlad_x.Contrail \
        --object-path /io/github/x_vlad_x/Contrail/Debug 2>/dev/null | grep -q org.gtk.Actions; do
  i=$((i + 1))
  if [ $i -gt 600 ] || ! kill -0 $APP 2>/dev/null; then
    echo "Contrail did not start; log:" >&2
    tail -n 50 "$OUT/contrail.log" >&2
    exit 1
  fi
  sleep 1
done
sleep "${SMOKE_SETTLE:-5}"
shot 1-sign-in
cmd demo 8
shot 2-chats
cmd "chat 0"
shot 3-chat
cmd back
cmd demo-sponsored
for n in 1 2 3; do
  cmd "chat $n"
  shot 4-chat-$n
  cmd back
done
cmd about
shot 5-about

kill -TERM $APP
wait $APP || true
kill $XVFB 2>/dev/null || true
grep -E "Screenshot .*(saved|FAILED)" "$OUT/contrail.log" || true
# The program ran every command and quit cleanly; some screenshot exists.
grep -q "Debug command: about" "$OUT/contrail.log"
ls "$OUT"/*.png
