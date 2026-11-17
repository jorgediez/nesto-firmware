#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"

namespace nesto::net {

/// What a finished command printed, and whether it succeeded.
struct CommandResult {
  bool ok = false;  // exited with 0, in time
  std::string output;  // with what it said on stderr
};

/// Runs one command at a time without waiting for it: the main loop polls,
/// so the screen and the ring keep going while connman scans or connects.
class CommandRunner {
 public:
  virtual ~CommandRunner() = default;
  /// Starts `argv`, killed if it runs longer than `timeout`. False if one is
  /// still running or it can't start.
  virtual bool start(const std::vector<std::string>& argv, Millis timeout, Millis now) = 0;
  virtual bool running() const = 0;
  /// The result, once, when the command has finished.
  virtual std::optional<CommandResult> poll(Millis now) = 0;
};

/// Runs real programs: posix_spawn, the output through a pipe read as it
/// comes, never a shell.
class ProcessRunner : public CommandRunner {
 public:
  ProcessRunner() = default;
  ~ProcessRunner() override;
  ProcessRunner(const ProcessRunner&) = delete;
  ProcessRunner& operator=(const ProcessRunner&) = delete;

  bool start(const std::vector<std::string>& argv, Millis timeout, Millis now) override;
  bool running() const override { return pid_ > 0; }
  std::optional<CommandResult> poll(Millis now) override;

 private:
  void read_available();
  void stop();

  int pid_ = -1;
  int fd_ = -1;
  Millis deadline_ = 0;
  std::string output_;
};

}  // namespace nesto::net
