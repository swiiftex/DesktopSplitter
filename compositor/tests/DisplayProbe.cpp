// DisplayProbe.cpp - read-only feasibility probe for specialized displays.
//
// Answers, for this machine and today's OS edition:
//   * what Windows.Devices.Display.Core reports for every display target,
//   * each monitor's DisplayMonitorUsageKind (Standard vs SpecialPurpose) -
//     the definitive read-only indicator of whether a display is "specialized",
//   * whether the monitor's EDID carries the Microsoft CTA VSDB,
//   * optionally, the DisplayManagerResult from TryAcquireTarget.
//
// SAFETY: everything except --try-acquire is pure enumeration and cannot
// disturb the desktop. --try-acquire is opt-in, is expected to be DENIED for
// any monitor whose UsageKind is Standard, and always calls ReleaseTarget in
// the (unexpected) event that acquisition succeeds.

#include <Windows.h>

// The C++/WinRT and SDK projection headers are not /W4-clean; this project is
// built /W4 /WX, so silence warnings for them only.
#pragma warning(push, 0)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#pragma warning(pop)

#include <cstdio>
#include <string>
#include <vector>

namespace core = winrt::Windows::Devices::Display::Core;
namespace disp = winrt::Windows::Devices::Display;

namespace {

bool         g_tryAcquire = false;
std::wstring g_only;          // restrict --try-acquire to DeviceIds containing this
bool         g_connectedOnly = false;

const wchar_t* UsageKindName(disp::DisplayMonitorUsageKind k) {
    switch (k) {
    case disp::DisplayMonitorUsageKind::Standard:       return L"Standard (desktop)";
    case disp::DisplayMonitorUsageKind::HeadMounted:    return L"HeadMounted";
    case disp::DisplayMonitorUsageKind::SpecialPurpose: return L"SpecialPurpose (SPECIALIZED)";
    default:                                            return L"<unknown>";
    }
}

const wchar_t* ManagerResultName(core::DisplayManagerResult r) {
    switch (r) {
    case core::DisplayManagerResult::Success:                   return L"Success";
    case core::DisplayManagerResult::UnknownFailure:            return L"UnknownFailure";
    case core::DisplayManagerResult::TargetAccessDenied:        return L"TargetAccessDenied";
    case core::DisplayManagerResult::TargetStale:               return L"TargetStale";
    case core::DisplayManagerResult::RemoteSessionNotSupported: return L"RemoteSessionNotSupported";
    default:                                                    return L"<unknown>";
    }
}

const wchar_t* ConnectionKindName(disp::DisplayMonitorConnectionKind k) {
    switch (k) {
    case disp::DisplayMonitorConnectionKind::Internal: return L"Internal";
    case disp::DisplayMonitorConnectionKind::Wired:    return L"Wired";
    case disp::DisplayMonitorConnectionKind::Wireless: return L"Wireless";
    case disp::DisplayMonitorConnectionKind::Virtual:  return L"Virtual";
    default:                                           return L"<unknown>";
    }
}

void PrintOsInfo() {
    ::wprintf(L"== OS ==\n");
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                        KEY_READ, &key) == ERROR_SUCCESS) {
        const wchar_t* names[] = { L"EditionID", L"ProductName", L"DisplayVersion",
                                   L"CurrentBuild" };
        for (const wchar_t* n : names) {
            wchar_t buf[256] = {};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            if (::RegQueryValueExW(key, n, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
                type == REG_SZ) {
                ::wprintf(L"  %-16s : %s\n", n, buf);
            }
        }
        DWORD ubr = 0;
        DWORD cb = sizeof(ubr);
        if (::RegQueryValueExW(key, L"UBR", nullptr, nullptr,
                               reinterpret_cast<LPBYTE>(&ubr), &cb) == ERROR_SUCCESS) {
            ::wprintf(L"  %-16s : %lu\n", L"UBR", ubr);
        }
        ::RegCloseKey(key);
    }
    ::wprintf(L"\n");
}

// Maps every \\.\DISPLAYn to the monitor device-interface path Display.Core
// reports as DisplayMonitor.DeviceId, so the two views can be correlated.
struct GdiMonitor {
    std::wstring gdiName;       // \\.\DISPLAY1
    std::wstring adapterName;   // adapter description
    std::wstring interfacePath; // \\?\DISPLAY#PHLC310#...
    RECT         rect = {};
};

