#!/usr/bin/env bash
# Builds, tests and runs the simulator inside the dev container, so the only
# thing needed on the host is Docker. Run it from Linux or from WSL on Windows.
#
#   scripts/dev.sh image                  build the dev image
#   scripts/dev.sh build                  build the simulator, the Nest target and the tests
#   scripts/dev.sh test                   run the unit tests
#   scripts/dev.sh sim [options]          open the simulator window (WSLg or X11)
#   scripts/dev.sh shot FILE "STEPS"      play STEPS headless, save the screen to FILE
#   scripts/dev.sh shell                  a shell in the container
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
      cmake -S /src -B /build/nest -G Ninja -DNESTO_PLATFORM=nest -DNESTO_BUILD_TESTS=OFF
      cmake --build /build/nest'
    ;;
  test)
    run ctest --test-dir /build/sim --output-on-failure
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
  shell)
    run bash
    ;;
  *)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
