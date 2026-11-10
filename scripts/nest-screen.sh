#!/bin/sh
# Takes the screen, ring and press of a rooted Nest Learning Thermostat (2nd
# generation) away from the stock software, and gives them back. Runs in the
# thermostat's busybox shell; scripts/dev.sh device-screen sends it over SSH
# (docs/DEVICE.md).
#
#   nest-screen.sh status          what runs, and who holds the screen
#   nest-screen.sh take [SECONDS]  stop nlclient; give it back after SECONDS
#                                  (default 600) whatever happens
#   nest-screen.sh give            start nlclient again now
#   nest-screen.sh watch SECONDS   report every 15 s for SECONDS: uptime,
#                                  nlclient, the heartbeat, new syslog lines
#   nest-screen.sh trial [SECONDS] take, watch for SECONDS (default 300), give
#   nest-screen.sh run SECONDS [NESTO OPTIONS]
#                                  take, run /nesto/nesto for at most SECONDS
#                                  with its memory reported, give
#
# The stock program nlclient owns the screen, the ring, the press and the
# backplate. monit restarts it if it stops, so taking it over means asking
# monit to stop watching it first, as unfr0zen's own Doom mode does. While it's
# stopped the thermostat doesn't control the heating.
#
# Safety: "take" first arms a timer, detached from the SSH session, that gives
# the screen back after SECONDS. If SSH drops, or anything goes wrong, the
# stock software comes back on its own. A reboot brings it back too, since
# nothing here is saved: monit watches nlclient again after every boot.
# Giving back always stops Nesto first.
#
# While Nesto runs, monit still watches the whole system: memory in use over
# 41 MB for 5 cycles (25 s) reboots the thermostat. "run" reports it every
# 30 s, with Nesto's own share.

MONIT=/usr/sbin/monit
NESTLABS=/etc/init.d/nestlabs
# A copy of this script in RAM, so the timer can run it after SSH is gone;
# /tmp is noexec, which doesn't matter to "sh FILE".
SELF=/tmp/nesto-screen.sh
TIMER_PID=/tmp/nesto-screen-timer.pid
SYSLOG=/var/log/messages
# Programs may only run from / and /media/scratch, and nlclient's start
# empties the latter (docs/DEVICE.md).
NESTO=/nesto/nesto

say() {
  echo "[$(date +%H:%M:%S) up $(cut -d' ' -f1 /proc/uptime)s] $*"
}

nlclient_pid() {
  pidof nlclient 2> /dev/null
}

nesto_pid() {
  pidof nesto 2> /dev/null
}

# Memory in use as monit counts it, in KiB: what isn't free, buffers or cache.
memory_used_kb() {
  awk '/^MemTotal:/ {t = $2} /^(MemFree|Buffers|Cached):/ {f += $2} END {print t - f}' /proc/meminfo
}

# Memory report: the system against monit's limit, and Nesto's own share.
memory() {
  local rss="" nesto=""
  [ -n "${1:-}" ] && rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$1/status" 2> /dev/null)"
  [ -n "$rss" ] && nesto="; Nesto $((rss / 1024)) MB"
  say "memory in use $(( $(memory_used_kb) / 1024 )) MB (monit reboots above 41 MB)$nesto"
}

# Stops Nesto, if it runs: TERM, which it handles, then KILL after 5 s.
stop_nesto() {
  local pid i
  pid="$(nesto_pid)" || return 0
  say "stopping Nesto (pid $pid)"
  kill $pid 2> /dev/null
  for i in 1 2 3 4 5; do
    nesto_pid > /dev/null || return 0
    sleep 1
  done
  kill -9 $pid 2> /dev/null
}

monit_line() {
  "$MONIT" summary 2> /dev/null | grep -E "'$1'" | sed "s/^Process //"
}

