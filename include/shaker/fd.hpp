// RAII ownership of a POSIX file descriptor (constitution IV: no paired
// manual close; cleanup belongs in destructors).
//
// Non-copyable, movable: moving leaves the source fd inert so ownership is
// never duplicated.
#pragma once

#include <unistd.h>

#include <utility>

namespace shaker {

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~UniqueFd() { reset(); }

    void reset(int fd = -1)
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

    // For poll()-style APIs that want -1 for "not present".
    [[nodiscard]] int release() { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

} // namespace shaker
