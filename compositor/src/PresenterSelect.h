// PresenterSelect.h - pure presenter-selection policy.
//
// Deliberately free of Windows display APIs: probing fills in a
// SpecializedAvailability, this decides what to do with it, and
// tests/PresenterProbe.cpp drives the same function with mocked probe results
// (including every failure mode) to prove the fallback behaves.
#pragma once

#include <string>

namespace ds {

enum class PresenterKind {
    Window,
    Specialized,
};

// What probing the specialized display path discovered. Each field is only
// meaningful when every field above it is true.
struct SpecializedAvailability {
    bool requested = false;          // --specialized or HidePhysicalDisplay=1
    bool apiAvailable = false;       // DisplayManager::Create succeeded
    bool targetFound = false;        // a target matched the physical monitor
    bool targetSpecialized = false;  // DisplayMonitorUsageKind::SpecialPurpose
    bool acquired = false;           // TryAcquireTarget returned Success
    bool presentationReady = false;  // mode set and primaries created
    std::wstring detail;             // API-specific error text, may be empty
};

struct PresenterSelection {
    PresenterKind kind = PresenterKind::Window;
    bool          fellBack = false;   // specialized was asked for but is unusable
    std::wstring  reason;
};

inline PresenterSelection ChoosePresenter(const SpecializedAvailability& a) {
    PresenterSelection s;

    if (!a.requested) {
        s.kind = PresenterKind::Window;
        s.fellBack = false;
        s.reason = L"specialized presentation not requested";
        return s;
    }

    s.fellBack = true;   // cleared below if everything checked out
    s.kind = PresenterKind::Window;

    if (!a.apiAvailable) {
        s.reason = L"Windows.Devices.Display.Core is unavailable on this system";
    } else if (!a.targetFound) {
        s.reason = L"no display target matched the configured physical monitor";
    } else if (!a.targetSpecialized) {
        s.reason = L"the display target could not be prepared";
    } else if (!a.acquired) {
        s.reason = L"the display could not be acquired - mark it specialized first "
                   L"(Settings > System > Display > Advanced display > Remove "
                   L"display from desktop), see docs/HIDING.md";
    } else if (!a.presentationReady) {
        s.reason = L"the display mode could not be set or primaries could not be "
                   L"created";
    } else {
        s.kind = PresenterKind::Specialized;
        s.fellBack = false;
        s.reason = L"specialized display acquired";
        return s;
    }

    if (!a.detail.empty()) {
        s.reason += L" (";
        s.reason += a.detail;
        s.reason += L")";
    }
    return s;
}

inline const wchar_t* PresenterKindName(PresenterKind k) {
    return (k == PresenterKind::Specialized) ? L"specialized" : L"window";
}

// ---------------------------------------------------------------------------
// Start-ordering policy
// ---------------------------------------------------------------------------
//
// The display goes black the moment the user removes it from the desktop,
// because the desktop window that was covering it dies with it. So dscomp has
// to be running and WAITING first: it then acquires the instant the display
// becomes specialized and the black period is a blink instead of an open-ended
// dark screen.

// Keep polling for the target to become acquirable?
inline bool ShouldKeepWaiting(int waitSeconds, double elapsedSeconds, bool acquired) {
    if (acquired) return false;              // got it - stop
    if (waitSeconds <= 0) return false;      // waiting disabled - one attempt only
    return elapsedSeconds < static_cast<double>(waitSeconds);
}

// Seconds still left on the wait, for the countdown log line.
inline int WaitSecondsRemaining(int waitSeconds, double elapsedSeconds) {
    const double left = static_cast<double>(waitSeconds) - elapsedSeconds;
    if (left <= 0.0) return 0;
    return static_cast<int>(left + 0.5);
}

// A display that is specialized but receiving no frames is just a black panel.
// Bound that: if the first scanout has not happened within the deadline, fail
// deterministically instead of hanging.
inline bool FrameDeadlineExceeded(int deadlineSeconds, bool liveSignalled,
                                  double elapsedSinceAcquireSeconds) {
    if (liveSignalled) return false;         // already presenting
    if (deadlineSeconds <= 0) return false;  // deadline disabled
    return elapsedSinceAcquireSeconds > static_cast<double>(deadlineSeconds);
}

// ---------------------------------------------------------------------------
// dscomp process exit codes (contract with the control app - do not renumber)
// ---------------------------------------------------------------------------
namespace exitcode {

constexpr int kOk = 0;                  // clean shutdown (stop event / WM_CLOSE / Ctrl+C)
constexpr int kBadArgs = 1;             // unknown command line argument
constexpr int kConfigFailed = 2;        // config.json missing or invalid
constexpr int kMonitorNotFound = 3;     // physicalDevice is not attached
constexpr int kWindowFailed = 4;        // could not create the compositor window
constexpr int kRendererFailed = 5;      // device / presenter / pipeline init failed

// Runtime failures live in their own range so they can never be confused with
// a startup failure.
constexpr int kDisplayLost = 20;        // presenting worked, then stopped

} // namespace exitcode

// End-of-run state -> process exit code. 'fatalRuntimeError' means the
// compositor was presenting successfully and then lost the display; the control
// app treats that as "the hidden display went dark" and un-specializes it.
inline int ExitCodeForRun(bool fatalRuntimeError) {
    return fatalRuntimeError ? exitcode::kDisplayLost : exitcode::kOk;
}

} // namespace ds
