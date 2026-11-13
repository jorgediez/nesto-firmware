#!/bin/sh
# Runs Nesto on a rooted Nest Learning Thermostat (2nd generation) for good,
# started at boot by busybox init (docs/DEVICE.md). One line in /etc/inittab,
# "::respawn:/bin/sh /nesto/nesto-boot.sh", runs it once the stock start-up
# (rcS, which starts monit, which starts nlclient) is done, and again whenever
# it exits.
#
#   nesto-boot.sh            the service: take the screen over from nlclient
#                            and run Nesto; init runs it again when it exits
#   nesto-boot.sh stock      the stock software from now on, across reboots
#   nesto-boot.sh nesto      Nesto again, from now on
#   nesto-boot.sh pause      the stock software until resume or a reboot,
#                            for a run of something else (dev.sh device-run)
#   nesto-boot.sh resume     Nesto again after a pause
#   nesto-boot.sh restart    stop Nesto; the service starts it again (a new
#                            /nesto/nesto, after an update)
#   nesto-boot.sh status     the mode, what runs, Nesto's recent exits, the log
#   nesto-boot.sh install    add the inittab line, keeping a copy of inittab
#   nesto-boot.sh uninstall  take the line out, and give back for good
#
# Giving back: if Nesto exits 3 times within 10 minutes, or the display is
# held as it starts (it exits with 3), the service gives the thermostat back
# to the stock software until the next boot, and waits. Nothing here touches
# the stock start-up, so a thermostat without the inittab line is as before.

MONIT="${NESTO_MONIT:-/usr/sbin/monit}"
NESTLABS="${NESTO_NESTLABS:-/etc/init.d/nestlabs}"
INITTAB="${NESTO_INITTAB:-/etc/inittab}"
INIT_PID="${NESTO_INIT_PID:-1}"
NESTO="${NESTO_BIN:-/nesto/nesto}"
SELF="${NESTO_BOOT:-/nesto/nesto-boot.sh}"
DIR="${NESTO_DIR:-/media/data/nesto}"
LOG="$DIR/nesto.log"
LOG_MAX=262144  # bytes; one older file is kept
STOCK="$DIR/stock"  # stock mode, kept across reboots
# This boot only: in RAM, gone after a reboot.
RUN="${NESTO_RUN_DIR:-/tmp}"
GIVEN="$RUN/nesto-boot.given"  # given back until the next boot, or resume
EXITS="$RUN/nesto-boot.exits"  # when Nesto exited, in seconds of uptime
PID_FILE="$RUN/nesto-boot.pid"  # the service while it waits, given back
STARTED="$RUN/nesto-boot.started"  # Nesto has run this boot
LINE="::respawn:/bin/sh $SELF"
EXIT_WINDOW=600
EXIT_LIMIT=3
HOLD_EXIT=3  # Nesto's exit code when the display was held as it started

uptime_s() { cut -d. -f1 /proc/uptime; }

say() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S') up $(uptime_s)s] nesto-boot: $*"
}

# Keeps the log under LOG_MAX: the older half goes to nesto.log.1. Nesto
# appends, so emptying the file in place is safe while it writes.
trim_log() {
  [ -f "$LOG" ] || return 0
  if [ "$(wc -c < "$LOG")" -gt "$LOG_MAX" ]; then
    cp "$LOG" "$LOG.1" && : > "$LOG"
  fi
}

log() {
  mkdir -p "$DIR"
  say "$*" >> "$LOG"
}

nlclient_pid() { pidof nlclient 2> /dev/null; }
nesto_pid() { pidof nesto 2> /dev/null; }

# Stops Nesto: TERM, which it handles, then KILL after 5 s.
stop_nesto() {
  local pid i
  pid="$(nesto_pid)" || return 0
  kill $pid 2> /dev/null
  for i in 1 2 3 4 5; do
    nesto_pid > /dev/null || return 0
    sleep 1
  done
  kill -9 $pid 2> /dev/null
}

