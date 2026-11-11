# The backplate

The backplate is the thermostat's base on the wall. It has its own
microcontroller (an STM32), and the thermostat reaches it over a serial port,
`/dev/ttyO2`. It charges the battery, drives the heating (or, in the UK and
EU, the Heatlink), and reads the temperature, humidity, proximity and
ambient light sensors.

It also watches the thermostat: when nothing has talked to it for about 30
minutes, it resets the thermostat ([DEVICE.md](DEVICE.md#what-a-long-run-showed-the-backplate-resets-the-thermostat)).
So once the stock software (`nlclient`) is stopped, Nesto has to keep a
session with it going. `src/core/Backplate.hpp` does that, and
`src/hal/nest/NestHal.cpp` connects it to the serial port.

## Who worked it out

Nesto's implementation is its own. It's written from what these projects
published about the protocol; none of their code is in Nesto:

- **GTVHacker's Nest Hacking wiki** described the backplate's start-up
  ([archived](https://web.archive.org/web/20141023183423/http://wiki.gtvhacker.com/index.php/Nest_Hacking#Backplate_initialisation)),
  and its successor, [Exploitee.rs's Nest Hacking wiki](https://wiki.exploitee.rs/index.php/Nest_Hacking),
  keeps that work.
- **cuckoo-nest**, an open firmware for the same thermostat, documented the
  protocol in its [wiki](https://github.com/cuckoo-nest/wiki/tree/main/backplate),
  from z3ugma's work and jfarre20's `ttyproxy`, and found that keeping a
  session with the backplate stops the 30-minute reset
  ([cuckoo_nest#29](https://github.com/cuckoo-nest/cuckoo_nest/issues/29)).
- **[nest-gallery](https://github.com/jasongraphix/nest-gallery)** takes
  another way: it pauses `nlclient` with `SIGSTOP` instead of stopping it,
  and lets it run again whenever its display sleeps, so `nlclient` charges
  the battery and keeps the backplate happy meanwhile.

## Frames

Everything is little-endian. A command to the backplate:

| Bytes | What |
|---|---|
| 3 | `d5 aa 96` |
| 2 | type |
| 2 | payload length, at most 1024 |
| n | payload |
| 2 | CRC |

The backplate's frames are the same after a four-byte preamble,
`d5 d5 aa 96`. The CRC is CRC-16/XMODEM (polynomial 0x1021, starting at 0,
not reflected) over the type, the length and the payload. For example, the
reset command is `d5 aa 96 ff 00 00 00 a3 4b`.

The serial port runs at 115200 baud, 8N1, raw. It must not turn carriage
returns into newlines, as some replies hold a `0x0d`.

## The session

1. A break on the line, a quarter of a second, then **reset** (`0x00ff`).
2. The backplate sends a burst: its firmware version as text (`0x0001`),
   which terminals have wires (`0x0004`), more text, and the text `BRK` to
   end it.
3. The wire presence is acknowledged with the same payload (`0x008f`).
4. Its versions and model are asked one at a time: `0x0098` (reply
   `0x0018`), `0x0099` (`0x0019`), `0x009d` (`0x001d`, for instance
   `Backplate-3.2`).
5. From then on: a keep-alive (`0x0083`) every 15 s, and a request for its
   buffered readings (`0x00a2`) every 30 s. The readings end with `0x002f`,
   which is acknowledged (`0x00a3`).

If the burst doesn't end within 5 s, a question goes unanswered for 1 s, or
the backplate says nothing for 2 minutes, the session starts again after
10 s.

cuckoo-nest's example also sets **power steal mode** (`0x00c0`, payload
`00 00 00 00`) after the questions; Nesto does so only with `--power-steal`,
until it's clear whether charging needs it.

## Readings

| Type | Payload |
|---|---|
| `0x0002` | temperature (signed, hundredths of a degree C), humidity (tenths of a percent) |
| `0x000b` | state: at byte 8 the input voltage (hundredths of a volt), at 10 the operating voltage and at 12 the battery's (thousandths) |
| `0x0007`, `0x000a` | proximity and ambient light, about every second once running |

## Trying it

With the stock software stopped, as for any run:

```bash
scripts/dev.sh device-run <nest-ip> backplate-test   # 3 minutes; every frame, decoded
```

Nesto itself talks to the backplate in every run (`--no-backplate` turns
that off), logging the battery, input voltage, temperature and humidity
every minute. To see a run outlast the backplate's 30 minutes, the cap on
how long the stock software may be away has to be lifted on purpose:

```bash
NESTO_TAKE_MAX=3600 scripts/dev.sh device-run <nest-ip> detach 2700
```
