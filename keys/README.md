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

## How a release is signed

A version tag's CI run drafts the release (`.github/workflows/ci.yml`). Its
release job, in the `release` environment, takes the program out of the
package the build job made and tested, and `scripts/release-sign.sh` signs it
with `NESTO_OTA_KEY`. The release gets two more files:

- `nesto-VERSION-nest-gen2.bin`: the package's `nesto`, byte for byte;
- `nesto-VERSION-nest-gen2.bin.sig`: its signature, in base64.

The script checks the signature against `ota-release.pub` before the release
gets it, so a wrong secret stops the release instead of shipping a signature
no panel takes. The `release` environment should let only `v*` tags deploy,
so no branch's workflow can read the key. Every build also runs the script
with the test key (`scripts/dev.sh release-sign`), to keep the signing
working between releases.

To check a release by hand (Python with `cryptography`):

```bash
python3 tools/ota_sign.py verify nesto-VERSION-nest-gen2.bin VERSION \
  nesto-VERSION-nest-gen2.bin.sig keys/ota-release.pub
```

## Other keys

Local builds also trust a dev key, made in the build volume
(`/build/ota-dev.key`), so `scripts/dev.sh sign` can sign them for an update
by hand. CI builds never trust one, and the simulator also trusts
`tests/ota/test-key.pub`, for the tests.
