#include "core/Log.hpp"

#include <cstdarg>
#include <cstdio>

namespace nesto::log {
namespace {

void write(const char* level, const char* format, std::va_list args) {
  std::fprintf(stderr, "[%s] ", level);
  std::vfprintf(stderr, format, args);
  std::fputc('\n', stderr);
}

}  // namespace

void info(const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  write("info", format, args);
  va_end(args);
}

void warn(const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  write("warn", format, args);
  va_end(args);
}

void error(const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  write("error", format, args);
  va_end(args);
}

}  // namespace nesto::log
