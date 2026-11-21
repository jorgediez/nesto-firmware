#!/usr/bin/env bash
# Signs a release's program for firmware updates from Home Assistant
# (PROTOCOL.md section 19): from DIR/nesto-VERSION-MODEL.tar.gz, the package
# scripts/package.sh made, it writes next to it
#
#   nesto-VERSION-MODEL.bin       the package's nesto, the same bytes
#   nesto-VERSION-MODEL.bin.sig   its signature, base64
#
# and checks the signature against PUBLIC_KEY, so a release never ships one
# no panel would take. The private key comes from PRIVATE_KEY_FILE, or from
# the NESTO_OTA_KEY environment variable (the release job's secret).
#
#   scripts/release-sign.sh TARBALL PUBLIC_KEY [PRIVATE_KEY_FILE]
#
# CI runs it on every build with tests/ota/test-key, and on a version tag with
# the release key, against keys/ota-release.pub. It needs python3 with the
# cryptography package (tools/ota_sign.py).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  sed -n '13p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
tarball="$1"
public="$2"
key=()
[ $# -eq 3 ] && key=(--key "$3")

name="$(basename "$tarball" .tar.gz)"
if [[ ! "$name" =~ ^nesto-([0-9]+\.[0-9]+\.[0-9]+)-([a-z0-9-]+)$ ]]; then
  echo "$tarball isn't named nesto-VERSION-MODEL.tar.gz" >&2
  exit 1
fi
version="${BASH_REMATCH[1]}"
model="${BASH_REMATCH[2]}"
dir="$(dirname "$tarball")"
bin="$dir/$name.bin"

tar -xzOf "$tarball" "$name/nesto" > "$bin"
"$PYTHON" "$ROOT/tools/ota_sign.py" sign "$bin" "$version" --model "$model" ${key[@]+"${key[@]}"} --out "$bin.sig"
if ! "$PYTHON" "$ROOT/tools/ota_sign.py" verify "$bin" "$version" "$bin.sig" "$public" --model "$model" > /dev/null 2>&1; then
  echo "the signature doesn't match $public: is the private key the right one?" >&2
  rm -f "$bin" "$bin.sig"
  exit 1
fi
echo "$bin: $(($(stat -c %s "$bin") / 1024)) KiB, signed for Nesto $version on $model, checked against $public"
