// Edition.h - Windows edition detection for the "hide the physical display"
// feature gate.
//
// Designating an ordinary monitor as a "specialized" display (so DWM stops
// extending the desktop onto it) is documented by Microsoft as available on
// Windows 10 Enterprise, Windows 10 Pro for Workstations and Windows 10 IoT
// Enterprise. Plain Pro / Home cannot do it, so the installer offers the
// feature flag only where the OS can honour it.
#pragma once

#include <windows.h>

namespace DeskSplit
{
    struct EditionInfo
    {
        wchar_t EditionId[64];      // registry EditionID, e.g. "ProfessionalWorkstation"
        wchar_t ProductName[128];   // registry ProductName
        wchar_t DisplayVersion[32]; // e.g. "25H2"
        DWORD   Build;
        bool    SupportsSpecializedDisplay;
    };

    // Reads HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion.
    void GetEditionInfo(_Out_ EditionInfo* pInfo);

    // True when EditionID is one of the specialized-display capable editions.
    bool EditionSupportsSpecializedDisplay(_In_z_ const wchar_t* EditionId);

    // Test hook: forces GetEditionInfo to report this EditionID. Pass nullptr
    // to go back to reading the real registry value. Used by /selftest to prove
    // both the enabled and the disabled gating paths.
    void SetEditionOverrideForTest(_In_opt_z_ const wchar_t* EditionId);
}
