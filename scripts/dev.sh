#!/usr/bin/env bash
# Builds, tests and runs the simulator inside the dev container, so the only
# thing needed on the host is Docker. Run it from Linux or from WSL on Windows.
#
#   scripts/dev.sh image                  build the dev image
#   scripts/dev.sh build                  build everything (see below)
#   scripts/dev.sh test                   run the unit tests, natively and on emulated ARM
#   scripts/dev.sh e2e                    run the real panel against the Python client, both builds
#   scripts/dev.sh check-arm              check the ARM programs can run on the thermostat
#   scripts/dev.sh dist                   the thermostat's programs, stripped, in dist/ (nesto, nesto-probe)
#   scripts/dev.sh vectors                regenerate the protocol test vectors from tools/
#   scripts/dev.sh sim [options]          open the simulator window (WSLg or X11), port 6170
#   scripts/dev.sh client pair|connect    talk to the running simulator as Home Assistant would
#   scripts/dev.sh client demo            serve a made-up room to the running simulator
#                                         (client --host HOST ...: to a panel elsewhere)
#   scripts/dev.sh screens DIR [options]  screenshots of every screen against the demo room
#   scripts/dev.sh shot FILE "STEPS"      play STEPS headless, save the screen to FILE
#   scripts/dev.sh shot-arm FILE "STEPS"  the same with the ARM build, under QEMU
#   scripts/dev.sh shell                  a shell in the container
#   scripts/dev.sh device-check HOST [SSH OPTIONS]
#                                         describe a rooted thermostat, read-only (docs/DEVICE.md)
#   scripts/dev.sh check-report           run the device scripts in busybox and dash
#   scripts/dev.sh device-screen HOST status|take|give|watch|trial [SECONDS] [SSH OPTIONS]
#                                         take the screen from the stock software, or give it back
#   scripts/dev.sh device-run HOST [input-test] [SECONDS] [SSH OPTIONS] [-- NESTO OPTIONS]
#                                         run Nesto on the thermostat for at most SECONDS
#                                         (default 600), then give the screen back
#
# `build` makes three builds:
#   /build/sim      the simulator for this machine, with sanitizers, and the tests
#   /build/nest     the thermostat's program (ARM): /build/nest/nesto is what gets installed
#   /build/sim-arm  the simulator and the tests built for ARM, run under QEMU, so
#                   the code is exercised on the thermostat's CPU before it gets there
#
# Builds live in a Docker volume, not in the working tree, which keeps them fast
# on WSL's Windows drives and keeps the tree clean.
set -euo pipefail

IMAGE=nesto-dev
VOLUME=nesto-build
# For check-report: busybox, the thermostat's shell and tools.
BUSYBOX_IMAGE=busybox:1.37.0
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run() {
  local tty=()
  if [ -t 0 ] && [ -t 1 ]; then tty=(-it); fi
  docker run --rm "${tty[@]}" \
    -v "$ROOT":/src \
    -v "$VOLUME":/build \
    "${EXTRA_ARGS[@]}" \
    "$IMAGE" "$@"
}

EXTRA_ARGS=()

