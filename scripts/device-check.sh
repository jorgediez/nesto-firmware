#!/bin/sh
# Describes a rooted Nest Learning Thermostat (2nd generation) for Nesto's
# Phase 2, without changing anything: it only reads and prints. Runs in the
# thermostat's busybox shell; scripts/dev.sh device-check sends it over SSH and
# saves what it prints (docs/DEVICE.md).
#
# What Nesto needs to know before it can replace the stock screen software:
# what starts and restarts that software, what keeps Wi-Fi and the watchdog
# going, who holds the screen, the ring, the backplate's serial port and the
# mDNS port, and how much memory and storage are free.
#
# It never prints Wi-Fi passwords or keys: configuration lines that may hold a
# secret are replaced by a note. MAC addresses and the network's name do
# appear, so read the report before sharing it.
#
# Deliberately not done: opening /dev/watchdog (that starts it), the
# framebuffer, input devices or /dev/ttyO2 (the stock software owns them), and
# any command that restarts or reconfigures.
#
# The one thing it writes: when nesto-probe was sent along (dev.sh does that),
# a copy of it goes where programs may run from, and is deleted after it ran.
# On a Nest every RAM file system is mounted noexec, so that is the flash's
# scratch partition, /media/scratch, which the stock software keeps for
# temporary files.

section() {
  printf '\n## %s\n' "$1"
}

# Prints a command and what it prints; missing commands and errors are part
# of the report, never a reason to stop.
show() {
  printf '$ %s\n' "$1"
  sh -c "$1" 2>&1 | sed 's/^/    /'
}

# Shows a file with any line that may hold a secret hidden. A property list's
# <key> tags are only names, so they don't count as mentioning a key; but when
# a key's name sounds secret, its value on the next line is hidden too.
show_file() {
  if [ -f "$1" ]; then
    printf -- '--- %s\n' "$1"
    awk 'hide_value { hide_value = 0; print "    (a line that may hold a secret is hidden)"; next }
         { line = tolower($0); gsub(/<\/?key>/, "", line) }
         line ~ /psk|pass|secret|token|key|credential/ {
           if ($0 ~ /<\/key>/) hide_value = 1
           print "    (a line that may hold a secret is hidden)"; next
         }
         { print "    " $0 }' "$1"
  elif [ -e "$1" ]; then
    printf -- '--- %s (not a regular file)\n' "$1"
  else
    printf -- '--- %s (none)\n' "$1"
  fi
}

have() {
  command -v "$1" > /dev/null 2>&1
}

# The processes holding a path open, by matching the links in /proc/*/fd.
holders() {
  for fd in /proc/[0-9]*/fd/*; do
    target="$(readlink "$fd" 2> /dev/null)" || continue
    case "$target" in
      $1)
        pid="${fd#/proc/}"
        pid="${pid%%/*}"
        name="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null)"
        printf '    %s is open in pid %s: %s\n' "$target" "$pid" "${name:-?}"
        ;;
    esac
  done | sort -u
}

echo "# Nesto device check"
echo "Made by scripts/device-check.sh, which only reads. Taken at: $(date -u 2>&1) (device clock)"

section "System"
show "uname -a"
show "cat /proc/version"
show "cat /proc/cmdline"
show "uptime"
show "cat /proc/cpuinfo"
for f in /etc/version /etc/issue /etc/os-release /etc/nestlabs/version /etc/nestlabs/build_info; do
  [ -f "$f" ] && show_file "$f"
done
show "ls -la /etc/nestlabs"

section "Memory"
show "free"
show "cat /proc/meminfo"
show "ps"
# Memory per process, where ps can say: the budget Nesto gets is roughly what
# nlclient uses now.
show "ps -eo pid,rss,vsz,args 2> /dev/null | grep -v ' 0 *0 \\[' | sort -k2 -n -r | head -25"
if have top; then
  show "top -b -n 1 | head -40"
fi

section "Storage"
show "cat /proc/mtd"
show "cat /proc/mounts"
show "df -k"
show "ls -la / /media"
show "ls -la /sys/class/ubi 2> /dev/null && for v in /sys/class/ubi/ubi*_*; do echo \"\$v: \$(cat \$v/name 2>/dev/null) \$(cat \$v/data_bytes 2>/dev/null) bytes\"; done"

section "Start-up and supervision"
show_file /etc/inittab
show "ls -la /etc/init.d"
show_file /etc/init.d/rcS
show_file /etc/init.d/rcH
for f in /etc/monitrc /etc/monit/monitrc /etc/monit.d/* /etc/monit/conf.d/*; do
  [ -e "$f" ] && show_file "$f"
done
if have monit; then
  show "monit -V"
  # Asks the running monit, over its local-only HTTP port, what it watches.
  show "monit summary"
fi
# The stock services Nesto lives next to: what starts and stops nlclient, the
# heartbeat and watchdog daemons, sleep and shutdown, and mDNS.
for f in nestlabs heartbeat watchdog nlsleep nlshutdown mdnsd connman wpasupplicant networking; do
  show_file "/etc/init.d/$f"
done
for f in /etc/nestlabs/*.options /etc/nestlabs/*.conf /etc/nestlabs/*.plist; do
  [ -e "$f" ] && show_file "$f"
done
show "ls -la /nestlabs /nestlabs/sbin /nestlabs/bin /nestlabs/etc"

section "Watchdog"
show "ls -la /dev/watchdog* /dev/wdt* /sys/class/watchdog 2> /dev/null"
show "grep -i -E 'watchdog|wdt' /proc/devices /proc/misc 2> /dev/null"
holders '*watchdog*'
show "dmesg | grep -i -E 'watchdog|wdt' | tail -20"
show "ps | grep -i -E 'watchdog|wdt' | grep -v grep"

section "Network"
show "ifconfig -a"
show "route -n"
show_file /etc/resolv.conf
show_file /etc/hosts
show "netstat -ln 2> /dev/null || cat /proc/net/tcp /proc/net/udp"
show "netstat -lnp 2> /dev/null | grep -E ':5353|:6170|:22 '"
show "cat /proc/net/igmp"
show "ps | grep -i -E 'wpa|connman|dhcp|udhcpc|ntp|avahi|mdns|dropbear|sshd|nlclient|nestlocal|monit' | grep -v grep"
if have wpa_cli; then
  show "wpa_cli status"
fi
if have iwconfig; then
  show "iwconfig"
fi
show "ls -la /etc/wpa_supplicant* /etc/connman /var/lib/connman 2> /dev/null"

section "Screen"
for f in /sys/class/graphics/fb0/*; do
  case "$f" in
    */virtual_size | */bits_per_pixel | */stride | */mode | */modes | */blank | */name | */rotate | */pan)
      printf '    %s: %s\n' "$f" "$(cat "$f" 2>&1 | tr '\n' ' ')"
      ;;
  esac
