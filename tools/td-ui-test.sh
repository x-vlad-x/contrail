#!/usr/bin/env bash
# Chat behaviour of the TDLib version on the scenario transport (xvfb, no account), checked from its debug state:
#   1. reaching the start of a chat leaves the screen where it was (the reserve above goes without a jump to the end);
#   2. only the rows near the screen keep their photos decoded, and photos far off come back the same;
#   3. the decodes of a chat that closed don't go on;
#   4. a group with topics: its list of topics, a topic with only its messages, a message sent and one received
#      landing only in their topics, the list closed when the group stops having topics;
#   5. forwarding: the chats to choose from (Saved Messages first, a search), a topic asked for, the message
#      waiting in the chosen place, sent with a comment and the sender's name, or alone as a copy;
#   6. pinned messages: the bar shows the newest, a tap goes back through them and round, the picture in it keeps
#      its size, the list of them, unpinning and pinning with the questions, a bar hidden in a topic where only
#      admins pin until a new message is pinned there;
#   7. reactions: the chips under messages, a tap setting and taking back, one reaction at a time, the double tap's
#      quick one, custom emoji as pictures, the menu's strip and all of them, who reacted in a group, the heart in
#      the list going once the message is seen, and a channel scrolled while reactions keep coming without building
#      a row again;
#   8. reading and writing: a channel's sponsored message (seen, its button counted), a video's progress and its
#      opening by itself, a file stopped and opened with a second tap, a GIF and a sticker opened, links, mentions,
#      a spoiler and a bot command among emoji, Save and Copy in the menu, a menu closed with its chat, a failed
#      send sent again, a slow one that doesn't move when confirmed, the up arrow, deleting only here.
#   9. places: a place, a venue and a live location in a chat with their maps, a location's map with the distance,
#      the picker sending the device's position, a place nearby, a place found by name and a point on the map;
#      live locations: the bar, one's own sent, moved, on the shared map and stopped there, and going on after a
#      restart; the chat's card with a group's members, a member's card, muting from it; the contacts with a
#      search; About;
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
  mkdir -p "$WORK/data" "$WORK/config"
  # Saved files go here, not to the user's folders.
  printf 'XDG_DOWNLOAD_DIR="%s/downloads"\nXDG_PICTURES_DIR="%s/pictures"\n' "$WORK" "$WORK" > "$WORK/config/user-dirs.dirs"
  CONTRAIL_SCENARIO=$1 CONTRAIL_SCENARIO_PHOTOS=1 CONTRAIL_DEBUG=1 G_MESSAGES_DEBUG=all GSETTINGS_BACKEND=memory \
    XDG_CONFIG_HOME=$WORK/config \
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
[[ $line == *'page=chats forum=0 shown=43 messages=1 rows: "Saved Messages" "Friend 1" "Friend 2" '* ]] \
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
echo "6. Pinned messages"
start 120
c 'chat 0' 3
pinned() { asked 'chat pinned:' pinned-state; }
line=$(pinned)
[[ $line == *'ids=[30,70,100] total=3 current=100 '*'title="Pinned message" text="Message 100" preview=0 '*' list=1 close=0' ]] \
  || fail "the bar: $line"
height=$(sed -n 's/.* bar=[0-9]*x\([0-9]*\) .*/\1/p' <<< "$line")
c pinned-bar 1.5
line=$(pinned)
[[ $line == *'current=70 clicked=100 '*'title="Pinned message #2" text="Photo, Pinned photo" preview=1 36x36 picture=36x36 '* ]] \
  || fail "the second tap: $line"
[[ $line == *" bar=410x$height "* ]] || fail "the picture changed the bar's height ($height): $line"
c pinned-bar 1.5
line=$(pinned)
[[ $line == *'current=30 clicked=70 '*'title="Pinned message #1" text="Message 30" preview=0 '* ]] || fail "the oldest: $line"
c pinned-bar 1.5
line=$(pinned)
[[ $line == *'current=100 clicked=30 '* ]] || fail "not round to the newest: $line"
# Jumping down to the newest goes on with the one before it.
c pinned-bar 1.5
line=$(pinned)
[[ $line == *'current=70 clicked=100 '* ]] || fail "after the newest: $line"
c pinned-list 1.5
line=$(asked 'pinned list:' pinned-list-state)
[[ $line == *'title="3 pinned messages" rows=3 action="Unpin all 3 messages" '*'"Message 100"]'*'"Photo, Pinned photo"]'*'"Message 30"]' ]] \
  || fail "the list: $line"
