// nesto-probe: checks, on the thermostat, what Nesto needs from its kernel,
// without changing anything. It's run by scripts/dev.sh device-check (see
// docs/DEVICE.md) while the stock software keeps running, so it:
//
//   - never writes to the screen, the backplate's serial port or the watchdog
//     (opening /dev/watchdog would start it);
//   - only reads the framebuffer's and input devices' descriptions;
//   - writes only to a temporary directory it removes again (--temp-dir, /tmp
//     by default, which dev.sh points at a RAM file system).
//
// Each check prints one line: "ok:", "FAIL:" or "info:". The exit status is
// the number of failures, capped at 100.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "probe/SelfTest.hpp"
#include "protocol/Crypto.hpp"
#include "protocol/PairingState.hpp"

namespace {

int failures = 0;

void say(const char* tag, const char* format, va_list args) {
  std::printf("%-6s", tag);
  std::vprintf(format, args);
  std::printf("\n");
  std::fflush(stdout);
}

__attribute__((format(printf, 1, 2))) void ok(const char* format, ...) {
  va_list args;
  va_start(args, format);
  say("ok:", format, args);
  va_end(args);
}

__attribute__((format(printf, 1, 2))) void fail(const char* format, ...) {
  ++failures;
  va_list args;
  va_start(args, format);
  say("FAIL:", format, args);
  va_end(args);
}

__attribute__((format(printf, 1, 2))) void info(const char* format, ...) {
  va_list args;
  va_start(args, format);
  say("info:", format, args);
  va_end(args);
}

const char* why() {
  return std::strerror(errno);
}

/// Closes a descriptor when it goes out of scope.
struct Fd {
  int fd = -1;
  explicit Fd(int f) : fd(f) {}
  ~Fd() {
    if (fd >= 0) close(fd);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

long long millis(clockid_t clock) {
  timespec ts{};
  clock_gettime(clock, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// --- The system -------------------------------------------------------------

void check_system() {
  utsname u{};
  if (uname(&u) == 0) info("kernel: %s %s %s", u.sysname, u.release, u.machine);
  info("probe built for: %s",
#if defined(__arm__)
       "ARM (the thermostat)"
#else
       "this computer, not the thermostat"
#endif
  );

#if defined(__arm__)
  // The kernel must save and restore NEON registers for every program, or
  // NEON code crashes or corrupts other programs' registers.
  constexpr unsigned long kHwcapNeon = 1UL << 12;
  constexpr unsigned long kHwcapVfpv3 = 1UL << 13;
  const unsigned long hwcap = getauxval(AT_HWCAP);
  if ((hwcap & kHwcapNeon) && (hwcap & kHwcapVfpv3)) {
    ok("the kernel supports NEON and VFPv3 (hwcap 0x%lx)", hwcap);
  } else {
    fail("the kernel doesn't report NEON and VFPv3 (hwcap 0x%lx); Nesto is built for both", hwcap);
  }
#endif
}

#if defined(__ARM_NEON)
sigjmp_buf illegal_jump;

void on_illegal(int) {
  siglongjmp(illegal_jump, 1);
}
#endif

void check_neon() {
#if defined(__ARM_NEON)
  struct sigaction action {};
  struct sigaction previous {};
  action.sa_handler = on_illegal;
  sigaction(SIGILL, &action, &previous);
  if (sigsetjmp(illegal_jump, 1) == 0) {
    const float a[4] = {1.5f, 2.5f, 3.5f, 4.5f};
    const float b[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float sum[4];
    vst1q_f32(sum, vaddq_f32(vld1q_f32(a), vld1q_f32(b)));
    if (sum[0] == 2.0f && sum[1] == 3.0f && sum[2] == 4.0f && sum[3] == 5.0f) {
      ok("NEON instructions run and add up");
    } else {
      fail("NEON instructions gave wrong results");
    }
  } else {
    fail("NEON instructions are illegal here (SIGILL)");
  }
  sigaction(SIGILL, &previous, nullptr);
#else
  info("NEON: not checked, this build has no NEON");
#endif
}

void check_clocks() {
  const long long before = millis(CLOCK_MONOTONIC);
  timespec pause{0, 50 * 1000 * 1000};
  nanosleep(&pause, nullptr);
  const long long elapsed = millis(CLOCK_MONOTONIC) - before;
  if (elapsed >= 40 && elapsed < 1000) {
    ok("monotonic clock: a 50 ms sleep took %lld ms", elapsed);
  } else {
    fail("monotonic clock: a 50 ms sleep took %lld ms", elapsed);
  }

  const time_t now = time(nullptr);
  tm utc{};
  gmtime_r(&now, &utc);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &utc);
  // Nesto gets the time from Home Assistant, so a wrong clock isn't a failure.
  info("wall clock: %s UTC%s", text,
       utc.tm_year + 1900 < 2025 ? " (wrong: never set, Home Assistant will set it)" : "");
}

void check_memory() {
  // About what Nesto needs at most: LVGL's buffers and fonts, a frame or two,
  // a carousel picture being decoded.
  constexpr std::size_t kSize = 8 * 1024 * 1024;
  auto* block = static_cast<unsigned char*>(std::malloc(kSize));
  if (!block) {
    fail("can't allocate %zu MiB", kSize >> 20);
    return;
  }
  std::memset(block, 0xA5, kSize);  // touch every page, so it's really there
  unsigned sum = 0;
  for (std::size_t i = 0; i < kSize; i += 4096) sum += block[i];
  std::free(block);
  if (sum == 0xA5u * (kSize / 4096)) {
    ok("allocated and used %zu MiB", kSize >> 20);
  } else {
    fail("memory read back wrong");
  }
}

void check_random() {
  std::uint8_t a[32]{};
  std::uint8_t b[32]{};
  if (!nesto::protocol::random_bytes(a, sizeof a) || !nesto::protocol::random_bytes(b, sizeof b)) {
    fail("can't read /dev/urandom: %s", why());
  } else if (std::all_of(a, a + sizeof a, [](std::uint8_t x) { return x == 0; }) ||
             std::memcmp(a, b, sizeof a) == 0) {
    fail("/dev/urandom gives zeros or repeats itself");
  } else {
    ok("/dev/urandom gives random bytes");
  }
}

void check_crypto() {
  const std::string failed = nesto::probe::crypto_self_test();
  if (failed.empty()) {
    ok("cryptography matches the test vectors (X25519, SHA-512, HKDF, HMAC, ChaCha20-Poly1305)");
  } else {
    fail("cryptography differs from the test vectors: %s", failed.c_str());
  }
}

void check_key_store(const std::string& temp_dir) {
  char path[256];
  std::snprintf(path, sizeof path, "%s/nesto-probe-XXXXXX", temp_dir.c_str());
  if (!mkdtemp(path)) {
    fail("can't make a temporary directory in %s: %s", temp_dir.c_str(), why());
    return;
  }
  const std::string dir = path;
  {
    nesto::protocol::FileKeyStore store(dir);
    nesto::protocol::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);
    struct stat st {};
    const std::string file = dir + "/pairing.key";
    if (!store.save(key)) {
      fail("can't save a pairing key in %s", dir.c_str());
    } else if (store.load() != key) {
      fail("the saved pairing key read back wrong");
    } else if (stat(file.c_str(), &st) != 0 || (st.st_mode & 0777) != 0600) {
      fail("the saved pairing key isn't private (mode 0600)");
    } else {
      ok("pairing key saved, read back and private, in %s", temp_dir.c_str());
    }
    store.erase();
  }
  // Whatever the store left, then the directory itself.
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) {
        unlink((dir + "/" + e->d_name).c_str());
      }
    }
    closedir(d);
  }
  rmdir(dir.c_str());
}

// --- The network ------------------------------------------------------------

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wait_for(int fd, bool for_write, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  const int n = for_write ? select(fd + 1, nullptr, &set, nullptr, &tv)
                          : select(fd + 1, &set, nullptr, nullptr, &tv);
  return n == 1;
}

/// Nesto's server (net/Server.cpp): a non-blocking TCP listener driven by
/// select(). Tried on Nesto's own port, and a free one if that's taken.
void check_tcp(std::uint16_t port) {
  Fd listener(socket(AF_INET, SOCK_STREAM, 0));
  if (listener.fd < 0) {
    fail("TCP: no socket: %s", why());
    return;
  }
  const int yes = 1;
  setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    ok("TCP port %u is free for Nesto", port);
  } else {
    fail("TCP port %u: %s; Nesto would need another port", port, why());
    addr.sin_port = 0;
    if (bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      fail("TCP: can't bind any port: %s", why());
      return;
    }
  }
  socklen_t len = sizeof addr;
  getsockname(listener.fd, reinterpret_cast<sockaddr*>(&addr), &len);
  if (listen(listener.fd, 4) != 0 || !set_nonblocking(listener.fd)) {
    fail("TCP: can't listen: %s", why());
    return;
  }

