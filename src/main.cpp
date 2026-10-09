#include <libinput.h>
#include <libudev.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <shaker/version.hpp>

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

// Minimal RAII wrappers so we don't leak libinput / udev objects.
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

const char* event_type_name(libinput_event_type type)
{
    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED: return "DEVICE_ADDED";
    case LIBINPUT_EVENT_DEVICE_REMOVED: return "DEVICE_REMOVED";
    case LIBINPUT_EVENT_KEYBOARD_KEY: return "KEYBOARD_KEY";
    case LIBINPUT_EVENT_POINTER_MOTION: return "POINTER_MOTION";
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: return "POINTER_MOTION_ABSOLUTE";
    case LIBINPUT_EVENT_POINTER_BUTTON: return "POINTER_BUTTON";
    case LIBINPUT_EVENT_POINTER_AXIS: return "POINTER_AXIS";
    case LIBINPUT_EVENT_TOUCH_DOWN: return "TOUCH_DOWN";
    case LIBINPUT_EVENT_TOUCH_UP: return "TOUCH_UP";
    case LIBINPUT_EVENT_TOUCH_MOTION: return "TOUCH_MOTION";
    case LIBINPUT_EVENT_TOUCH_FRAME: return "TOUCH_FRAME";
    default: return "OTHER";
    }
}

void handle_event(libinput_event* ev)
{
    const auto type = libinput_event_get_type(ev);

    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
        auto* dev = libinput_event_get_device(ev);
        std::printf("[+] device added: %s (%s)\n",
                    libinput_device_get_name(dev),
                    libinput_device_get_sysname(dev));
        // Take an FD reference so the device stays enumerable while we hold it.
        libinput_device_ref(dev);
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
        auto* dev = libinput_event_get_device(ev);
        std::printf("[-] device removed: %s\n", libinput_device_get_name(dev));
        libinput_device_unref(dev);
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto* k = libinput_event_get_keyboard_event(ev);
        std::printf("key %s code=%u seat=%s\n",
                    libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED
                        ? "down"
                        : "up",
                    libinput_event_keyboard_get_key(k),
                    libinput_seat_get_logical_name(libinput_device_get_seat(libinput_event_get_device(ev))));
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* p = libinput_event_get_pointer_event(ev);
        std::printf("motion dx=%.2f dy=%.2f\n",
                    libinput_event_pointer_get_dx(p),
                    libinput_event_pointer_get_dy(p));
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* p = libinput_event_get_pointer_event(ev);
        std::printf("button %s button=%u\n",
                    libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED
                        ? "down"
                        : "up",
                    libinput_event_pointer_get_button(p));
        break;
    }
    default:
        std::printf("event: %s\n", event_type_name(type));
        break;
    }
    std::fflush(stdout);
}

int run()
{
    UdevCtx udev;
    if (!udev) {
        std::fprintf(stderr, "failed to create udev context: %s\n", std::strerror(errno));
        return 1;
    }

    // Takes its own reference to the udev context.
    LibinputCtx li(libinput_udev_create_context(&device_interface, nullptr, udev.handle));
    if (!li) {
        std::fprintf(stderr, "failed to create libinput context: %s\n", std::strerror(errno));
        return 1;
    }

    if (libinput_udev_assign_seat(li.handle, "seat0") != 0) {
        std::fprintf(stderr, "failed to assign seat \"seat0\": %s\n", std::strerror(errno));
        return 1;
    }

    std::printf("shaker: listening on seat0 (Ctrl+C to quit). Root permissions may be required.\n");

    for (;;) {
        const int status = libinput_dispatch(li.handle);
        if (status != 0) {
            std::fprintf(stderr, "libinput_dispatch failed: %s\n", std::strerror(-status));
            return 1;
        }

        while (libinput_event* ev = libinput_get_event(li.handle)) {
            handle_event(ev);
            libinput_event_destroy(ev);
        }
    }
}

} // namespace

int main()
{
    return run();
}