# Waits until init and monit have nlclient running, at most $1 seconds.
wait_for_nlclient() {
  local i=0
  while [ "$i" -lt "$1" ]; do
    nlclient_pid > /dev/null && return 0
    sleep 1
    i=$((i + 1))
  done
  return 1
}

# Whether monit says it no longer watches nlclient.
unwatched() {
  "$MONIT" summary 2> /dev/null | grep "'nlclient'" | grep -qi "not monitored"
}

# Asks monit to stop watching nlclient until it says it has: while it's busy,
# starting nlclient at boot, it refuses ("Other action already in progress"),
# and a refusal unnoticed means monit starts nlclient again after it stops.
unwatch() {
  local i=0
  while [ "$i" -lt "${NESTO_UNWATCH_TRIES:-45}" ]; do
    "$MONIT" unmonitor nlclient > /dev/null 2>&1
    unwatched && return 0
    sleep 2
    i=$((i + 1))
  done
  return 1
}

take() {
  if ! unwatch; then
    log "monit kept watching nlclient"
    return 1
  fi
  nlclient_pid > /dev/null || return 0
  "$NESTLABS" monit_stop > /dev/null 2>&1
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
    nlclient_pid > /dev/null || return 0
    sleep 1
  done
  ! nlclient_pid > /dev/null
}

give() {
  stop_nesto
  "$MONIT" monitor nlclient > /dev/null 2>&1
  "$MONIT" start nlclient > /dev/null 2>&1
}

# The service, given back: it waits, so init doesn't run it again and again,
# until "nesto" or "resume" ends the wait.
wait_given() {
  echo $$ > "$PID_FILE"
  while [ -f "$GIVEN" ] || [ -f "$STOCK" ]; do
    sleep 5
  done
  exit 0
}

# How many times Nesto exited within EXIT_WINDOW, counting one now.
count_exit() {
  local now
  now="$(uptime_s)"
  echo "$now" >> "$EXITS"
  awk -v now="$now" -v w="$EXIT_WINDOW" '$1 > now - w {n++} END {print n + 0}' "$EXITS"
}

service() {
  mkdir -p "$DIR"
  trim_log
  if [ -f "$STOCK" ]; then
    log "stock mode ($STOCK): the stock software runs"
    give
    wait_given
  fi
  [ -f "$GIVEN" ] && wait_given
  if [ ! -x "$NESTO" ]; then
    log "$NESTO isn't there: the stock software runs"
    touch "$GIVEN"
    give
    wait_given
  fi
  # At boot, monit starts nlclient only after rcS; wait for it, so taking over
  # isn't undone by monit starting it afterwards.
  if [ ! -f "$STARTED" ] && ! nlclient_pid > /dev/null; then
    wait_for_nlclient 120 || log "nlclient never started; going on"
  fi
  if ! take; then
    log "nlclient wouldn't stop: the stock software runs until the next boot"
    touch "$GIVEN"
    give
    wait_given
  fi
  # Only a waiting service is ever ended from outside, never one running Nesto.
  rm -f "$PID_FILE"
  touch "$STARTED"
  log "starting Nesto"
  # While Nesto runs: the log stays small, and if nlclient comes back anyway,
  # monit is told again and it's stopped, so they never share the screen.
  (
    n=0
    while sleep 10; do
      if nlclient_pid > /dev/null; then
        log "nlclient came back while Nesto runs: stopping it"
        take || log "couldn't stop nlclient"
      fi
      n=$((n + 1))
      [ $((n % 60)) = 0 ] && trim_log
    done
  ) &
  local keeper=$! code
  "$NESTO" --hold-for-stock >> "$LOG" 2>&1
  code=$?
  kill $keeper 2> /dev/null
  log "Nesto exited with $code"
  if [ -f "$STOCK" ] || [ -f "$GIVEN" ]; then
    give
    log "given back to the stock software"
    wait_given
  fi
  if [ "$code" = "$HOLD_EXIT" ]; then
    log "the display was held as Nesto started: the stock software until the next boot"
    touch "$GIVEN"
    give
    wait_given
  fi
  if [ "$code" != 0 ] && [ "$(count_exit)" -ge "$EXIT_LIMIT" ]; then
    log "Nesto exited $EXIT_LIMIT times within $((EXIT_WINDOW / 60)) minutes:" \
      "the stock software until the next boot"
    touch "$GIVEN"
    give
    wait_given
  fi
  # Exit, and init runs this again: Nesto restarts.
  exit 0
}