  Fd client(socket(AF_INET, SOCK_STREAM, 0));
  set_nonblocking(client.fd);
  sockaddr_in to = addr;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int connected = connect(client.fd, reinterpret_cast<sockaddr*>(&to), sizeof to);
  if (connected != 0 && errno != EINPROGRESS) {
    fail("TCP: can't connect to itself: %s", why());
    return;
  }
  if (!wait_for(listener.fd, false, 2000)) {
    fail("TCP: select() never reported the incoming connection");
    return;
  }
  Fd accepted(accept(listener.fd, nullptr, nullptr));
  if (accepted.fd < 0 || !set_nonblocking(accepted.fd) || !wait_for(client.fd, true, 2000)) {
    fail("TCP: can't accept the connection: %s", why());
    return;
  }
  const char ping[] = "nesto";
  char got[sizeof ping]{};
  if (send(client.fd, ping, sizeof ping, 0) != static_cast<ssize_t>(sizeof ping) ||
      !wait_for(accepted.fd, false, 2000) ||
      recv(accepted.fd, got, sizeof got, 0) != static_cast<ssize_t>(sizeof ping) ||
      std::memcmp(ping, got, sizeof ping) != 0) {
    fail("TCP: bytes didn't arrive over the loopback connection");
    return;
  }
  ok("TCP: non-blocking listen, connect, select, accept and send work");
}

