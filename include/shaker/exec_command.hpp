#pragma once

#include <string_view>

#include <shaker/fd.hpp>

namespace shaker {

// Detached command execution (contracts/runtime-behavior.md §5).
//
// Must be called exactly once at startup so the parent never needs to
// wait() on spawned children (SIGCHLD = SIG_IGN → automatic reaping,
// no zombies).
void ignore_sigchld();

// Spawn the command through /bin/sh -c in a detached child; the daemon
// never waits for it and detection is never stalled (FR-016/FR-018).
// Spawn failure is a non-fatal stderr diagnostic (US1 scenario 3).
//
// Returns a pidfd for the child (invalid on spawn failure, or if the child
// already exited before it could be opened) so the caller can tell whether
// the previous command is still running.
[[nodiscard]] UniqueFd spawn_detached_shell_command(std::string_view command);

// True while the process behind `pidfd` has not exited yet. Never blocks;
// an invalid fd counts as "not running".
[[nodiscard]] bool command_running(const UniqueFd& pidfd);

} // namespace shaker