c 'pinned-list-pick 2' 2
line=$(pinned)
[[ $line == *'clicked=30 '* ]] || fail "picked from the list: $line"
c 'unpin 100' 1
line=$(asked 'dialog:' dialog)
[[ $line == *'heading="Would you like to unpin this message?"'* ]] || fail "the question: $line"
c 'dialog-answer unpin' 1
line=$(pinned)
[[ $line == *'ids=[30,70] total=2 '* ]] || fail "unpinned: $line"
c 'scroll bottom' 1
c 'pin 110' 1
line=$(asked 'dialog:' dialog)
[[ $line == *'heading="Would you like to pin this message?" body="" check="Also pin for Friend" checked=0' ]] \
  || fail "the question: $line"
c 'dialog-answer pin' 1.5
line=$(pinned)
[[ $line == *'ids=[30,70,110] total=3 current=110 '*'text="Message 110"'* ]] || fail "pinned: $line"
# Only admins pin in the group: the bar can be hidden, until something new is pinned.
c 'chat 40' 2
c 'topic 2' 2
line=$(pinned)
[[ $line == *'ids=[103] total=1 current=103 clicked=0 shown=1 hidden=0 canPin=0 '*' close=1' ]] || fail "in the topic: $line"
c pinned-close 1
line=$(asked 'dialog:' dialog)
[[ $line == *'body="Do you want to hide the pinned message bar?'* ]] || fail "the question: $line"
c 'dialog-answer hide' 1
line=$(pinned)
[[ $line == *'shown=0 hidden=1 '* ]] || fail "not hidden: $line"
c 'chat 0' 2
c 'chat 40' 2
c 'topic 2' 2
line=$(pinned)
[[ $line == *'ids=[103] '*'shown=0 hidden=1 '* ]] || fail "back after leaving: $line"
c 'send /pin 20' 1.5
line=$(pinned)
[[ $line == *'ids=[103,203] total=2 current=203 clicked=0 shown=1 hidden=0 '* ]] || fail "a new one pinned: $line"
echo "   ok"
stop
echo "7. Reactions"
start 120
# The chat list's heart: Friend 2's newest message has a new reaction.
hearts() {
  c tree 1
  awk '/Message: .* tree$/ { n = NR } { line[NR] = $0 } END { for (i = n; i <= NR; ++i) print line[i] }' "$WORK/log.txt" \
    | grep -c 'GtkLabel "❤" ' || true
}
(( $(hearts) == 1 )) || fail "no heart in the list"
react() { asked 'chat reactions:' "reactions-state $1"; }
c 'chat 0' 3
line=$(react 112)
[[ $line == *'message=112 [ 👍=1 ❤=1* ] chips=[👍 avatar][❤* avatar] '* ]] || fail "the chips: $line"
builds=$(sed -n 's/.* builds=\([0-9]*\) .*/\1/p' <<< "$line")
c 'react 👍 112' 0.5
line=$(react 112)
# One reaction at a time: the heart goes.
[[ $line == *'[ 👍=2* ] chips=[👍* avatar avatar] '*" builds=$builds rebuilds=0 "* ]] || fail "a tap on the chip: $line"
c 'react 👍 112' 0.5
line=$(react 112)
[[ $line == *'[ 👍=1 ] chips=[👍 avatar] '* ]] || fail "taken back: $line"
c 'react-quick 118' 0.5
line=$(react 118)
[[ $line == *'[ ❤=1* ] '*' quick=❤ '* ]] || fail "the quick reaction: $line"
c 'react-quick 118' 0.5
line=$(react 118)
[[ $line == *'[ ] chips= '* ]] || fail "the quick reaction taken back: $line"
line=$(react 117)
[[ $line == *'chips=[👍 avatar][custom:5000000001 picture=18px@18x18 avatar] '* ]] || fail "a custom emoji: $line"
line=$(react 116)
[[ $line == *'chips=[custom:5000000002 picture=18px@18x18 avatar] '* ]] || fail "an animated custom emoji: $line"
line=$(react 115)
[[ $line == *'chips=[custom:5000000003 alt=🐟'*' avatar] '* ]] || fail "a custom emoji without a still picture: $line"
c 'menu 117' 1
line=$(asked 'chat reactions menu:' 'reactions-state 117')
[[ $line == *': 6 [👍][❤][🔥][🥰][👏][😁]' ]] || fail "the menu: $line"
c 'reactions-more 117' 1
line=$(asked 'chat reactions all:' 'reactions-state 117')
[[ $line == *': 10 [👍][❤][🔥][🥰][👏][😁][🤔][🤯][😱][🎉]' ]] || fail "all of them: $line"
# Seen: the heart goes (the dialog goes with the chat).
c 'chat 1' 3
line=$(react 120)
[[ $line == *'[ 🔥=1 ] '*' unread=0 chatUnread=0 '* ]] || fail "not read: $line"
c back 1
(( $(hearts) == 0 )) || fail "the heart stays"
# Who reacted, in a group.
c 'chat 40' 2
c 'topic 0' 2
line=$(react 292)
[[ $line == *'[ 👍=2 ] chips=[👍 avatar avatar] who=1 '* ]] || fail "in the group: $line"
c 'reacted 292' 1
line=$(asked 'chat reactions who:' 'reactions-state 292')
[[ $line == *'title="2 Reacted" rows: "Clara M" "Anna K"' ]] || fail "who reacted: $line"
stop
# A busy channel: reactions come to its posts all the time while it is scrolled through.
start 400
c 'chat 41' 3
for _ in $(seq 12); do c 'scroll -1500' 0.4; done
for _ in $(seq 12); do c 'scroll 1500' 0.4; done
sleep 1
line=$(react 400)
rows=$(sed -n 's/.* rows=\([0-9]*\) .*/\1/p' <<< "$line")
builds=$(sed -n 's/.* builds=\([0-9]*\) .*/\1/p' <<< "$line")
bars=$(sed -n 's/.* bars=\([0-9]*\) .*/\1/p' <<< "$line")
updates=$(sed -n 's/.* updates=\([0-9]*\) .*/\1/p' <<< "$line")
[[ $line == *' rebuilds=0 '* ]] || fail "rows built again: $line"
(( updates > 50 && bars == builds && rows > 100 )) || fail "the chips made again: $line"
echo "   ok: $updates reactions came to $rows rows, none built again"
stop
echo "8. Reading and writing: a sponsored message, media, links, the menu, failed and slow sends"
start 120
c 'chat 42' 3
line=$(asked 'chat sponsored:' sponsored)
[[ $line == *'received=1 waiting=0 placed=1 after=20 shown=1 viewed=1 clicks=0' ]] || fail "the sponsored message: $line"
grep -q 'Scenario: sponsored message 5 viewed' "$WORK/log.txt" || fail "the sponsored message wasn't counted as seen"
c 'menu -1' 1.5
line=$(asked 'chat menu:' menu-state)
[[ $line == *'open=0 '* ]] || fail "the sponsored message has a menu: $line"
# Its button: the click is counted, then its t.me link opens the chat it leads to.
c 'tap -1' 2
grep -q 'Scenario: sponsored message 5 clicked, media=0' "$WORK/log.txt" || fail "the click wasn't counted"
[[ $(asked 'chat topic=' state) == *' id=1' ]] || fail "the sponsored link didn't open Friend 1"
c back 1
c 'chat 42' 3
# A video: how much of it came, then it opens by itself.
c 'tap 4' 0.6
line=$(asked 'chat media:' 'media 4')
[[ $line == *' downloading '*' opens status="'*' MB · '* ]] || fail "the video's progress: $line"
sleep 5
grep -q 'open: file://.*showcase-3000001.bin' "$WORK/log.txt" || fail "the video didn't open by itself"
# A file: a tap downloads it, another while it comes stops it; once here a second tap opens it.
c 'tap 14' 0.6
line=$(asked 'chat media:' 'media 14')
[[ $line == *' downloading '*'document="'*' MB · '*'progress='*'icon=process-stop-symbolic'* ]] || fail "the file's progress: $line"
c 'tap 14' 0.5
line=$(asked 'chat media:' 'media 14')
[[ $line == *' stopped '* && $line != *progress=* ]] || fail "the download didn't stop: $line"
c 'tap 14' 3.5
grep -q 'showcase-3000004' "$WORK/log.txt" && fail "the file opened without a second tap"
c 'tap 14' 0.5
grep -q 'open: file://.*showcase-3000004.bin' "$WORK/log.txt" || fail "the file didn't open"
# A GIF and a sticker go to the system's player and viewer.
c 'tap 5' 3.5
grep -q 'open: file://.*showcase-3000002.bin' "$WORK/log.txt" || fail "the GIF didn't open"
c 'tap 6' 0.5
grep -q 'open: file://.*sticker.png' "$WORK/log.txt" || fail "the sticker didn't open"
# Entities among emoji are where they belong; a spoiler opens with a tap; links lead to their chats.
line=$(asked 'chat tap:' 'tap-text 2 a tg link')
[[ $line == *'tg://resolve?domain=friend2 "a tg link"' ]] || fail "the tg link: $line"
sleep 1.5
[[ $(asked 'chat topic=' state) == *' id=2' ]] || fail "the tg link didn't open Friend 2"
c back 1
c 'chat 42' 3
line=$(asked 'chat tap:' 'tap-text 2 t.me/friend1/5')
[[ $line == *'https://t.me/friend1/5 "t.me/friend1/5"' ]] || fail "the message link: $line"
sleep 1.5
[[ $(asked 'chat topic=' state) == *' id=1' && $(state oldest) == 1 ]] || fail "the message link didn't open message 5"
c back 1
c 'chat 42' 3
for part in 'a link' '/help' '@friend1'; do
  line=$(asked 'chat tap:' "tap-text 2 $part")
  [[ $line == *"\"$part\"" ]] || fail "$part isn't where it belongs: $line"
