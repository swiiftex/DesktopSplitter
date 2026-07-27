// Main.cpp - dscomp: DesktopSplitter compositor.
//
// Captures each virtual monitor with DXGI Desktop Duplication and composites
// them into their segment rects on a borderless topmost window that covers the
// physical monitor.

#include "Common.h"

#include <winioctl.h>       // CTL_CODE, needed by DeskSplitProtocol.h
#include <DeskSplitProtocol.h>

#include "Log.h"
#include "Config.h"
#include "MonitorUtil.h"
#include "Renderer.h"
#include "Capture.h"
#include "CursorConfine.h"
#include "WindowRescuer.h"
#include "SegmentShared.h"

#include <shellapi.h>
#include <memory>
#include <thread>
#include <cstdio>

namespace ds {

namespace {

constexpr wchar_t kWindowClass[] = L"DeskSplitCompositorWindow";
constexpr wchar_t kWindowTitle[] = L"DesktopSplitter Compositor";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT     kTimerPeriodMs = 2000;

struct Options {
    bool         verbose = false;
    bool         confine = true;      // --no-confine
    bool         rescue = true;       // --no-rescue
    bool         specialized = false; // --specialized / HidePhysicalDisplay=1
    std::wstring configPath;
};

// The GUI installer writes HKLM\SOFTWARE\DesktopSplitter\HidePhysicalDisplay=1
// when the user opts into truly hiding the physical monitor.
bool HidePhysicalDisplayFlag() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DesktopSplitter", 0,
                        KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD cb = sizeof(value);
    DWORD type = 0;
    const LONG r = ::RegQueryValueExW(key, L"HidePhysicalDisplay", nullptr, &type,
                                      reinterpret_cast<LPBYTE>(&value), &cb);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

struct App {
    Options                                      opt;
    AppConfig                                    config;
    HWND                                         hwnd = nullptr;
    RECT                                         physicalRect = {};
    Renderer                                     renderer;
    std::vector<SegmentShared>                   segments;
    std::vector<std::unique_ptr<SegmentCapture>> captures;
    CursorConfiner                               confiner;
    WindowRescuer                                rescuer;

    HANDLE      stopEvent = nullptr;      // DESKSPLIT_COMPOSITOR_STOP_EVENT
    HANDLE      quitWatcher = nullptr;    // signals the watcher thread to exit
    std::thread watcherThread;
};

App* g_app = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT ||
        type == CTRL_LOGOFF_EVENT) {
        if (g_app && g_app->hwnd) {
            ::PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        }
    }
    return FALSE;
}

void ApplyWindowPlacement(App& app) {
    const int x = app.physicalRect.left;
    const int y = app.physicalRect.top;
    const int w = app.physicalRect.right - app.physicalRect.left;
    const int h = app.physicalRect.bottom - app.physicalRect.top;
    ::SetWindowPos(app.hwnd, HWND_TOPMOST, x, y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OnDisplayChange(App& app) {
    RECT r = {};
    if (FindMonitorRect(app.config.physicalDevice, r)) {
        app.physicalRect = r;
        ApplyWindowPlacement(app);
        const UINT w = static_cast<UINT>(r.right - r.left);
        const UINT h = static_cast<UINT>(r.bottom - r.top);
        app.renderer.RequestResize(w, h);
        DS_LOG(L"display change: physical monitor is now (%ld,%ld)-(%ld,%ld)",
               r.left, r.top, r.right, r.bottom);
    } else {
        DS_WARN(L"display change: physical device '%s' is gone",
                app.config.physicalDevice.c_str());
    }
    // Making a virtual monitor primary re-bases virtual-desktop coordinates, so
    // every cached rect (including negative-coordinate ones) has to be redone.
    if (app.opt.confine) app.confiner.Refresh();
    if (app.opt.rescue) app.rescuer.OnDisplayChange();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* app = g_app;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        ::SetCursor(nullptr);   // the composited cursor is drawn by the renderer
        return TRUE;

    case WM_ERASEBKGND:
        return 1;

    case WM_DISPLAYCHANGE:
        if (app) OnDisplayChange(*app);
        return 0;

    case WM_TIMER:
        if (wParam == kTimerId && app) {
            if (app->opt.confine) app->confiner.Reapply();
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_CLOSE:
        DS_LOG(L"WM_CLOSE received, shutting down");
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool CreateCompositorWindow(App& app, HINSTANCE hinst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = nullptr;
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc)) {
        DS_ERR(L"RegisterClassEx failed: %s",
               HrString(HRESULT_FROM_WIN32(::GetLastError())));
        return false;
    }

    const int x = app.physicalRect.left;
    const int y = app.physicalRect.top;
    const int w = app.physicalRect.right - app.physicalRect.left;
    const int h = app.physicalRect.bottom - app.physicalRect.top;

    app.hwnd = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kWindowClass, kWindowTitle, WS_POPUP,
        x, y, w, h, nullptr, nullptr, hinst, nullptr);
    if (!app.hwnd) {
        DS_ERR(L"CreateWindowEx failed: %s",
               HrString(HRESULT_FROM_WIN32(::GetLastError())));
        return false;
    }

    ::ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
    ApplyWindowPlacement(app);
    DS_LOG(L"window created at (%d,%d) %dx%d", x, y, w, h);
    return true;
}

HANDLE OpenOrCreateStopEvent() {
    HANDLE h = ::OpenEventW(SYNCHRONIZE, FALSE, DESKSPLIT_COMPOSITOR_STOP_EVENT);
    if (h) {
        DS_VERB(L"opened existing stop event");
        return h;
    }
    h = ::CreateEventW(nullptr, TRUE, FALSE, DESKSPLIT_COMPOSITOR_STOP_EVENT);
    if (h) {
        DS_VERB(L"created stop event");
        return h;
    }
    DS_WARN(L"cannot open or create '%s': %s (use WM_CLOSE or Ctrl+C to stop)",
            DESKSPLIT_COMPOSITOR_STOP_EVENT,
            HrString(HRESULT_FROM_WIN32(::GetLastError())));
    return nullptr;
}

void StartStopWatcher(App& app) {
    app.stopEvent = OpenOrCreateStopEvent();
    if (!app.stopEvent) return;

    app.quitWatcher = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!app.quitWatcher) return;

    App* a = &app;
    app.watcherThread = std::thread([a] {
        ::SetThreadDescription(::GetCurrentThread(), L"dscomp-stopwatch");
        HANDLE handles[2] = { a->stopEvent, a->quitWatcher };
        const DWORD r = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            DS_LOG(L"stop event signalled");
            if (a->hwnd) ::PostMessageW(a->hwnd, WM_CLOSE, 0, 0);
        }
    });
}

