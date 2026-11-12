# Nesto Panel firmware

Turns a retired Nest Learning Thermostat (2nd generation) into a Home Assistant
room panel. Turn the ring and press to control the lights, climate, covers and
scenes of a room. The panel can also show notifications, and a clock or photo
carousel while idle.

Nesto ("nest" in Esperanto) is the software that runs on the thermostat. It
works with the [ha-nesto](https://github.com/jorgediez/ha-nesto) Home Assistant
integration, which discovers the panel, pairs with it and decides what it shows.

> **Status: early development.** In the simulator, the panel pairs with Home
> Assistant and controls a room's lights, climate, covers, switches and scenes
> from the layout Home Assistant sends, with icons and colours for what each is
> doing, and a Settings menu to pair again or forget Home Assistant. On a real
> thermostat it runs for a supervised while, with its screen, ring and press,
> and gives the stock software its place back afterwards. It talks to the
> thermostat's base, which would otherwise reset it after 30 minutes
> ([docs/BACKPLATE.md](docs/BACKPLATE.md)), so runs last hours, with the
> display resting to keep the battery up. Installing it for good comes next.
> The wire protocol between the panel and Home Assistant is in
> [PROTOCOL.md](PROTOCOL.md).

## Development

Everything builds inside a Docker container, so the only requirement is Docker.
On Windows, run the commands from a WSL terminal (Ubuntu): WSLg shows the
simulator's window on the Windows desktop.

```bash
scripts/dev.sh image   # once, and after changing docker/Dockerfile
scripts/dev.sh build   # simulator, the thermostat's program, and the tests
scripts/dev.sh test    # unit tests, natively and on emulated ARM
scripts/dev.sh e2e     # the real panel against the Python client, natively and on emulated ARM
scripts/dev.sh sim     # open the simulator
```

In the simulator, the mouse wheel or arrow keys turn the ring, and the left
mouse button, Space or Enter press the display. M stands for someone coming
near, as the thermostat's proximity sensor would, and F for someone moving
in the room, as its motion sensor would. L sets the room's light in turn to
dark, lit and bright, as the thermostat's light sensor would; until it's
pressed there's no sensor. Escape quits.

`scripts/dev.sh sim --fake-base` gives the simulator a base, as the thermostat has: fixed
temperature, humidity and voltages, the light from L, and motion from M and F.
The panel then sends Home Assistant its `sensors` messages, as it does on the
thermostat ([PROTOCOL.md](PROTOCOL.md) section 17). It always sends each
press gesture as an `input` event, including the double press in standby or
off, which doesn't wake the display and is there for automations.
`client demo` prints both.

Home Assistant's notifications pop up on the panel, waking it as their
priority says, and wait in a list the home screen opens
([PROTOCOL.md](PROTOCOL.md) section 14). The ring chooses an action or
Dismiss, a press sends it, and a long press keeps the notification for later.
`client demo` sends one to try:

```bash
scripts/dev.sh client demo --notify "Someone is at the door" --title "Front door" \
  --icon door --priority high --action open=Open --action ignore=Ignore
```

With Home Assistant's `auto_brightness` (on unless it's turned off), the
light sensor scales the brightness: the configured one in a lit room, down to
a quarter of it in the dark (5% at least), the standby clock too. It dims over
about 10 s, so a hand or a shadow doesn't dim it, and comes back up within a
few seconds.

After 30 seconds without input (or what Home Assistant sets) the display
fades to standby, a clock at 20% (analog or digital, as Home Assistant
sets), once the panel knows the time; until then, dark. The ring, a press or
someone near wakes it, and the input that wakes it does nothing else
([PROTOCOL.md](PROTOCOL.md) section 13). It wakes to the top of the home
screen: the Notifications entry while there are any, else the first item. Home Assistant's `wake_on_motion`
setting turns waking on someone near off; `client demo --no-motion` tries it.

The simulator listens for Home Assistant on port 6170 and announces itself over
mDNS as a `_nesto._tcp` service, which is how Home Assistant discovers it
(`--no-mdns` turns that off). In Docker, the announcements stay inside Docker's
network, so from outside the panel is reached by address. Until the
integration exists, `tools/nesto_client.py` stands in for it. With the
simulator running:

```bash
scripts/dev.sh client pair      # type the code the simulator shows
scripts/dev.sh client demo      # serve a made-up living room to the simulator (--timeout N, --no-motion, --clock digital, --seconds, --hour12)
scripts/dev.sh client connect   # or: authenticate, synchronise, print what the panel sends
```

With `client demo` running, the simulator shows the room: turn the ring to
choose an item, press to open it, turn to change it, press to step through its
controls (brightness, colour, effect...), and hold to go back. On the home
screen, a press switches a switch or runs a scene, and holding switches a light.
The demo carries out each change as Home Assistant would and prints it.

The pairing is kept in the build volume, so it survives restarts. **Settings**,
the last entry on the home screen (or a press on the waiting screen), has:

- **Pair**: lets Home Assistant pair again for 5 minutes, for instance after
  `client.key` is lost or to move to another Home Assistant;
- **Forget**: after a confirming press, forgets Home Assistant and drops the
  connection (holding cancels);
- **About**: version, panel ID, address and whether it's paired.

Hold to go back from any of them.

