#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "app/App.hpp"

#if defined(NESTO_PLATFORM_SIM)
#include "hal/sim/SimHal.hpp"
#elif defined(NESTO_PLATFORM_NEST)
#include "hal/nest/NestHal.hpp"
#endif

namespace {

void print_usage(const char* program) {
  std::printf("Usage: %s [options]\n\n", program);
  std::printf("Nesto Panel %s: Home Assistant room panel for the Nest Learning Thermostat (2nd gen)\n\n",
              NESTO_VERSION);
#if defined(NESTO_PLATFORM_SIM)
  std::printf(
      "  --headless          no window; use with --script and --screenshot\n"
      "  --scale N           window pixels per screen pixel, 1 to 4 (default 2)\n"
      "  --script STEPS      play input: r+N r-N (ring), p (press), h (long press), wN (wait ms)\n"
      "  --screenshot FILE   save the screen as PNG after the script, then quit\n");
#endif
  std::printf(
      "  --version           print the version and exit\n"
      "  --help              print this and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
#if defined(NESTO_PLATFORM_SIM)
  nesto::SimOptions options;
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
#if defined(NESTO_PLATFORM_SIM)
    if (arg == "--headless") {
      options.headless = true;
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
#endif
    (void)has_value;
    std::fprintf(stderr, "Unknown or incomplete option: %s\n\n", arg.c_str());
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

#if defined(NESTO_PLATFORM_SIM)
  auto hal = std::make_unique<nesto::SimHal>(options);
#elif defined(NESTO_PLATFORM_NEST)
  auto hal = std::make_unique<nesto::NestHal>();
#endif

  nesto::App app(*hal);
  if (!app.init()) return EXIT_FAILURE;
  return app.run();
}