/// What the mDNS library Nesto uses does with its socket (mdns.h,
/// mdns_socket_setup_ipv4), one step at a time, since the library ignores
/// failures. The stock software may already hold port 5353.
void check_mdns() {
  Fd sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (sock.fd < 0) {
    fail("mDNS: no UDP socket: %s", why());
    return;
  }
  const int yes = 1;
  if (setsockopt(sock.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) != 0) {
    fail("mDNS: SO_REUSEADDR: %s", why());
  }
#ifdef SO_REUSEPORT
  if (setsockopt(sock.fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes) == 0) {
    info("mDNS: SO_REUSEPORT is supported");
  } else {
    info("mDNS: SO_REUSEPORT isn't supported (%s), only SO_REUSEADDR (expected on 2.6.37)", why());
  }
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(5353);
  if (bind(sock.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    ok("mDNS: UDP port 5353 can be bound (shared or free)");
  } else {
    fail("mDNS: UDP port 5353: %s; Nesto would run without discovery", why());
    return;
  }

  ip_mreq group{};
  group.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
  group.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(sock.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) == 0) {
    ok("mDNS: joined the multicast group 224.0.0.251");
  } else {
    fail("mDNS: can't join 224.0.0.251: %s (no network, or no multicast route)", why());
  }
}

// --- The hardware, read-only ------------------------------------------------

