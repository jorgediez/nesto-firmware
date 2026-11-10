#!/usr/bin/env bash
# Builds, tests and runs the simulator inside the dev container, so the only
# thing needed on the host is Docker. Run it from Linux or from WSL on Windows.
#
#   scripts/dev.sh image                  build the dev image
#   scripts/dev.sh build                  build everything (see below)
#   scripts/dev.sh test                   run the unit tests, natively and on emulated ARM
#   scripts/dev.sh check-arm              check the ARM programs can run on the thermostat
#   scripts/dev.sh dist                   copy the thermostat's program, stripped, to dist/nesto
#   scripts/dev.sh vectors                regenerate the protocol test vectors from tools/
#   scripts/dev.sh sim [options]          open the simulator window (WSLg or X11)
#   scripts/dev.sh shot FILE "STEPS"      play STEPS headless, save the screen to FILE
#   scripts/dev.sh shot-arm FILE "STEPS"  the same with the ARM build, under QEMU
#   scripts/dev.sh shell                  a shell in the container
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
  check-arm)
    run bash -c '/src/scripts/check-arm.sh /build/nest/nesto && /src/scripts/check-arm.sh /build/sim-arm/nesto'
    ;;
  dist)
    run bash -c '
      set -e
      mkdir -p /src/dist
      "$NESTO_ARM_TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-strip" -o /src/dist/nesto /build/nest/nesto
      echo "dist/nesto: $(( $(stat -c %s /src/dist/nesto) / 1024 )) KiB"'
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
    run /build/sim/nesto "$@"
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
  *)
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