done
holders '/dev/fb*'
show "ls -la /sys/class/backlight"
for d in /sys/class/backlight/*; do
  [ -d "$d" ] || continue
  for f in brightness max_brightness actual_brightness bl_power; do
    [ -f "$d/$f" ] && printf '    %s/%s: %s\n' "$d" "$f" "$(cat "$d/$f" 2>&1)"
  done
done
show "ls -la /sys/class/leds /sys/class/lcd 2> /dev/null"
show "dmesg | grep -i -E 'fb|lcd|dss|backlight|panel' | tail -30"

section "Ring, press and other input"
show "cat /proc/bus/input/devices"
show "ls -la /dev/input"
holders '/dev/input/*'

section "Backplate"
show "ls -la /dev/ttyO*"
holders '/dev/ttyO*'
show "dmesg | grep -i -E 'ttyO|serial|uart' | tail -20"

section "Power and sleep"
show "cat /sys/power/state"
show "ls /sys/class/power_supply 2> /dev/null && for s in /sys/class/power_supply/*; do echo \"\$s: \$(cat \$s/type 2>/dev/null) \$(cat \$s/status 2>/dev/null) \$(cat \$s/voltage_now 2>/dev/null)\"; done"

section "Software"
show "ls -la /lib/ld-* /lib/libc.so* /lib/libc-* 2> /dev/null"
show "busybox 2>&1 | head -1"
show "busybox --list 2> /dev/null | tr '\n' ' '; echo"
for tool in sh dropbear dropbearmulti sshd scp sftp-server tar gzip xz wget curl openssl \
  monit start-stop-daemon wpa_cli iwconfig ntpd ntpdate; do
  if have "$tool"; then
    printf '    %s: %s\n' "$tool" "$(command -v "$tool")"
  else
    printf '    %s: missing\n' "$tool"
  fi
done

section "Kernel"
show "lsmod 2> /dev/null || cat /proc/modules"
if [ -f /proc/config.gz ] && have zcat; then
  show "zcat /proc/config.gz | grep -E 'CONFIG_(VFP|VFPv3|NEON|WATCHDOG|OMAP_WATCHDOG|WATCHDOG_NOWAYOUT|FB|FB_OMAP2|OMAP2_DSS|INPUT_EVDEV|TMPFS|INOTIFY_USER|EPOLL|SIGNALFD|TIMERFD|EVENTFD|UBIFS_FS|JFFS2_FS|IP_MULTICAST)[= ]'"
else
  echo "    (no /proc/config.gz)"
fi
show "dmesg | tail -40"

section "Probe"
probe="$(dirname "$0")/nesto-probe"
if [ ! -f "$probe" ]; then
  echo "    (nesto-probe wasn't sent along; scripts/dev.sh device-check sends it)"
else
  # The first of these mounted without noexec: RAM file systems first, the
  # flash's scratch partition last.
  run_dir=""
  for d in /tmp /var/run /run /dev/shm /media/scratch; do
    if awk -v d="$d" '$2 == d && $4 !~ /(^|,)noexec(,|$)/ { found = 1 } END { exit !found }' /proc/mounts; then
      run_dir="$d"
      break
    fi
  done
  if [ -z "$run_dir" ]; then
    echo "    (skipped: nowhere to run it from; every candidate is mounted noexec)"
  else
    echo "    running from $run_dir"
    copy="$run_dir/nesto-probe-run.$$"
    if cp "$probe" "$copy" && chmod 700 "$copy"; then
      "$copy" --temp-dir /tmp
      echo "(nesto-probe exited with $?)"
    else
      echo "    (couldn't copy it to $run_dir)"
    fi
    rm -f "$copy"
  fi
fi

printf '\n## End of report\n'