Screens can also be captured without a window. CI does this against the demo
room, and keeps the screenshots:

```bash
scripts/dev.sh screens screenshots              # every screen, into screenshots/
scripts/dev.sh shot screenshots/x.png "r+2 p"   # one screen, without Home Assistant
```

The steps are `r+N`/`r-N` (turn the ring N clicks, as many wheel notches),
`p` (press), `h` (long press), `m` (someone near) and
`wN` (wait N ms). See `src/core/InputScript.hpp`.

### Layout

| Path | What's there |
|---|---|
| `src/core/` | Logic with no hardware or LVGL dependency, covered by unit tests, including the backplate's protocol ([docs/BACKPLATE.md](docs/BACKPLATE.md)) |
| `src/protocol/` | The link to Home Assistant: framing, messages, pairing and encryption ([PROTOCOL.md](PROTOCOL.md)) |
| `src/room/` | The room: Home Assistant's layout and states, and what the ring and press do with them |
| `src/hal/` | The hardware interface, with the simulator (`sim/`) and the thermostat (`nest/`) |
| `src/app/` | The main loop and the screens, built with [LVGL](https://lvgl.io) |
| `src/probe/` | `nesto-probe`, the read-only check of a real thermostat ([docs/DEVICE.md](docs/DEVICE.md)) |
| `tests/` | Unit tests ([doctest](https://github.com/doctest/doctest)) |
| `tests/e2e/` | End-to-end tests: the real panel program against the Python client over TCP, and against python-zeroconf for discovery |
| `tools/` | The protocol's Python reference implementation, a client that stands in for Home Assistant, and the test vector generator |

### Protocol test vectors

`tests/vectors/protocol-v1.json` holds pairing codes, keys, MACs and encrypted
frames computed by the Python reference in `tools/`. The panel's tests check
its own results against them, and the Home Assistant integration does the same,
so both sides agree byte for byte. After changing the protocol's cryptography,
regenerate the file and commit it; CI fails if it's stale.

```bash
scripts/dev.sh vectors
```

### The thermostat's program

The thermostat has an ARM Cortex-A8 and glibc 2.19, from 2014, so its program is
cross-compiled with a [Bootlin](https://toolchains.bootlin.com) toolchain (GCC
and musl) and linked fully statically: it needs nothing from the device. The
toolchain comes with the dev image; `cmake/toolchain-nest.cmake` sets it up.

```bash
scripts/dev.sh check-arm   # static, 32-bit ARM, no instructions the Cortex-A8 lacks
scripts/dev.sh dist        # the stripped programs, in dist/nesto and dist/nesto-probe
```

The simulator and the tests are also built for ARM and run under QEMU emulating
a Cortex-A8, so the code runs on the thermostat's instruction set before it gets
there. `scripts/dev.sh shot-arm` takes screenshots with that build; CI checks
they match the native ones pixel for pixel.

Everything built into the program, its version and licence, is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

### On a real thermostat

Before Nesto runs on a thermostat, a read-only check describes it: what runs
the stock software, the watchdog, Wi-Fi, the screen, the ring and the
backplate, and whether its 2.6.37 kernel runs our programs. Root it with
unfr0zen and run the check, then run Nesto in the stock software's place for
a while, as [docs/DEVICE.md](docs/DEVICE.md) explains:

```bash
scripts/dev.sh device-check <nest-ip>             # the report goes to device-reports/
scripts/dev.sh device-screen <nest-ip> trial      # stop the stock software for 5 minutes, watch, restart it
scripts/dev.sh device-run <nest-ip> input-test    # print what the ring and the press send
scripts/dev.sh device-run <nest-ip> backplate-test  # talk to the backplate, printing every frame
scripts/dev.sh device-run <nest-ip> light-test      # light sensor readings, backlight off and on
scripts/dev.sh device-run <nest-ip>               # run Nesto for 10 minutes, then restart the stock software
scripts/dev.sh client --host <nest-ip> pair       # meanwhile, pair with it (then: demo)
scripts/dev.sh device-run <nest-ip> detach        # run Nesto for an hour, apart from SSH, watching the battery
scripts/dev.sh device-log <nest-ip> [follow]      # that run's log, into device-reports/
scripts/dev.sh check-report                       # run the device scripts in busybox, as CI does
```

`nesto-probe` (`src/probe/`), the check's program, is built with the rest and
tested natively and under QEMU. What a real thermostat showed is in
[docs/DEVICE.md](docs/DEVICE.md).

### Fonts and icons

Text is Montserrat and icons are Material Design Icons, the set Home Assistant
uses, so an icon name from Home Assistant needs no translation. At build time
`tools/make_fonts.py` cuts both down to what the panel draws (Latin-1 and Latin
Extended-A for text, which cover Spanish and Esperanto, and about 30 icons) and
embeds them; LVGL draws them at runtime at any size. To add an icon, add it to
`ICONS` in that script and to PROTOCOL.md appendix C.

## Not affiliated

Nesto is an independent project. It is not affiliated with, endorsed by or
supported by Google or Nest Labs. "Nest" is a trademark of Google LLC, used here
only to say which hardware this software runs on.

Installing Nesto means rooting the thermostat, which can leave it unusable.
Only use it on a device you no longer need as a thermostat.

## License

[MIT](LICENSE).
