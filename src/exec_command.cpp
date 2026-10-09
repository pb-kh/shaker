#include "shaker/exec_command.hpp"

#include <signal.h>
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

void spawn_detached_shell_command(const std::string_view command)
{
    std::fflush(nullptr); // don't duplicate buffered output into the child

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "shaker: spawn failed: %s\n", std::strerror(errno));
        return;
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

    // Parent: nothing to do — SIGCHLD is ignored, so the child is reaped
    // automatically and the daemon continues detecting without waiting.
}

} // namespace shaker
