#!/usr/bin/env bash
# Measures Contrail's memory, CPU time, wakeups and network on the phone, for comparing releases (PHONE,
# default "furios"; the app as installed by update-contrail.sh). Results go to out/measure/ as TSV.
#
#   tools/measure.sh background [NAME]   the background scenario, about 20 minutes: start, open Saved Messages
#                                        and load its history with the media, close the window, then samples
#                                        1, 5 and 15 minutes after closing
#   tools/measure.sh energy MINUTES [NAME]
#                                        the charge used with the window closed and the screen locked, sampled
#                                        every 30 s with the current and the screen's state; the phone must be off
#                                        the charger. Run once with the app stopped (NAME idle) for the
#                                        phone's own use, then with the app in the background, under the same
#                                        conditions (network, account, time of day), at least an hour each
#   tools/measure.sh snapshot [LABEL]    one sample now
#
# Only Saved Messages is opened, so nothing is read in the user's chats. Closing the window needs "Run in the
# background" on: the scenario switches it on (and leaves it on).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
PHONE="${PHONE:-furios}"
OUT=out/measure
mkdir -p "$OUT"
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
rsh 'mkdir -p ~/.cache/contrail-tools' && scp -q tools/phone-measure.py "$PHONE:.cache/contrail-tools/"
sample() { rsh "python3 ~/.cache/contrail-tools/phone-measure.py snapshot '$1'" | tee -a "$2"; }
cmd() { tools/phone.sh cmd "$1" >/dev/null; }
version() { rsh 'cat ~/.local/share/contrail/VERSION 2>/dev/null' || echo unknown; }

case "${1:-}" in
  snapshot)
    rsh "python3 ~/.cache/contrail-tools/phone-measure.py header; python3 ~/.cache/contrail-tools/phone-measure.py snapshot '${2:-now}'"
    ;;
  background)
    file="$OUT/background-${2:-$(version)}-$(date +%Y%m%d-%H%M).tsv"
    rsh "python3 ~/.cache/contrail-tools/phone-measure.py header" > "$file"
    echo "# $(version)" >> "$file"
    tools/phone.sh run
    sleep 60
    cmd 'background on'
    sample started "$file"
    cmd saved
    for _ in 1 2 3 4 5 6; do sleep 10; cmd scroll-top; done
    sleep 30
    sample media "$file"
    cmd close
    closed=$(date +%s)
    for minutes in 1 5 15; do
      sleep $(( closed + minutes * 60 - $(date +%s) ))
      sample "closed-${minutes}m" "$file"
    done
    rsh 'python3 ~/.cache/contrail-tools/phone-measure.py inhibitors' | tee -a "$OUT/inhibitors-$(basename "$file" .tsv).txt"
    echo "Saved: $file"
    ;;
  energy)
    minutes="${2:?minutes}"
    name=${3:-$(version)}
    file="$OUT/energy-$name-$(date +%Y%m%d-%H%M).tsv"
    status=$(rsh 'cat /sys/class/power_supply/battery/status')
    [[ "$status" == Discharging ]] || { echo "The phone is $status: take it off the charger first." >&2; exit 1; }
    # The log is written on the phone: a lost SSH session doesn't end the run.
    remote="/tmp/contrail-energy-$name.tsv"
    rsh "nohup python3 ~/.cache/contrail-tools/phone-measure.py battery-log $(( minutes * 60 )) 30 $remote \
           >/dev/null 2>&1 &"
    sleep $(( minutes * 60 + 20 ))
    until rsh "grep -q '^#' $remote"; do sleep 10; done
    { echo "# $name ($(version))"; rsh "cat $remote"; } > "$file"
    tail -n 1 "$file"
    echo "Saved: $file"
    ;;
  *)
    sed -n '2,17p' "$0"
    exit 1
    ;;
esac
