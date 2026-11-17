# Contributing

Thanks for looking. Nesto is young, and reports from real thermostats are what
it needs most.

## Reporting a thermostat, or a problem

Bugs are best as an [issue](https://github.com/jorgediez/nesto-firmware/issues/new/choose),
where there is a form for bugs, one for telling us how Nesto does on your
thermostat, and one for ideas. (A community thread for questions will follow
the first release.)

What makes a report actionable:

- Nesto's version, and the display's and base's models: all on the
  thermostat, under *Settings → Technical info* (Nesto, Display and Base).
- How the thermostat is powered: on a USB stand, or on its wires, with or
  without a common (C) wire.
- The lines of Nesto's log around the moment it went wrong:
  `/media/data/nesto/nesto.log` on the thermostat, or
  `scripts/dev.sh device-status <nest-ip>`.
- For a thermostat that behaves unlike ours, a device report:
  `scripts/dev.sh device-check <nest-ip>`
  ([docs/DEVICE.md](docs/DEVICE.md)). It leaves out Wi-Fi passwords and keys,
  but read it before attaching it.

A problem with what Home Assistant puts on the panel, or with its entities,
belongs to the [ha-nesto](https://github.com/jorgediez/ha-nesto/issues)
integration instead.

A security problem should not go in an issue: see [SECURITY.md](SECURITY.md)
for how to report it privately.

## What belongs here

This repository is the software that runs on the thermostat, and the protocol
it speaks with Home Assistant.

Changes that fit:

- The panel's screens, controls, sounds and settings.
- The thermostat's hardware: display, ring and press, base, sensors, Wi-Fi,
  power.
- Installing, running and giving the thermostat back to the stock software.
- The protocol, [PROTOCOL.md](PROTOCOL.md).
- Documentation.

A change to the protocol starts here: PROTOCOL.md, the Python reference
(`tools/nesto_protocol.py`) and the test vectors (`tools/make_vectors.py`)
change together, and ha-nesto then copies the vectors.

**If a change is more than a small fix, open an issue first.** A short
description of what you want and how you'd do it takes minutes and can save
you days.

### Other projects' code

Cuckoo, nest-gallery and the exploitee notes taught us much of what we know
about this hardware ([docs/BACKPLATE.md](docs/BACKPLATE.md) credits them), but
they have no licence. Nesto reimplements what they document; it never copies
their code. Please do the same, and say where a fact came from.

## Working on the code

Everything builds inside a Docker container, so Docker is the only
requirement. On Windows, run the commands from WSL or Git Bash (with
`MSYS_NO_PATHCONV=1`). The README's [Development](README.md#development)
section has the full list; the ones a change needs:

```bash
scripts/dev.sh build                            # the simulator, the thermostat's program, the tests
scripts/dev.sh test                             # unit tests, natively and on emulated ARM
scripts/dev.sh e2e                              # the real panel against the Python client, both builds
scripts/dev.sh check-arm                        # the ARM programs can run on the thermostat
scripts/dev.sh screens /src/build/shots         # the screenshots CI keeps
scripts/dev.sh sim                              # the simulator, in a window
```

A pull request is expected to:

- Pass the unit and end-to-end tests on both builds, `check-arm` and ruff.
  CI runs these, takes the screenshots, and checks the ARM build draws the
  same screen as the native one.
- Come with tests. Most of the panel is plain C++ without hardware
  (`src/room`, `src/core`, `src/protocol`), tested with doctest; the
  hardware sits behind `src/hal`.
- Show a changed screen: add or update its entry in `tools/screens.py`.
- Say what it was tried on, if it touches the hardware: the thermostat, its
  base, and how it was powered.
- Update the docs it affects.
- Explain, in the commit message, why the change is the way it is.

Keep a pull request to one subject. Two unrelated improvements are two pull
requests, and they'll both move faster.

## Style

Follow the code already here: C++17, nothing blocking the main loop (slow
work, such as talking to connman, runs in another process and is polled),
comments that say why rather than what, and texts on screen kept short enough
for a round 320-pixel display. Python follows ruff.

## License

Contributions are under the [MIT License](LICENSE), like the rest of the
project.
