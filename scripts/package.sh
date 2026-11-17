#!/usr/bin/env bash
# Packs a release from the thermostat's programs in dist/ (scripts/dev.sh
# dist): dist/nesto-VERSION-nest-gen2.tar.gz, and its checksum next to it.
# VERSION is the project's, in CMakeLists.txt. CI runs it on every build, and
# a release attaches what a tag's build packed.
#
# The archive is the same for the same files: names sorted, owner root, every
# date the commit's (or SOURCE_DATE_EPOCH's).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

version="$(sed -n 's/^project(nesto VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)"
if [ -z "$version" ]; then
  echo "no version in CMakeLists.txt's project()" >&2
  exit 1
fi
for program in nesto nesto-probe; do
  if [ ! -f "dist/$program" ]; then
    echo "dist/$program is missing: run scripts/dev.sh dist first" >&2
    exit 1
  fi
done

name="nesto-$version-nest-gen2"
stage="dist/$name"
rm -rf "$stage" "dist/$name.tar.gz" "dist/$name.tar.gz.sha256"
mkdir -p "$stage"
cp dist/nesto dist/nesto-probe \
  scripts/install.sh scripts/nesto-boot.sh scripts/nest-screen.sh scripts/device-check.sh \
  docs/INSTALL.md LICENSE THIRD_PARTY.md "$stage/"
chmod 755 "$stage"/nesto "$stage"/nesto-probe "$stage"/*.sh
echo "$version" > "$stage/VERSION"
(cd "$stage" && sha256sum -- * > ../SHA256SUMS && mv ../SHA256SUMS .)

epoch="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}"
tar -C dist --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
  -cf - "$name" | gzip -n -9 > "dist/$name.tar.gz"
(cd dist && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
rm -rf "$stage"
echo "dist/$name.tar.gz: $(( $(stat -c %s "dist/$name.tar.gz") / 1024 )) KiB, Nesto $version"
