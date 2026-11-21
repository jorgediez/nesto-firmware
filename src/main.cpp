#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "app/App.hpp"
#include "core/Log.hpp"

#if defined(NESTO_PLATFORM_SIM)
#include "hal/sim/SimHal.hpp"
#elif defined(NESTO_PLATFORM_NEST)
#include <spawn.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hal/nest/NestHal.hpp"

extern char** environ;
#endif

namespace {

// Reboots, as Home Assistant asked (PROTOCOL.md section 18), once Nesto has
// stopped and handed the base back. On the thermostat it doesn't return when
// it works: init stops everything, the boot script too, so Nesto's exit is
// never taken for a crash. An update only replaces the program, so this
// can't count on a boot script that knows another exit code.
int reboot_now() {
#if defined(NESTO_PLATFORM_NEST)
  nesto::log::info("rebooting the thermostat");
  sync();
  // Busybox's reboot asks init, which runs the stock shutdown script
  // (/etc/init.d/rcH: the services stopped, the filesystems unmounted), then
  // ends every process and reboots. Nesto is one of them: with the default
  // handlers back, init's SIGTERM ends it, rather than cut the wait short.
  for (int signal : {SIGINT, SIGTERM, SIGHUP}) std::signal(signal, SIG_DFL);
  char program[] = "/sbin/reboot";
  char* argv[] = {program, nullptr};
  pid_t pid = 0;
  if (posix_spawn(&pid, program, nullptr, nullptr, argv, environ) == 0) {
    int status = 0;
    waitpid(pid, &status, 0);
    // The shutdown script takes a while; wait for init rather than exit.
    unsigned left = 120;
    while (left > 0) left = sleep(left);
  }
  nesto::log::warn("init didn't reboot the thermostat: rebooting it directly");
  sync();
  ::reboot(RB_AUTOBOOT);
  nesto::log::error("couldn't reboot: %s", std::strerror(errno));
  return EXIT_FAILURE;
#else
  nesto::log::info("the thermostat would reboot now; the simulator exits");
  return nesto::kRebootExit;
#endif
}

std::string default_state_dir() {
#if defined(NESTO_PLATFORM_SIM)
  const char* home = std::getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.nesto";
#else
  // The flash's data partition: kept across reboots, and left alone by the
  // stock software (docs/DEVICE.md).
  return "/media/data/nesto";
#endif
}

void print_usage(const char* program) {
  std::printf("Usage: %s [options]\n\n", program);
  std::printf("Nesto Panel %s: Home Assistant room panel for the Nest Learning Thermostat (2nd gen)\n\n",
              NESTO_VERSION);
#if defined(NESTO_PLATFORM_SIM)
  std::printf(
      "  --headless          no window; use with --script and --screenshot\n"
      "  --scale N           window pixels per screen pixel, 1 to 4 (default 2)\n"
      "  --script STEPS      play input: r+N r-N (ring), p (press), h (long press), wN (wait ms)\n"
      "  --screenshot FILE   save the screen as PNG after the script, then quit\n"
      "  --fake-base         a base with fixed sensor readings, as on the thermostat\n");
#elif defined(NESTO_PLATFORM_NEST)
  std::printf(
      "  --ring-counts N     the ring's sensor counts per tick (default %d)\n"
      "  --ring-reverse      turn the ring's direction round\n"
      "  --input-test        print the ring's and the press's events, for measuring the ring\n"
      "  --backplate-test    talk to the backplate alone, printing every frame\n"
      "  --light-test        print the light sensor's readings, switching the backlight\n"
      "  --sound-test        play candidate clicks and beeps, saying which is which\n"
      "  --no-backplate      don't talk to the backplate (it resets the thermostat after ~30 min)\n"
      "  --power-steal       set the backplate's power steal mode after the handshake\n",
      nesto::NestOptions{}.ring.counts_per_tick);
#endif
  std::printf(
      "  --port N            TCP port for Home Assistant (default 6170)\n"
      "  --state-dir DIR     where the pairing key is kept (default %s)\n"
      "  --panel-id ID       panel ID instead of the network interface's MAC address\n"
      "  --no-mdns           don't announce the panel on the network\n"
      "  --hold-for-stock    held in the first 3 s, exit with 3 (for the boot script)\n"
      "  --ota-target FILE   take firmware updates from Home Assistant, replacing FILE,\n"
      "                      then exit with 4 to be started again (for the boot script)\n"
      "  --version          print the version and exit\n"
      "  --help              print this and exit\n",
      default_state_dir().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  nesto::AppOptions app_options;
  app_options.state_dir = default_state_dir();
#if defined(NESTO_PLATFORM_SIM)
  nesto::SimOptions options;
  app_options.fake_wifi = true;  // no connman here
#elif defined(NESTO_PLATFORM_NEST)
  nesto::NestOptions options;
  bool input_test = false;
  bool backplate_test = false;
  bool light_test = false;
  bool sound_test = false;
#endif

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--help") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }
    if (arg == "--version") {
      std::printf("%s\n", NESTO_VERSION);
      return EXIT_SUCCESS;
    }
    if (arg == "--port" && has_value) {
      const long port = std::strtol(argv[++i], nullptr, 10);
      if (port < 1 || port > 65535) {
        std::fprintf(stderr, "--port must be between 1 and 65535\n");
        return EXIT_FAILURE;
      }
      app_options.port = static_cast<std::uint16_t>(port);
      continue;
    }
    if (arg == "--state-dir" && has_value) {
      app_options.state_dir = argv[++i];
      continue;
    }
    if (arg == "--no-mdns") {
      app_options.mdns = false;
      continue;
    }
    if (arg == "--ota-target" && has_value) {
      app_options.ota_target = argv[++i];
      continue;
    }
    if (arg == "--hold-for-stock") {
      app_options.hold_for_stock = true;
      continue;
    }
    if (arg == "--panel-id" && has_value) {
      app_options.panel_id = argv[++i];
      continue;
    }
#if defined(NESTO_PLATFORM_SIM)
    if (arg == "--headless") {
      options.headless = true;
      continue;
    }
    if (arg == "--fake-base") {
      options.fake_base = true;
      continue;
    }
    if (arg == "--scale" && has_value) {
      options.scale = std::atoi(argv[++i]);
      continue;
    }
    if (arg == "--script" && has_value) {
      options.script = argv[++i];
      continue;
    }
    if (arg == "--screenshot" && has_value) {
      options.screenshot = argv[++i];
      continue;
    }
#elif defined(NESTO_PLATFORM_NEST)
    if (arg == "--ring-counts" && has_value) {
      const long counts = std::strtol(argv[++i], nullptr, 10);
      if (counts < 1 || counts > 10000) {
        std::fprintf(stderr, "--ring-counts must be between 1 and 10000\n");
        return EXIT_FAILURE;
      }
      options.ring.counts_per_tick = static_cast<int>(counts);
      continue;
    }
    if (arg == "--ring-reverse") {
      options.ring.reverse = !options.ring.reverse;
      continue;
    }
    if (arg == "--input-test") {
      input_test = true;
      continue;
    }
    if (arg == "--light-test") {
      light_test = true;
      continue;
    }
    if (arg == "--sound-test") {
      sound_test = true;
      continue;
    }
    if (arg == "--backplate-test") {
      backplate_test = true;
      continue;
    }
    if (arg == "--no-backplate") {
      options.backplate = false;
      continue;
    }
    if (arg == "--power-steal") {
      options.session.power_steal = true;
      continue;
    }
#endif
    (void)has_value;
    std::fprintf(stderr, "Unknown or incomplete option: %s\n\n", arg.c_str());
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

#if defined(NESTO_PLATFORM_SIM)
  auto hal = std::make_unique<nesto::SimHal>(options);
#elif defined(NESTO_PLATFORM_NEST)
  auto hal = std::make_unique<nesto::NestHal>(options);
  if (input_test) return hal->input_test();
  if (backplate_test) return hal->backplate_test();
  if (sound_test) return hal->sound_test();
  if (light_test) return hal->light_test();
#endif

  int code = EXIT_FAILURE;
  {
    nesto::App app(*hal, app_options);
    if (!app.init()) return EXIT_FAILURE;
    code = app.run();
  }
  if (code != nesto::kRebootExit) return code;
  // The hardware let go first: the base handed back, the backlight restored.
  hal.reset();
  return reboot_now();
}
