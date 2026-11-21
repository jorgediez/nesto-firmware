# The OTA test key

`test-key` is the private half of `test-key.pub`, an Ed25519 key pair made
with `tools/ota_sign.py keygen` for the tests only. It is public on purpose:
the simulator's builds trust `test-key.pub` (CMakeLists.txt), so the tests can
sign updates for it. The thermostat's builds never trust it; they trust
`keys/ota-release.pub`, whose private half isn't in the repository.
