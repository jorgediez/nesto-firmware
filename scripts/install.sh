#!/bin/sh
# Installs Nesto on a rooted Nest Learning Thermostat (2nd generation), over
# SSH, from the folder this script is in: an unpacked release, or a build
# (scripts/dev.sh device-install passes NESTO_FILES). It needs only ssh and
# tar on this computer: Linux, macOS, WSL or Git Bash on Windows.
#
#   sh install.sh HOST [COMMAND] [SSH OPTIONS]
#
#   install     install Nesto, started at every boot; on a thermostat that
#               has it already, put this version in its place (the default)
#   update      the same
#   status      the mode, what runs, Nesto's recent exits, the log's end
#   stock       the stock software from now on, across reboots
#   nesto       Nesto again, from now on
#   uninstall   the stock software back for good, and Nesto removed (its
#               settings and log stay in /media/data/nesto)
#   check       a read-only report on the thermostat, saved here
#
# Further arguments go to ssh, for example -i ~/.ssh/nest. See INSTALL.md.

set -eu

FILES="${NESTO_FILES:-$(cd "$(dirname "$0")" && pwd)}"
SSH="${NESTO_SSH:-ssh}"
REPORTS="${NESTO_REPORTS:-.}"
INITTAB="\${NESTO_INITTAB:-/etc/inittab}"  # expanded on the thermostat

usage() {
  sed -n '7,20p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

[ $# -ge 1 ] || usage
host="$1"
shift
command=install
case "${1:-}" in
  install | update | status | stock | nesto | uninstall | check)
    command="$1"
    shift
    ;;
  -*) ;;
  "") ;;
  *) usage ;;
esac

on_nest() {
  "$SSH" -o ConnectTimeout=10 "$@" "root@$host" "$remote"
}

# The files, checked against the release's list when there is one.
need() {
  for file in "$@"; do
    if [ ! -f "$FILES/$file" ]; then
      echo "$FILES/$file is missing: run this from an unpacked release" >&2
      exit 1
    fi
  done
  [ -f "$FILES/SHA256SUMS" ] || return 0
  if command -v sha256sum > /dev/null; then
    sum="sha256sum"
  elif command -v shasum > /dev/null; then
    sum="shasum -a 256"
  else
    echo "(no sha256sum or shasum here: the files aren't checked)" >&2
    return 0
  fi
  for file in "$@"; do
    expected="$(grep " \*\{0,1\}$file\$" "$FILES/SHA256SUMS" | cut -d' ' -f1)"
    actual="$(cd "$FILES" && $sum "$file" | cut -d' ' -f1)"
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
      echo "$file doesn't match SHA256SUMS: download the release again" >&2
      exit 1
    fi
  done
}

# The files as a tar on SSH's input, so one session (one password) does it.
pack() {
  if tar --version 2> /dev/null | grep -q GNU; then
    tar -C "$FILES" --format=ustar --owner=0 --group=0 -cf - "$@"
  else
    tar -C "$FILES" -cf - "$@"
  fi
}

case "$command" in
  install | update)
    need nesto nesto-boot.sh nest-screen.sh
    [ -f "$FILES/VERSION" ] && echo "Nesto $(cat "$FILES/VERSION"), for $host"
    # Nesto and its scripts go to /, where programs may run from and stay
    # (/media/scratch is emptied at boot). A new Nesto replaces the old one in
    # one step, so the service never starts half a file.
    remote="
      set -e
      was=\$(/nesto/nesto --version 2> /dev/null || echo none)
      installed=
      [ -f /nesto/nesto-boot.sh ] && grep -q nesto-boot.sh $INITTAB && installed=1
      rm -rf /tmp/nesto-install && mkdir /tmp/nesto-install && cd /tmp/nesto-install
      tar xf -
      chown 0:0 nesto nesto-boot.sh nest-screen.sh
      # Whatever modes the archive brought (a Windows checkout's are 777):
      # root runs these, so only root may change them.
      chmod 755 nesto nesto-boot.sh nest-screen.sh
      mkdir -p /nesto /media/data/nesto
      # Installed by hand, any version: no update waits to be confirmed, and
      # nothing to roll back to.
      rm -f /nesto/nesto.prev /media/data/nesto/ota-pending /media/data/nesto/ota-pending.starts
      mv nesto /nesto/nesto.new && mv /nesto/nesto.new /nesto/nesto
      mv nesto-boot.sh nest-screen.sh /nesto/
      cd / && rm -rf /tmp/nesto-install
      echo \"Nesto: \$was -> \$(/nesto/nesto --version)\"
      if [ -n \"\$installed\" ]; then
        sh /nesto/nesto-boot.sh restart
      else
        sh /nesto/nesto-boot.sh install
        sleep \${NESTO_SETTLE:-10}
        sh /nesto/nesto-boot.sh status
      fi"
    pack nesto nesto-boot.sh nest-screen.sh | on_nest "$@"
    ;;
  status)
    remote="sh /nesto/nesto-boot.sh status; echo; sh /nesto/nest-screen.sh status"
    on_nest "$@"
    ;;
  stock | nesto)
    remote="sh /nesto/nesto-boot.sh $command"
    on_nest "$@"
    ;;
  uninstall)
    remote="sh /nesto/nesto-boot.sh uninstall && rm -rf /nesto && echo 'removed /nesto'"
    on_nest "$@"
    ;;
  check)
    need device-check.sh nesto-probe
    mkdir -p "$REPORTS"
    report="$REPORTS/nesto-check-$(date +%Y%m%d-%H%M%S).txt"
    # Unpacked into RAM (/tmp) and deleted when the report is done; the
    # script picks where the probe can run from.
    remote='rm -rf /tmp/nesto-check && mkdir /tmp/nesto-check && cd /tmp/nesto-check &&
            tar xf - && sh ./device-check.sh; status=$?; cd / && rm -rf /tmp/nesto-check; exit $status'
    pack device-check.sh nesto-probe | on_nest "$@" | tee "$report" ||
      echo "(the check over SSH didn't finish)"
    echo
    echo "Saved to $report. It has MAC addresses and the Wi-Fi network's name:"
    echo "read it before sharing it."
    ;;
esac
