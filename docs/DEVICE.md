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

## What happens next

The report decides how Nesto takes over the screen: probably by asking monit
to stop watching `nlclient`, as unfr0zen's own Doom mode does, and what must
keep running for Wi-Fi and the watchdog. Those findings go into this document
and into the thermostat's hardware layer (`src/hal/nest/`).
