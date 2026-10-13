// Unit tests for the single-instance lock: first acquire wins, a second
// acquire is refused while the first is alive, and the lock is free again
// once released.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shaker/instance_lock.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        char tmpl[] = "/tmp/shaker-test-XXXXXX";
        path = ::mkdtemp(tmpl);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("pid file path follows XDG_RUNTIME_DIR")
{
    ::setenv("XDG_RUNTIME_DIR", "/run/user/4242", 1);
    CHECK(shaker::pid_file_path() == std::filesystem::path("/run/user/4242/shaker.pid"));

    ::setenv("XDG_RUNTIME_DIR", "relative", 1);
    CHECK(shaker::pid_file_path().parent_path() == "/tmp");
}

TEST_CASE("first acquire writes its PID and a second is refused")
{
    TempDir dir;
    const auto file = dir.path / "shaker.pid";

    auto first = shaker::acquire_instance_lock(file);
    REQUIRE(first.has_value());

    std::string recorded;
    std::getline(std::ifstream(file), recorded);
    CHECK(recorded == std::to_string(::getpid()));

    auto second = shaker::acquire_instance_lock(file);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().find("already running") != std::string::npos);
    CHECK(second.error().find(recorded) != std::string::npos);
}

TEST_CASE("lock is available again after the holder releases it")
{
    TempDir dir;
    const auto file = dir.path / "shaker.pid";

    {
        auto held = shaker::acquire_instance_lock(file);
        REQUIRE(held.has_value());
    }

    auto again = shaker::acquire_instance_lock(file);
    CHECK(again.has_value());
}
