// shaker — shake-triggered command daemon (US1 wired).
//
// Flow (contracts/runtime-behavior.md): CLI → config load (fatal on error,
// before any device is opened) → libinput/udev on seat0 → pointer-motion
// samples → pure DetectionCore → on Trigger: one stdout line + detached
// command spawn (never waited, auto-reaped). SIGINT/SIGTERM exit 0 via a
// signalfd; the loop blocks in poll(2) (idle ≈ 0% CPU).
#include <libinput.h>
#include <libudev.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string_view>

#include <shaker/config.hpp>
#include <shaker/detection.hpp>
#include <shaker/exec_command.hpp>
#include <shaker/fd.hpp>
#include <shaker/instance_lock.hpp>
#include <shaker/version.hpp>

#include <string>
#include <vector>

namespace {

// libinput calls these to open/close evdev device nodes. As a plain
// (non-compositor) client we just open() them directly, which typically
// requires root or membership in an appropriate group.
int open_restricted(const char* path, int flags, void* /*user_data*/)
{
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open_restricted");
    }
    return fd;
}

void close_restricted(int fd, void* /*user_data*/)
{
    ::close(fd);
}

constexpr libinput_interface device_interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

// Minimal RAII wrappers so we don't leak libinput / udev objects
// (constitution IV).
struct UdevCtx {
    udev* handle;

    UdevCtx() : handle(udev_new()) {}
    UdevCtx(const UdevCtx&) = delete;
    UdevCtx& operator=(const UdevCtx&) = delete;
    ~UdevCtx()
    {
        if (handle) {
            udev_unref(handle);
        }
    }
    explicit operator bool() const { return handle != nullptr; }
};

struct LibinputCtx {
    libinput* handle;

    explicit LibinputCtx(libinput* li) : handle(li) {}
    LibinputCtx(const LibinputCtx&) = delete;
    LibinputCtx& operator=(const LibinputCtx&) = delete;
    ~LibinputCtx()
    {
        if (handle) {
            libinput_unref(handle);
        }
    }
    explicit operator bool() const { return handle != nullptr; }
};

// ---- Command line ----------------------------------------------------------

constexpr std::string_view kUsageText =
    "usage: shaker [FLAG]\n"
    "\n"
    "Detect shakes of your pointer devices and run a configured command.\n"
    "\n"
    "Flags:\n"
    "  --debug      verbose detection trace on stderr (calibration mode)\n"
    "  --device,-d  only watch devices matching this name/sysname substring\n"
    "  --list-devices,-l list detected pointer devices and exit\n"
    "  --help       print this usage and exit 0\n"
    "  --version    print version and exit 0\n"
    "\n"
    "Configured from:\n"
    "  $XDG_CONFIG_HOME/shaker/config.toml\n"
    "  (defaults to ~/.config/shaker/config.toml)\n"
    "  Also accepts 'device' key in config; CLI --device overrides config.\n";

struct Options {
    bool debug = false;
    bool list_devices = false;
    std::string device_filter;
};

enum class ParseResult { Continue, StopExit };

ParseResult parse_args(const int argc, const char* const* argv, Options& options,
                       int& exit_code)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            std::printf("%s", kUsageText.data());
            std::fflush(stdout);
            exit_code = 0;
            return ParseResult::StopExit;
        }
        if (arg == "--version") {
            std::printf("shaker %s\n", shaker::project_version.data());
            std::fflush(stdout);
            exit_code = 0;
            return ParseResult::StopExit;
        }
        if (arg == "--debug") {
            options.debug = true;
            continue;
        }
        if (arg == "--device" || arg == "-d") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "shaker: flag '%.*s' requires an argument\n",
                             static_cast<int>(arg.size()), arg.data());
                exit_code = 2;
                return ParseResult::StopExit;
            }
            options.device_filter = argv[++i];
            continue;
        }
        // also support --device=<value>
        if (arg.starts_with("--device=")) {
            const size_t eq = arg.find('=');
            if (eq + 1 < arg.size()) {
                options.device_filter = std::string(arg.substr(eq + 1));
            } else {
                std::fprintf(stderr, "shaker: flag '--device' requires an argument\n");
                exit_code = 2;
                return ParseResult::StopExit;
            }
            continue;
        }
        if (arg == "--list-devices" || arg == "-l") {
            options.list_devices = true;
            continue;
        }
        std::fprintf(stderr, "shaker: unrecognized flag '%.*s'\n",
                     static_cast<int>(arg.size()), arg.data());
        std::fprintf(stderr, "usage: shaker [--debug] [--device NAME]\n");
        exit_code = 2;
        return ParseResult::StopExit;
    }
    return ParseResult::Continue;
}