std::vector<GdiMonitor> EnumerateGdiMonitors() {
    std::vector<GdiMonitor> out;
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD ai = 0; ::EnumDisplayDevicesW(nullptr, ai, &adapter, 0); ++ai) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;

        GdiMonitor m;
        m.gdiName = adapter.DeviceName;
        m.adapterName = adapter.DeviceString;

        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        if (::EnumDisplayDevicesW(adapter.DeviceName, 0, &mon,
                                  EDD_GET_DEVICE_INTERFACE_NAME)) {
            m.interfacePath = mon.DeviceID;
        }

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (::EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            m.rect.left = dm.dmPosition.x;
            m.rect.top = dm.dmPosition.y;
            m.rect.right = dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth);
            m.rect.bottom = dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight);
        }
        out.push_back(std::move(m));
    }
    return out;
}

void PrintGdiMonitors(const std::vector<GdiMonitor>& mons) {
    ::wprintf(L"== GDI display devices ==\n");
    for (const GdiMonitor& m : mons) {
        ::wprintf(L"  %-16s (%ld,%ld)-(%ld,%ld)  %s\n", m.gdiName.c_str(), m.rect.left,
                  m.rect.top, m.rect.right, m.rect.bottom, m.adapterName.c_str());
        ::wprintf(L"      interface: %s\n", m.interfacePath.c_str());
    }
    ::wprintf(L"\n");
}

const GdiMonitor* MatchGdi(const std::vector<GdiMonitor>& mons,
                           const std::wstring& deviceId) {
    // DeviceId and the EDD interface path describe the same device but differ
    // in case and in the \\?\ vs \\.\ prefix; compare on the devnode portion.
    auto norm = [](std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(::towlower(c));
        const size_t p = s.find(L"display#");
        return (p == std::wstring::npos) ? s : s.substr(p);
    };
    const std::wstring want = norm(deviceId);
    for (const GdiMonitor& m : mons) {
        if (!m.interfacePath.empty() && norm(m.interfacePath) == want) return &m;
    }
    return nullptr;
}

// Locates the Microsoft VSDB (IEEE OUI 5C-12-CA) in raw EDID bytes, testing
// both possible byte orders so the probe reports what is actually there.
void ReportMicrosoftVsdb(const uint8_t* edid, size_t len) {
    bool found = false;
    for (size_t i = 0; i + 2 < len; ++i) {
        if (edid[i] == 0xCA && edid[i + 1] == 0x12 && edid[i + 2] == 0x5C) {
            ::wprintf(L"      Microsoft VSDB: FOUND at offset %zu (CTA LSB-first CA 12 5C)\n", i);
            found = true;
        } else if (edid[i] == 0x5C && edid[i + 1] == 0x12 && edid[i + 2] == 0xCA) {
            ::wprintf(L"      Microsoft VSDB: FOUND at offset %zu (big-endian 5C 12 CA)\n", i);
            found = true;
        }
    }
    if (!found) ::wprintf(L"      Microsoft VSDB: absent\n");
}

void PrintEdidSummary(const disp::DisplayMonitor& monitor) {
    try {
        const auto desc = monitor.GetDescriptor(disp::DisplayMonitorDescriptorKind::Edid);
        if (desc.size() == 0) {
            ::wprintf(L"      EDID: (empty)\n");
            return;
        }
        const uint8_t* e = desc.data();
        const size_t   n = desc.size();
        ::wprintf(L"      EDID: %zu bytes, version %u.%u, %u extension block(s)\n", n,
                  n > 19 ? e[18] : 0, n > 19 ? e[19] : 0, n > 126 ? e[126] : 0);
        for (size_t b = 1; b * 128 < n; ++b) {
            ::wprintf(L"        ext block %zu: tag 0x%02X\n", b, e[b * 128]);
        }
        ReportMicrosoftVsdb(e, n);
    } catch (const winrt::hresult_error& ex) {
        ::wprintf(L"      EDID: unavailable (0x%08X %s)\n",
                  static_cast<unsigned>(ex.code()), ex.message().c_str());
    }
}