void StopWatcher(App& app) {
    if (app.quitWatcher) ::SetEvent(app.quitWatcher);
    if (app.watcherThread.joinable()) app.watcherThread.join();
    if (app.quitWatcher) { ::CloseHandle(app.quitWatcher); app.quitWatcher = nullptr; }
    if (app.stopEvent)   { ::CloseHandle(app.stopEvent);   app.stopEvent = nullptr; }
}

void PrintUsage() {
    ::fwprintf(stderr,
               L"dscomp - DesktopSplitter compositor\n"
               L"\n"
               L"Usage: dscomp.exe [--verbose] [--config <path>] [--no-confine]\n"
               L"                  [--no-rescue] [--specialized]\n"
               L"\n"
               L"  --verbose, -v    Verbose logging to stderr / OutputDebugString.\n"
               L"  --config <path>  Override %%ProgramData%%\\DesktopSplitter\\config.json.\n"
               L"  --no-confine     Do not keep the cursor off the covered monitor.\n"
               L"  --no-rescue      Do not relocate windows that open behind the\n"
               L"                   compositor onto the first virtual monitor.\n"
               L"  --specialized    Drive the physical monitor directly through\n"
               L"                   Windows.Devices.Display.Core, removing it from the\n"
               L"                   desktop entirely. Needs the monitor marked as a\n"
               L"                   specialized display (see docs/HIDING.md); falls back\n"
               L"                   to the window path otherwise. Also enabled by HKLM\\\n"
               L"                   SOFTWARE\\DesktopSplitter HidePhysicalDisplay=1.\n"
               L"  --help, -h       Show this message.\n"
               L"\n"
               L"Stops on the named event %s, WM_CLOSE, or Ctrl+C.\n",
               DESKSPLIT_COMPOSITOR_STOP_EVENT);
}

