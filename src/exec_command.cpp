#include "shaker/exec_command.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace shaker {

void ignore_sigchld()
{
    ::signal(SIGCHLD, SIG_IGN);
}

UniqueFd spawn_detached_shell_command(const std::string_view command)
{
    std::fflush(nullptr); // don't duplicate buffered output into the child

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "shaker: spawn failed: %s\n", std::strerror(errno));
        return {};
    }

    if (pid == 0) {
        // Child: exec the shell immediately. If the shell itself cannot be
        // launched, report and _exit(127); the shell reports failing
        // commands by itself on the inherited stderr.
        char argv0[] = "sh";
        char argv_opt[] = "-c";
        std::string cmd(command);
        char* argv[] = {argv0, argv_opt, cmd.data(), nullptr};

        ::execvp("/bin/sh", argv);

        std::fprintf(stderr, "shaker: spawn failed: %s\n", std::strerror(errno));
        ::_exit(127);
    }

    // Parent: SIGCHLD is ignored, so the child is reaped automatically and
    // the daemon continues detecting without waiting. The pidfd only lets
    // us observe whether it is still alive. ESRCH means the child already
    // exited (and was reaped) — an invalid fd reads as "not running".
    return UniqueFd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
}

bool command_running(const UniqueFd& pidfd)
{
    if (!pidfd.valid()) {
        return false;
    }
    // A pidfd becomes readable once its process has terminated.
    pollfd pfd{.fd = pidfd.get(), .events = POLLIN, .revents = 0};
    return ::poll(&pfd, 1, 0) == 0;
}

} // namespace shaker
