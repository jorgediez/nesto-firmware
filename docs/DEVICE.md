# Checking a real thermostat

Before Nesto replaces the stock screen software, Phase 2 finds out how a real
Nest Learning Thermostat (2nd generation) is put together, without changing
how it works. You root it with [unfr0zen](https://github.com/unfr0zen/unfr0zen),
then `scripts/dev.sh device-check` describes it in a report.

Rooting is the only step that changes the thermostat. The check reads and
prints: it doesn't stop, restart or reconfigure anything. The stock software
keeps running and keeps controlling the heating throughout. The one thing it
writes is a temporary copy of `nesto-probe`, deleted after it ran (see step 5).

## What the check answers

- What starts the stock software (`nlclient`), and what restarts it
  (`monit`), so Nesto can take the screen over and give it back.
- Which watchdog exists, and who keeps it fed.
- What keeps Wi-Fi up, and whether something already holds UDP port 5353
  (mDNS), which Nesto needs to be found by Home Assistant.
- The screen's real pixel format, the backlight's controls, which input
  devices the ring and the press are, and who holds the backplate's serial
  port `/dev/ttyO2`.
- Free memory and storage, the C library and the tools on board.
- Whether the kernel, Linux 2.6.37, runs our programs. `nesto-probe`, built
  with Nesto's toolchain, tries everything Nesto needs from it: the clock,
  random numbers, NEON, the protocol's cryptography against the test vectors,
  a TCP server, the mDNS socket, and reading the screen's and the input
  devices' descriptions.

## Before you start

- A USB **data** cable (micro-USB); charge-only cables don't work. It goes in
  the USB port on the back of the display itself, detached from its base.
- Another way to keep the house warm while the display is off its base.
- unfr0zen, from its [releases](https://github.com/unfr0zen/unfr0zen/releases):
  the app, or its command line. It runs on Windows, macOS and Linux. On a Mac
  or Linux it needs no driver; on Windows it needs Zadig once (step 2).
- Optionally, an SSH key, so SSH doesn't ask for root's password. In WSL:
  `ssh-keygen -t ed25519`, then give unfr0zen `~/.ssh/id_ed25519.pub`.

## 1. Join Wi-Fi

On the thermostat: Settings → Network, and pick your network. SSH reaches the
thermostat only over Wi-Fi, since it has no other network port. No Google
account is needed. Note its IP address, which the technical information in
Settings shows, or your router's list of devices; giving it a fixed address in
your router helps later.

## 2. On Windows only: install the USB driver, once

Connect the display to the PC. With [Zadig](https://zadig.akeo.ie/), bind the
**WinUSB** driver to the device `0451:d00e` (the processor's boot ROM, which
unfr0zen talks to). A Mac or Linux computer skips this.

## 3. Back up the whole flash

In the app: Backups → Back up unit. From the command line:

```bash
unfr0zen backup
unfr0zen verify <the backup file it names>
```

unfr0zen tells you when to press or reconnect the display. The backup is read
twice and compared; `verify` compares it with the unit once more. Copy the
backup to another disk. It is the way back if anything goes wrong.

## 4. Root it, with SSH and a USB shell

In the app, choose no integration (no MQTT, Matter, Zigbee or Doom), turn on
the SSH server and the USB shell, set a root password, and optionally add your
SSH public key. Blocking the cloud is fine too. The command line does the same:

```bash
unfr0zen --dry-run --ssh --usb-shell --root-password <a password> [--ssh-public-key <key.pub>]
unfr0zen --backup --ssh --usb-shell --root-password <a password> [--ssh-public-key <key.pub>]
```

The first line lists what would change and writes nothing; the second does it.

- The SSH server (Dropbear) is new: the stock software has none. Root logs in
  with the password, or with the key if you gave one.
- The USB shell is a root shell on the USB cable, a way in if Wi-Fi or SSH
  ever stop working.
- Without an integration unfr0zen installs none of its own services, so the
  thermostat stays stock apart from SSH, the USB shell and, if you chose it,
  the blocked cloud.

Put the display back on its base. It boots as before.

## 5. Run the check

From WSL, in this repository:

```bash
scripts/dev.sh device-check <nest-ip>
```

The first time, SSH asks you to accept the thermostat's host key. Without a
key it asks for root's password, once. Any further arguments go to `ssh`, for
instance `-i ~/.ssh/other_key`. The check:

1. builds `nesto-probe` for the thermostat;
2. sends it and `scripts/device-check.sh` over one SSH session, unpacked into
   RAM (`/tmp`), and runs the script, which reads and prints;
3. runs `nesto-probe` from the first place programs may run from, and deletes
   it. On a Nest every RAM file system is mounted `noexec`, so that is the
   flash's scratch partition, `/media/scratch`, which the stock software keeps
   for temporary files;
4. deletes what it unpacked, and saves the report to `device-reports/`, which
   git ignores.

It takes a minute or so. The report never shows Wi-Fi passwords or keys, but
it does show MAC addresses, the serial number (the host name) and the Wi-Fi
network's name: read it before sharing it.

If SSH can't connect, the USB shell is still there: connect the display to
the computer, and see unfr0zen's Advanced section for opening it.

## Undoing it

To put the thermostat back exactly as it was, restore the backup: in the app,
Backups → Restore to unit, or `unfr0zen restore <the backup file>`.

## What a real thermostat showed

From the reports of a 2nd-generation Nest (model `Display-2.14`) rooted with
unfr0zen, without integrations, with the cloud blocked:

**System.** Linux 2.6.37 (rebuilt by unfr0zen), glibc 2.19, busybox 1.19.3,
58 MB of RAM. `nesto-probe` passes every check: Nesto's static musl programs
run, with NEON, the clocks, `/dev/urandom` and the protocol's cryptography
all as on any other machine. `SO_REUSEPORT` doesn't exist, as expected;
`SO_REUSEADDR` is enough.

**Screen.** `/dev/fb0` (`omapfb`): 320×320 visible, 320×640 virtual (two
pages, the second one showing), 32 bits per pixel, XRGB8888, 1280 bytes per
line. The backlight is `/sys/class/backlight/3-0036`, from 0 to 120.

**Input.**

| Device | Name | What it is |
|---|---|---|
| `/dev/input/event0` | `pwm-beeper` | The beeper: sound events, so Nesto could click |
| `/dev/input/event1` | `avago-adbs-a330` | The ring: an optical sensor, relative `REL_X` |
| `/dev/input/event2` | `twl4030_pwrbutton` | The press: `KEY_POWER` (116) |

The backplate is `/dev/ttyO2`.

**Who holds them.** The stock program `nlclient` holds the screen, the ring,
the press and the backplate, and uses about 20 MB of memory: roughly the
budget Nesto gets in its place. It's started and stopped by
`/etc/init.d/nestlabs` on monit's behalf.

**What watches what.**

- **monit** (5 s cycles) restarts `nlclient`, the Wi-Fi services and the
  Nest daemons. For the whole system it reboots the thermostat when memory in
  use stays over 41 MB for 25 s, or the 15-minute load average goes over 4.
- **watchdogd** alone holds the hardware watchdog (`/dev/watchdog`, 60 s;
  the kernel won't let it stop once started). It doesn't depend on
  `nlclient`.
- **nlheartbeatd** expects a heartbeat over D-Bus from `nlclient` and from
  `connman` every 5 s, starting 10 s after they start. After 8 missed ones it
  runs `/etc/init.d/nestlabs kill` (or `connman kill`). With `nlclient`
  stopped on purpose, nothing follows: see the trial below.
- **nlsleepd** turns on the processor's idle power saving; nothing suggests
  it suspends the system.

**Network.** `connmand` and `wpa_supplicant` keep Wi-Fi up, under monit,
independently of `nlclient`. connman also answers DNS on `127.0.0.1:53`. UDP
port 5353 is free (the stock mDNS daemon isn't running), and so is 6170.

**Storage.** Every RAM file system, and `/media/data`, `/media/log`,
`/media/system-config` and `/media/user-config`, are mounted `noexec`.
Programs can run only from `/` (18 MB free) and `/media/scratch`, and every
time `nlclient` starts, its init script empties `/media/scratch` of
everything not named with a leading dot. So Nesto is installed on `/`.

## Taking the screen over: the trial

Before Nesto replaces `nlclient`, a supervised trial stops `nlclient` for a
few minutes, watches what the rest of the system does, and starts it again:

```bash
scripts/dev.sh device-screen <nest-ip> trial      # 5 minutes; or: trial 600
```

It saves what it saw to `device-reports/screen-trial-*.txt`. While
`nlclient` is stopped, the screen shows what `nlclient` drew as it stopped
(white, with "the thermostat is restarting" in the thermostat's language),
the ring and press do nothing, and the thermostat doesn't control the
heating.

Before stopping anything, the trial arms a timer on the thermostat that
starts `nlclient` again two minutes after the trial should have ended, so
the stock software comes back even if SSH drops. A reboot brings it back
too: nothing is saved. The same script can be driven by hand:

```bash
scripts/dev.sh device-screen <nest-ip> status       # what runs, who holds the screen
scripts/dev.sh device-screen <nest-ip> take 600      # stop nlclient for at most 600 s
scripts/dev.sh device-screen <nest-ip> watch 120     # report every 15 s
scripts/dev.sh device-screen <nest-ip> give          # start it again now
```

### What the trial showed

Five minutes with `nlclient` stopped, on the thermostat above:

- **No reboot, nothing escalated.** The uptime ran on throughout. monit kept
  `nlclient` unwatched, `nlheartbeatd` kept running and logged nothing, and
  `watchdogd` fed the hardware watchdog every 30 s as before.
- **Wi-Fi and SSH stayed up** the whole time.
- **The screen** stayed white with `nlclient`'s "restarting" message: it's
  left in the framebuffer, and Nesto draws over it.
- **Giving back works.** `monit monitor` and `monit start` brought
  `nlclient` back within two seconds, and the thermostat's own screen with it.
- **To keep an eye on:** `nlshutdownd`'s shutdown manager logs the battery
  every 15 s (3.86 to 3.90 V, steady over the five minutes). With `nlclient`
  stopped, nothing talks to the backplate, which is what charges the battery
  from the base. Before Nesto runs for hours, check the battery holds up, or
  have Nesto talk to the backplate. (A long run later showed both matter:
  see below.)

So Nesto can take the screen over the same way: monit stops watching
`nlclient`, `nlclient` stops, and Nesto starts; giving back is the reverse.

## Running Nesto on the thermostat

The thermostat's hardware layer (`src/hal/nest/`) draws on `/dev/fb0`, reads
the ring and the press from the input devices named above (it finds them by
name, and keeps their events to itself), and sets the backlight. To try it:

```bash
scripts/dev.sh device-run <nest-ip>                # 10 minutes; or: device-run <nest-ip> 1500
```

It builds Nesto for the thermostat, copies it to `/nesto/nesto` (`/` is
where programs may run from and stay), takes the screen as the trial does,
and runs Nesto with its log in your terminal, saved to
`device-reports/run-*.txt`. Every 30 s it reports the memory in use, which
monit holds under 41 MB, Nesto's own share, and the battery (see
[Long runs](#long-runs-and-the-battery)). Ctrl-C, the time running
out, or SSH dropping stops Nesto and gives the screen back; the give-back
timer, a minute longer, is there in case even that fails.

Nesto keeps its pairing in `/media/data/nesto`, which survives reboots. While
it runs, pair it from another terminal, typing the code the thermostat shows,
and serve it the demo room:

```bash
scripts/dev.sh client --host <nest-ip> pair
scripts/dev.sh client --host <nest-ip> demo
```

Options after `--` go to Nesto, for instance
`scripts/dev.sh device-run <nest-ip> -- --ring-counts 100`; options before it
go to `ssh`.

### The ring

The ring has no clicks or marks: its optical sensor reports movement in
counts, much finer than anything on the screen. Nesto adds them up into
ticks (`src/core/RingTicks.hpp`), and the screens use the ticks two ways
(`src/room/Controller.hpp`):

- **Smooth values** change with every tick: brightness, a cover's position
  and tilt by 1%, colour temperature by 20 K, hue by 2 degrees.
- **Everything else** moves a place per click of 5 ticks: the home screen,
  Settings, lists of choices, switching, and climate targets (a half degree
  a click, so a thermostat doesn't race away).

In the simulator a wheel notch, an arrow key or a script's `r+1` is one
click. To see what the thermostat's sensor reports:

```bash
scripts/dev.sh device-run <nest-ip> input-test     # 2 minutes; Ctrl-C ends it earlier
```

The screen stays as `nlclient` left it while every event is printed. On the
thermostat above, a turn clockwise (roughly one: with no mark it's hard to
tell) added up to about 7300 counts, in events of 1 to 25 counts, all
negative. So by default Nesto takes negative counts as clockwise, and makes a
tick of 49 counts: about 150 ticks a turn. That's 30 items a turn, and
brightness from 0 to 100% in two thirds of one. `--ring-counts` makes ticks
finer (fewer counts) or coarser, and `--ring-reverse` turns the direction
round.

The press is `KEY_POWER`, down and up, as expected. With `nlclient` stopped,
the memory in use dropped from 24 MB to 15 MB, which leaves Nesto about
26 MB under monit's limit.

### What a run showed

Nesto ran on the thermostat above in `nlclient`'s place:

- **It paired** with the stand-in client over Wi-Fi, synchronised the demo
  room, and the ring, press and long press drove it as in the simulator.
- **Memory** in use stayed at 15 to 16 MB for the whole system, Nesto
  included: far under monit's 41 MB.
- **The screen.** `nlclient` doesn't always leave the same page showing (the
  first check found the second, this run the first); Nesto draws into
  whichever shows. It also left the backlight at 0, which Nesto turns up.
- **Ctrl-C** stopped Nesto at once, and `nlclient` was back two seconds
  later.
- **The ring's feel:** one step of 150 counts for everything (49 a turn)
  felt right overall; a little fine for moving between items, a little coarse
  for values such as brightness. Hence the ticks and clicks above.

### Long runs and the battery

The thermostat runs on its battery, which the base charges through the
backplate, and the backplate is `nlclient`'s: with `nlclient` stopped,
nothing may be charging it. There's no `/sys/class/power_supply` to ask, but
`nlshutdownd`, which keeps running, logs the voltage every 15 s
("shutdown-manager: batteryLevel @ 3.891" in `/var/log/messages`). So every
run reports it, and gives the screen back as soon as it's under 3.70 V,
letting `nlclient` charge again; a run doesn't start under it. The 3.70 V is
a cautious guess, not a measured limit, and `nlshutdownd`'s own shutdown is
still there below it.

```bash
scripts/dev.sh device-screen <nest-ip> battery     # the latest voltage
```

A run shouldn't hang on an SSH session, so it can go on by itself:

```bash
scripts/dev.sh device-run <nest-ip> detach         # 25 minutes, the longest there is
scripts/dev.sh device-log <nest-ip> follow         # watch it (Ctrl-C stops watching only)
scripts/dev.sh device-log <nest-ip>                # copy it to device-reports/
scripts/dev.sh device-screen <nest-ip> give        # end it early
```

A detached run writes to `/media/data/nesto/run.log`, on the flash, so the
log survives even a reboot (the uptime starting again shows one); the run
before it is kept as `run.log.1`. It ends, like any run, with what it adds up
to: the most memory in use, and the battery at the start, at its lowest and
at the end.

For the answer to mean something, the thermostat should be on its base, as
it would hang; on USB it may charge whatever the backplate does. Nesto keeps
the screen lit throughout, as it doesn't dim yet, so this is the heaviest
drain to expect.

### What a long run showed: the backplate resets the thermostat

A detached run meant for 4 hours, on the base, ended after 30 minutes: the
thermostat rebooted. The run's log on the flash stops mid-run, and the
uptime afterwards puts the boot 30 minutes and 8 seconds after `nlclient`
stopped. Its event log (`/media/log/nlevent*`) records the backplate
(`Backplate-3.2`) with reset reason 2 as `nlclient` stopped, and reason 9
when it started again after the boot. Nothing in monit's or `nlheartbeatd`'s
configuration acts after 30 minutes, and memory was at 16 MB throughout. So,
most likely, **the backplate's own controller resets the thermostat when
nothing has talked to it for about 30 minutes**. What the reason codes mean
isn't known, and the kernel's lines from before the reset were already
pruned. Note when reading the logs: the system log is in UTC, the run's in
local time.

Over those 30 minutes the battery fell from 3.906 V to 3.816 V, about
0.18 V an hour, while `nlclient`, once back, had it at 3.95 V within the
hour. The backplate reports it to `nlclient` too (`vb 3.952V`, and an input
of 0 V). So charging also seems to need `nlclient`'s conversation with the
backplate.

Until Nesto talks to the backplate itself, `nlclient` is never away for more
than 27 minutes: `nest-screen.sh` refuses longer (`NESTO_TAKE_MAX`, 1620 s),
which caps a run at 26 minutes and a trial at 25. Running Nesto for good
needs the backplate's protocol on `/dev/ttyO2`: at least keeping it from
resetting the thermostat, and charging the battery.