// ---- Device events and the detection pipeline ------------------------------

struct Pipeline {
    shaker::DetectionCore core;
    std::string command;
    std::string device_filter;
    bool debug = false;
    int64_t last_sample_t_usec = -1;
    int64_t reference_t_usec = -1;
    int buttons_held = 0; // seat-wide pressed pointer buttons
    shaker::UniqueFd running_command; // pidfd of the last spawned command
};

// Case-insensitive substring search: does `hay` contain `needle`?
[[nodiscard]] bool contains_ci(std::string_view hay, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    if (hay.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return true;
        }
    }
    return false;
}

// Does this device's name or sysname match the configured filter (if any)?
[[nodiscard]] bool device_matches_filter(libinput_device* dev,
                                         std::string_view filter)
{
    if (filter.empty()) {
        return true;
    }
    return contains_ci(libinput_device_get_name(dev), filter)
           || contains_ci(libinput_device_get_sysname(dev), filter);
}

void handle_device_event(Pipeline& pipeline, libinput_event* ev)
{
    switch (libinput_event_get_type(ev)) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
        auto* dev = libinput_event_get_device(ev);
        const bool matches = device_matches_filter(dev, pipeline.device_filter);
        if (pipeline.device_filter.empty()) {
            std::printf("[+] device added: %s (%s)\n",
                        libinput_device_get_name(dev),
                        libinput_device_get_sysname(dev));
        } else {
            std::printf("[+] device added: %s (%s) — %s\n",
                        libinput_device_get_name(dev),
                        libinput_device_get_sysname(dev),
                        matches ? "matches filter" : "not in filter");
        }
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
        auto* dev = libinput_event_get_device(ev);
        std::printf("[-] device removed: %s\n", libinput_device_get_name(dev));
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* dev = libinput_event_get_device(ev);
        if (!device_matches_filter(dev, pipeline.device_filter)) {
            // A device filter is configured and this device is not it.
            break;
        }
        auto* pointer = libinput_event_get_pointer_event(ev);
        const int64_t t_usec =
            static_cast<int64_t>(libinput_event_pointer_get_time_usec(pointer));
        const double dx = libinput_event_pointer_get_dx(pointer);
        const double dy = libinput_event_pointer_get_dy(pointer);

        if (pipeline.last_sample_t_usec >= 0 && t_usec < pipeline.last_sample_t_usec) {
            // libinput's queue is monotonic, but a clock regression from a
            // device must not corrupt detection (contract §2.2).
            std::fprintf(stderr,
                         "shaker: non-monotonic timestamps from device; "
                         "sample dropped\n");
            break;
        }
        pipeline.last_sample_t_usec = t_usec;

        if (pipeline.buttons_held > 0) {
            // Dragging (text selection, window moves) is never a shake.
            pipeline.core.reset();
            break;
        }

        const shaker::DetectionResult result =
            pipeline.core.feed(shaker::Sample{t_usec, dx, dy});

        if (pipeline.debug) {
            // Calibration trace (constitution VI); adds visibility only
            // (contract §2.2). Timeline is relative to the first sample.
            if (pipeline.reference_t_usec < 0) {
                pipeline.reference_t_usec = t_usec;
            }
            const double elapsed =
                static_cast<double>(t_usec - pipeline.reference_t_usec) / 1e6;
            std::fprintf(stderr,
                         "debug: t=+%.3fs speed=%.0f segments=%zu factor=%.2f "
                         "state=%s\n",
                         elapsed, pipeline.core.last_speed(),
                         pipeline.core.segments(), pipeline.core.shake_factor(),
                         shaker::state_name(pipeline.core.state()).data());
            if (pipeline.core.state() == shaker::DetectionState::Sustaining) {
                std::fprintf(stderr, "debug: window=+%.3fs\n",
                             static_cast<double>(pipeline.core.window_elapsed_usec())
                                 / 1e6);
            }
        }

        if (result.triggered) {
            if (pipeline.debug) {
                std::fprintf(stderr, "debug: TRIGGER at t=+%.3fs peak=%.0f\n",
                             static_cast<double>(t_usec - pipeline.reference_t_usec)
                                 / 1e6,
                             result.peak_speed);
            }
            if (shaker::command_running(pipeline.running_command)) {
                // Never stack a second instance on top of a still-running
                // one (e.g. a region picker that is still open).
                std::printf("shake! speed=%.0f units/s → skipped: previous "
                            "command still running\n",
                            result.peak_speed);
                std::fflush(stdout);
                break;
            }
            std::printf("shake! speed=%.0f units/s → running: %s\n",
                        result.peak_speed,
                        [&] {
                            const std::string_view cmd = pipeline.command;
                            const size_t nl = cmd.find('\n');
                            if (nl == std::string_view::npos) {
                                return std::string(cmd);
                            }
                            // Multi-line scripts: first line only (the rest
                            // would flood the observation stream).
                            return std::string(cmd.substr(0, nl)) + " …";
                        }()
                            .c_str());
            std::fflush(stdout);
            pipeline.running_command =
                shaker::spawn_detached_shell_command(pipeline.command);
        }
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* pointer = libinput_event_get_pointer_event(ev);
        if (libinput_event_pointer_get_button_state(pointer)
            == LIBINPUT_BUTTON_STATE_PRESSED) {
            ++pipeline.buttons_held;
        } else if (pipeline.buttons_held > 0) {
            --pipeline.buttons_held;
        }
        break;
    }
    default:
        // Keyboard, axis, touch, gestures … are not shake inputs;
        // every pointer-capable device is covered by the seat model
        // (FR-015) and consumption is silent (contract §2.1).
        break;
    }
    std::fflush(stdout);
}

