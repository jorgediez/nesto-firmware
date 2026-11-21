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
#   scripts/dev.sh package                those, packed as a release in dist/ (scripts/package.sh)
#   scripts/dev.sh release-sign           sign the package's program with the test key, as a release
#                                         is signed (scripts/release-sign.sh)
#   scripts/dev.sh sign [VERSION]         sign dist/nesto with this machine's dev key, for an update
#   scripts/dev.sh ota-keygen             make the release key pair, once (keys/README.md)
#   scripts/dev.sh vectors                regenerate the protocol test vectors from tools/
#   scripts/dev.sh sim [options]          open the simulator window (WSLg or X11), port 6170
#   scripts/dev.sh client pair|connect    talk to the running simulator as Home Assistant would
#   scripts/dev.sh client demo            serve a made-up room to the running simulator
#                                         (client --host HOST ...: to a panel elsewhere)
#   scripts/dev.sh screens DIR [options]  screenshots of every screen against the demo room
#   scripts/dev.sh readme-images          the README's pictures, small, into docs/images/
#   scripts/dev.sh shot FILE "STEPS"      play STEPS headless, save the screen to FILE
#   scripts/dev.sh shot-arm FILE "STEPS"  the same with the ARM build, under QEMU
#   scripts/dev.sh shell                  a shell in the container
#   scripts/dev.sh device-check HOST [SSH OPTIONS]
#                                         describe a rooted thermostat, read-only (docs/DEVICE.md)
#   scripts/dev.sh check-report           run the device scripts in busybox and dash, and
#                                         them and install.sh against a pretend thermostat
#   scripts/dev.sh device-screen HOST status|take|give|watch|trial|battery [SECONDS] [SSH OPTIONS]
#                                         take the screen from the stock software, or give it back
#   scripts/dev.sh device-run HOST [input-test|backplate-test|light-test|sound-test|detach] [SECONDS] [SSH OPTIONS] [-- NESTO OPTIONS]
#                                         run Nesto on the thermostat for at most SECONDS
#                                         (default 600), then give the screen back;
#                                         detach: in the background, default an hour;
#                                         at most 4 hours (NESTO_RUN_MAX, seconds), or
#                                         26 minutes with --no-backplate (NESTO_TAKE_MAX)
#   scripts/dev.sh device-log HOST [follow] [SSH OPTIONS]
#                                         copy a detached run's log to device-reports/, or follow it
#   scripts/dev.sh device-install HOST [update] [SSH OPTIONS]
#                                         install Nesto for good, started at boot; update: a new Nesto
#   scripts/dev.sh device-mode HOST stock|nesto [SSH OPTIONS]
#                                         the stock software or Nesto, from now on
#   scripts/dev.sh device-status HOST [SSH OPTIONS]
#                                         the mode, what runs, and the end of Nesto's log
#   scripts/dev.sh device-uninstall HOST [SSH OPTIONS]
#                                         the stock software back for good
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
  # CI passes through, so a CI build trusts no dev key (see build).
  docker run --rm "${tty[@]}" \
    -e CI \
    -v "$ROOT":/src \
    -v "$VOLUME":/build \
    "${EXTRA_ARGS[@]}" \
    "$IMAGE" "$@"
}

EXTRA_ARGS=()

# Nesto, built for the thermostat and stripped, to dist/nesto.
build_nest_binary() {
  run bash -c '
    set -e
    cmake --build /build/nest --target nesto > /dev/null
    mkdir -p /src/dist
    "$NESTO_ARM_TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-strip" -o /src/dist/nesto /build/nest/nesto'
}

# The scripts the installer sends, next to the built programs in dist/, as a
# release has them.
stage_scripts() {
  mkdir -p "$ROOT/dist"
  cp "$ROOT/scripts/nesto-boot.sh" "$ROOT/scripts/nest-screen.sh" "$ROOT/scripts/device-check.sh" "$ROOT/dist/"
}

