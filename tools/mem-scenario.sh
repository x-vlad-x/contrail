#!/usr/bin/env bash
# Memory of the TDLib version on the scenario transport, here on the PC (xvfb, no account):
# start -> a chat with photos scrolled to its beginning -> the chat closed -> a while later -> malloc_trim.
#   tools/mem-scenario.sh [binary] [VAR=value...]       e.g. GSK_RENDERER=cairo, LD_PRELOAD=...
#   IDLE=seconds (30), SCROLLS=steps (80), CONTRAIL_SCENARIO=messages in a chat (400)
# Prints anonymous memory, PSS, threads and what malloc holds at each point, as tools/phone.sh td-mem-run does
# on the phone. The pictures it shows are made once in ~/.cache/contrail-scenario.
set -euo pipefail
BIN=$(realpath "${1:-build-td/agent-chat/contrail}")
shift || true
for a in "$@"; do
  [[ $a == *=* ]] || { echo "not VAR=value: $a" >&2; exit 2; }
done
# Built in the Debian container: a library named otherwise here (libjpeg.so.62 is libjpeg.so.8 on Arch) comes from
# that container, into ~/.cache/contrail-scenario/libs.
missing=$(ldd "$BIN" | awk '/not found/{print $1}')
if [[ -n $missing ]]; then
  LIBS=~/.cache/contrail-scenario/libs
  mkdir -p "$LIBS"
  for lib in $missing; do
    [[ -e $LIBS/$lib ]] || podman run --rm -v "$LIBS":/out:z localhost/contrail-desktop \
      sh -c "cp -L /usr/lib/x86_64-linux-gnu/$lib /out/" || { echo "no $lib in localhost/contrail-desktop" >&2; exit 1; }
  done
  export LD_LIBRARY_PATH=$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi
if [[ -z ${CONTRAIL_MEM_INNER:-} ]]; then
  exec env CONTRAIL_MEM_INNER=1 dbus-run-session -- xvfb-run -a -s '-screen 0 420x900x24' "$0" "$BIN" "$@"
fi
DATA=$(mktemp -d)
LOG=$(mktemp /tmp/mem-scenario.XXXXXX.log)
P=
trap '[[ -n $P ]] && kill "$P" 2>/dev/null; rm -rf "$DATA"; echo "log: $LOG"' EXIT
export CONTRAIL_SCENARIO=${CONTRAIL_SCENARIO:-400} CONTRAIL_SCENARIO_PHOTOS=1 CONTRAIL_DEBUG=1 G_MESSAGES_DEBUG=all \
  GSETTINGS_BACKEND=memory GTK_A11Y=none XDG_DATA_HOME=$DATA LANG=C.UTF-8 ADW_DISABLE_PORTAL=1 GDK_BACKEND=x11
env "$@" "$BIN" > "$LOG" 2>&1 &
P=$!
c() {
  gdbus call --session --dest io.github.x_vlad_x.Contrail.Devel --object-path /io/github/x_vlad_x/Contrail/Devel \
    --method org.gtk.Actions.Activate debug "[<'$1'>]" '{}' > /dev/null
  sleep "${2:-0.3}"
}
m() {
  awk -v at="$1" '/^Pss:/{p=$2} /^Anonymous:/{a=$2} /^Swap:/{s=$2}
    END{printf "%-28s anon+swap %6.1f MiB  pss %6.1f MiB", at, (a+s)/1024, p/1024}' "/proc/$P/smaps_rollup"
  echo "  threads $(ls "/proc/$P/task" | wc -l)  heap $(awk '/\[heap\]/{f=1} f&&/^Pss:/{printf "%.1f", $2/1024; exit}' "/proc/$P/smaps") MiB"
  local before
  before=$(grep -c 'mem:' "$LOG" || true)
  c mem 0
  for _ in $(seq 1 50); do
    (( $(grep -c 'mem:' "$LOG" || true) > before )) && break
    sleep 0.1
  done
  grep 'mem:' "$LOG" | tail -1 | sed 's/^.*mem:/    malloc:/'
}
# The pictures are made and the chats are there once it answers on the bus.
for _ in $(seq 1 100); do
  gdbus introspect --session --dest io.github.x_vlad_x.Contrail.Devel --object-path /io/github/x_vlad_x/Contrail/Devel \
    > /dev/null 2>&1 && break
  kill -0 "$P" 2>/dev/null || { echo "it exited:"; tail -5 "$LOG"; exit 1; }
  sleep 0.2
done
sleep 3
m start
c 'chat 0' 2
m 'chat open'
for _ in $(seq 1 "${SCROLLS:-80}"); do c 'scroll -3000' 0.25; done
sleep 2
m 'chat scrolled to the top'
c back 1
m 'chat closed, 1 s'
sleep 3
m 'chat closed, 4 s (trimmed)'
sleep "${IDLE:-30}"
m "after ${IDLE:-30} s more"
c trim 1
m 'after another malloc_trim'
c close 2
wait "$P" || true
P=
grep -E 'CRITICAL|WARNING' "$LOG" | grep -v 'language pack' | head -5 || true
grep -o 'Memory: trimmed.*' "$LOG" || true