done
grep -q 'open: https://example.org/page' "$WORK/log.txt" || fail "the web link didn't open"
sleep 1.5
[[ $(asked 'chat topic=' state) == *' id=1' ]] || fail "the mention didn't open Friend 1"
c back 1
c 'chat 42' 3
line=$(asked 'chat tap:' 'tap-text 18 hidden')
[[ $line == *'spoiler "hidden words"' ]] || fail "the spoiler: $line"
line=$(asked 'chat tap:' 'tap-text 18 hidden')
[[ $line == *'no link with "hidden"'* ]] || fail "the spoiler stays hidden: $line"
c 'tap-text 18 Friend 2' 1.5
[[ $(asked 'chat topic=' state) == *' id=2' ]] || fail "the mention didn't open Friend 2"
c back 1
c 'chat 42' 3
# The menu: a photo saved to Pictures, a file to Downloads, a text copied; a sticker isn't saved.
c 'menu 20' 1.5
[[ $(asked 'chat menu:' menu-state) == *'[Save As...=msg.save]'* ]] || fail "no Save for a photo"
c 'menu-do save' 1
grep -q 'toast: photo_.*\.jpg' "$WORK/log.txt" || fail "the photo wasn't saved"
ls "$WORK"/pictures/photo_*.jpg > /dev/null 2>&1 || fail "no photo in Pictures"
c 'menu 14' 1.5
c 'menu-do save' 1
grep -q 'toast: Image was saved to your Downloads folder' "$WORK/log.txt" || fail "the file wasn't saved"
[[ -f $WORK/downloads/report.pdf ]] || fail "no report.pdf in Downloads"
c 'menu 6' 1.5
[[ $(asked 'chat menu:' menu-state) != *'msg.save'* ]] || fail "a sticker can be saved"
c 'menu 15' 1.5
c 'menu-do copy' 0.5
grep -q 'toast: Text copied to clipboard.' "$WORK/log.txt" || fail "no toast for a copied text"
# A menu open when the chat closes goes with it.
c 'menu 18' 1.5
c back 1
grep -q 'Finalizing GtkListBoxRow' "$WORK/log.txt" && fail "a row was finalized with its menu"
# Writing: a send that fails is shown and sent again; one the server confirms later stays where it is.
c 'chat 0' 3
c 'send /fail broken message' 1
line=$(asked 'chat media:' 'media 0')
[[ $line == *'local=1 failed=1 '*'status=dialog-error-symbolic'* ]] || fail "the failed send: $line"
c 'menu 0' 1.5
[[ $(asked 'chat menu:' menu-state) == *'[Retry=msg.retry]'* ]] || fail "no Retry"
c 'menu-do retry' 1
[[ $(asked 'chat media:' 'media 0') == *'local=0 failed=0 '*'status=contrail-check-symbolic'* ]] || fail "not sent again"
c 'send /slow slow message' 0.2
before=$(asked 'chat media:' 'media 0')
[[ $before == *'local=1 '*'status=contrail-clock-symbolic'* ]] || fail "the slow send: $before"
sleep 2
after=$(asked 'chat media:' 'media 0')
[[ $after == *'local=0 '* ]] || fail "not confirmed: $after"
[[ $(sed -n 's/.* y=\([-0-9]*\) .*/\1/p' <<< "$before") == $(sed -n 's/.* y=\([-0-9]*\) .*/\1/p' <<< "$after") ]] \
  || fail "the message moved when confirmed: $before / $after"
