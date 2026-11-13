#!/bin/sh
# Runs scripts/nesto-boot.sh against a pretend thermostat, inside a busybox
# container, with a loop standing in for init's respawn. scripts/dev.sh
# check-report runs it; it prints "ok" lines, and exits non-zero on a failure.
#
#   docker run --rm -v "$PWD/scripts":/scripts:ro -v "$PWD/tests/device":/t:ro \
#     busybox:1.37.0 sh /t/fake-boot.sh

set -u
export NESTO_INITTAB=/etc/inittab
export NESTO_INIT_PID=999999  # no such process: nothing to tell
export NESTO_RUN_DIR=/tmp/run
export NESTO_DIR=/media/data/nesto
BOOT=/nesto/nesto-boot.sh
LOG=$NESTO_DIR/nesto.log
failures=0

ok() { echo "ok:   $*"; }
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}
# within SECONDS COMMAND...: COMMAND succeeds within SECONDS.
within() {
  local n="$1" i=0
  shift
  while [ "$i" -lt "$n" ]; do
    "$@" && return 0
    sleep 1
    i=$((i + 1))
  done
  return 1
}
nlclient_running() { pidof nlclient > /dev/null; }
nlclient_stopped() { ! pidof nlclient > /dev/null; }
nesto_running() { pidof nesto > /dev/null; }
nesto_stopped() { ! pidof nesto > /dev/null; }
logged() { grep -q -- "$1" "$LOG"; }
# check WHAT COMMAND...: a named check, within 10 s.
check() {
  local what="$1"
  shift
  if within 10 "$@"; then ok "$what"; else fail "$what"; fi
}
fake_mode() { echo "$1" > /tmp/fake-nesto-mode; }

# --- The pretend thermostat ---
mkdir -p /nestlabs/sbin /usr/sbin /etc/init.d /nesto "$NESTO_DIR" "$NESTO_RUN_DIR"
cp /scripts/nesto-boot.sh $BOOT
cat > /nestlabs/sbin/nlclient << 'EOF'
#!/bin/sh
while true; do sleep 1; done
EOF
cat > /etc/init.d/nestlabs << 'EOF'
#!/bin/sh
case "$1" in
  monit_start) setsid /nestlabs/sbin/nlclient > /dev/null 2>&1 < /dev/null & ;;
  monit_stop) kill $(pidof nlclient) ;;
esac
EOF
# monit: watches nlclient or not; refuses while busy, as the real one does
# while it starts nlclient at boot ("Other action already in progress").
cat > /usr/sbin/monit << 'EOF'
#!/bin/sh
state=/tmp/monit-nlclient
[ -f $state ] || echo Running > $state
busy() { [ -f /tmp/monit-busy-until ] && [ "$(cut -d. -f1 /proc/uptime)" -lt "$(cat /tmp/monit-busy-until)" ]; }
case "$1" in
  unmonitor)
    if busy; then echo "monit: Other action already in progress" >&2; exit 1; fi
    echo "Not monitored" > $state ;;
  monitor) echo Running > $state ;;
  start) pidof nlclient > /dev/null || /etc/init.d/nestlabs monit_start ;;
  summary) echo "Process 'nlclient'                   $(cat $state)" ;;
esac
EOF
# Nesto: runs until TERM, or exits at once with 1 (a crash) or 3 (held).
cat > /nesto/nesto << 'EOF'
#!/bin/sh
case "$(cat /tmp/fake-nesto-mode)" in
  crash) echo "fake nesto: crashing"; exit 1 ;;
  hold) echo "fake nesto: held"; exit 3 ;;
esac
echo "fake nesto: $*"
trap 'echo "fake nesto: TERM"; exit 0' TERM
while true; do sleep 1; done
EOF
chmod 755 /nestlabs/sbin/nlclient /etc/init.d/nestlabs /usr/sbin/monit /nesto/nesto
printf '::sysinit:/etc/init.d/rcS\n::shutdown:/etc/init.d/rcH\n' > /etc/inittab
fake_mode run
/etc/init.d/nestlabs monit_start
sleep 1

# --- Installing: one line, and a copy of inittab ---
sh $BOOT install > /dev/null
sh $BOOT install > /dev/null
[ "$(grep -c nesto-boot /etc/inittab)" = 1 ] && ok "install: one inittab line" || fail "install: $(cat /etc/inittab)"
grep -q "^::respawn:/bin/sh /nesto/nesto-boot.sh$" /etc/inittab && ok "install: a respawn line" ||
  fail "install: no respawn line"
[ "$(cat /etc/inittab.nesto-backup)" = "$(printf '::sysinit:/etc/init.d/rcS\n::shutdown:/etc/init.d/rcH')" ] &&
  ok "install: the copy is the original" || fail "install: the copy isn't the original"

