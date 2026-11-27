#!/usr/bin/env bash
# Checks the built packages on the phone Contrail supports, the FuriPhone FLX1s with FuriOS (PHONE, default
# "furios", reached over SSH), the way users install them. The program runs on the scripted transport (a made-up
# account, nothing reaches Telegram), on a virtual X display, with a session bus and a home of its own: the screen,
# the account and the data of the Contrail already on the phone stay as they are.
# CONTRAIL_FLAVOUR (devel by default, as in tools/build-packages.sh) picks the packages.
#
#   tools/smoke-test.sh deb       installed with apt (sudo) with the dependencies it declares, checked, then removed
#                                 with what it brought in, unless it was installed before
#   tools/smoke-test.sh flatpak   installed into the user's Flatpak installation (it stays: the version on the phone
#                                 is the one checked); started with the command of its exported desktop entry
#   tools/smoke-test.sh all       both
#
# Needs passwordless sudo on the phone for apt (xvfb, the test's only tool, is installed with it when missing).
# SMOKE_DARK=1 runs the program with the dark style. Logs and screenshots go to out/smoke/<format>-arm64-flx1s/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PHONE="${PHONE:-furios}"
FLAVOUR="${CONTRAIL_FLAVOUR:-devel}"
read -r APP_ID NAME < <(python3 -c "import sys; sys.path.insert(0, 'tools'); import package
package.configure(sys.argv[1]); print(package.APP_ID, package.NAME)" "$FLAVOUR")
file_of() {
  python3 -c "import sys; sys.path.insert(0, 'tools'); import package; package.configure(sys.argv[1])
print(package.package_file(sys.argv[2], 'aarch64', package.package_version(sys.argv[1])))" "$FLAVOUR" "$1"
}
rsh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE" "$@"; }
REMOTE=.cache/contrail-smoke

prepare() {
  rsh "mkdir -p $REMOTE && rm -rf $REMOTE/out && mkdir -p $REMOTE/out"
  scp -q tests/smoke/run-demo.sh "$PHONE:$REMOTE/"
  rsh "chmod +x $REMOTE/run-demo.sh"
  rsh 'command -v Xvfb >/dev/null || sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends xvfb'
}
# demo NAME COMMAND...: the scripted run (tests/smoke/run-demo.sh) on the phone, its results copied back.
demo() {
  local out=$1; shift
  local status=0
  rsh "SMOKE_DARK='${SMOKE_DARK:-}' SMOKE_SETTLE='${SMOKE_SETTLE:-8}' sh $REMOTE/run-demo.sh $REMOTE/out/$out $APP_ID $*" \
    < /dev/null || status=$?
  rm -rf "out/smoke/$out" && mkdir -p out/smoke
  scp -q -r "$PHONE:$REMOTE/out/$out" out/smoke/ || true
  return $status
}

smoke_deb() {
  local file out=deb-arm64-flx1s
  file=$(file_of deb)
  [[ -f out/packages/$file ]] || { echo "No out/packages/$file: run tools/build-packages.sh" >&2; return 1; }
  echo "==> $file on $PHONE"
  scp -q "out/packages/$file" "$PHONE:$REMOTE/"
  # What apt would add: only these go again afterwards.
  local before added
  before=$(rsh "dpkg-query -W -f='\${Status}' $NAME 2>/dev/null | grep -q 'install ok installed' && echo yes || true")
  added=$(rsh "apt-get -s install --no-install-recommends ./$REMOTE/$file 2>/dev/null | sed -n 's/^Inst \\([^ ]*\\) .*/\\1/p'" \
    | grep -vx "$NAME" | tr '\n' ' ' || true)
  rsh "sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ./$REMOTE/$file" \
    > /tmp/smoke-apt.log 2>&1 || { tail -n 30 /tmp/smoke-apt.log; return 1; }
  local status=0
  rsh "dpkg -s $NAME | sed -n '/^Version/p;/^Depends/p'; $NAME --check-libs" || status=$?
  [[ $status != 0 ]] || demo "$out" "$NAME" || status=$?
  if [[ -z $before ]]; then
    rsh "sudo -n DEBIAN_FRONTEND=noninteractive apt-get purge -y -qq $NAME $added" > /dev/null 2>&1 \
      || echo "warning: $NAME stayed installed on the phone" >&2
  fi
  return $status
}

smoke_flatpak() {
  local file out=flatpak-arm64-flx1s
  file=$(file_of flatpak)
  [[ -f out/packages/$file ]] || { echo "No out/packages/$file: run tools/build-packages.sh" >&2; return 1; }
  echo "==> $file on $PHONE"
  scp -q "out/packages/$file" "$PHONE:$REMOTE/"
  # Again if the same build is there already: its files are the ones checked.
  rsh "set -e; export LC_ALL=C; flatpak install --user -y --noninteractive --reinstall --bundle $REMOTE/$file > /dev/null
    flatpak info --user $APP_ID | sed -n '/Version\\|Runtime\\|Arch\\|Commit/p'"
  # As the menu starts it. The run-demo home is a new one: Flatpak's user installation is named for it.
  local launcher
  launcher=$(rsh "sed -n 's/^Exec=.* --command=\\([^ ]*\\) .*/\\1/p' \
    \$(flatpak info --user --show-location $APP_ID)/export/share/applications/$APP_ID.desktop")
  [[ -n $launcher ]] || { echo "No launcher in the exported desktop entry" >&2; return 1; }
  demo "$out" "env FLATPAK_USER_DIR=\$HOME/.local/share/flatpak flatpak run --socket=x11 --command=$launcher $APP_ID"
}

[[ $# -gt 0 ]] || { sed -n '2,16p' "$0"; exit 2; }
prepare
for target in "$@"; do
  case $target in
    all) smoke_deb; smoke_flatpak ;;
    deb) smoke_deb ;;
    flatpak) smoke_flatpak ;;
    *) sed -n '2,16p' "$0"; exit 2 ;;
  esac
done
echo "Logs and screenshots: out/smoke/"
