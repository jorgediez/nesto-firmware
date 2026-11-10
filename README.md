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
> from the layout Home Assistant sends, on plain screens for now, with a
> Settings menu to pair again or forget Home Assistant. The finished look and the
> thermostat itself come next. The wire protocol
> between the panel and Home Assistant is in [PROTOCOL.md](PROTOCOL.md).

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
mouse button, Space or Enter press the display. Escape quits.

The simulator listens for Home Assistant on port 6170 and announces itself over
mDNS as a `_nesto._tcp` service, which is how Home Assistant discovers it
(`--no-mdns` turns that off). In Docker, the announcements stay inside Docker's
network, so from outside the panel is reached by address. Until the
integration exists, `tools/nesto_client.py` stands in for it. With the
simulator running:

```bash
scripts/dev.sh client pair      # type the code the simulator shows
scripts/dev.sh client demo      # serve a made-up living room to the simulator
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

The steps are `r+N`/`r-N` (turn the ring), `p` (press), `h` (long press) and
`wN` (wait N ms). See `src/core/InputScript.hpp`.

### Layout

| Path | What's there |
|---|---|
| `src/core/` | Logic with no hardware or LVGL dependency, covered by unit tests |
| `src/protocol/` | The link to Home Assistant: framing, messages, pairing and encryption ([PROTOCOL.md](PROTOCOL.md)) |
| `src/room/` | The room: Home Assistant's layout and states, and what the ring and press do with them |
| `src/hal/` | The hardware interface, with the simulator (`sim/`) and the thermostat (`nest/`) |
| `src/app/` | The main loop and the screens, built with [LVGL](https://lvgl.io) |
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

The thermostat has an ARM Cortex-A8 and a C library from 2011, so its program is
cross-compiled with a [Bootlin](https://toolchains.bootlin.com) toolchain (GCC
and musl) and linked fully statically: it needs nothing from the device. The
toolchain comes with the dev image; `cmake/toolchain-nest.cmake` sets it up.

```bash
scripts/dev.sh check-arm   # static, 32-bit ARM, no instructions the Cortex-A8 lacks
scripts/dev.sh dist        # the stripped program, in dist/nesto
```

The simulator and the tests are also built for ARM and run under QEMU emulating
a Cortex-A8, so the code runs on the thermostat's instruction set before it gets
there. `scripts/dev.sh shot-arm` takes screenshots with that build; CI checks
they match the native ones pixel for pixel.

The statically linked program includes musl (MIT) and GCC's runtime libraries
(GPL with the GCC Runtime Library Exception), which allow this. The other
libraries built into it are LVGL (MIT), Monocypher (BSD-2-Clause or CC0),
nlohmann/json (MIT) and mdns (public domain).

## Not affiliated

Nesto is an independent project. It is not affiliated with, endorsed by or
supported by Google or Nest Labs. "Nest" is a trademark of Google LLC, used here
only to say which hardware this software runs on.

Installing Nesto means rooting the thermostat, which can leave it unusable.
Only use it on a device you no longer need as a thermostat.

## License

[MIT](LICENSE).
