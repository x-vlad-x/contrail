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
#   11. the photo viewer: the full size shown, a pinch up to 6x and no further, a drag within the edges, a double
#      tap and Ctrl+wheel, Save to Pictures, its textures gone once it is closed, by itself or with its chat.
#   12. the player: a voice message downloading plays, pauses, plays on, the next voice message follows by itself; a
#      song downloaded slower than it plays waits for its data, a seek far ahead moves the download there, the next
#      song follows; a drag along the bar; it plays on with the chat closed; stopped, its thread is gone.
#   13. recording a voice message: the microphone only with an empty field, held and let go it is sent as Ogg Opus
#      with its waveform and plays; a tap only says to hold, slid left it is deleted, slid up it records on with the
#      chat closed and is sent from it opened again; another chat ends it; it pauses what plays.
#   14. sending files: the refused ones and why, the dialog's tiles, removing one, its title and switches, a photo and a
#      video in an album with the caption on the photo, as documents, twelve photos in albums of ten and two in
#      order, the caption's limit, the upload's progress and a tap that stops it; pasting a picture, files and text
#      (and paste-test, which needs a display).
#   tools/td-ui-test.sh [binary]        default build-td/agent-chat/contrail
set -euo pipefail
BIN=$(realpath "${1:-build-td/agent-chat/contrail}")
# Built in the Debian container: what is named otherwise here comes from that container (as mem-scenario.sh does).
missing=$(ldd "$BIN" | awk '/not found/{print $1}')
if [[ -n $missing ]]; then
  LIBS=~/.cache/contrail-scenario/libs
  mkdir -p "$LIBS"
  export LD_LIBRARY_PATH=$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  # A library copied may need others missing here (FFmpeg's do): ldd sees them once it is there.
  for _ in 1 2 3 4 5; do
    [[ -n $missing ]] || break
    for lib in $missing; do
      [[ -e $LIBS/$lib ]] || podman run --rm -v "$LIBS":/out:z localhost/contrail-desktop \
        sh -c "cp -L /usr/lib/x86_64-linux-gnu/$lib /out/" || { echo "no $lib in localhost/contrail-desktop" >&2; exit 1; }
    done
    missing=$(ldd "$BIN" | awk '/not found/{print $1}')
  done
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
[[ $line == *'page=chats forum=0 shown=44 messages=1 rows: "Saved Messages" "Friend 1" "Friend 2" '* ]] \
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
# Section background: the settings, the app without its window, the tray icon, notifications, the proxy. A fake
# notification server and tray watcher (td-fake-desktop.py) stand for the desktop.
section_background() {
  echo "10. Settings and the app in the background"
  local desktop=$WORK/desktop.txt fake
  : > "$desktop"
  "$(dirname "$(realpath "$0")")/td-fake-desktop.py" "$desktop" &
  fake=$!
  for _ in $(seq 50); do grep -q '^ready' "$desktop" && break; sleep 0.1; done
  grep -q '^ready' "$desktop" || fail "the fake desktop didn't start"
  bg() { asked 'bg: ' bg-state; }
  clean() { ! grep -E 'CRITICAL|Gtk-WARNING' "$WORK/log.txt" | grep -v 'Vulkan' | head -3 | grep . || fail "GTK complained"; }
  start 120
  c settings 1.5
  local line
  line=$(asked 'settings:' settings-state)
  [[ $line == *'notify=1 sound=1 preview=1 background=0 account="Vlad" phone=15550001000'* ]] \
    || fail "the settings: $line"
  c tree 0.5
  for text in 'Notifications and Sounds' 'Desktop notifications' 'Allow sound' 'Show message text' \
      'Run in the background' 'About Contrail' 'Log out' 'Saved Messages' '+15550001000'; do
    grep -q "\"$text\"" "$WORK/log.txt" || fail "no \"$text\" in the settings"
  done
  c 'settings-set notify 0' 0.3
  c tree 0.5
  grep 'AdwSwitchRow "Allow sound"' "$WORK/log.txt" | tail -1 | grep -q insensitive \
    || fail "the sound can be turned on without notifications"
  c 'settings-set notify 1' 0.3
  # In the background: the tray icon with the unread chats, the window closed and opened again on the same chat.
  [[ $(bg) == *'window=1 visible=1 background=0 tray=none'* ]] || fail "a tray icon without the background: $(bg)"
  c 'settings-set background 1' 1.5
  line=$(bg)
  [[ $line == *'background=1 tray=registered unread=10'* ]] || fail "no tray icon: $line"
  grep -q '^tray-register' "$desktop" || fail "the tray icon didn't register with the watcher"
  local item="org.kde.StatusNotifierItem-$P-1"
  [[ $(gdbus call --session --dest "$item" --object-path /StatusNotifierItem \
      --method org.freedesktop.DBus.Properties.Get org.kde.StatusNotifierItem Title) == *"Contrail (10)"* ]] \
    || fail "the tray icon's title has no unread chats"
  c back 0.5
  c 'chat 3' 2
  local chat place
  chat=$(bg | sed -n 's/.* chat=\([-0-9]*\).*/\1/p')
  [[ -n $chat && $chat != 0 ]] || fail "no chat opened: $(bg)"
  # Up from the end: the window built again comes back to the same place, not to the end.
  for _ in 1 2 3; do c 'scroll -900' 0.5; done
  sleep 1
  place=$(bg | sed -n 's/.* place=\([0-9]*\/[0-9]*\) .*/\1/p')
  [[ -n $place && $place != 0/* ]] || fail "no place up in the chat: $(bg)"
  c bg-close 2
  line=$(bg)
  [[ $line == *'window=0 '*'chat=0'* ]] || fail "the window stayed: $line"
  kill -0 "$P" || fail "it quit instead of staying in the background"
  grep -q 'background: the window is released' "$WORK/log.txt" || fail "the interface wasn't released"
  gdbus call --session --dest "$item" --object-path /StatusNotifierItem \
    --method org.kde.StatusNotifierItem.Activate 0 0 > /dev/null
  sleep 2
  line=$(bg)
  [[ $line == *"window=1 visible=1 "*"place=$place chat=$chat" ]] || fail "the tray icon didn't bring the chat back: $line"
  c bg-close 2
  c bg-open 2
  [[ $(bg) == *"window=1 visible=1 "*"place=$place chat=$chat" ]] || fail "the launcher didn't bring the chat back: $(bg)"
  clean
  # A network gone and back: TDLib is told once each time.
  [[ $(asked 'network:' net-state) == *'sent=0'* ]] || fail "the network type sent at the start: $(asked 'network:' net-state)"
  c 'net-simulate none' 0.2
  c 'net-simulate none' 0.2
  c 'net-simulate mobile' 0.5
  (( $(grep -c 'scenario: network' "$WORK/log.txt") == 2 )) || fail "the network changes weren't told once each"
  # The settings stay for the next start.
  kill "$P"; wait "$P" 2>/dev/null || true; P=
  start 120
  [[ $(bg) == *'background=1 tray=registered'* ]] || fail "the background setting was lost: $(bg)"
  [[ $(asked 'mem-guard:' mem-guard) == *'level=normal'* ]] || fail "no memory guard"
  c 'mem-simulate low' 0.5
  grep -q 'memory low, .* freeing what can be freed' "$WORK/log.txt" || fail "low memory isn't handled"
  # Notifications: two messages to a chat that isn't open, the window closed; one notification for both.
  notes() { grep -c "^$1" "$desktop" || true; }
  c 'chat 0' 2
  [[ $(bg) != *' chat=5' ]] || fail "the chat that gets messages is open"
  c 'send /notify2' 0.3
  c bg-close 3
  line=$(grep '^notify ' "$desktop" | tail -1)
  [[ $line == *'summary=Friend 5 body=Are you coming tonight? 1\nAre you coming tonight? 2 '* ]] \
    || fail "the notification: $line"
  [[ $line == *'actions=default,mark-read,inline-reply '*'sound=message-new-instant'* ]] || fail "its buttons: $line"
  (( $(notes notify) == 2 )) && [[ $line == *' replaces='[1-9]* ]] || fail "not one notification for the chat: $line"
  local id
  id=$(sed -n 's/^notify id=\([0-9]*\) .*/\1/p' <<< "$line")
  [[ $(asked 'notify:' notify-state) == *"[group=1 chat=5 shown=$id count=2 adopted=0]"* ]] || fail "$(asked 'notify:' notify-state)"
  # Answered from the notification: sent to the chat, which is read then, and the notification goes.
  gdbus call --session --dest org.contrail.FakeDesktop --object-path /org/freedesktop/Notifications \
    --method org.contrail.FakeDesktop.Reply "$id" 'On my way' > /dev/null
  sleep 1
  grep -q 'scenario: sent to 5 "On my way" reply_to=0' "$WORK/log.txt" || fail "the answer wasn't sent"
  grep -q "^close id=$id" "$desktop" || fail "the notification stayed after the chat was read"
  [[ $(bg) == *'window=0 '* ]] || fail "answering opened the window"
  # Without the text of messages; a tap opens the chat.
  c bg-open 2
  c settings 1
  c 'settings-set preview 0' 0.3
  c back 0.5
  c 'chat 0' 2
  c 'send /notify' 0.3
  c bg-close 3
  line=$(grep '^notify ' "$desktop" | tail -1)
  [[ $line == *'summary=Friend 5 body=You have a new message '* ]] || fail "the text is shown: $line"
  id=$(sed -n 's/^notify id=\([0-9]*\) .*/\1/p' <<< "$line")
  gdbus call --session --dest org.contrail.FakeDesktop --object-path /org/freedesktop/Notifications \
    --method org.contrail.FakeDesktop.Invoke "$id" default > /dev/null
  sleep 2
  [[ $(bg) == *'window=1 visible=1 '*' chat=5' ]] || fail "a tap didn't open the chat: $(bg)"
  # Turned off: nothing comes.
  c settings 1
  c 'settings-set preview 1' 0.3
  c 'settings-set notify 0' 0.3
  local before
  before=$(notes notify)
  c back 0.5
  c 'chat 0' 2
  c 'send /notify' 2.5
  (( $(notes notify) == before )) || fail "a notification came with them turned off"
  c settings 1
  c 'settings-set notify 1' 0.3
  c 'settings-set background 0' 1
  [[ $(bg) == *'window=1 visible=1 background=0 tray=none'* ]] || fail "the tray icon stayed: $(bg)"
  # The proxies: added through the dialog, used, checked, edited, removed, turned off and on.
  proxies() { asked 'proxy: use' proxy-state; }
  c settings 1
  c proxy 1
  [[ $(proxies) == *'proxy: use=0' ]] || fail "proxies before any was added: $(proxies)"
  c 'proxy-add socks5 good.example 1080 user secret' 0.5
  line=$(proxies)
  [[ $line == *'use=1 [0 SOCKS5 good.example:1080 enabled status="connecting…"]' ]] || fail "not used: $line"
  sleep 2
  [[ $(proxies) == *'[0 SOCKS5 good.example:1080 enabled status="online"]' ]] || fail "not connected: $(proxies)"
  c 'proxy-add mtproto down.example 443' 0.5
  grep -q 'proxy: dialog save=0' "$WORK/log.txt" || fail "an MTProto proxy without a secret can be saved"
  c 'proxy-add mtproto down.example 443 dd0123456789abcdef' 2.5
  line=$(proxies)
  [[ $line == *'[0 SOCKS5 good.example:1080 status="available (ping: 42 ms)"] [1 MTProto down.example:443 enabled status="online"]' ]] \
    || fail "two proxies: $line"
  c 'proxy-enable 0' 2.5
  [[ $(proxies) == *'enabled status="online"] [1 MTProto down.example:443 status="not available"]' ]] \
    || fail "the other one: $(proxies)"
  c 'proxy-edit 0 http good.example 8080' 1
  c 'proxy-remove 1' 1
  [[ $(proxies) == *'use=1 [0 HTTP good.example:8080 enabled status="'* ]] || fail "edited and removed: $(proxies)"
  [[ $(proxies) != *'[1 '* ]] || fail "not removed: $(proxies)"
  c 'proxy-use 0' 1
  [[ $(proxies) == *'use=0 [0 HTTP good.example:8080 status="available (ping: 42 ms)"]' ]] || fail "turned off: $(proxies)"
  c 'proxy-use 1' 2.5
  [[ $(proxies) == *'use=1 [0 HTTP good.example:8080 enabled status="online"]' ]] || fail "turned on again: $(proxies)"
  # A proxy link asks first.
  c 'proxy-link tg://socks?server=link.example&port=1081' 1
  [[ $(asked 'dialog:' dialog) == *'heading="Enable proxy" body="link.example:1081 (SOCKS5)"'* ]] \
    || fail "no question for a proxy link: $(asked 'dialog:' dialog)"
  c 'dialog-answer connect' 1
  c back 0.5
  c proxy 1.5
  [[ $(proxies) == *'[1 SOCKS5 link.example:1081 enabled status="'* ]] || fail "the link's proxy: $(proxies)"
  c back 0.5
  c back 0.5
  # Logging out asks first; then a new session starts, without "you were logged out".
  c settings-logout 0.5
  [[ $(asked 'dialog:' dialog) == *'heading="Are you sure you want to log out?"'* ]] || fail "no question before logging out"
  c 'dialog-answer logout' 3
  grep -q 'You were logged out' "$WORK/log.txt" && fail "logging out was taken for a lost session"
  c tree 0.5
  grep -q '"Friend 1"' <(sed -n '/tree$/,$p' "$WORK/log.txt" | tail -400) || fail "no chats after logging out"
  clean
  stop
  kill "$fake" 2>/dev/null || true
  echo "   ok"
}
section_background
# Section media-view: the photo viewer.
viewer() { asked 'viewer:' viewer-state; }
section_media_view_viewer() {
  echo "11. The photo viewer"
  start 150
  c 'chat 42' 3
  c 'tap 20' 1.5
  local line
  line=$(viewer)
  [[ $line == *'open=1 message='*' shown=1/2 size=2560x1920 loading=0 '*'scale=1.000 '*'texture=2560x1920'* ]] \
    || fail "the viewer didn't show the full photo: $line"
  c tree 0.5
  grep -q 'CRITICAL' "$WORK/log.txt" && fail "criticals in the log"
  # A pinch zooms around the middle, up to 6x and no further.
  c 'viewer-pinch 2' 0.3
  line=$(viewer)
  [[ $line == *'scale=2.000 '*'middle=0.5000,0.5000'* ]] || fail "the pinch: $line"
  c 'viewer-pinch 10' 0.3
  [[ $(viewer) == *'scale=6.000 '* ]] || fail "zoomed past 6x: $(viewer)"
  # A drag moves the zoomed photo, never past its edge.
  c 'viewer-pan 100 0' 0.3
  line=$(viewer)
  [[ $line != *'middle=0.5000,0.5000'* ]] || fail "the drag didn't move it: $line"
  c 'viewer-pan 100000 100000' 0.3
  [[ $(viewer) == *'photo=0.0,0.0 '* ]] || fail "dragged past the edge: $(viewer)"
  # A double tap goes back to the whole photo, another zooms in where it was.
  c 'viewer-double 100 100' 0.3
  [[ $(viewer) == *'scale=1.000 '* ]] || fail "a double tap on a zoomed photo: $(viewer)"
  c 'viewer-double 100 100' 0.3
  [[ $(viewer) == *'scale=2.500 '* ]] || fail "a double tap: $(viewer)"
  c 'viewer-double 100 100' 0.3
  # Ctrl+wheel: up zooms in, down out.
  c 'viewer-wheel -1' 0.3
  [[ $(viewer) == *'scale=1.150 '* ]] || fail "Ctrl+wheel: $(viewer)"
  c 'viewer-wheel 5' 0.3
  [[ $(viewer) == *'scale=1.000 '* ]] || fail "Ctrl+wheel out: $(viewer)"
  # Save copies the photo to Pictures.
  c viewer-save 1
  ls "$WORK"/pictures/photo_*.jpg > /dev/null 2>&1 || fail "Save in the viewer put nothing in Pictures"
  # Closed, its textures go.
  c 'viewer-close' 1.5
  line=$(viewer)
  [[ $line == *'open=0 textures=0'* ]] || fail "the textures stayed after closing: $line"
  # A chat closed under the viewer takes it along.
  c 'tap 20' 1.5
  c back 1.5
  [[ $(viewer) == *'open=0 textures=0'* ]] || fail "the viewer stayed with its chat closed: $(viewer)"
  grep -q 'CRITICAL' "$WORK/log.txt" && fail "criticals in the log"
  stop
  echo "   ok"
}
section_media_view_viewer

# Section media-view: the player. The sound goes nowhere (OpenAL's null output).
player() { asked 'player:' player-state; }
# Waits up to $2 seconds for the player's state to match $1.
player_until() {
  local line
  for _ in $(seq $(( ${2:-5} * 5 ))); do
    line=$(player)
    [[ $line == $1 ]] && return 0
    sleep 0.2
  done
  fail "the player never got to $1: $line"
}
position() { player | sed -n 's/.* position=\([0-9.]*\).*/\1/p'; }
# The app's threads, without GLib's shared pool: its idle threads come and go with any async call.
own_threads() { cat /proc/"$P"/task/*/comm | grep -vc '^pool-'; }
section_media_view_player() {
  echo "12. The player"
  export ALSOFT_DRIVERS=null
  start 150
  local threads line before after
  threads=$(own_threads)
  c 'chat 44' 3
  line=$(asked 'chat media:' 'media 2')
  [[ $line == *'document="0:03"'*'icon=media-playback-start-symbolic'* ]] || fail "a voice message before it plays: $line"
  # A voice message not downloaded yet: "Loading…", then it plays.
  c 'player-play 2' 0.3
  player_until '*status=loading message=2 *' 2
  player_until '*status=playing message=2 *thread=1*' 6
  line=$(asked 'chat media:' 'media 2')
  [[ $line == *'document="0:0'?' / 0:03"'*'icon=media-playback-pause-symbolic'* ]] || fail "a voice message playing: $line"
  # A pause keeps the place, a tap plays on from it.
  c player-pause 0.5
  before=$(position)
  sleep 0.6
  after=$(position)
  [[ $(player) == *'status=paused '* && $before == "$after" ]] || fail "the pause: $before then $after, $(player)"
  [[ $(asked 'chat media:' 'media 2') == *'icon=media-playback-start-symbolic'* ]] || fail "the icon of a paused one"
  c 'player-play 2' 0.8
  player_until '*status=playing message=2 *' 2
  # It ends, the next voice message plays, and nothing after it: the song isn't a voice message.
  player_until '*status=playing message=3 *' 6
  player_until '*status=stopped *thread=0*' 5
  [[ $(asked 'chat media:' 'media 2') == *'document="0:03"'*'bar=0 '* ]] || fail "the bar after the end"
  # A song downloaded slower than it plays waits for its data.
  c 'player-play 4' 0.3
  player_until '*status=playing message=4 *' 8
  player_until '*waiting=1*' 10
  line=$(asked 'chat media:' 'media 4')
  [[ $line == *'fill='*'Loading…'* || $line == *'Loading…'*'fill='* ]] || fail "waiting doesn't show: $line"
  # A seek far ahead asks for the download from there.
  c 'player-seek 0.9' 0.3
  player_until '*status=playing message=4 position=11.*' 8
  grep -q 'media file 4000003 asked from [1-9]' "$WORK/log.txt" || fail "the download wasn't moved for the seek"
  # The next song follows; a drag along its bar plays it from there.
  player_until '*status=playing message=5 *' 8
  c 'player-drag 0.5' 0.5
  line=$(player)
  [[ $line == *'status=playing message=5 position=2.'* || $line == *'status=playing message=5 position=3.'* ]] \
    || fail "the drag: $line"
  # The chat closed, the song plays on.
  c back 1
  [[ $(player) == *'status=playing message=5 '* ]] || fail "closing the chat stopped the song: $(player)"
  c 'chat 44' 2
  c player-stop 0.5
  [[ $(player) == *'status=stopped '*'thread=0'* ]] || fail "stopped: $(player)"
  (( $(own_threads) <= threads + 1 )) || fail "threads stayed: $threads before, $(own_threads) now"
  grep -q 'CRITICAL' "$WORK/log.txt" && fail "criticals in the log"
  stop
  unset ALSOFT_DRIVERS
  echo "   ok"
}
section_media_view_player

# Section media-view: recording a voice message. The microphone is the scenario's tone.
voice() { asked 'voice:' voice-state; }
section_media_view_recording() {
  echo "13. Recording a voice message"
  export ALSOFT_DRIVERS=null CONTRAIL_SCENARIO_MICROPHONE=1
  start 150
  local line sent
  c 'chat 44' 3
  [[ $(voice) == *'recording=0 '*'mic=1 bar=0 '* ]] || fail "the microphone with an empty field: $(voice)"
  c 'type Hello' 0.3
  [[ $(voice) == *'mic=0 '* ]] || fail "the microphone with a text: $(voice)"
  c 'type' 0.3
  [[ $(voice) == *'mic=1 '* ]] || fail "the microphone after the text went: $(voice)"
  # Held: the bar, the lock above; let go, it is sent.
  c voice-hold 1.6
  line=$(voice)
  [[ $line == *'recording=1 holding=1 locked=0 '*'bar=1 time=0:01'*' lock=1 active=1 '*'thread=1'* ]] \
    || fail "recording while held: $line"
  c 'voice-drag -60 -10' 0.3
  [[ $(voice) == *'recording=1 '*' hint=0.6'* ]] || fail "slide to cancel doesn't follow the finger: $(voice)"
  c voice-release 1
  line=$(voice)
  [[ $line == *'recording=0 '*'mic=1 bar=0 '*'active=0 '*'thread=0 sent=2s waveform=63'* ]] || fail "sent: $line"
  grep -q 'Scenario: voice message sent: duration=2 waveform=63 size=[1-9]' "$WORK/log.txt" || fail "nothing sent"
  # It is Ogg Opus that plays, as long as it was recorded.
  c 'player-play 7' 0.3
  player_until '*status=playing message=7 *length=1.[5-9]*' 5
  c player-stop 0.3
  # A tap only says to hold the button.
  c voice-hold 0.1
  c voice-release 0.5
  grep -q 'toast: Please hold' "$WORK/log.txt" || fail "a tap didn't say to hold"
  # Slid left, it is deleted.
  c voice-hold 1.2
  c 'voice-drag -130 0' 0.5
  [[ $(voice) == *'recording=0 '*'bar=0 '*'active=0 '* ]] || fail "slid left: $(voice)"
  sent=$(grep -c 'Scenario: voice message sent' "$WORK/log.txt")
  (( sent == 1 )) || fail "$sent sent after deleting"
  # Slid up, it records on without the finger, with the chat closed too.
  c voice-hold 0.5
  c 'voice-drag 0 -100' 0.3
  [[ $(voice) == *'recording=1 holding=0 locked=1 '*'lock=0 '* ]] || fail "locked: $(voice)"
  c back 1.5
  [[ $(voice) == *'active=1 '* ]] || fail "closing the chat ended the recording: $(voice)"
  c 'chat 44' 2
  line=$(voice)
  [[ $line == *'recording=1 holding=0 locked=1 '*'bar=1 '*'active=1 '* ]] || fail "the chat opened again: $line"
  c voice-send 1
  grep -q 'Scenario: voice message sent: duration=[3-9]' "$WORK/log.txt" || fail "the locked one wasn't sent"
  # Another chat opened ends it.
  c voice-start 1
  c 'chat 0' 2
  [[ $(voice) == *'recording=0 '*'active=0 '* ]] || fail "another chat didn't end it: $(voice)"
  (( $(grep -c 'Scenario: voice message sent' "$WORK/log.txt") == 2 )) || fail "the one ended by another chat was sent"
  # It pauses what plays.
  c 'chat 44' 2
  c 'player-play 3' 0.3
  player_until '*status=playing message=3 *' 4
  c voice-start 0.5
  [[ $(player) == *'status=paused '* ]] || fail "the player plays on while recording: $(player)"
  c voice-delete 0.5
  grep -q 'CRITICAL' "$WORK/log.txt" && fail "criticals in the log"
  stop
  unset ALSOFT_DRIVERS CONTRAIL_SCENARIO_MICROPHONE
  echo "   ok"
}
section_media_view_recording

# Section media-send: files sent from the attach dialog and the clipboard.
attach() { asked 'attach state:' attach-state; }
sent() { grep -c 'Scenario: sent ' "$WORK/log.txt" || true; }
section_media_send_attach() {
  echo "14. Sending files"
  "$(dirname "$BIN")/paste-test" > /dev/null || fail "paste-test"
  start 150
  local F=$WORK/files line before
  mkdir -p "$F/folder"
  magick -size 3000x2000 xc:skyblue "$F/photo.png"
  magick -size 400x300 xc:orange "$F/small.jpg"
  ffmpeg -loglevel error -f lavfi -i testsrc=size=320x240:rate=10 -t 2 -pix_fmt yuv420p "$F/clip.mp4"
  printf 'notes\n' > "$F/notes.txt"
  : > "$F/empty.txt"
  head -c 2097152 /dev/urandom > "$F/big.bin"
  truncate -s 2100M "$F/huge.bin"  # Sparse: nothing is written.
  c 'chat 2' 3
  # Nothing that can be sent: the window says why, no dialog.
  c "attach-files $F/empty.txt|$F/folder" 1
  grep -q 'toast: File: empty.txt is empty' "$WORK/log.txt" || fail "an empty file wasn't refused"
  grep -q 'toast: File: folder is empty' "$WORK/log.txt" || fail "a folder wasn't refused"
  [[ $(attach) == *'no dialog'* ]] || fail "a dialog for nothing to send: $(attach)"
  c "attach-files $F/huge.bin" 1
  grep -q "toast: huge.bin: The document can't be sent, because it is larger than 2000 MB." "$WORK/log.txt" \
    || fail "a file too large wasn't refused"
  # Some refused: the dialog says why; a photo, a video and a file.
  c "attach-files $F/photo.png|$F/clip.mp4|$F/notes.txt|$F/empty.txt" 3
  line=$(attach)
  [[ $line == *'entries=3 ready=3 documents=0 documentsShown=1 group=1 groupShown=1 send=1 refused="File: empty.txt is empty and can'"'"'t be sent." title="3 files selected"'* ]] \
    || fail "the dialog: $line"
  grep -q 'attach entry: type=1 size=[0-9]* 2560x170[67] .*thumbnail=0 photo=1' "$WORK/log.txt" || fail "the photo wasn't made 2560 wide"
  grep -q 'attach entry: type=2 size=[0-9]* 320x240 duration=2 thumbnail=1' "$WORK/log.txt" || fail "the video's thumbnail"
  c 'attach-remove 2' 0.3
  [[ $(attach) == *'entries=2 '* ]] || fail "removing one: $(attach)"
  # An album of the photo and the video, the caption on the photo.
  before=$(sent)
  c 'attach-caption Hello' 0.3
  c attach-send 1
  [[ $(attach) == *'no dialog'* ]] || fail "the dialog stayed after Send: $(attach)"
  (( $(sent) == before + 2 )) || fail "$(( $(sent) - before )) sent instead of 2"
  grep -q 'Scenario: sent photo .*album=1 caption="Hello"' "$WORK/log.txt" || fail "the photo with the caption in an album"
  grep -q 'Scenario: sent video .*thumbnail=1 album=1 caption=""' "$WORK/log.txt" || fail "the video in the album"
  sleep 2
  [[ $(asked 'chat media:' 'media 0') == *'[photo] ['* ]] || fail "the album isn't one row: $(asked 'chat media:' 'media 0')"
  # Only photos: "images", and as documents one album of files.
  c "attach-files $F/small.jpg|$F/photo.png" 3
  [[ $(attach) == *'title="2 images selected"'* ]] || fail "images: $(attach)"
  c 'attach-documents on' 0.3
  [[ $(attach) == *'documents=1 '*'title="2 files selected"'* ]] || fail "as documents: $(attach)"
  c attach-send 1
  grep -q 'Scenario: sent document name=small.jpg .*album=2' "$WORK/log.txt" || fail "as documents in an album"
  grep -q 'Scenario: sent document name=photo.png .*album=2' "$WORK/log.txt" || fail "the second document"
  # Twelve photos: albums of ten and two, in order.
  local list=
  for i in $(seq 12); do cp "$F/small.jpg" "$F/p$i.jpg"; list+="${list:+|}$F/p$i.jpg"; done
  c "attach-files $list" 4
  [[ $(attach) == *'entries=12 ready=12 '* ]] || fail "twelve: $(attach)"
  before=$(sent)
  c attach-send 1.5
  (( $(sent) == before + 12 )) || fail "$(( $(sent) - before )) of twelve sent"
  (( $(grep -c 'Scenario: sent photo .*album=3 ' "$WORK/log.txt") == 10 )) || fail "the first album isn't ten"
  (( $(grep -c 'Scenario: sent photo .*album=4 ' "$WORK/log.txt") == 2 )) || fail "the second album isn't two"
  # A caption too long: said, and the files stay.
  c "attach-files $F/notes.txt" 2
  c "attach-caption $(printf 'x%.0s' $(seq 1100))" 0.3
  c attach-send 1
  grep -q 'toast: You.ve reached the media caption limit. Please make the caption shorter by 76 characters.' "$WORK/log.txt" \
    || fail "the caption's limit"
  [[ $(attach) == *'entries=1 '* ]] || fail "the files went with a caption too long: $(attach)"
  c attach-cancel 1
  # A large file: its upload's progress, and a tap stops it.
  c "attach-files $F/big.bin" 2
  c attach-send 1.5
  line=$(asked 'chat media:' 'media 0')
  [[ $line == *'local=1 '*'document="'*' MB · '*'%"'*'progress='*'icon=process-stop-symbolic'* ]] || fail "the upload's progress: $line"
  c 'tap 0 0' 1
  grep -q 'Scenario: upload of file [0-9]* stopped at [1-9]' "$WORK/log.txt" || fail "the tap didn't stop the upload"
  [[ $(asked 'chat media:' 'media 0') != *'local=1 '* ]] || fail "the message stopped stays"
  # Pasted: a picture goes to the dialog, its text back to the field when it is cancelled; files; text stays text.
  c "attach-copy-image $F/small.jpg" 0.3
  c attach-paste 2
  # (The plural form is the language's: LANG=C has none of its own.)
  [[ $(attach) == *'entries=1 ready=1 '*'title="1 image'* ]] || fail "a pasted picture: $(attach)"
  c attach-cancel 1
  c "attach-copy-files $F/notes.txt|$F/clip.mp4" 0.3
  c attach-paste 3
  [[ $(attach) == *'entries=2 ready=2 '* ]] || fail "pasted files: $(attach)"
  c attach-cancel 1
  c 'attach-copy-text Pasted text' 0.3
  c attach-paste 1
  [[ $(asked 'attach field:' attach-field) == *'"Pasted text"'* ]] || fail "pasted text: $(asked 'attach field:' attach-field)"
  [[ $(attach) == *'no dialog'* ]] || fail "text opened the dialog"
  grep -q 'CRITICAL' "$WORK/log.txt" && fail "criticals in the log"
  stop
  echo "   ok"
}
section_media_send_attach
echo "PASS"
