#include "prx/libSceVideoOut/include/LoadingWindow.hpp"
#include "prx/libSceAgcDriver/Execution/include/LoadingProgress.hpp"
#include "SDL_syswm.h"
#include <fstream>
#include <memory>

namespace {

struct Bounds { int x = 0, y = 0, width = 0, height = 0; bool focused = false; };

#if defined(SDL_VIDEO_DRIVER_X11)
class X11Window {
public:
    X11Window() {
        library = SDL_LoadObject("libX11.so.6");
        if (library == nullptr) return;
        open = reinterpret_cast<decltype(open)>(SDL_LoadFunction(library, "XOpenDisplay"));
        close = reinterpret_cast<decltype(close)>(SDL_LoadFunction(library, "XCloseDisplay"));
        attributes = reinterpret_cast<decltype(attributes)>(SDL_LoadFunction(library, "XGetWindowAttributes"));
        translate = reinterpret_cast<decltype(translate)>(SDL_LoadFunction(library, "XTranslateCoordinates"));
        focus = reinterpret_cast<decltype(focus)>(SDL_LoadFunction(library, "XGetInputFocus"));
        if (open && close && attributes && translate && focus) display = open(nullptr);
    }
    ~X11Window() {
        if (display) close(display);
        if (library) SDL_UnloadObject(library);
    }
    Bounds Read(Window window) {
        Bounds result;
        if (!display) return result;
        XWindowAttributes info{};
        if (!attributes(display, window, &info)) return result;
        Window child = 0, focused = 0;
        int revert = 0;
        translate(display, window, info.root, 0, 0, &result.x, &result.y, &child);
        focus(display, &focused, &revert);
        result.width = info.width;
        result.height = info.height;
        result.focused = focused == window && info.map_state == IsViewable;
        return result;
    }
private:
    void* library = nullptr;
    Display* display = nullptr;
    Display* (*open)(const char*) = nullptr;
    int (*close)(Display*) = nullptr;
    int (*attributes)(Display*, Window, XWindowAttributes*) = nullptr;
    Bool (*translate)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
    int (*focus)(Display*, Window*, int*) = nullptr;
};
#endif

}

void LoadingWindow::Start(SDL_Window* window) {
    Stop();
    frames.store(0, std::memory_order_relaxed);
    const auto path = AnyPS5::Loading::StatusPath(".window");
    if (path.empty()) return;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    worker = std::jthread([this, info, path](std::stop_token stop) {
#if defined(SDL_VIDEO_DRIVER_X11)
        X11Window x11;
#endif
        while (!stop.stop_requested()) {
            Bounds bounds;
#ifdef _WIN32
            if (info.subsystem == SDL_SYSWM_WINDOWS) {
                const auto handle = info.info.win.window;
                RECT rect{};
                POINT origin{};
                if (GetClientRect(handle, &rect) && ClientToScreen(handle, &origin)) {
                    bounds = {origin.x, origin.y, rect.right, rect.bottom,
                              GetForegroundWindow() == handle && IsWindowVisible(handle) && !IsIconic(handle)};
                }
            }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
            if (info.subsystem == SDL_SYSWM_X11) bounds = x11.Read(info.info.x11.window);
#endif
            try {
                std::ofstream output(path, std::ios::trunc);
                output << "{\"version\":1,\"updatedAt\":" << AnyPS5::Loading::Timestamp()
                       << ",\"frames\":" << frames.load(std::memory_order_relaxed)
                       << ",\"focused\":" << (bounds.focused ? "true" : "false")
                       << ",\"x\":" << bounds.x << ",\"y\":" << bounds.y
                       << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height << "}";
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
}