c 'edit-last' 1
[[ $(state editing) == 122 ]] || fail "the up arrow doesn't edit the last message"
c 'send edited' 1
c 'menu 120' 1.5
c 'menu-do delete' 1
[[ $(asked 'dialog:' dialog) == *'check="Delete for everyone" checked=1' ]] || fail "no choice for whom to delete"
c 'dialog-check 0' 0.2
c 'dialog-answer delete' 1
(( $(state messages) == 101 )) || fail "not deleted"
# A bot command in a message is sent with a tap.
c 'chat 2' 3
c 'tap-text 120 /help' 1.5
[[ $(asked 'chat topic=' state) == *'newestText="/help"'* ]] || fail "the bot command wasn't sent"
# A hashtag: the chat list searches for it.
c back 1
c 'chat 42' 3
c 'tap-text 2 #contrail' 1
c tree 0.5
grep -q 'GtkSearchEntry "#contrail"' "$WORK/log.txt" || fail "the hashtag isn't searched for"
stop

# Section places: locations in a chat, their map, the picker.
section_places() {
  echo "9. Places"
  start 150
  c 'chat 43' 3
  [[ $(asked 'chat media:' 'media 2') == *'location map='*' shown'* ]] || fail "no map of the place: $(asked 'chat media:' 'media 2')"
  [[ $(asked 'chat media:' 'media 4') == *'live="updated 2 minutes ago" left="4'?'m"'* ]] \
    || fail "the live location: $(asked 'chat media:' 'media 4')"
  c tree 0.5
  grep -q 'GtkLabel "Red Square"' "$WORK/log.txt" || fail "no venue's name"
  # A place on the map: how far it is only after "my location".
  c 'tap 2' 1
  [[ $(asked 'Location viewer:' loc-state) == *'"Location" "" details="55.75580, 37.61730" distance="" '*'watching=0'* ]] \
    || fail "the place's map: $(grep 'Location viewer:' "$WORK/log.txt" | tail -1)"
  c loc-mine 0.3
  c 'loc-fix 55.7558 37.6273 8' 0.3
  [[ $(asked 'Location viewer:' loc-state) == *'distance="626 m away"'*'watching=1'* ]] \
    || fail "no distance: $(grep 'Location viewer:' "$WORK/log.txt" | tail -1)"
  c loc-close 0.5
  c 'tap 4' 1
  [[ $(asked 'Location viewer:' loc-state) == *'"Live Location" "Boris Places" details="updated 2 minutes ago"'* ]] \
    || fail "the live location's map: $(grep 'Location viewer:' "$WORK/log.txt" | tail -1)"
  c loc-close 0.5
  # The picker: the device's position, places nearby, a place found by name, a point on the map.
  c loc-picker 1
  [[ $(asked 'Location picker:' loc-state) == *'sensitive=1'*'watching=1'* ]] \
    || fail "the picker: $(grep 'Location picker:' "$WORK/log.txt" | tail -1)"
  c 'loc-fix 55.7000 37.6000 12' 1.5
  [[ $(asked 'Location picker:' loc-state) == *'send="Send My Current Location" "Accurate to 12 m" sensitive=1 pin=0 follow=1 venues=3'* ]] \
    || fail "the position in the picker: $(grep 'Location picker:' "$WORK/log.txt" | tail -1)"
  c 'loc-venue 1' 1
  [[ $(asked 'chat media:' 'media 6') == *'location map='* ]] || fail "the place nearby wasn't sent"
  grep -q 'Scenario: sent to Places:  55.70200 37.60000 "City Park"' "$WORK/log.txt" || fail "not the place chosen"
  c loc-picker 1
  c 'loc-search Pizza' 1.5
  grep -q 'Scenario: places near 55.70000 37.60000 "Pizza"' "$WORK/log.txt" || fail "no search by name"
  c 'loc-move 55.8000 37.7000' 1.5
  [[ $(asked 'Location picker:' loc-state) == *'send="Send This Location" "55.80000, 37.70000" sensitive=1 pin=1 follow=0'* ]] \
    || fail "a point on the map: $(grep 'Location picker:' "$WORK/log.txt" | tail -1)"
  c loc-send 1
  grep -q 'Scenario: sent to Places:  55.80000 37.70000 "" period 0' "$WORK/log.txt" || fail "the point wasn't sent"
  c 'loc-state' 0.3
  grep -q 'Location: no dialog shown' "$WORK/log.txt" || fail "the picker stayed"
  # Live locations: Boris's in the bar, one's own started from the picker, moved, seen on the shared map, stopped there.
  [[ $(asked 'Live bar:' live-bar) == *'revealed=1 text="Boris" stop=0 active=1' ]] || fail "no bar with Boris"
  c loc-picker 1
  c 'loc-fix 55.7000 37.6000 12' 1
  c loc-live 1
  c 'dialog-answer 15m' 1.5
  [[ $(asked 'Live bar:' live-bar) == *'text="Boris and you" stop=1 active=2' ]] \
    || fail "one's own live location isn't in the bar: $(grep 'Live bar:' "$WORK/log.txt" | tail -1)"
  [[ $(asked 'chat media:' 'media 8') == *'live="updated just now" left="1'?'m"'* ]] \
    || fail "the live location sent: $(grep 'chat media:' "$WORK/log.txt" | tail -1)"
  c live-update 0.3
  c 'loc-fix 55.7100 37.6100 10' 1
  grep -q 'Scenario: live location 8 moved to 55.71000 37.61000' "$WORK/log.txt" || fail "the live location didn't move"
  c live-bar-tap 1.5
  [[ $(asked 'Live map:' live-state) == *'pins=2 share="Stop Sharing My Location" watching=1 moved=0 rows: ["You" "updated just now"] ["Boris Places" "updated 2 minutes ago · '* ]] \
    || fail "the shared map: $(grep 'Live map:' "$WORK/log.txt" | tail -1)"
  c live-share 1
  grep -q 'Scenario: live location 8 stopped' "$WORK/log.txt" || fail "not stopped from the shared map"
  [[ $(asked 'Live map:' live-state) == *'pins=1 share="Share My Live Location"'* ]] \
    || fail "the shared map after stopping: $(grep 'Live map:' "$WORK/log.txt" | tail -1)"
  c loc-close 0.5
  [[ $(asked 'Live bar:' live-bar) == *'text="Boris" stop=0 active=1' ]] || fail "the bar after stopping"
  # The chat's card: the group's description and members, a member's card, muting from it.
  c profile 1.5
  [[ $(asked 'Profile: name' profile-state) == *'name="Places" status="3 members" mute=0 info: [Description=Where we meet] members: "Vlad" "Anna Places" "Boris Places"'* ]] \
    || fail "the group's card: $(grep 'Profile: name' "$WORK/log.txt" | tail -1)"
  c 'profile-member 1' 1.5
  [[ $(asked 'Profile: name' profile-state) == *'name="Anna Places" status="last seen '*'mute=0 info: [Mobile=+79990001001] [Bio=Loves maps]'* ]] \
    || fail "a member's card: $(grep 'Profile: name' "$WORK/log.txt" | tail -1)"
  c profile-mute 1
  [[ $(asked 'Profile: name' profile-state) == *'name="Anna Places"'*' mute=1 '* ]] || fail "not muted from the card"
  c loc-close 0.5
  c back 1
  # The contacts: all of them, a search with the server's answer, a tap opening the chat; About.
  c contacts 1.5
  [[ $(asked 'Contacts:' contacts-state) == *'rows: "Anna Places" "Boris Places" "Friend 1" "Friend 2" "Friend 3"' ]] \
    || fail "the contacts: $(grep 'Contacts:' "$WORK/log.txt" | tail -1)"
  c 'contacts-search bor' 1.5
  [[ $(asked 'Contacts:' contacts-state) == *'query="bor" empty="" rows: "Boris Places"' ]] \
    || fail "the contacts found: $(grep 'Contacts:' "$WORK/log.txt" | tail -1)"
  c 'contacts-open 0' 2
  [[ $(asked 'chat topic=' state) == *' id=3002'* ]] || fail "the contact's chat didn't open"
  c about 1
  grep -q 'About: shown' "$WORK/log.txt" || fail "no About"
  stop
  # After a restart TDLib tells about one's own live location still shared: it goes on.
  CONTRAIL_SCENARIO_LIVE=1 start 150
  [[ $(asked 'Live locations: shares' live-locations) == *'shares=1 waiting=1 '*'[-700/6 period=2147483647 '* ]] \
    || fail "the live location of the last run: $(grep 'Live locations: shares' "$WORK/log.txt" | tail -1)"
  c 'loc-fix 55.7700 37.6300 8' 1
  grep -q 'Scenario: live location 6 moved to 55.77000 37.63000' "$WORK/log.txt" || fail "it didn't go on"
  stop
}
section_places
echo "PASS"
