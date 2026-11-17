# Installing Nesto

Nesto turns a Nest Learning Thermostat (2nd generation) into a Home Assistant
room panel. This guide installs a release on one. It takes about half an
hour, most of it rooting.

> **Use at your own risk.** Nesto is a personal project, provided as is,
> without warranty of any kind (see the
> [licence](https://github.com/jorgediez/nesto-firmware/blob/main/LICENSE)).
> Rooting the thermostat and installing Nesto can leave it unusable. The
> authors aren't responsible for any damage, data loss or other harm, to the
> thermostat, your heating or anything else. Only use it on a thermostat you
> no longer need, and back it up first (step 2).

Nesto replaces the thermostat's own software: once it runs, the thermostat
no longer controls any heating. It has been tested on one thermostat on a USB
stand; running it on a wall's heating wires is untested. Nesto is not
affiliated with Google or Nest.

## What you need

- The thermostat, on its base or on a USB stand.
- A USB **data** cable (micro-USB) for rooting; charge-only cables don't work.
- A computer with `ssh` and `tar`: Linux, macOS, or Windows with WSL or
  [Git Bash](https://git-scm.com/downloads).
- [unfr0zen](https://github.com/unfr0zen/unfr0zen/releases), which roots the
  thermostat. On Windows it also needs Zadig, once.
- Home Assistant with the [ha-nesto](https://github.com/jorgediez/ha-nesto)
  integration.

## 1. Join Wi-Fi

On the thermostat: Settings → Network, and pick your network. Note its IP
address (Settings → Technical info, or your router's list of devices); a
fixed address in your router helps later. Once Nesto runs, its own
Settings → Wi-Fi changes networks.

## 2. Back up and root it

With unfr0zen, first back up the whole flash and keep the backup somewhere
safe: it's the way back if anything goes wrong. Then root the thermostat with
**the SSH server** and **the USB shell** on, **no integration** (no MQTT,
Matter, Zigbee or Doom), and a **strong root password**. Adding your SSH
public key saves typing the password. Blocking the cloud is fine.

The steps, with the command-line equivalents and the Windows driver, are in
[DEVICE.md](https://github.com/jorgediez/nesto-firmware/blob/main/docs/DEVICE.md#2-on-windows-only-install-the-usb-driver-once),
steps 2 to 4. Put the display back on its base afterwards: it boots as
before, now with SSH.

## 3. Download a release

From the [releases](https://github.com/jorgediez/nesto-firmware/releases),
download `nesto-VERSION-nest-gen2.tar.gz`, and unpack it:

```bash
tar xzf nesto-VERSION-nest-gen2.tar.gz
cd nesto-VERSION-nest-gen2
```

The release page gives the archive's SHA-256; `sha256sum` (or
`shasum -a 256` on a Mac) shows the one you downloaded. The installer checks
each file inside against `SHA256SUMS` by itself.

## 4. Install it

```bash
sh install.sh <nest-ip>
```

The first time, SSH asks you to accept the thermostat's host key, then for
root's password unless you added a key. Further arguments go to `ssh`, for
example `-i ~/.ssh/nest`.

The installer copies Nesto to `/nesto` and adds one line to `/etc/inittab`,
keeping a copy of it, so Nesto starts at every boot. Nothing else of the
stock software changes. Within a minute the stock screen gives way to Nesto,
waiting for Home Assistant.

## 5. Pair it with Home Assistant

Home Assistant finds the panel by itself (Settings → Devices & services →
Discovered), or add **Nesto Panel** with the thermostat's address. The panel
shows a six-digit code; type it in Home Assistant.

Then put the panel in an area, on its device page: it shows that room's
lights, climate, covers, switches and scenes. The integration's **Configure**
options choose and order what it shows. The
[ha-nesto README](https://github.com/jorgediez/ha-nesto#readme) has the rest.

## Updating

Download and unpack the new release, then:

```bash
sh install.sh <nest-ip> update
```

Nesto's pairing and settings stay. The installer says which version it
replaced.

## Getting the stock software back

- **Until the next boot, without a computer:** hold the display while "Hold
  to start Nest" shows, in the first seconds after Nesto starts.
- **From now on:** `sh install.sh <nest-ip> stock`; `sh install.sh <nest-ip>
  nesto` brings Nesto back.
- **By itself:** if Nesto exits three times within ten minutes, the stock
  software runs until the next boot.
- **For good:** `sh install.sh <nest-ip> uninstall` takes the line out of
  `/etc/inittab` and removes `/nesto`. Nesto's settings and log stay in
  `/media/data/nesto`. Rooting stays; the backup from step 2 undoes it.

## When something's wrong

- `sh install.sh <nest-ip> status`: the mode, what runs, Nesto's exits this
  boot, and the end of its log (`/media/data/nesto/nesto.log`).
- `sh install.sh <nest-ip> check`: a read-only report on the thermostat,
  saved in the current folder. It leaves out Wi-Fi passwords and keys, but
  has MAC addresses and the Wi-Fi network's name: read it before sharing it.
- [Open an issue](https://github.com/jorgediez/nesto-firmware/issues/new/choose)
  with what Settings → Technical info shows, and the log or the report.