# --- monit's daemon, standing in: it starts nlclient while it watches it ---
(while [ ! -f /tmp/stop-init ]; do
  if [ "$(cat /tmp/monit-nlclient 2> /dev/null)" != "Not monitored" ] && ! pidof nlclient > /dev/null; then
    /etc/init.d/nestlabs monit_start
  fi
  sleep 2
done) > /dev/null 2>&1 &

# --- init's respawn, standing in, at a boot with monit busy for 8 s ---
echo $(( $(cut -d. -f1 /proc/uptime) + 8 )) > /tmp/monit-busy-until
(while [ ! -f /tmp/stop-init ]; do sh $BOOT; sleep 1; done) > /dev/null 2>&1 &

within 25 nesto_running && ok "boot, monit busy: Nesto runs" || fail "boot, monit busy: Nesto runs"
check "boot: nlclient stopped" nlclient_stopped
# monit agreed before nlclient stopped, so it doesn't start it again.
sleep 5
nlclient_stopped && ok "boot, monit busy: nlclient stays stopped" ||
  fail "boot, monit busy: monit started nlclient again"

# --- nlclient back anyway, while Nesto runs: stopped again ---
echo Running > /tmp/monit-nlclient
within 5 nlclient_running && ok "comeback: monit starts nlclient" || fail "comeback: monit didn't start it"
within 15 nlclient_stopped && ok "comeback: stopped again" || fail "comeback: nlclient still running"
check "comeback: logged" logged "nlclient came back while Nesto runs"
nesto_running && ok "comeback: Nesto still runs" || fail "comeback: Nesto stopped"
logged "fake nesto: --hold-for-stock" && ok "boot: Nesto may be held for the stock software" ||
  fail "boot: no --hold-for-stock"

# --- An update: Nesto stops, and starts again ---
before="$(pidof nesto)"
sh $BOOT restart > /dev/null
check "restart: Nesto runs again" sh -c "pidof nesto > /dev/null && [ \"\$(pidof nesto)\" != $before ]"
nlclient_stopped && ok "restart: nlclient stays stopped" || fail "restart: nlclient started"

# --- Stock mode, across reboots, and back ---
sh $BOOT stock > /dev/null
check "stock: nlclient back" nlclient_running
check "stock: Nesto stopped" nesto_stopped
[ -f $NESTO_DIR/stock ] && ok "stock: kept on the flash" || fail "stock: no flag on the flash"
# A reboot: this boot's state goes, the flag stays.
rm -rf "$NESTO_RUN_DIR"/*
sleep 7
nesto_stopped && nlclient_running && ok "stock: still the stock software after a reboot" ||
  fail "stock: Nesto came back after a reboot"
sh $BOOT nesto > /dev/null
check "nesto: Nesto runs again" nesto_running
check "nesto: nlclient stopped" nlclient_stopped

# --- Paused for a run of something else, and resumed ---
sh $BOOT pause > /dev/null
check "pause: nlclient back" nlclient_running
check "pause: Nesto stopped" nesto_stopped
[ -f $NESTO_DIR/stock ] && fail "pause: set stock mode"
sh $BOOT resume > /dev/null
check "resume: Nesto runs again" nesto_running

# --- Crashing: three exits within 10 minutes give back until the next boot ---
fake_mode crash
sh $BOOT restart > /dev/null
check "crashes: the stock software runs" nlclient_running
check "crashes: logged" logged "exited 3 times within 10 minutes"
sleep 3
nesto_stopped && ok "crashes: no more tries" || fail "crashes: Nesto still restarting"
fake_mode run
sh $BOOT resume > /dev/null
check "crashes, resumed: Nesto runs" nesto_running

# --- Held as it starts: the stock software until the next boot ---
fake_mode hold
sh $BOOT restart > /dev/null
check "held: the stock software runs" nlclient_running
check "held: logged" logged "the display was held"
fake_mode run
sh $BOOT resume > /dev/null
check "held, resumed: Nesto runs" nesto_running

# --- The status ---
sh $BOOT status > /tmp/out
grep -q "mode: Nesto" /tmp/out && grep -q "installed:" /tmp/out && ok "status: mode and install" ||
  { fail "status:"; cat /tmp/out >&2; }

# --- The log stays small ---
head -c 300000 /dev/zero | tr '\0' x > $LOG
sh $BOOT restart > /dev/null
check "log: trimmed at a start" sh -c "[ \$(wc -c < $LOG) -lt 262144 ]"
[ -f $LOG.1 ] && ok "log: the older one kept" || fail "log: no nesto.log.1"

# --- Uninstalling: inittab as before, the stock software back ---
sh $BOOT uninstall > /dev/null
touch /tmp/stop-init
grep -q nesto-boot /etc/inittab && fail "uninstall: the line is still there" || ok "uninstall: the line is gone"
check "uninstall: nlclient back" nlclient_running
check "uninstall: Nesto stopped" nesto_stopped

[ "$failures" -eq 0 ] && echo "all good" || echo "$failures failed" >&2
exit "$failures"
