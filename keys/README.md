# Release keys

`ota-release.pub` is the public half of the Ed25519 key that signs Nesto's
releases. Every build for the thermostat trusts it, and takes a firmware
update from Home Assistant only when it signed the program (PROTOCOL.md
section 19). The private half is never in the repository: the maintainer
keeps it, and CI reads it from the `NESTO_OTA_KEY` secret to sign a release.

Making the pair, once:

```bash
scripts/dev.sh ota-keygen
```

It writes `ota-release.pub` and shows the private key, once. Keep it in a
password manager, with a backup, and store it as the `NESTO_OTA_KEY` secret
of the repository's `release` environment. Losing it means panels can't take
updates from Home Assistant until the next release is installed over SSH;
leaking it means someone could sign programs panels would install, so a new
key, and a release installed over SSH, would be needed.

Local builds also trust a dev key, made in the build volume
(`/build/ota-dev.key`), so `scripts/dev.sh sign` can sign them for an update
by hand. CI builds never trust one, and the simulator also trusts
`tests/ota/test-key.pub`, for the tests.
