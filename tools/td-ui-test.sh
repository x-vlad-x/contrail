#!/usr/bin/env bash
# Chat behaviour of the TDLib version on the scenario transport (xvfb, no account), checked from its debug state:
#   1. reaching the start of a chat leaves the screen where it was (the reserve above goes without a jump to the end);
#   2. only the rows near the screen keep their photos decoded, and photos far off come back the same;
#   3. the decodes of a chat that closed don't go on;
#   4. a group with topics: its list of topics, a topic with only its messages, a message sent and one received
#      landing only in their topics, the list closed when the group stops having topics;
#   5. forwarding: the chats to choose from (Saved Messages first, a search), a topic asked for, the message
#      waiting in the chosen place, sent with a comment and the sender's name, or alone as a copy.
#   tools/td-ui-test.sh [binary]        default build-td/agent-chat/contrail
set -euo pipefail
BIN=$(realpath "${1:-build-td/agent-chat/contrail}")
# Built in the Debian container: what is named otherwise here comes from that container (as mem-scenario.sh does).
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
if [[ -z ${CONTRAIL_UI_INNER:-} ]]; then
  exec env CONTRAIL_UI_INNER=1 dbus-run-session -- xvfb-run -a -s '-screen 0 420x900x24' "$0" "$BIN"
fi
command -v import > /dev/null && command -v compare > /dev/null || { echo "needs ImageMagick (import, compare)" >&2; exit 2; }
WORK=$(mktemp -d)
P=
trap '[[ -n $P ]] && kill "$P" 2>/dev/null; rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $*"; echo "log: $WORK/log.txt (kept)"; trap - EXIT; [[ -n $P ]] && kill "$P" 2>/dev/null; exit 1; }

start() {
  mkdir -p "$WORK/data"
  CONTRAIL_SCENARIO=$1 CONTRAIL_SCENARIO_PHOTOS=1 CONTRAIL_DEBUG=1 G_MESSAGES_DEBUG=all GSETTINGS_BACKEND=memory \
    GTK_A11Y=none XDG_DATA_HOME=$WORK/data LANG=C.UTF-8 ADW_DISABLE_PORTAL=1 GDK_BACKEND=x11 \
    "$BIN" > "$WORK/log.txt" 2>&1 &
  P=$!
  for _ in $(seq 100); do
    gdbus introspect --session --dest io.github.x_vlad_x.Contrail.Devel --object-path /io/github/x_vlad_x/Contrail/Devel \
      > /dev/null 2>&1 && break
    kill -0 "$P" 2>/dev/null || fail "it exited: $(tail -3 "$WORK/log.txt")"
    sleep 0.2
  done
  sleep 2
}
stop() { kill "$P" 2>/dev/null || true; wait "$P" 2>/dev/null || true; P=; rm -rf "$WORK/data"; }
c() {
  gdbus call --session --dest io.github.x_vlad_x.Contrail.Devel --object-path /io/github/x_vlad_x/Contrail/Devel \
    --method org.gtk.Actions.Activate debug "[<'$1'>]" '{}' > /dev/null
  sleep "${2:-0.3}"
}
# One field of the chat state, asked for now.
state() {
  local before
  before=$(grep -c 'chat state:' "$WORK/log.txt" || true)
  c state 0
  for _ in $(seq 50); do (( $(grep -c 'chat state:' "$WORK/log.txt" || true) > before )) && break; sleep 0.1; done
  local value
  value=$(grep 'chat state:' "$WORK/log.txt" | tail -1 | sed -n "s/.* $1=\([-0-9]*\).*/\1/p")
  [[ -n $value ]] || fail "no $1 in the chat state"
  echo "$value"
}
# The newest log line with $1, after asking for it with the debug command $2.
asked() {
  local before
  before=$(grep -c "$1" "$WORK/log.txt" || true)
  c "$2" 0
  for _ in $(seq 50); do (( $(grep -c "$1" "$WORK/log.txt" || true) > before )) && break; sleep 0.1; done
  grep "$1" "$WORK/log.txt" | tail -1
}

echo "1. The start of a chat"
start 150
c 'chat 0' 3
reached=0
for _ in $(seq 12); do
  c 'scroll -3000' 0.6
  if [[ $(state top) == 1 && $(state reserve) == 0 ]]; then reached=1; break; fi
done
(( reached )) || fail "the start of the chat wasn't reached"
value=$(state value) upper=$(state upper) page=$(state page)
[[ $(state stick) == 0 ]] || fail "it sticks to the bottom after reaching the start"
(( value + page < upper - 1000 )) || fail "the screen went to the end of the chat (value=$value upper=$upper)"
echo "   ok: value=$value of $upper"
stop

echo "2. Photos near the screen and far off"
start 400
c 'chat 0' 3
for _ in $(seq 6); do c 'scroll -600' 0.4; done
sleep 2
import -window root "$WORK/before.png"
for _ in $(seq 20); do c 'scroll -1500' 0.3; done
sleep 2
rows=$(state rows) near=$(state nearRows)
(( near > 0 && near * 2 < rows )) || fail "rows with photos decoded: $near of $rows"
echo "   ok: $near of $rows rows hold their photos"
for _ in $(seq 20); do c 'scroll 1500' 0.3; done
sleep 1.5
import -window root "$WORK/back.png"
diff=$(compare -metric AE -fuzz 2% "$WORK/before.png" "$WORK/back.png" null: 2>&1 | cut -d' ' -f1) || true
[[ $diff =~ ^[0-9.e+]+$ ]] || fail "compare: $diff"
diff=$(LC_ALL=C printf '%.0f' "$diff")
(( diff < 200 )) || fail "the screen differs after coming back: $diff pixels"
echo "   ok: back where it was, $diff pixels differ"