case "${1:-}" in
  image)
    docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile" "$ROOT/docker"
    ;;
  build)
    run bash -c '
      set -e
      cmake -S /src -B /build/sim -G Ninja -DNESTO_PLATFORM=sim -DNESTO_SANITIZE=ON
      cmake --build /build/sim
      arm=-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-nest.cmake
      # Earlier versions built /build/nest natively; CMake cannot switch an
      # existing build to another compiler, so start that one afresh.
      grep -qs toolchain-nest /build/nest/CMakeCache.txt || rm -rf /build/nest
      cmake -S /src -B /build/nest -G Ninja "$arm" -DNESTO_PLATFORM=nest -DNESTO_BUILD_TESTS=OFF
      cmake --build /build/nest
      cmake -S /src -B /build/sim-arm -G Ninja "$arm" -DNESTO_PLATFORM=sim
      cmake --build /build/sim-arm'
    ;;
  test)
    run bash -c '
      set -e
      ctest --test-dir /build/sim --output-on-failure
      ctest --test-dir /build/sim-arm --output-on-failure'
    ;;
  e2e)
    run bash -c '
      set -e
      cd /src/tests/e2e
      # The panel, and the probe as it ships (the nest build) on emulated ARM.
      run_tests() {
        echo "== end-to-end tests with: $1, and $2"
        NESTO_PANEL_CMD="$1" NESTO_PROBE_CMD="$2" python3 -m pytest -q -p no:cacheprovider .
      }
      run_tests "/build/sim/nesto --headless" "/build/sim/nesto-probe"
      run_tests "qemu-arm -cpu cortex-a8 /build/sim-arm/nesto --headless" \
                "qemu-arm -cpu cortex-a8 /build/nest/nesto-probe"'
    ;;
  check-arm)
    run bash -c '
      set -e
      for program in /build/nest/nesto /build/nest/nesto-probe /build/sim-arm/nesto; do
        /src/scripts/check-arm.sh "$program"
      done'
    ;;
  dist)
    run bash -c '
      set -e
      mkdir -p /src/dist
      for program in nesto nesto-probe; do
        "$NESTO_ARM_TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-strip" -o "/src/dist/$program" "/build/nest/$program"
        echo "dist/$program: $(( $(stat -c %s "/src/dist/$program") / 1024 )) KiB"
      done'
    ;;
  vectors)
    run python3 /src/tools/make_vectors.py /src/tests/vectors/protocol-v1.json
    ;;
  sim)
    shift
    if [ -d /mnt/wslg ]; then
      # WSL on Windows: show the window through WSLg.
      EXTRA_ARGS=(-e DISPLAY -e WAYLAND_DISPLAY -e XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
                  -v /tmp/.X11-unix:/tmp/.X11-unix -v /mnt/wslg:/mnt/wslg)
    else
      # Plain Linux with X11. The X server may need `xhost +local:` first.
      EXTRA_ARGS=(-e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix)
    fi
    # Publish the port for `client` (or a real Home Assistant), and keep the
    # pairing in the build volume so it survives restarts.
    EXTRA_ARGS+=(-p 6170:6170)
    run /build/sim/nesto --state-dir /build/sim-state "$@"
    ;;
  client)
    shift
    # The simulator's port is published on the Docker host; reach it from
    # here. --host HOST talks to a panel elsewhere, such as a thermostat.
    # Each panel's pairing is kept apart.
    panel=host.docker.internal
    key=/build/client.key
    if [ "${1:-}" = --host ] && [ $# -ge 2 ]; then
      panel="$2"
      key="/build/client-${panel//[^A-Za-z0-9.-]/_}.key"
      shift 2
    fi
    EXTRA_ARGS=(--add-host=host.docker.internal:host-gateway)
    run python3 /src/tools/nesto_client.py "$panel" --key-file "$key" "$@"
    ;;
  screens)
    shift
    run python3 /src/tools/screens.py "$@"
    ;;
  shot)
    if [ $# -lt 3 ]; then
      echo "usage: scripts/dev.sh shot FILE \"STEPS\"" >&2
      exit 2
    fi
    run /build/sim/nesto --headless --script "$3" --screenshot "/src/$2"
    ;;
  shot-arm)
    if [ $# -lt 3 ]; then
      echo "usage: scripts/dev.sh shot-arm FILE \"STEPS\"" >&2
      exit 2
    fi
    run qemu-arm -cpu cortex-a8 /build/sim-arm/nesto --headless --script "$3" --screenshot "/src/$2"
    ;;
  shell)
    run bash
    ;;
  check-report)
    # The thermostat's shell is busybox's; the script must also work in a
    # plain POSIX shell, and finish however little of a Nest it finds.
    # Every section, and the closing line.
    expected="$(( $(grep -c '^section "' "$ROOT/scripts/device-check.sh") + 1 ))"
    for shell in "$BUSYBOX_IMAGE sh" "$IMAGE sh"; do
      echo "== the report script in: $shell"
      # shellcheck disable=SC2086
      out="$(docker run --rm -v "$ROOT/scripts":/scripts:ro $shell /scripts/device-check.sh 2>&1)"
      sections="$(grep -c '^## ' <<< "$out" || true)"
      if [ "$(tail -1 <<< "$out")" != "## End of report" ] || [ "$sections" -ne "$expected" ]; then
        tail -30 <<< "$out"
        echo "FAIL: the report stopped early ($sections of $expected sections)" >&2
        exit 1
      fi
      echo "ok:   $sections sections, $(wc -l <<< "$out") lines"
      # shellcheck disable=SC2086
      if ! docker run --rm -v "$ROOT/scripts":/scripts:ro $shell -n /scripts/nest-screen.sh; then
        echo "FAIL: nest-screen.sh doesn't parse in $shell" >&2
        exit 1
      fi
      echo "ok:   nest-screen.sh parses"
    done
    ;;
  device-screen)
    if [ $# -lt 3 ] || ! [[ "$3" =~ ^(status|take|give|watch|trial)$ ]]; then
      echo "usage: scripts/dev.sh device-screen HOST status|take|give|watch|trial [SECONDS] [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    action="$3"
    shift 3
    seconds=""
    if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then
      seconds="$1"
      shift
    fi
    # The script goes into RAM first, so its give-back timer can run it after
    # the session ends (scripts/nest-screen.sh).
    remote="cat > /tmp/nesto-screen.sh && sh /tmp/nesto-screen.sh $action $seconds"
    if [ "$action" = trial ]; then
      mkdir -p "$ROOT/device-reports"
      log="$ROOT/device-reports/screen-trial-$(date +%Y%m%d-%H%M%S).txt"
      ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 "$@" "root@$host" "$remote" \
        < "$ROOT/scripts/nest-screen.sh" | tee "$log"
      echo
      echo "Saved to device-reports/$(basename "$log")."
    else
      ssh -o ConnectTimeout=10 "$@" "root@$host" "$remote" < "$ROOT/scripts/nest-screen.sh"
    fi
    ;;
  device-run)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-run HOST [input-test] [SECONDS] [SSH OPTIONS] [-- NESTO OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    nesto_args=()
    seconds=600
    if [ "${1:-}" = input-test ]; then
      nesto_args=(--input-test)
      seconds=120
      shift
    fi
    if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then
      seconds="$1"
      shift
    fi
    ssh_args=()
    while [ $# -gt 0 ] && [ "$1" != -- ]; do
      ssh_args+=("$1")
      shift
    done
    [ $# -gt 0 ] && shift  # the --
    nesto_args+=("$@")
    # Nesto, built for the thermostat and stripped.
    run bash -c '
      set -e
      cmake --build /build/nest --target nesto > /dev/null
      mkdir -p /src/dist
      "$NESTO_ARM_TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-strip" -o /src/dist/nesto /build/nest/nesto'
    # Two SSH sessions share one connection, so a password is asked for once.
    control="$(mktemp -u /tmp/nesto-ssh-XXXXXX)"
    ssh_args=(-o ConnectTimeout=10 -o ServerAliveInterval=15 -o ControlMaster=auto
              -o "ControlPath=$control" -o ControlPersist=60 "${ssh_args[@]}")
    # 1. Nesto goes to /, where programs may run from and stay; the screen
    #    script to RAM, where its give-back timer finds it.
    echo "Copying Nesto ($(( $(stat -c %s "$ROOT/dist/nesto") / 1024 )) KiB) to $host:/nesto/nesto"
    tar -C "$ROOT" --format=ustar --owner=0 --group=0 -cf - \
        --transform 's|.*/||' dist/nesto scripts/nest-screen.sh |
      ssh "${ssh_args[@]}" "root@$host" \
        'rm -rf /tmp/nesto-run && mkdir /tmp/nesto-run && tar xf - -C /tmp/nesto-run &&
         mkdir -p /nesto && mv /tmp/nesto-run/nesto /nesto/nesto.new && chmod 755 /nesto/nesto.new &&
         mv /nesto/nesto.new /nesto/nesto && mv /tmp/nesto-run/nest-screen.sh /tmp/nesto-screen.sh;
         status=$?; rm -rf /tmp/nesto-run; exit $status'
    # 2. Run it, with a terminal, so Ctrl-C reaches it.
    mkdir -p "$ROOT/device-reports"
    log="$ROOT/device-reports/run-$(date +%Y%m%d-%H%M%S).txt"
    remote="sh /tmp/nesto-screen.sh run $seconds"
    [ ${#nesto_args[@]} -gt 0 ] && remote+=" $(printf '%q ' "${nesto_args[@]}")"
    ssh -t "${ssh_args[@]}" "root@$host" "$remote" | tee "$log" || true
    ssh -O exit -o "ControlPath=$control" "root@$host" 2> /dev/null || true
    echo
    echo "Saved to device-reports/$(basename "$log")."
    ;;
  device-check)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-check HOST [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    if ! command -v ssh > /dev/null; then
      echo "device-check needs ssh here (on Ubuntu: sudo apt install openssh-client)" >&2
      exit 1
    fi
    # The probe, built for the thermostat and stripped.
    run bash -c '
      set -e
      cmake --build /build/nest --target nesto-probe > /dev/null
      mkdir -p /src/dist
      "$NESTO_ARM_TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-strip" -o /src/dist/nesto-probe /build/nest/nesto-probe'
    mkdir -p "$ROOT/device-reports"
    report="$ROOT/device-reports/$(date +%Y%m%d-%H%M%S).txt"
    # One SSH session, so a password is asked for once: the report script and
    # the probe travel as a tar on its input, unpack into RAM (/tmp), and are
    # deleted when the report is done. The script picks where the probe can
    # run from; see its header.
    remote='rm -rf /tmp/nesto-check && mkdir /tmp/nesto-check && cd /tmp/nesto-check &&
            tar xf - && sh ./device-check.sh; status=$?; cd / && rm -rf /tmp/nesto-check; exit $status'
    tar -C "$ROOT" --format=ustar --owner=0 --group=0 -cf - \
        --transform 's|.*/||' scripts/device-check.sh dist/nesto-probe |
      ssh -o ConnectTimeout=10 "$@" "root@$host" "$remote" |
      tee "$report" || echo "(the check over SSH exited with ${PIPESTATUS[1]})"
    echo
    echo "Saved to device-reports/$(basename "$report"). It has MAC addresses and the Wi-Fi network's name:"
    echo "read it before sharing it."
    ;;
  *)
    sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