void check_framebuffer() {
  Fd fb(open("/dev/fb0", O_RDONLY));
  if (fb.fd < 0) {
    fail("framebuffer /dev/fb0: %s", why());
    return;
  }
  fb_var_screeninfo var{};
  fb_fix_screeninfo fix{};
  if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fb.fd, FBIOGET_FSCREENINFO, &fix) != 0) {
    fail("framebuffer: can't read its format: %s", why());
    return;
  }
  info("framebuffer: \"%.16s\", %ux%u visible, %ux%u virtual, offset %u,%u", fix.id, var.xres,
       var.yres, var.xres_virtual, var.yres_virtual, var.xoffset, var.yoffset);
  info("framebuffer: %u bits per pixel, red %u/%u green %u/%u blue %u/%u alpha %u/%u "
       "(offset/length)",
       var.bits_per_pixel, var.red.offset, var.red.length, var.green.offset, var.green.length,
       var.blue.offset, var.blue.length, var.transp.offset, var.transp.length);
  info("framebuffer: %u bytes per line, %u bytes of memory, %s", fix.line_length, fix.smem_len,
       var.yres_virtual >= 2 * var.yres ? "room for two pages" : "one page");
  // NestHal assumes 32-bit pixels with blue in the lowest byte (XRGB8888).
  if (var.bits_per_pixel == 32 && var.xres >= 320 && var.yres >= 320 && var.blue.offset == 0 &&
      var.green.offset == 8 && var.red.offset == 16) {
    ok("framebuffer: the 320x320 XRGB8888 layout Nesto draws");
  } else {
    fail("framebuffer: not the 320x320 XRGB8888 layout Nesto draws; NestHal needs changing");
  }
}

std::string event_types(unsigned long bits) {
  static const struct {
    int bit;
    const char* name;
  } kTypes[] = {{EV_SYN, "SYN"}, {EV_KEY, "KEY"}, {EV_REL, "REL"}, {EV_ABS, "ABS"},
                {EV_MSC, "MSC"}, {EV_SW, "SW"},   {EV_LED, "LED"}, {EV_SND, "SND"},
                {EV_REP, "REP"}, {EV_FF, "FF"},   {EV_PWR, "PWR"}};
  std::string out;
  for (const auto& t : kTypes) {
    if (bits & (1UL << t.bit)) out += out.empty() ? t.name : std::string(" ") + t.name;
  }
  return out;
}

void check_input() {
  int found = 0;
  for (int i = 0; i < 16; ++i) {
    char path[32];
    std::snprintf(path, sizeof path, "/dev/input/event%d", i);
    // Read-only and without EVIOCGRAB, so the stock software keeps its events.
    Fd dev(open(path, O_RDONLY | O_NONBLOCK));
    if (dev.fd < 0) continue;
    ++found;
    char name[64] = "?";
    ioctl(dev.fd, EVIOCGNAME(sizeof name - 1), name);
    unsigned long types = 0;
    ioctl(dev.fd, EVIOCGBIT(0, sizeof types), &types);
    std::string detail = event_types(types);
    if (types & (1UL << EV_REL)) {
      unsigned long rel = 0;
      ioctl(dev.fd, EVIOCGBIT(EV_REL, sizeof rel), &rel);
      char axes[48];
      std::snprintf(axes, sizeof axes, ", relative axes 0x%lx", rel);
      detail += axes;
    }
    if (types & (1UL << EV_KEY)) {
      unsigned char keys[KEY_MAX / 8 + 1]{};
      ioctl(dev.fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
      std::string codes;
      for (int k = 0; k < KEY_MAX && codes.size() < 60; ++k) {
        if (keys[k / 8] & (1 << (k % 8))) codes += (codes.empty() ? "" : ",") + std::to_string(k);
      }
      detail += ", keys " + codes;
    }
    info("input %s: \"%s\": %s", path, name, detail.c_str());
  }
  if (found == 0) {
    fail("no input devices readable in /dev/input");
  } else {
    ok("%d input device(s) readable; the ring and the press should be among them", found);
  }
}

void usage() {
  std::printf(
      "usage: nesto-probe [--port N] [--temp-dir DIR]\n"
      "Checks what Nesto needs from this system, without changing anything.\n"
      "  --port N        the TCP port to try for Nesto (default 6170)\n"
      "  --temp-dir DIR  where to try saving a pairing key (default /tmp); removed after\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 6170;
  std::string temp_dir = "/tmp";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--temp-dir" && i + 1 < argc) {
      temp_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }

  std::printf("nesto-probe %s\n", NESTO_VERSION);
  check_system();
  check_neon();
  check_clocks();
  check_memory();
  check_random();
  check_crypto();
  check_key_store(temp_dir);
  check_tcp(port);
  check_mdns();
  check_framebuffer();
  check_input();
  std::printf("%d check(s) failed\n", failures);
  return std::min(failures, 100);
}
