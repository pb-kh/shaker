#include "shaker/instance_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace shaker {

std::filesystem::path pid_file_path()
{
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] == '/') {
        return std::filesystem::path(runtime) / "shaker.pid";
    }
    return std::filesystem::path("/tmp")
           / ("shaker-" + std::to_string(::getuid()) + ".pid");
}

std::expected<InstanceLock, std::string>
acquire_instance_lock(const std::filesystem::path& path)
{
    // O_CLOEXEC keeps the lock out of spawned commands: a long-running
    // child must not keep the lock alive after the daemon has exited.
    UniqueFd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
    if (!fd.valid()) {
        return std::unexpected("cannot open '" + path.string() + "': "
                               + std::strerror(errno));
    }

    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK) {
            return std::unexpected("cannot lock '" + path.string() + "': "
                                   + std::strerror(errno));
        }
        std::string pid;
        std::getline(std::ifstream(path), pid);
        if (pid.empty()) {
            return std::unexpected("another shaker instance is already running");
        }
        return std::unexpected("another shaker instance is already running (pid "
                               + pid + ")");
    }

    const std::string text = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd.get(), 0) != 0
        || ::write(fd.get(), text.data(), text.size())
               != static_cast<ssize_t>(text.size())) {
        return std::unexpected("cannot write '" + path.string() + "': "
                               + std::strerror(errno));
    }

    return InstanceLock(std::move(fd));
}

} // namespace shaker
