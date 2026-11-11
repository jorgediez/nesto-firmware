#!/bin/sh
# Runs scripts/nest-screen.sh against a pretend thermostat, inside a busybox
# container: stand-ins for monit, nlclient, the nestlabs init script, Nesto,
# and nlshutdownd's battery lines in the system log. scripts/dev.sh
# check-report runs it; it prints "ok" lines, and exits non-zero on a failure.
#
#   docker run --rm -v "$PWD/scripts":/scripts:ro -v "$PWD/tests/device":/t:ro \
#     busybox:1.37.0 sh /t/fake-nest.sh

set -u
SCREEN=/scripts/nest-screen.sh
export NESTO_SYSLOG=/var/log/messages
export NESTO_RUN_LOG=/media/data/nesto/run.log
export NESTO_REPORT_EVERY=2
failures=0

ok() { echo "ok:   $*"; }
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}
# expect FILE PATTERN WHAT: FILE has a line matching PATTERN.
expect() {
  if grep -q -- "$2" "$1"; then ok "$3"; else fail "$3: no \"$2\" in:"; sed 's/^/      /' "$1" >&2; fi
}
nlclient_running() { pidof nlclient > /dev/null; }
battery_line() {
  echo "Nov 10 nlscpm-shutdown-manager[316]: shutdown-manager: batteryLevel @ $1" >> "$NESTO_SYSLOG"
}

# --- The pretend thermostat ---
mkdir -p /nestlabs/sbin /usr/sbin /etc/init.d /var/log /nesto /media/data/nesto
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
cat > /usr/sbin/monit << 'EOF'
#!/bin/sh
state=/tmp/monit-nlclient
[ -f $state ] || echo Running > $state
case "$1" in
  unmonitor) echo "Not monitored" > $state ;;
  monitor) echo Running > $state ;;
  start) pidof nlclient > /dev/null || /etc/init.d/nestlabs monit_start ;;
  summary)
    echo "Process 'nlheartbeatd'               Running"
    echo "Process 'nlclient'                   $(cat $state)" ;;
esac
EOF
# Nesto: says what it was given, stops on TERM.
cat > /nesto/nesto << 'EOF'
#!/bin/sh
echo "fake nesto: $*"
trap 'echo "fake nesto: TERM"; exit 0' TERM
while true; do sleep 1; done
EOF
chmod 755 /nestlabs/sbin/nlclient /etc/init.d/nestlabs /usr/sbin/monit /nesto/nesto
/etc/init.d/nestlabs monit_start
sleep 1
out=/tmp/out

# --- The battery's voltage, across the log's rotation ---
battery_line 3.912
mv "$NESTO_SYSLOG" "$NESTO_SYSLOG.0"
echo "Nov 10 something else" > "$NESTO_SYSLOG"
[ "$(sh $SCREEN battery)" = 3.912 ] && ok "battery: read from the rotated log" ||
  fail "battery: $(sh $SCREEN battery) instead of 3.912 from the rotated log"
battery_line 3.897
[ "$(sh $SCREEN battery)" = 3.897 ] && ok "battery: the latest line" ||
  fail "battery: $(sh $SCREEN battery) instead of 3.897"
rm -f "$NESTO_SYSLOG.0"
echo "nothing yet" > "$NESTO_SYSLOG"
if sh $SCREEN battery > /dev/null 2>&1; then fail "battery: no reading should fail"; else ok "battery: none yet"; fi

# --- A run with the battery steady ---
battery_line 3.890
sh $SCREEN run 7 --ring-reverse > $out 2>&1
expect $out "battery 3.890 V; Nesto gives way under 3.70 V" "run: the battery at the start"
expect $out "fake nesto: --ring-reverse" "run: Nesto gets its options"
expect $out "MB; battery 3.890 V" "run: reports carry the battery"
expect $out "time is up" "run: ends by its deadline"
expect $out "memory in use at most [0-9]* MB; battery 3.890 V at the start, 3.890 V at the lowest, 3.890 V at the end" \
  "run: the summary"
nlclient_running && ok "run: nlclient is back" || fail "run: nlclient isn't back"

# --- Too low to start ---
battery_line 3.650
sh $SCREEN run 30 > $out 2>&1
expect $out "the battery is at 3.650 V, under 3.70 V: not running Nesto" "low battery: Nesto doesn't start"
grep -q "fake nesto" $out && fail "low battery: Nesto ran anyway"
nlclient_running && ok "low battery: nlclient left alone" || fail "low battery: nlclient stopped"

