# Nesto Panel firmware

Turns a retired Nest Learning Thermostat (2nd generation) into a Home Assistant
room panel. Turn the ring and press to control the lights, climate, covers and
scenes of a room. The panel can also show notifications, and a clock or photo
carousel while idle.

Nesto ("nest" in Esperanto) is the software that runs on the thermostat. It
works with the [ha-nesto](https://github.com/jorgediez/ha-nesto) Home Assistant
integration, which discovers the panel, pairs with it and decides what it shows.

> **Status: early development.** The desktop simulator runs a demo screen; the
> thermostat itself and the link to Home Assistant come next. The wire protocol
> between the panel and Home Assistant is in [PROTOCOL.md](PROTOCOL.md).

## Development

Everything builds inside a Docker container, so the only requirement is Docker.
On Windows, run the commands from a WSL terminal (Ubuntu): WSLg shows the
simulator's window on the Windows desktop.

```bash
scripts/dev.sh image   # once, and after changing docker/Dockerfile
scripts/dev.sh build   # simulator (with sanitizers), Nest target and tests
scripts/dev.sh test    # unit tests
scripts/dev.sh sim     # open the simulator
```

In the simulator, the mouse wheel or arrow keys turn the ring, and the left
mouse button, Space or Enter press the display. Escape quits.

Screens can also be captured without a window, which is what CI does:

```bash
scripts/dev.sh shot screenshots/home.png "r+2 p w100"
```

The steps are `r+N`/`r-N` (turn the ring), `p` (press), `h` (long press) and
`wN` (wait N ms). See `src/core/InputScript.hpp`.

### Layout

| Path | What's there |
|---|---|
| `src/core/` | Logic with no hardware or LVGL dependency, covered by unit tests |
| `src/hal/` | The hardware interface, with the simulator (`sim/`) and the thermostat (`nest/`) |
| `src/app/` | The main loop and the screens, built with [LVGL](https://lvgl.io) |
| `tests/` | Unit tests ([doctest](https://github.com/doctest/doctest)) |

The `nest` build is compiled for x86 for now, to keep it building; compiling it
for the thermostat's ARM processor comes next.

## Not affiliated

Nesto is an independent project. It is not affiliated with, endorsed by or
supported by Google or Nest Labs. "Nest" is a trademark of Google LLC, used here
only to say which hardware this software runs on.

Installing Nesto means rooting the thermostat, which can leave it unusable.
Only use it on a device you no longer need as a thermostat.

## License

[MIT](LICENSE).
