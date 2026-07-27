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
        s.reason = L"the monitor is not marked as a specialized display "
                   L"(DisplayMonitorUsageKind is not SpecialPurpose) - apply the "
                   L"EDID override first, see docs/HIDING.md";
    } else if (!a.acquired) {
        s.reason = L"TryAcquireTarget did not succeed";
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

} // namespace ds
