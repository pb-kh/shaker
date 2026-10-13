// Single-instance guard: at most one shaker daemon per user session.
//
// The lock is an flock(2) on $XDG_RUNTIME_DIR/shaker.pid, and the file holds
// the running daemon's PID. The kernel releases the lock when the process
// exits (even on a crash or SIGKILL), so a leftover file never blocks a new
// start. XDG_RUNTIME_DIR is cleared at logout, so the file resets on re-login.
#pragma once

#include <shaker/fd.hpp>

#include <expected>
#include <filesystem>
#include <string>

namespace shaker {

// $XDG_RUNTIME_DIR/shaker.pid, or /tmp/shaker-<uid>.pid when the variable is
// unset, empty, or not absolute.
[[nodiscard]] std::filesystem::path pid_file_path();

// Holds the lock for its whole lifetime; released on destruction.
class InstanceLock {
public:
    explicit InstanceLock(UniqueFd fd) : fd_(std::move(fd)) {}

private:
    UniqueFd fd_;
};

// Takes the lock and writes this process's PID into the file. Returns a
// single-line detail (without the `shaker: ` prefix) on failure, naming the
// PID of the running instance when there is one.
[[nodiscard]] std::expected<InstanceLock, std::string>
acquire_instance_lock(const std::filesystem::path& path);

} // namespace shaker
