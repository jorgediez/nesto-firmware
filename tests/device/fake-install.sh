#!/bin/sh
# Runs scripts/install.sh as a user would, from an unpacked release, against a
# pretend thermostat in the same busybox container: "ssh" runs the remote
# command here. scripts/dev.sh check-report runs it; it prints "ok" lines, and
# exits non-zero on a failure.
#
#   docker run --rm -v "$PWD/scripts":/scripts:ro -v "$PWD/tests/device":/t:ro \
#     busybox:1.37.0 sh /t/fake-install.sh

set -u
failures=0
ok() { echo "ok:   $*"; }
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

# --- The pretend thermostat: init, monit and nlclient stand-ins ---
export NESTO_INITTAB=/etc/inittab
export NESTO_INIT_PID=999999
export NESTO_RUN_DIR=/tmp/run
export NESTO_SETTLE=0
export NESTO_MONIT=/bin/true
export NESTO_NESTLABS=/bin/true
mkdir -p "$NESTO_RUN_DIR"
echo "::sysinit:/etc/init.d/rcS" > /etc/inittab
# ssh HOST-OPTIONS... root@HOST COMMAND: the command, run here.
mkdir -p /fake
cat > /fake/ssh << 'EOF'
#!/bin/sh
for last; do :; done
exec sh -c "$last"
EOF
chmod 755 /fake/ssh
export NESTO_SSH=/fake/ssh

# --- An unpacked release ---
rel=/tmp/nesto-9.8.7-nest-gen2
mkdir -p "$rel"
cp /scripts/install.sh /scripts/nesto-boot.sh /scripts/nest-screen.sh /scripts/device-check.sh "$rel/"
chmod 777 "$rel/nesto-boot.sh" "$rel/nest-screen.sh"  # as a Windows checkout packs them
cat > "$rel/nesto" << 'EOF'
#!/bin/sh
[ "${1:-}" = --version ] && echo 9.8.7 && exit 0
exec sleep 3600
EOF
cp "$rel/nesto" "$rel/nesto-probe"
echo 9.8.7 > "$rel/VERSION"
(cd "$rel" && sha256sum nesto nesto-probe install.sh nesto-boot.sh nest-screen.sh device-check.sh VERSION > SHA256SUMS)

run() { (cd /tmp && sh "$rel/install.sh" nest.local "$@" 2>&1); }

# Install.
out="$(run)"
if [ -x /nesto/nesto ] && [ -f /nesto/nesto-boot.sh ] && [ -f /nesto/nest-screen.sh ]; then
  ok "install puts Nesto and its scripts in /nesto"
else
  fail "install: /nesto is incomplete: $(ls -l /nesto 2>&1)"
fi
modes="$(stat -c %a /nesto/nesto /nesto/nesto-boot.sh /nesto/nest-screen.sh | tr '\n' ' ')"
[ "$modes" = "755 755 755 " ] && ok "install: only root may change them" || fail "install: modes $modes"
grep -q "::respawn:/bin/sh /nesto/nesto-boot.sh" /etc/inittab && ok "install adds the boot line" ||
  fail "install: no boot line in /etc/inittab"
echo "$out" | grep -q "Nesto: none -> 9.8.7" && ok "install says which version" ||
  fail "install didn't say the version: $out"
[ -d /tmp/nesto-install ] && fail "install left /tmp/nesto-install behind" || ok "install cleans up"

# Update: the boot line once, the version change said.
out="$(run update)"
[ "$(grep -c nesto-boot.sh /etc/inittab)" = 1 ] && ok "update keeps one boot line" ||
  fail "update: $(grep -c nesto-boot.sh /etc/inittab) boot lines"
echo "$out" | grep -q "Nesto: 9.8.7 -> 9.8.7" && ok "update says the versions" ||
  fail "update didn't say the versions: $out"

# A file that doesn't match its checksum stops it before anything is sent.
echo tampered >> "$rel/nesto-boot.sh"
rm -rf /nesto
out="$(run)"
status=$?
if [ "$status" -ne 0 ] && [ ! -d /nesto ] && echo "$out" | grep -q "doesn't match SHA256SUMS"; then
  ok "a changed file is refused"
else
  fail "a changed file wasn't refused (exit $status): $out"
fi
cp /scripts/nesto-boot.sh "$rel/"
run > /dev/null

# Status, modes, uninstall.
run status | grep -q "installed: /etc/inittab starts it" && ok "status" || fail "status"
run stock > /dev/null
[ -f /media/data/nesto/stock ] && ok "stock mode" || fail "stock mode wasn't set"
run nesto > /dev/null
[ ! -f /media/data/nesto/stock ] && ok "Nesto mode" || fail "Nesto mode wasn't set"
run uninstall > /dev/null
if ! grep -q nesto-boot.sh /etc/inittab && [ ! -d /nesto ]; then
  ok "uninstall takes the line out and removes /nesto"
else
  fail "uninstall left $(grep nesto-boot.sh /etc/inittab) $(ls /nesto 2>&1)"
fi

# The usage, for a command it doesn't know.
sh "$rel/install.sh" nest.local frobnicate 2>&1 | grep -q "sh install.sh HOST" && ok "usage" ||
  fail "no usage for an unknown command"

[ "$failures" -eq 0 ] || exit 1