# On the thermostat: whether Nesto is installed there, started at boot.
INSTALLED_CHECK='[ -f /nesto/nesto-boot.sh ] && grep -q nesto-boot.sh /etc/inittab'

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
      # Outside CI, the thermostat'"'"'s build also trusts this machine'"'"'s dev key,
      # so a local build can be updated with a locally signed one (dev.sh sign).
      dev_key=
      if [ -z "${CI:-}" ]; then
        [ -f /build/ota-dev.key ] || python3 /src/tools/ota_sign.py keygen /build/ota-dev.pub \
          | tail -1 > /build/ota-dev.key
        dev_key=/build/ota-dev.pub
      fi
      cmake -S /src -B /build/nest -G Ninja "$arm" -DNESTO_PLATFORM=nest -DNESTO_BUILD_TESTS=OFF \
        -DNESTO_OTA_DEV_KEY="$dev_key"
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
  sign)
    # dist/nesto, signed with this machine's dev key, for an update by hand:
    # scripts/dev.sh client --host HOST ota /src/dist/nesto VERSION
    version="${2:-$(sed -n 's/^project(nesto VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")}"
    run python3 /src/tools/ota_sign.py sign /src/dist/nesto "$version" --key /build/ota-dev.key
    ;;
  ota-keygen)
    # The release key pair, once: the public half to keys/ota-release.pub, the
    # private half shown here, for the maintainer to keep and give to CI.
    if [ -f "$ROOT/keys/ota-release.pub" ]; then
      echo "keys/ota-release.pub exists already; remove it first to make a new key" >&2
      exit 1
    fi
    run python3 /src/tools/ota_sign.py keygen /src/keys/ota-release.pub
    ;;
  package)
    "$0" dist
    # In the container, like dist: it owns dist/, which the CI runner's user
    # can't write to. The dates come from the commit, read here, where git
    # knows the repository.
    run env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" bash /src/scripts/package.sh
    ;;
  release-sign)
    # The release's .bin and .bin.sig from the package in dist/, signed with
    # the test key: CI checks the signing this way on every build, and a tag's
    # release signs with the release key (.github/workflows/ci.yml).
    version="$(sed -n 's/^project(nesto VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
    run bash /src/scripts/release-sign.sh "/src/dist/nesto-$version-nest-gen2.tar.gz" \
      /src/tests/ota/test-key.pub /src/tests/ota/test-key
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
    # The container's time zone is UTC: give it this computer's UTC offset, as
    # a POSIX TZ (whose sign is the other way round), so the time sent to the
    # panel is local. Daylight saving changes need a new run.
    z=$(date +%z)
    tz="<$z>$([ "${z:0:1}" = + ] && echo - || echo +)${z:1:2}:${z:3:2}"
    EXTRA_ARGS=(--add-host=host.docker.internal:host-gateway -e "TZ=$tz")
    run python3 /src/tools/nesto_client.py "$panel" --key-file "$key" "$@"
    ;;
  screens)
    shift
    run python3 /src/tools/screens.py "$@"
    ;;
  readme-images)
    run python3 /src/tools/readme_images.py
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
      # shellcheck disable=SC2086
      if ! docker run --rm -v "$ROOT/scripts":/scripts:ro $shell -n /scripts/nesto-boot.sh; then
        echo "FAIL: nesto-boot.sh doesn't parse in $shell" >&2
        exit 1
      fi
      echo "ok:   nesto-boot.sh parses"
      # shellcheck disable=SC2086
      if ! docker run --rm -v "$ROOT/scripts":/scripts:ro $shell -n /scripts/install.sh; then
        echo "FAIL: install.sh doesn't parse in $shell" >&2
        exit 1
      fi
      echo "ok:   install.sh parses"
    done
    # nest-screen.sh's runs, against a pretend thermostat in busybox.
    echo "== nest-screen.sh on a pretend thermostat, in busybox"
    docker run --rm -v "$ROOT/scripts":/scripts:ro -v "$ROOT/tests/device":/t:ro \
      "$BUSYBOX_IMAGE" sh /t/fake-nest.sh
    # nesto-boot.sh, installed and started at boot, against the same.
    echo "== nesto-boot.sh on a pretend thermostat, in busybox"
    docker run --rm -v "$ROOT/scripts":/scripts:ro -v "$ROOT/tests/device":/t:ro       "$BUSYBOX_IMAGE" sh /t/fake-boot.sh
    # install.sh, from an unpacked release, against the same.
    echo "== install.sh on a pretend thermostat, in busybox"
    docker run --rm -v "$ROOT/scripts":/scripts:ro -v "$ROOT/tests/device":/t:ro       "$BUSYBOX_IMAGE" sh /t/fake-install.sh
    ;;
  device-screen)
    if [ $# -lt 3 ] || ! [[ "$3" =~ ^(status|take|give|watch|trial|battery)$ ]]; then
      echo "usage: scripts/dev.sh device-screen HOST status|take|give|watch|trial|battery [SECONDS] [SSH OPTIONS]" >&2
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
    # the session ends (scripts/nest-screen.sh). It replaces the copy rather
    # than writing over it: a detached run may still be reading that one.
    remote="cat > /tmp/nesto-screen.sh.new && mv /tmp/nesto-screen.sh.new /tmp/nesto-screen.sh &&
            sh /tmp/nesto-screen.sh $action $seconds"
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
      echo "usage: scripts/dev.sh device-run HOST [input-test|backplate-test|light-test|sound-test|detach] [SECONDS] [SSH OPTIONS] [-- NESTO OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    nesto_args=()
    seconds=600
    mode=run
    if [ "${1:-}" = input-test ]; then
      nesto_args=(--input-test)
      seconds=120
      shift
    elif [ "${1:-}" = backplate-test ]; then
      nesto_args=(--backplate-test)
      seconds=180
      shift
    elif [ "${1:-}" = light-test ]; then
      nesto_args=(--light-test)
      seconds=180
      shift
    elif [ "${1:-}" = sound-test ]; then
      nesto_args=(--sound-test)
      seconds=60
      shift
    elif [ "${1:-}" = detach ]; then
      mode=detach
      seconds=3600
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
    build_nest_binary
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
    # With Nesto installed, its service gives way first (scripts/nesto-boot.sh).
    ssh "${ssh_args[@]}" "root@$host"       "if $INSTALLED_CHECK; then sh /nesto/nesto-boot.sh pause; fi" || true
    remote="sh /tmp/nesto-screen.sh $mode $seconds"
    # The limits on how long the stock software may be away (scripts/nest-screen.sh).
    for limit in NESTO_TAKE_MAX NESTO_RUN_MAX; do
      if [ -n "${!limit:-}" ]; then
        [[ "${!limit}" =~ ^[0-9]+$ ]] || { echo "$limit must be seconds" >&2; exit 2; }
        remote="$limit=${!limit} $remote"
      fi
    done
    [ ${#nesto_args[@]} -gt 0 ] && remote+=" $(printf '%q ' "${nesto_args[@]}")"
    if [ "$mode" = detach ]; then
      # 2. Start it on the thermostat, where it goes on without this session.
      ssh "${ssh_args[@]}" "root@$host" "$remote" || true
      ssh -O exit -o "ControlPath=$control" "root@$host" 2> /dev/null || true
      echo
      echo "If Nesto is installed, it's paused until: scripts/dev.sh device-mode $host nesto"
      echo "Follow it:  scripts/dev.sh device-log $host follow"
      echo "Copy it:    scripts/dev.sh device-log $host"
      echo "Stop early: scripts/dev.sh device-screen $host give"
      exit 0
    fi
    # 2. Run it, with a terminal, so Ctrl-C reaches it.
    mkdir -p "$ROOT/device-reports"
    log="$ROOT/device-reports/run-$(date +%Y%m%d-%H%M%S).txt"
    ssh -t "${ssh_args[@]}" "root@$host" "$remote" | tee "$log" || true
    ssh "${ssh_args[@]}" "root@$host"       "if $INSTALLED_CHECK; then sh /nesto/nesto-boot.sh resume; fi" || true
    ssh -O exit -o "ControlPath=$control" "root@$host" 2> /dev/null || true
    echo
    echo "Saved to device-reports/$(basename "$log")."
    ;;
  device-install)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-install HOST [update] [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    [ "${1:-}" = update ] && shift
    build_nest_binary
    stage_scripts
    # Installing or updating are the same to the installer: it puts this
    # build in place, and adds the boot line if it isn't there yet.
    NESTO_FILES="$ROOT/dist" sh "$ROOT/scripts/install.sh" "$host" install "$@"
    ;;
  device-mode)
    if [ $# -lt 3 ] || ! [[ "$3" =~ ^(stock|nesto)$ ]]; then
      echo "usage: scripts/dev.sh device-mode HOST stock|nesto [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    mode="$3"
    shift 3
    sh "$ROOT/scripts/install.sh" "$host" "$mode" "$@"
    ;;
  device-status)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-status HOST [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    sh "$ROOT/scripts/install.sh" "$host" status "$@"
    ;;
  device-uninstall)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-uninstall HOST [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    # The stock software back for good; Nesto's settings and log stay in
    # /media/data/nesto.
    sh "$ROOT/scripts/install.sh" "$host" uninstall "$@"
    ;;
  device-log)
    if [ $# -lt 2 ]; then
      echo "usage: scripts/dev.sh device-log HOST [follow] [SSH OPTIONS]" >&2
      exit 2
    fi
    host="$2"
    shift 2
    # Where a detached run writes (scripts/nest-screen.sh).
    run_log=/media/data/nesto/run.log
    if [ "${1:-}" = follow ]; then
      shift
      echo "Following $host:$run_log; Ctrl-C stops following, not the run."
      ssh -t -o ConnectTimeout=10 -o ServerAliveInterval=15 "$@" "root@$host" "tail -n 40 -f $run_log" || true
      exit 0
    fi
    mkdir -p "$ROOT/device-reports"
    log="$ROOT/device-reports/run-detached-$(date +%Y%m%d-%H%M%S).txt"
    if ! ssh -o ConnectTimeout=10 "$@" "root@$host" "cat $run_log" > "$log"; then
      rm -f "$log"
      echo "No run log on $host ($run_log)." >&2
      exit 1
    fi
    tail -n 5 "$log"
    echo
    echo "Saved to device-reports/$(basename "$log") ($(wc -l < "$log") lines)."
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
    stage_scripts
    NESTO_FILES="$ROOT/dist" NESTO_REPORTS="$ROOT/device-reports" sh "$ROOT/scripts/install.sh" "$host" check "$@"
    ;;
  *)
    sed -n '2,47p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