int Run() {
    PrintOsInfo();
    const std::vector<GdiMonitor> gdi = EnumerateGdiMonitors();
    PrintGdiMonitors(gdi);

    ::wprintf(L"== Windows.Devices.Display.Core ==\n");
    core::DisplayManager manager{ nullptr };
    try {
        manager = core::DisplayManager::Create(core::DisplayManagerOptions::None);
    } catch (const winrt::hresult_error& ex) {
        ::wprintf(L"  DisplayManager::Create FAILED: 0x%08X %s\n",
                  static_cast<unsigned>(ex.code()), ex.message().c_str());
        return 2;
    }
    ::wprintf(L"  DisplayManager::Create: OK\n\n");

    const auto targets = manager.GetCurrentTargets();
    ::wprintf(L"  %u target(s)\n\n", targets.Size());

    unsigned idx = 0;
    for (const auto& target : targets) {
        ::wprintf(L"  --- target %u ---\n", idx++);
        ::wprintf(L"      connected: %s   stale: %s\n",
                  target.IsConnected() ? L"yes" : L"no",
                  target.IsStale() ? L"yes" : L"no");

        disp::DisplayMonitor monitor{ nullptr };
        try {
            monitor = target.TryGetMonitor();
        } catch (const winrt::hresult_error&) {
        }

        if (!monitor) {
            if (!g_connectedOnly) {
                ::wprintf(L"      monitor: <none> (target not attached to a monitor)\n\n");
            }
            continue;
        }

        const std::wstring deviceId{ monitor.DeviceId() };
        ::wprintf(L"      DeviceId : %s\n", deviceId.c_str());
        ::wprintf(L"      Name     : %s\n", std::wstring{ monitor.DisplayName() }.c_str());
        ::wprintf(L"      Connection: %s\n", ConnectionKindName(monitor.ConnectionKind()));
        ::wprintf(L"      >>> UsageKind: %s\n", UsageKindName(monitor.UsageKind()));

        if (const GdiMonitor* g = MatchGdi(gdi, deviceId)) {
            ::wprintf(L"      GDI name : %s\n", g->gdiName.c_str());
        } else {
            ::wprintf(L"      GDI name : <not attached to the desktop>\n");
        }

        PrintEdidSummary(monitor);

        const bool selected = g_only.empty() || deviceId.find(g_only) != std::wstring::npos;
        if (g_tryAcquire && !selected) {
            ::wprintf(L"      TryAcquireTarget: skipped (does not match --only)\n");
        } else if (g_tryAcquire) {
            if (monitor.UsageKind() == disp::DisplayMonitorUsageKind::Standard) {
                ::wprintf(L"      TryAcquireTarget: ");
                const auto r = manager.TryAcquireTarget(target);
                ::wprintf(L"%s (%d)\n", ManagerResultName(r), static_cast<int>(r));
                if (r == core::DisplayManagerResult::Success) {
                    // Not expected for a desktop-owned monitor. Hand it back at
                    // once so the desktop is not disturbed.
                    ::wprintf(L"      !! unexpectedly acquired - releasing immediately\n");
                    manager.ReleaseTarget(target);
                }
            } else {
                ::wprintf(L"      TryAcquireTarget: SKIPPED (UsageKind is not Standard; "
                          L"acquiring could really take this display)\n");
            }
        }
        ::wprintf(L"\n");
    }

    ::wprintf(L"== interpretation ==\n");
    ::wprintf(L"  A display can be driven by the specialized (DisplayManager) path only\n");
    ::wprintf(L"  when its UsageKind is SpecialPurpose. UsageKind is derived from the\n");
    ::wprintf(L"  EDID: Windows marks a monitor non-desktop when the Microsoft CTA VSDB\n");
    ::wprintf(L"  is present with the desktop-usage bit clear.\n");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--try-acquire") {
            g_tryAcquire = true;
        } else if (a == L"--connected-only") {
            g_connectedOnly = true;
        } else if (a == L"--only" && i + 1 < argc) {
            g_only = argv[++i];
        } else if (a == L"--help" || a == L"-h") {
            ::wprintf(L"DisplayProbe [--connected-only] [--try-acquire] [--only <substr>]\n\n"
                      L"  Read-only enumeration of display targets and monitors.\n"
                      L"  --connected-only  Hide targets with no attached monitor.\n"
                      L"  --try-acquire     Additionally call DisplayManager.TryAcquireTarget\n"
                      L"                    on Standard (desktop-owned) monitors to record the\n"
                      L"                    DisplayManagerResult. Skipped for non-Standard\n"
                      L"                    monitors; always released if it somehow succeeds.\n"
                      L"  --only <substr>   Restrict --try-acquire to DeviceIds containing\n"
                      L"                    <substr> (e.g. HPN3636). Strongly recommended so\n"
                      L"                    the attempt targets a secondary display first.\n");
            return 0;
        } else {
            ::wprintf(L"unknown argument '%s'\n", a.c_str());
            return 1;
        }
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& ex) {
        ::wprintf(L"init_apartment failed: 0x%08X\n", static_cast<unsigned>(ex.code()));
        return 2;
    }
    int rc = 2;
    try {
        rc = Run();
    } catch (const winrt::hresult_error& ex) {
        ::wprintf(L"\nunhandled WinRT error: 0x%08X %s\n",
                  static_cast<unsigned>(ex.code()), ex.message().c_str());
    }
    winrt::uninit_apartment();
    return rc;
}