// Block the terminate signals and hand deliver them to a signalfd so the
// loop reacts to them as poll data (no async signal-handler side effects)
// — contracts/runtime-behavior.md §6. SIGHUP is blocked too but ignored in
// the loop (reserved for a future config reload).
[[nodiscard]] int create_signal_fd(shaker::UniqueFd& out)
{
    sigset_t mask;
    ::sigemptyset(&mask);
    for (const int signo : {SIGINT, SIGTERM, SIGHUP}) {
        ::sigaddset(&mask, signo);
    }
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return -errno;
    }
    out.reset(::signalfd(-1, &mask, SFD_CLOEXEC));
    if (!out.valid()) {
        return -errno;
    }
    return 0;
}

// Runs until the daemon wants to exit; returns the process exit code.
[[nodiscard]] int event_loop(libinput* li, int signal_fd, Pipeline& pipeline)
{
    pollfd fds[2] = {
        {.fd = libinput_get_fd(li), .events = POLLIN, .revents = 0},
        {.fd = signal_fd, .events = POLLIN, .revents = 0},
    };

    for (;;) {
        int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "shaker: poll failed: %s\n", std::strerror(errno));
            return 1;
        }

        if ((fds[1].revents & POLLIN) != 0) {
            signalfd_siginfo info{};
            const ssize_t n = ::read(fds[1].fd, &info, sizeof(info));
            if (n == static_cast<ssize_t>(sizeof(info))
                && (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM)) {
                std::fprintf(stderr, "shaker: exiting on signal\n");
                return 0;
            }
            // Anything else (e.g. SIGHUP in v1): ignore and keep running.
        }

        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }

        const int status = libinput_dispatch(li);
        if (status != 0) {
            std::fprintf(stderr, "shaker: libinput_dispatch failed: %s\n", std::strerror(-status));
            return 1;
        }

        while (libinput_event* ev = libinput_get_event(li)) {
            handle_device_event(pipeline, ev);
            libinput_event_destroy(ev);
        }
    }
}

