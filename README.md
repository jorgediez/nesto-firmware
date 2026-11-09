# Nesto Panel firmware

Turns a retired Nest Learning Thermostat (2nd generation) into a Home Assistant
room panel. Turn the ring and press to control the lights, climate, covers and
scenes of a room. The panel can also show notifications, and a clock or photo
carousel while idle.

Nesto ("nest" in Esperanto) is the software that runs on the thermostat. It
works with the [ha-nesto](https://github.com/jorgediez/ha-nesto) Home Assistant
integration, which discovers the panel, pairs with it and decides what it shows.

> **Status: design phase.** Nothing here runs yet. The wire protocol between the
> panel and Home Assistant is drafted in [PROTOCOL.md](PROTOCOL.md).

## Not affiliated

Nesto is an independent project. It is not affiliated with, endorsed by or
supported by Google or Nest Labs. "Nest" is a trademark of Google LLC, used here
only to say which hardware this software runs on.

Installing Nesto means rooting the thermostat, which can leave it unusable.
Only use it on a device you no longer need as a thermostat.

## License

[MIT](LICENSE).
