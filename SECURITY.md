# Security policy

## Reporting a vulnerability

Please report it privately, through
[Report a vulnerability](https://github.com/jorgediez/nesto-firmware/security/advisories/new)
on this repository's Security tab. Don't open a public issue or post it in the
community thread. Only the maintainer sees the report, and no email address is
needed.

Describe what you found, how to reproduce it, and which versions of Nesto,
and of the ha-nesto integration if it's involved, you saw it on.

## What counts

- A way to pair with a panel without the code it shows on screen, to take
  over its session, or to read or change its traffic by watching the network.
- A way to crash the panel, or make it run code, from what reaches it over
  the network: protocol frames, before or after authentication, and the mDNS
  responder.
- The Wi-Fi password, or the Thread network key the thermostat keeps, showing
  up where it shouldn't: Nesto's log, Technical info, the protocol, or the
  device reports.
- The Wi-Fi provisioning file Nesto writes being readable by anyone but root.

Not a vulnerability in Nesto:

- Anything that needs root or physical access to the thermostat. Rooting it
  gives SSH and a USB shell with the root password chosen then; choose a
  strong one, and keep SSH to a trusted network.
- The stock Nest software, or Google's services.
- What Home Assistant chooses to put on the panel: that's the integration's
  side, with [its own policy](https://github.com/jorgediez/ha-nesto/security/policy).

## Supported versions

Only the latest release gets fixes.

## What to expect

This is a volunteer project. Reports are read and fixed on a best-effort basis,
and you'll hear back in the report itself, where any fix and its release are
coordinated before details are made public.
