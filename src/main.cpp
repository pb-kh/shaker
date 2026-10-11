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
#include <string_view>

#include <shaker/config.hpp>
#include <shaker/detection.hpp>
#include <shaker/exec_command.hpp>
#include <shaker/fd.hpp>
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
    "  --help       print this usage and exit 0\n"
    "  --version    print version and exit 0\n"
    "\n"
    "Configured entirely from:\n"
    "  $XDG_CONFIG_HOME/shaker/config.toml\n"
    "  (defaults to ~/.config/shaker/config.toml)\n";

struct Options {
    bool debug = false;
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
        std::fprintf(stderr, "shaker: unrecognized flag '%.*s'\n",
                     static_cast<int>(arg.size()), arg.data());
        std::fprintf(stderr, "usage: shaker [--debug]\n");
        exit_code = 2;
        return ParseResult::StopExit;
    }
    return ParseResult::Continue;
}

// ---- Device events and the detection pipeline ------------------------------

struct Pipeline {
    shaker::DetectionCore core;
    std::string command;
    bool debug = false;
    int64_t last_sample_t_usec = -1;
    int64_t reference_t_usec = -1;
    int buttons_held = 0; // seat-wide pressed pointer buttons
    shaker::UniqueFd running_command; // pidfd of the last spawned command
};

void handle_device_event(Pipeline& pipeline, libinput_event* ev)
{
    switch (libinput_event_get_type(ev)) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
        auto* dev = libinput_event_get_device(ev);
        std::printf("[+] device added: %s (%s)\n",
                    libinput_device_get_name(dev),
                    libinput_device_get_sysname(dev));
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
        auto* dev = libinput_event_get_device(ev);
        std::printf("[-] device removed: %s\n", libinput_device_get_name(dev));
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
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

int run(const bool debug)
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

    Pipeline pipeline{
        .core = shaker::DetectionCore{shaker::to_detection_params(config)},
        .command = config.command,
        .debug = debug,
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

    std::printf("shaker %s: watching seat0 — pid %d\n",
                shaker::project_version.data(), ::getpid());
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
    return run(options.debug);
}
