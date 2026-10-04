// WindowRescuer.h - relocates app windows that open behind the compositor.
//
// The compositor covers the physical monitor, so any window that opens or is
// restored inside that region is simply invisible. This watcher spots them and
// moves them onto the first configured virtual monitor.
//
// It runs on its own thread with its own message pump. That matters: relocating
// another process's window is a cross-process SetWindowPos which can block on a
// hung app, and the UI thread also services the WH_MOUSE_LL confinement hook -
// stalling it would stall system-wide mouse input.
#pragma once

#include "Common.h"

#include <atomic>
#include <thread>
#include <unordered_map>

namespace ds {

class WindowRescuer {
public:
    WindowRescuer() = default;
    ~WindowRescuer();

    WindowRescuer(const WindowRescuer&) = delete;
    WindowRescuer& operator=(const WindowRescuer&) = delete;

    // 'coveredDevice' - the physical monitor dscomp hides.
    // 'targetDevice'  - segment 1's virtual monitor (config segments[0]).
    bool Start(const std::wstring& coveredDevice, const std::wstring& targetDevice);
    void Stop();

    // Display topology changed (re-basing moves every rect): drop cached rects.
    // Callable from any thread.
    void OnDisplayChange();

private:
    static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild, DWORD thread,
                                   DWORD time);

    void ThreadMain();
    void HandleEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild);
    void FlushPending();
    void Evaluate(HWND hwnd);
    bool ShouldRescue(HWND hwnd, RECT& windowRect) const;
    bool RefreshRects();
    bool RateLimit(HWND hwnd);

    std::wstring m_coveredDevice;
    std::wstring m_targetDevice;

    std::thread        m_thread;
    std::atomic<DWORD> m_threadId{0};
    HANDLE             m_ready = nullptr;
    std::atomic<bool>  m_rectsDirty{true};

    // Everything below is touched only by the rescuer thread.
    HWINEVENTHOOK m_hookShow = nullptr;
    HWINEVENTHOOK m_hookMoveSizeEnd = nullptr;
    HWINEVENTHOOK m_hookLocation = nullptr;

    RECT  m_coveredRect = {};
    RECT  m_targetWork = {};
    bool  m_rectsValid = false;
    DWORD m_rectsTick = 0;
    DWORD m_ownPid = 0;

    // Thread timer (no window). SetTimer generates the id for NULL-hwnd timers,
    // so it must be kept and fed back in to reset rather than create another.
    UINT_PTR m_debounceTimer = 0;

    std::vector<HWND>               m_pending;      // debounced LOCATIONCHANGE
    std::unordered_map<HWND, DWORD> m_lastRescue;   // per-hwnd rate limit
};

} // namespace ds
