#!/usr/bin/env bash
# Checks that an ARM build of Nesto can run on the thermostat. Runs inside the
# dev container (scripts/dev.sh check-arm). The program must:
#
#   - be a 32-bit ARM executable,
#   - be fully static: no program interpreter and no shared libraries, since
#     the thermostat's own C library is far older than the toolchain's,
#   - use no instruction a Cortex-A8 lacks. The toolchain's prebuilt libraries
#     target a Cortex-A9, whose multiprocessing extension adds PLDW, and later
#     cores add hardware divide (SDIV/UDIV).
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "usage: $0 BINARY" >&2
  exit 2
fi
binary="$1"
tools="${NESTO_ARM_TOOLCHAIN:-/opt/arm-toolchain}/bin/arm-buildroot-linux-musleabihf"
failed=0

fail() {
  echo "FAIL: $*"
  failed=1
}

echo "Checking $binary"

machine="$("$tools-readelf" -h "$binary" | sed -n 's/^ *Machine: *//p')"
class="$("$tools-readelf" -h "$binary" | sed -n 's/^ *Class: *//p')"
if [ "$machine" = "ARM" ] && [ "$class" = "ELF32" ]; then
  echo "ok:   32-bit ARM executable"
else
  fail "expected a 32-bit ARM executable, got $class $machine"
fi

if "$tools-readelf" -l "$binary" | grep -q "Requesting program interpreter"; then
  fail "dynamically linked: it needs a program interpreter"
else
  echo "ok:   no program interpreter"
fi

needed="$("$tools-readelf" -d "$binary" 2>/dev/null | grep "(NEEDED)" || true)"
if [ -n "$needed" ]; then
  fail "needs shared libraries:"
  echo "$needed"
else
  echo "ok:   no shared libraries"
fi

# Disassemble once; match instruction names as whole words in the mnemonic
# column only, so symbol names and comments can't trigger a false alarm.
disassembly="$("$tools-objdump" -d --no-show-raw-insn "$binary")"
for insn in pldw sdiv udiv; do
  hits="$(printf '%s\n' "$disassembly" | awk -v insn="$insn" -F'\t' '$2 == insn' | wc -l)"
  if [ "$hits" -gt 0 ]; then
    fail "$hits '$insn' instruction(s), which a Cortex-A8 doesn't have"
  else
    echo "ok:   no '$insn' instructions"
  fi
done

size="$(stat -c %s "$binary")"
echo "size: $((size / 1024)) KiB"

exit "$failed"
