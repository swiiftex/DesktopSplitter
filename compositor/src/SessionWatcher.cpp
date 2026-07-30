#include "SessionWatcher.h"
#include "Log.h"

#include <sddl.h>
#include <wtsapi32.h>

namespace ds {

namespace {

SessionWatcher* g_instance = nullptr;

constexpr int kHotkeyId = 0xD501;   // Ctrl+Alt+Shift+S

// The app may be unelevated, so it needs to wait on and read these.
const wchar_t* const kIpcSddl =
    L"D:P(A;;0x1f0003;;;BA)(A;;0x1f0003;;;SY)(A;;0x00120002;;;AU)";

HANDLE CreateIpcEvent(const wchar_t* name, bool initiallySet) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kIpcSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;

    std::wstring full = std::wstring(L"Global\\") + name;
    HANDLE h = ::CreateEventW(&sa, TRUE, initiallySet ? TRUE : FALSE, full.c_str());
    if (!h && ::GetLastError() == ERROR_ACCESS_DENIED) {
        h = ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, full.c_str());
        if (!h) {
            full = std::wstring(L"Local\\") + name;
            h = ::CreateEventW(&sa, TRUE, initiallySet ? TRUE : FALSE, full.c_str());
        }
    }
    ::LocalFree(sd);
    if (h) {
        DS_VERB(L"session: IPC event ready: %s", full.c_str());
    } else {
        DS_WARN(L"session: could not create IPC event %s (0x%08X)", name,
                ::GetLastError());
    }
    return h;
}

} // namespace

const wchar_t* SuspendReasonName(SuspendReason r) {
    switch (r) {
    case SuspendReason::SecureDesktop:   return L"secure desktop (UAC / Ctrl+Alt+Del)";
    case SuspendReason::SessionLock:     return L"session locked";
    case SuspendReason::SessionDetached: return L"session disconnected";
    case SuspendReason::Manual:          return L"manual hotkey";
    default:                             return L"none";
    }
}

// OpenInputDesktop fails with ACCESS_DENIED while the secure desktop is up
// (our process has no rights on the Winlogon desktop) - that failure IS the
// signal, and it is the most reliable one available to an unprivileged process.
bool InputDesktopIsSecure(std::wstring& nameOut) {
    nameOut.clear();
    HDESK desk = ::OpenInputDesktop(0, FALSE, READ_CONTROL | DESKTOP_READOBJECTS);
    if (!desk) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            nameOut = L"<access denied - secure desktop>";
            return true;
        }
        return false;   // unknown; do not flap the split on a transient error
    }
    wchar_t name[256] = {};
    DWORD   needed = 0;
    const BOOL ok = ::GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name),
                                                &needed);
    ::CloseDesktop(desk);
    if (!ok) return false;
    nameOut = name;
    return ::_wcsicmp(name, L"Default") != 0;   // Winlogon, Screen-saver, ...
}

SessionWatcher::~SessionWatcher() {
    Stop();
}

bool SessionWatcher::Start(HWND hwnd, StateChanged callback, void* context) {
    m_hwnd = hwnd;
    m_callback = callback;
    m_context = context;
    g_instance = this;

    m_suspendEvent = CreateIpcEvent(L"DeskSplitSuspendRequest", false);
    m_resumeEvent = CreateIpcEvent(L"DeskSplitResumeRequest", true);

    m_wtsRegistered =
        ::WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!m_wtsRegistered) {
        DS_WARN(L"session: WTSRegisterSessionNotification failed (0x%08X) - lock "
                L"and unlock will only be caught by the 2s desktop poll",
                ::GetLastError());
    }

    // Out-of-context so it cannot wedge whatever process switched desktops.
    m_desktopHook = ::SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH,
                                      EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
                                      DesktopSwitchProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT);
    if (!m_desktopHook) {
        DS_WARN(L"session: EVENT_SYSTEM_DESKTOPSWITCH hook failed - relying on the "
                L"2s desktop poll only");
    }

    m_hotkeyRegistered = ::RegisterHotKey(hwnd, kHotkeyId,
                                          MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                          'S') != FALSE;
    DS_LOG(L"session watcher: desktop-switch hook %s, WTS notifications %s, "
           L"Ctrl+Alt+Shift+S suspend hotkey %s",
           m_desktopHook ? L"on" : L"OFF", m_wtsRegistered ? L"on" : L"OFF",
           m_hotkeyRegistered ? L"on" : L"OFF (already taken?)");
    return true;
}