echo "3. A closed chat's decodes"
# A fling's worth of photos asked for, and the chat closed before they are all decoded.
for _ in $(seq 8); do c 'scroll -3000' 0.05; done
c back 0.2
decodes=$(grep -c 'Picture: [0-9]' "$WORK/log.txt" || true)
sleep 3
later=$(grep -c 'Picture: [0-9]' "$WORK/log.txt" || true)
# Only the ones already running finish: two decode threads.
(( later - decodes <= 2 )) || fail "pictures still decoded after the chat closed: $((later - decodes))"
echo "   ok: $((later - decodes)) decodes finished after closing"
stop

echo "4. Topics of a group"
start 120
# The scenario's group with topics is below the 40 friends.
c 'chat 40' 2
line=$(asked 'topics chat=' topic-state)
[[ $line == *": [2 pinned unread=0] [4 muted unread=0] [3 unread=2] [1 unread=0]" ]] || fail "topics: $line"
c 'topic 2' 2
line=$(asked 'chat topic=' state)
[[ $line == *"topic=3 topics=3 "* ]] || fail "topic 3 holds other messages: $line"
c 'send hello topic' 1
line=$(asked 'chat topic=' state)
[[ $line == *'topics=3 newestText="hello topic"'* ]] || fail "sent in topic 3: $line"
# Somebody writes in General while topic 3 is open.
c 'send /in 1 ping general' 1
line=$(asked 'chat topic=' state)
[[ $line == *'topics=3 newestText="hello topic"'* ]] || fail "a message of General in topic 3: $line"
line=$(asked 'topics chat=' topic-state)
[[ $line == *"[3 unread=0 current]"*"[1 unread=1]"* ]] || fail "topics after the messages: $line"
c 'topic 2' 2
line=$(asked 'chat topic=' state)
[[ $line == *'topic=1 topics=1 newestText="ping general"'* ]] || fail "General: $line"
c 'send /unforum' 1.5
line=$(asked 'list shown=' list-state)
[[ $line == *"selected=-1"* ]] || fail "the topic is still open: $line"
before=$(grep -c 'topics chat=' "$WORK/log.txt" || true)
c topic-state 1
(( $(grep -c 'topics chat=' "$WORK/log.txt" || true) == before )) || fail "the topics stay without a forum"
echo "   ok"
stop

echo "5. Forwarding"
start 120
# Friend 2's newest message, 120, is this account's.
c 'chat 1' 3
c forward 1
line=$(asked 'forward picker:' forward-picker)
[[ $line == *'page=chats forum=0 shown=42 messages=1 rows: "Saved Messages" "Friend 1" "Friend 2" '* ]] \
  || fail "the chats to forward to: $line"
c 'forward-search club' 0.5
line=$(asked 'forward picker:' forward-picker)
[[ $line == *'shown=1 messages=1 rows: "Contrail Club"' ]] || fail "the search: $line"
c forward-enter 1.5
line=$(asked 'forward picker:' forward-picker)
# The closed topic is not offered.
[[ $line == *'page=topics forum=-400 shown=3 messages=1 rows: "Help" "Random" "General"' ]] || fail "topics: $line"
c 'forward-pick-topic 1' 2
line=$(asked 'chat topic=' state)
[[ $line == *"topic=3 "* ]] || fail "Random isn't open: $line"
line=$(asked 'chat forward:' state)
[[ $line == *'waiting=1 names=1 bar=1 sendable=1 title="Forward Message" text="Vlad: Message 120"'* ]] \
  || fail "the bar: $line"
# It waits there while other chats are open.
c 'chat 0' 2
c 'chat 40' 2
c 'topic 2' 2
line=$(asked 'chat forward:' state)
[[ $line == *"waiting=1 names=1 bar=1 "* ]] || fail "gone after leaving: $line"
c 'send with a comment' 1.5
line=$(asked 'chat forward:' state)
[[ $line == *'waiting=0 '*' ["with a comment"] ["Message 120" from="Vlad"]' ]] || fail "sent with a comment: $line"
line=$(asked 'chat topic=' state)
[[ $line == *"topic=3 topics=3 "* ]] || fail "not in the topic: $line"
# Without the names and without a comment: a copy.
c 'chat 0' 2
c forward 1
c 'forward-search friend 3' 0.5
c forward-enter 2
c 'forward-mode no-names' 0.5
line=$(asked 'chat forward:' state)
[[ $line == *'waiting=1 names=0 bar=1 sendable=1 title="Forward Message" text="Sender names removed: Message 120"'* ]] \
  || fail "no names: $line"
c send 1.5
line=$(asked 'chat forward:' state)
[[ $line == *'waiting=0 '*' ["Message 120"]' && $line != *from=* ]] || fail "sent as a copy: $line"
echo "   ok"
stop
echo "PASS"
