// SessionWatcher.h - suspends the split around the secure desktop.
//
// THE PROBLEM
//
// With the split active, Windows' primary display is a VIRTUAL monitor, and the
// only way the user can see that monitor is THROUGH our compositor on the
// physical panel. UAC prompts, Ctrl+Alt+Del and the lock screen all render on
// the secure desktop, which Desktop Duplication cannot capture by design. So
// the compositor goes blank (or freezes on its last frame) at exactly the
// moment a dialog the user must interact with appears - on a display they
// cannot see.
//
// WHAT THIS CLASS DOES
//
// Detects those transitions and suspends the compositor: hides the window so
// the physical monitor shows whatever Windows actually renders on it, drops the
// cursor clip, and signals the control app over a named-event contract so it
// can restore the physical monitor as primary for the duration. Reverses all of
// it on return.
//
// IMPORTANT: hiding alone is NOT sufficient - see README.md. The dialog is on
// the primary (a virtual monitor), so the user still cannot see it until the
// app acts on the suspend signal. Hiding is what makes that possible, not a
// fix on its own.
#pragma once

#include "Common.h"

#include <atomic>

namespace ds {

// Why the split is currently suspended. Ordered by precedence; a manual
// suspend is never undone by an automatic resume.
enum class SuspendReason {
    None,
    SecureDesktop,   // EVENT_SYSTEM_DESKTOPSWITCH away from "Default"
    SessionLock,     // WTS_SESSION_LOCK
    SessionDetached, // WTS_CONSOLE_DISCONNECT / remote connect
    Manual,          // the user pressed the suspend hotkey
};

const wchar_t* SuspendReasonName(SuspendReason r);

class SessionWatcher {
public:
    // Called on the UI thread whenever the suspended state changes.
    using StateChanged = void (*)(void* context, bool suspended, SuspendReason why);

    SessionWatcher() = default;
    ~SessionWatcher();

    SessionWatcher(const SessionWatcher&) = delete;
    SessionWatcher& operator=(const SessionWatcher&) = delete;

    // 'hwnd' must be the compositor's window: WTS notifications and the hotkey
    // are delivered to it, and the desktop-switch hook runs on its thread.
    bool Start(HWND hwnd, StateChanged callback, void* context);
    void Stop();

    // Message-loop hooks. Return true when the message was consumed.
    bool OnSessionChange(WPARAM wParam);
    bool OnHotkey(WPARAM wParam);

    // Cheap poll from the existing 2 s timer. EVENT_SYSTEM_DESKTOPSWITCH is not
    // fully reliable (see README), so the input-desktop name is the backstop.
    void PollInputDesktop();

    bool IsSuspended() const { return m_suspended; }
    SuspendReason Reason() const { return m_reason; }

    // Toggles a manual suspend, for the hotkey and for external callers.
    void ToggleManual();

private:
    static void CALLBACK DesktopSwitchProc(HWINEVENTHOOK hook, DWORD event,
                                           HWND hwnd, LONG idObject, LONG idChild,
                                           DWORD thread, DWORD time);
    void Apply(bool suspended, SuspendReason why);
    void SignalApp(bool suspended);

    HWND          m_hwnd = nullptr;
    StateChanged  m_callback = nullptr;
    void*         m_context = nullptr;
    HWINEVENTHOOK m_desktopHook = nullptr;
    bool          m_wtsRegistered = false;
    bool          m_hotkeyRegistered = false;

    bool          m_suspended = false;
    SuspendReason m_reason = SuspendReason::None;
    bool          m_manualLatch = false;   // manual suspend outranks automatic

    HANDLE m_suspendEvent = nullptr;   // Global\DeskSplitSuspendRequest
    HANDLE m_resumeEvent = nullptr;    // Global\DeskSplitResumeRequest
};

// True when the interactive input desktop is NOT the normal "Default" desktop -
// i.e. the secure desktop (Winlogon) or the screen saver desktop is up.
// 'nameOut' receives the desktop name when it could be read.
bool InputDesktopIsSecure(std::wstring& nameOut);

} // namespace ds