void SessionWatcher::Stop() {
    if (m_hotkeyRegistered && m_hwnd) {
        ::UnregisterHotKey(m_hwnd, kHotkeyId);
        m_hotkeyRegistered = false;
    }
    if (m_desktopHook) {
        ::UnhookWinEvent(m_desktopHook);
        m_desktopHook = nullptr;
    }
    if (m_wtsRegistered && m_hwnd) {
        ::WTSUnRegisterSessionNotification(m_hwnd);
        m_wtsRegistered = false;
    }
    // Leave the app in the "resumed" state so it never strands the display
    // config in a suspended layout because we exited.
    if (m_suspended) SignalApp(false);
    if (m_suspendEvent) { ::CloseHandle(m_suspendEvent); m_suspendEvent = nullptr; }
    if (m_resumeEvent) { ::CloseHandle(m_resumeEvent); m_resumeEvent = nullptr; }
    if (g_instance == this) g_instance = nullptr;
}

void CALLBACK SessionWatcher::DesktopSwitchProc(HWINEVENTHOOK, DWORD, HWND, LONG,
                                                LONG, DWORD, DWORD) {
    // The event fires AFTER the switch and carries no useful payload, so ask
    // the OS which desktop is now current.
    if (g_instance) g_instance->PollInputDesktop();
}

void SessionWatcher::PollInputDesktop() {
    if (m_manualLatch) return;   // a manual suspend outranks anything automatic

    std::wstring name;
    const bool secure = InputDesktopIsSecure(name);

    if (secure && !m_suspended) {
        DS_LOG(L"session: input desktop is now '%s'", name.c_str());
        Apply(true, SuspendReason::SecureDesktop);
    } else if (!secure && m_suspended && m_reason == SuspendReason::SecureDesktop) {
        DS_LOG(L"session: input desktop is back to '%s'",
               name.empty() ? L"Default" : name.c_str());
        Apply(false, SuspendReason::None);
    }
}

bool SessionWatcher::OnSessionChange(WPARAM wParam) {
    switch (wParam) {
    case WTS_SESSION_LOCK:
        if (!m_manualLatch) Apply(true, SuspendReason::SessionLock);
        return true;
    case WTS_SESSION_UNLOCK:
        if (!m_manualLatch && m_reason == SuspendReason::SessionLock) {
            Apply(false, SuspendReason::None);
        }
        return true;
    case WTS_CONSOLE_DISCONNECT:
    case WTS_REMOTE_CONNECT:
        if (!m_manualLatch) Apply(true, SuspendReason::SessionDetached);
        return true;
    case WTS_CONSOLE_CONNECT:
    case WTS_REMOTE_DISCONNECT:
        if (!m_manualLatch && m_reason == SuspendReason::SessionDetached) {
            Apply(false, SuspendReason::None);
        }
        return true;
    default:
        return false;
    }
}

bool SessionWatcher::OnHotkey(WPARAM wParam) {
    if (wParam != kHotkeyId) return false;
    ToggleManual();
    return true;
}

void SessionWatcher::ToggleManual() {
    if (m_manualLatch) {
        m_manualLatch = false;
        DS_LOG(L"session: manual suspend released by the user");
        Apply(false, SuspendReason::None);
    } else {
        m_manualLatch = true;
        DS_LOG(L"session: manual suspend requested by the user");
        Apply(true, SuspendReason::Manual);
    }
}

void SessionWatcher::Apply(bool suspended, SuspendReason why) {
    if (suspended == m_suspended && why == m_reason) return;
    m_suspended = suspended;
    m_reason = why;

    if (suspended) {
        DS_LOG(L"=== SPLIT SUSPENDED (%s) - hiding the compositor so the physical "
               L"monitor shows the real desktop ===", SuspendReasonName(why));
    } else {
        DS_LOG(L"=== SPLIT RESUMED ===");
    }
    SignalApp(suspended);
    if (m_callback) m_callback(m_context, suspended, why);
}

void SessionWatcher::SignalApp(bool suspended) {
    // Manual-reset and mutually exclusive, so the app can either wait on one or
    // poll both to learn the current state at any time.
    if (suspended) {
        if (m_resumeEvent) ::ResetEvent(m_resumeEvent);
        if (m_suspendEvent) ::SetEvent(m_suspendEvent);
    } else {
        if (m_suspendEvent) ::ResetEvent(m_suspendEvent);
        if (m_resumeEvent) ::SetEvent(m_resumeEvent);
    }
}

} // namespace ds