// Print every pointer-capable device libinput exposes on seat0 and exit.
// The filter argument (may be empty) marks which devices the daemon would
// actually watch with the given `device` setting.
[[nodiscard]] int list_devices(const std::string& filter)
{
    UdevCtx udev;
    if (!udev) {
        std::fprintf(stderr, "failed to create udev context: %s\n", std::strerror(errno));
        return 1;
    }

    LibinputCtx li(libinput_udev_create_context(&device_interface, nullptr, udev.handle));
    if (!li) {
        std::fprintf(stderr, "failed to create libinput context: %s\n", std::strerror(errno));
        return 1;
    }
    if (libinput_udev_assign_seat(li.handle, "seat0") != 0) {
        std::fprintf(stderr, "failed to assign seat \"seat0\": %s\n", std::strerror(errno));
        return 1;
    }

    if (!filter.empty()) {
        std::printf("device filter: \"%s\" (case-insensitive substring of "
                    "name or sysname)\n\n",
                    filter.c_str());
    }

    int pointer_devices = 0;

    // Device-added events are queued during assign_seat; dispatch until the
    // queue drains (a short poll timeout catches the rest).
    for (int round = 0; round < 50; ++round) {
        if (libinput_dispatch(li.handle) != 0) {
            break;
        }
        bool saw_event = false;
        while (libinput_event* ev = libinput_get_event(li.handle)) {
            saw_event = true;
            if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
                auto* dev = libinput_event_get_device(ev);
                const bool pointer =
                    libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER) != 0;
                const bool gesture =
                    libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_GESTURE) != 0;
                if (pointer || gesture) {
                    ++pointer_devices;
                    std::printf("%s %s (%s)\n",
                                device_matches_filter(dev, filter) ? "[*]" : "[ ]",
                                libinput_device_get_name(dev),
                                libinput_device_get_sysname(dev));
                }
            }
            libinput_event_destroy(ev);
        }
        if (!saw_event) {
            // Give the udev backend a moment, then stop when still idle.
            pollfd pfd{.fd = libinput_get_fd(li.handle), .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 100) <= 0) {
                break;
            }
        }
    }

    std::printf("\n%u pointer-capable device(s) on seat0\n",
                static_cast<unsigned>(pointer_devices));
    if (!filter.empty()) {
        std::printf("[*] = would be watched with the current device filter\n");
    }
    return 0;
}

int run(const Options& options)
{
    // Reserve the SIGHUP slot; spawned children are reaped automatically
    // (contracts/runtime-behavior.md §5).
    shaker::ignore_sigchld();

    // Configuration first: an invalid config must never reach libinput
    // (FR-010/FR-011, US3 scenarios). Unknown keys warn on stderr but are
    // non-fatal (contract §2.2); the three numeric keys fall back to their
    // documented defaults (FR-012).
    std::vector<std::string> warnings;
    const shaker::Config config = [&] {
        const auto loaded = shaker::load_config(&warnings);
        if (!loaded.has_value()) {
            // Contract §3 prints exactly `shaker: ...` + exits 1, before
            // any device is opened.
            std::fprintf(stderr, "shaker: %s\n", loaded.error().detail.c_str());
            std::exit(1);
        }
        for (const std::string& warning : warnings) {
            std::fprintf(stderr, "shaker: %s\n", warning.c_str());
        }
        return *loaded;
    }();

    std::string device_filter = config.device;
    if (!options.device_filter.empty()) {
        device_filter = options.device_filter;
    }

    if (options.list_devices) {
        return list_devices(device_filter);
    }

    // Held until run() returns, so a second daemon fails here instead of
    // detecting the same shake and spawning the command again.
    const auto instance = shaker::acquire_instance_lock(shaker::pid_file_path());
    if (!instance) {
        std::fprintf(stderr, "shaker: %s\n", instance.error().c_str());
        return 1;
    }

    Pipeline pipeline{
        .core = shaker::DetectionCore{shaker::to_detection_params(config)},
        .command = config.command,
        .device_filter = device_filter,
        .debug = options.debug,
        .running_command = {},
    };

    UdevCtx udev;
    if (!udev) {
        std::fprintf(stderr, "failed to create udev context: %s\n", std::strerror(errno));
        return 1;
    }

    LibinputCtx li(libinput_udev_create_context(&device_interface, nullptr, udev.handle));
    if (!li) {
        std::fprintf(stderr, "failed to create libinput context: %s\n", std::strerror(errno));
        return 1;
    }

    if (libinput_udev_assign_seat(li.handle, "seat0") != 0) {
        std::fprintf(stderr, "failed to assign seat \"seat0\": %s\n", std::strerror(errno));
        return 1;
    }

    if (pipeline.device_filter.empty()) {
        std::printf("shaker %s: watching seat0 — pid %d\n",
                    shaker::project_version.data(), ::getpid());
    } else {
        std::printf("shaker %s: watching seat0 (device filter: %s) — pid %d\n",
                    shaker::project_version.data(), pipeline.device_filter.c_str(), ::getpid());
    }
    std::fflush(stdout);

    shaker::UniqueFd signal_fd;
    if (const int rc = create_signal_fd(signal_fd); rc < 0) {
        std::fprintf(stderr, "failed to set up signal handling: %s\n", std::strerror(-rc));
        return 1;
    }

    return event_loop(li.handle, signal_fd.get(), pipeline);
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    int exit_code = 0;
    if (parse_args(argc, argv, options, exit_code) == ParseResult::StopExit) {
        return exit_code;
    }
    return run(options);
}
