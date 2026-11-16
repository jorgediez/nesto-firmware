#include "net/Command.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

extern char** environ;

namespace nesto::net {

ProcessRunner::~ProcessRunner() {
  stop();
}

bool ProcessRunner::start(const std::vector<std::string>& argv, Millis timeout, Millis now) {
  if (running() || argv.empty()) return false;
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return false;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  // Errors too: dbus-send says what went wrong there.
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = -1;
  const int error = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_fds[1]);
  if (error != 0) {
    close(pipe_fds[0]);
    return false;
  }
  fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL) | O_NONBLOCK);
  fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
  pid_ = pid;
  fd_ = pipe_fds[0];
  deadline_ = now + timeout;
  output_.clear();
  return true;
}

void ProcessRunner::read_available() {
  if (fd_ < 0) return;
  char buffer[4096];
  for (;;) {
    const ssize_t n = read(fd_, buffer, sizeof buffer);
    if (n > 0) {
      output_.append(buffer, static_cast<std::size_t>(n));
    } else {
      if (n == 0 || errno != EINTR) break;
    }
  }
}

std::optional<CommandResult> ProcessRunner::poll(Millis now) {
  if (!running()) return std::nullopt;
  read_available();
  int status = 0;
  const pid_t done = waitpid(pid_, &status, WNOHANG);
  if (done == 0) {
    if (now < deadline_) return std::nullopt;
    stop();  // too long: killed, and a failure
    return CommandResult{false, std::move(output_)};
  }
  read_available();
  close(fd_);
  fd_ = -1;
  pid_ = -1;
  const bool ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return CommandResult{ok, std::move(output_)};
}

void ProcessRunner::stop() {
  if (pid_ > 0) {
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
  }
  if (fd_ >= 0) close(fd_);
  pid_ = -1;
  fd_ = -1;
}

}  // namespace nesto::net
