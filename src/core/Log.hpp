#pragma once

namespace nesto::log {

// printf-style messages to stderr, prefixed with the level.
void info(const char* format, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* format, ...) __attribute__((format(printf, 1, 2)));
void error(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace nesto::log