# --- The battery falls during a run ---
battery_line 3.800
(sleep 3 && battery_line 3.690) &
sh $SCREEN run 60 > $out 2>&1
expect $out "the battery is under 3.70 V: stopping Nesto" "falling battery: Nesto stopped"
expect $out "3.800 V at the start, 3.690 V at the lowest, 3.690 V at the end" "falling battery: the summary"
grep -q "time is up" $out && fail "falling battery: ran to the deadline"
nlclient_running && ok "falling battery: nlclient is back" || fail "falling battery: nlclient isn't back"

# --- Detached: the session ends at once, the run goes on into the log ---
battery_line 3.880
echo "an earlier run" > "$NESTO_RUN_LOG"
started=$(cut -d. -f1 /proc/uptime)
sh $SCREEN detach 5 > $out 2>&1
took=$(( $(cut -d. -f1 /proc/uptime) - started ))
expect $out "running in the background" "detach: says where it runs"
[ "$took" -le 2 ] && ok "detach: returns at once" || fail "detach: took $took s to return"
expect "$NESTO_RUN_LOG.1" "an earlier run" "detach: the earlier log is kept"
sleep 2
pidof nesto > /dev/null && ok "detach: Nesto runs in the background" || fail "detach: Nesto isn't running"
if sh $SCREEN detach 5 > $out 2>&1; then fail "detach: a second run started"; else
  expect $out "Nesto is already running" "detach: one run at a time"
fi
for i in 1 2 3 4 5 6 7 8 9 10; do
  grep -q "nlclient is back" "$NESTO_RUN_LOG" && break
  sleep 1
done
expect "$NESTO_RUN_LOG" "time is up" "detach: ends by its deadline"
expect "$NESTO_RUN_LOG" "3.880 V at the end" "detach: the summary is in the log"
nlclient_running && ok "detach: nlclient is back" || fail "detach: nlclient isn't back"

# --- Detached, stopped early by "give" from another session ---
sh $SCREEN detach 60 > /dev/null 2>&1
sleep 2
sh $SCREEN give > /dev/null 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do
  grep -q "Nesto ended" "$NESTO_RUN_LOG" && break
  sleep 1
done
expect "$NESTO_RUN_LOG" "Nesto ended" "detach, given back early: the run ends"
pidof nesto > /dev/null && fail "detach, given back early: Nesto still runs"
nlclient_running && ok "detach, given back early: nlclient is back" || fail "detach, given back early: nlclient isn't back"
[ -f /tmp/nesto-screen-timer.pid ] && fail "a give-back timer is left over"

# --- Without the backplate, never long enough for its reset (about 30 minutes) ---
sh $SCREEN run 1561 --no-backplate > $out 2>&1
expect $out "over 1620 s: the backplate" "cap: a run without the backplate over 1560 s doesn't start"
grep -q "fake nesto" $out && fail "cap: Nesto ran anyway"
sh $SCREEN detach 1561 --input-test > $out 2>&1
expect $out "at most 1560 s: the backplate" "cap: a detached input test over 1560 s is refused at once"
# --- With it, up to NESTO_RUN_MAX ---
NESTO_RUN_MAX=1000 sh $SCREEN run 941 > $out 2>&1
expect $out "over 1000 s (NESTO_RUN_MAX)" "cap: a run talking to the backplate stops at NESTO_RUN_MAX"
grep -q "fake nesto" $out && fail "cap: Nesto ran anyway"
NESTO_RUN_MAX=1000 sh $SCREEN detach 941 > $out 2>&1
expect $out "at most 940 s (NESTO_RUN_MAX)" "cap: a detached run over NESTO_RUN_MAX is refused at once"
sh $SCREEN trial 1501 > $out 2>&1
expect $out "over 1620 s: the backplate" "cap: a trial over 1500 s doesn't start"
nlclient_running && ok "cap: nlclient left alone" || fail "cap: nlclient stopped"
[ -f /tmp/nesto-screen-timer.pid ] && fail "cap: a give-back timer was armed"

[ "$failures" -eq 0 ] && echo "all good" || echo "$failures failed" >&2
exit "$failures"