status() {
  say "nlclient: $(nlclient_pid > /dev/null && echo "running, pid $(nlclient_pid)" || echo stopped)"
  say "monit: $(monit_line nlclient)"
  say "monit: $(monit_line nlheartbeatd)"
  nesto_pid > /dev/null && say "Nesto: running, pid $(nesto_pid)"
  memory
  holder=""
  for fd in /proc/[0-9]*/fd/*; do
    case "$(readlink "$fd" 2> /dev/null)" in
      /dev/fb0)
        pid="${fd#/proc/}"
        holder="$holder ${pid%%/*}"
        ;;
    esac
  done
  say "the screen (/dev/fb0) is open in:${holder:- nothing}"
  if [ -f "$TIMER_PID" ] && kill -0 "$(cat "$TIMER_PID")" 2> /dev/null; then
    say "a give-back timer is armed (pid $(cat "$TIMER_PID"))"
  fi
}

cancel_timer() {
  if [ -f "$TIMER_PID" ]; then
    kill "$(cat "$TIMER_PID")" 2> /dev/null
    rm -f "$TIMER_PID"
  fi
}

take() {
  local seconds="${1:-600}"
  if ! nlclient_pid > /dev/null; then
    say "nlclient isn't running; nothing to take"
    return 1
  fi
  # Arm the timer before anything else.
  cancel_timer
  [ "$0" = "$SELF" ] || cp "$0" "$SELF"
  setsid sh -c "sleep $seconds; sh $SELF timer-give" > /dev/null 2>&1 < /dev/null &
  echo $! > "$TIMER_PID"
  say "the screen comes back by itself in $seconds s (timer pid $!)"

  say "monit stops watching nlclient"
  "$MONIT" unmonitor nlclient
  say "stopping nlclient"
  "$NESTLABS" monit_stop > /dev/null 2>&1
  # monit_stop waits for nlclient to end (TERM, then harder after 180 s); make
  # sure before going on.
  for i in 1 2 3 4 5 6 7 8 9 10; do
    nlclient_pid > /dev/null || break
    sleep 1
  done
  if nlclient_pid > /dev/null; then
    say "nlclient is still running; giving back"
    give
    return 1
  fi
  say "nlclient stopped: the screen, ring and press are free"
}

give() {
  cancel_timer
  stop_nesto
  say "monit watches nlclient again, and starts it"
  "$MONIT" monitor nlclient
  "$MONIT" start nlclient
  for i in $(seq 1 30); do
    nlclient_pid > /dev/null && break
    sleep 1
  done
  if nlclient_pid > /dev/null; then
    say "nlclient is back (pid $(nlclient_pid))"
  else
    say "nlclient isn't back yet; monit keeps trying (check: monit summary)"
  fi
  rm -f "$SELF"
}

watch() {
  local seconds="${1:-300}" lines=0 now waited=0
  [ -f "$SYSLOG" ] && lines="$(wc -l < "$SYSLOG")"
  while [ "$waited" -lt "$seconds" ]; do
    sleep 15
    waited=$((waited + 15))
    say "after ${waited} s: nlclient $(nlclient_pid > /dev/null && echo running || echo stopped);" \
      "heartbeat: $(monit_line nlheartbeatd | sed 's/  */ /g')"
    if [ -f "$SYSLOG" ]; then
      now="$(wc -l < "$SYSLOG")"
      # The log rotates; if it shrank, show its end.
      if [ "$now" -lt "$lines" ]; then lines=0; fi
      tail -n "+$((lines + 1))" "$SYSLOG" | sed 's/^/    log: /'
      lines="$now"
    fi
  done
}

run() {
  local seconds="$1" pid deadline reporter code
  shift
  if [ ! -x "$NESTO" ]; then
    say "$NESTO isn't there"
    return 1
  fi
  # The timer outlasts the run by a minute, in case this shell is killed.
  take $((seconds + 60)) || return 1
  "$NESTO" "$@" &
  pid=$!
  say "Nesto started (pid $pid), for at most $seconds s; Ctrl-C stops it"
  # Ctrl-C, or the session ending, stops Nesto; giving back follows below.
  trap 'kill $pid 2> /dev/null' INT TERM HUP
  (sleep "$seconds" && say "time is up" && kill $pid 2> /dev/null) &
  deadline=$!
  (while sleep 30 && kill -0 $pid 2> /dev/null; do memory $pid; done) &
  reporter=$!
  # A signal ends "wait" early; wait again until Nesto has really gone.
  code=0
  while kill -0 $pid 2> /dev/null; do
    wait $pid
    code=$?
  done
  trap - INT TERM HUP
  kill $deadline $reporter 2> /dev/null
  say "Nesto ended (exit code $code)"
  give
}

case "${1:-}" in
  status)
    status
    # Keep the copy in RAM only while a timer may need it.
    [ -f "$TIMER_PID" ] || rm -f "$SELF"
    ;;
  take) take "$2" && status ;;
  give) give && status ;;
  timer-give)
    # The timer itself: it mustn't cancel itself first.
    rm -f "$TIMER_PID"
    logger -t nesto-screen "the give-back timer ran out: giving the screen back" 2> /dev/null
    give
    ;;
  watch)
    watch "$2"
    [ -f "$TIMER_PID" ] || rm -f "$SELF"
    ;;
  trial)
    # The timer outlasts the watch by two minutes, in case the session drops
    # just before "give".
    trial_seconds="${2:-300}"
    status
    take $((trial_seconds + 120)) || exit 1
    watch "$trial_seconds"
    give
    status
    ;;
  run)
    if ! [ "${2:-}" -gt 0 ] 2> /dev/null; then
      echo "usage: nest-screen.sh run SECONDS [NESTO OPTIONS]" >&2
      exit 2
    fi
    run_seconds="$2"
    shift 2
    status
    run "$run_seconds" "$@"
    status
    ;;
  *)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