int Run(HINSTANCE hinst, const Options& opt) {
    LogInit(opt.verbose);
    DS_LOG(L"dscomp starting");

    App app;
    app.opt = opt;
    g_app = &app;

    if (!LoadConfig(opt.configPath, app.config)) {
        DS_ERR(L"failed to load config from '%s'", opt.configPath.c_str());
        return 2;
    }

    if (!FindMonitorRect(app.config.physicalDevice, app.physicalRect)) {
        DS_ERR(L"physical monitor '%s' not found", app.config.physicalDevice.c_str());
        return 3;
    }
    DS_LOG(L"physical monitor '%s' at (%ld,%ld)-(%ld,%ld)",
           app.config.physicalDevice.c_str(), app.physicalRect.left,
           app.physicalRect.top, app.physicalRect.right, app.physicalRect.bottom);

    if (!CreateCompositorWindow(app, hinst)) return 4;

    app.segments = std::vector<SegmentShared>(app.config.segments.size());
    if (!app.renderer.Init(app.hwnd, app.config, &app.segments, opt.specialized)) {
        DS_ERR(L"renderer initialisation failed");
        ::DestroyWindow(app.hwnd);
        return 5;
    }

    for (size_t i = 0; i < app.config.segments.size(); ++i) {
        auto cap = std::make_unique<SegmentCapture>();
        if (!cap->Start(i, app.config.segments[i], app.renderer.Gpu(),
                        &app.segments[i])) {
            DS_ERR(L"failed to start capture for segment %zu", i);
        } else {
            app.captures.push_back(std::move(cap));
        }
    }

    // In specialized mode the physical monitor is not part of the desktop at
    // all, so there is nothing to confine the cursor away from and no window can
    // land behind the compositor. Both helpers are pointless there.
    const bool specializedActive =
        app.renderer.Selection().kind == PresenterKind::Specialized;
    if (specializedActive) {
        DS_LOG(L"specialized presentation active: skipping cursor confinement "
               L"and the window rescuer (the display is out of the desktop)");
    }

    if (app.opt.confine && !specializedActive) {
        app.confiner.Configure(app.config.physicalDevice);
        if (!app.confiner.InstallHook()) {
            // Exclusion is hook-only, so no hook means no confinement. Carry on
            // rather than degrading cursor movement with a bad ClipCursor.
            DS_WARN(L"running without cursor confinement");
        }
    } else if (!specializedActive) {
        DS_LOG(L"cursor confinement disabled (--no-confine)");
    }

    if (app.opt.rescue && !specializedActive) {
        // Segment 1 is the relocation target: config segments[0].
        app.rescuer.Start(app.config.physicalDevice,
                          app.config.segments[0].virtualDevice);
    } else if (!specializedActive) {
        DS_LOG(L"window rescuer disabled (--no-rescue)");
    }

    ::SetTimer(app.hwnd, kTimerId, kTimerPeriodMs, nullptr);
    StartStopWatcher(app);
    app.renderer.StartThread();
    DS_LOG(L"running");

    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    DS_LOG(L"shutting down");
    ::KillTimer(app.hwnd, kTimerId);
    app.rescuer.Stop();
    app.renderer.StopThread();
    for (auto& c : app.captures) c->Stop();
    app.captures.clear();
    app.renderer.Shutdown();
    app.confiner.Release();
    StopWatcher(app);
    app.segments.clear();
    g_app = nullptr;

    ::UnregisterClassW(kWindowClass, hinst);
    DS_LOG(L"dscomp exited");
    return 0;
}

} // namespace
} // namespace ds

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR,
                      _In_ int) {
    // Inherit the launching console (if any) so stderr logging and Ctrl+C work.
    // Only redirect stderr to the console when it was not already redirected by
    // the caller (e.g. "dscomp.exe 2> log.txt"), otherwise we would clobber it.
    {
        const HANDLE herr = ::GetStdHandle(STD_ERROR_HANDLE);
        const bool haveStdErr = (herr != nullptr && herr != INVALID_HANDLE_VALUE);
        if (::AttachConsole(ATTACH_PARENT_PROCESS) && !haveStdErr) {
            FILE* f = nullptr;
            ::freopen_s(&f, "CONOUT$", "w", stderr);
        }
    }
    ::SetConsoleCtrlHandler(ds::ConsoleCtrlHandler, TRUE);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ds::Options opt;
    opt.configPath = ds::DefaultConfigPath();
    opt.specialized = ds::HidePhysicalDisplayFlag();

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--verbose" || a == L"-v") {
                opt.verbose = true;
            } else if ((a == L"--config" || a == L"-c") && i + 1 < argc) {
                opt.configPath = argv[++i];
            } else if (a == L"--no-confine") {
                opt.confine = false;
            } else if (a == L"--no-rescue") {
                opt.rescue = false;
            } else if (a == L"--specialized") {
                opt.specialized = true;
            } else if (a == L"--help" || a == L"-h" || a == L"/?") {
                ds::PrintUsage();
                ::LocalFree(argv);
                return 0;
            } else {
                ::fwprintf(stderr, L"dscomp: unknown argument '%s'\n", a.c_str());
                ds::PrintUsage();
                ::LocalFree(argv);
                return 1;
            }
        }
        ::LocalFree(argv);
    }

    const int rc = ds::Run(hInstance, opt);
    ::SetConsoleCtrlHandler(ds::ConsoleCtrlHandler, FALSE);
    return rc;
}