# Ends a waiting service, so init runs it again.
wake_service() {
  local pid
  pid="$(cat "$PID_FILE" 2> /dev/null)"
  [ -n "$pid" ] && kill "$pid" 2> /dev/null
  rm -f "$PID_FILE"
}

status() {
  local mode="Nesto"
  [ -f "$GIVEN" ] && mode="the stock software until the next boot (or resume)"
  [ -f "$STOCK" ] && mode="the stock software (stock mode)"
  say "mode: $mode"
  grep -qF "$LINE" "$INITTAB" 2> /dev/null && say "installed: $INITTAB starts it" ||
    say "not installed: $INITTAB doesn't start it"
  say "Nesto: $(nesto_pid > /dev/null && echo "running, pid $(nesto_pid)" || echo stopped)"
  say "nlclient: $(nlclient_pid > /dev/null && echo "running, pid $(nlclient_pid)" || echo stopped)"
  [ -s "$EXITS" ] && say "Nesto's exits this boot (uptime): $(tr '\n' ' ' < "$EXITS")"
  if [ -f "$LOG" ]; then
    say "the log's end ($LOG):"
    tail -n 20 "$LOG" | sed 's/^/    /'
  fi
}

install_line() {
  [ -f "$INITTAB.nesto-backup" ] || cp "$INITTAB" "$INITTAB.nesto-backup" || return 1
  if ! grep -qF "$LINE" "$INITTAB"; then
    echo "$LINE" >> "$INITTAB" || return 1
  fi
  # busybox init reads inittab again on HUP, and starts the new line.
  kill -HUP "$INIT_PID" 2> /dev/null
  say "installed: $INITTAB starts $SELF (its copy: $INITTAB.nesto-backup)"
}

uninstall_line() {
  local tmp="$INITTAB.nesto-new"
  grep -vF "$LINE" "$INITTAB" > "$tmp" && cat "$tmp" > "$INITTAB" && rm -f "$tmp"
  kill -HUP "$INIT_PID" 2> /dev/null
  # Whatever init does with the line's process, it's ended here.
  touch "$GIVEN"
  wake_service
  give
  rm -f "$GIVEN" "$EXITS"
  say "uninstalled: $INITTAB is as before, and the stock software runs"
}

case "${1:-}" in
  "") service ;;
  stock)
    mkdir -p "$DIR"
    touch "$STOCK"
    log "stock mode set"
    give
    say "the stock software runs, now and after reboots, until: nesto-boot.sh nesto"
    ;;
  nesto)
    rm -f "$STOCK" "$GIVEN"
    log "Nesto mode set"
    wake_service
    say "Nesto starts in a moment"
    ;;
  pause)
    touch "$GIVEN"
    log "paused"
    give
    # Whoever paused it may want to take nlclient's place in turn.
    wait_for_nlclient 30 || say "nlclient isn't back yet; monit keeps trying"
    say "the stock software runs until: nesto-boot.sh resume (or a reboot)"
    ;;
  resume)
    rm -f "$GIVEN" "$EXITS"
    log "resumed"
    wake_service
    say "Nesto starts in a moment"
    ;;
  restart)
    log "restarting Nesto"
    stop_nesto
    say "Nesto stopped; the service starts it again"
    ;;
  status) status ;;
  install) install_line ;;
  uninstall) uninstall_line ;;
  *)
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
