#include "TaskbarControl.h"
#include "MonitorUtil.h"
#include "Log.h"

#include <algorithm>

namespace ds {

namespace {

struct FoundTaskbar {
    HWND hwnd;
    RECT monitorRect;
    bool visible;
};

BOOL CALLBACK EnumTaskbarProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[64] = {};
    if (::GetClassNameW(hwnd, cls, _countof(cls)) == 0) return TRUE;
    // Shell_TrayWnd is the MAIN taskbar (primary display);
    // Shell_SecondaryTrayWnd is one per additional display.
    if (::_wcsicmp(cls, L"Shell_TrayWnd") != 0 &&
        ::_wcsicmp(cls, L"Shell_SecondaryTrayWnd") != 0) {
        return TRUE;
    }
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon) return TRUE;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;

    auto* out = reinterpret_cast<std::vector<FoundTaskbar>*>(lp);
    FoundTaskbar f;
    f.hwnd = hwnd;
    f.monitorRect = mi.rcMonitor;
    f.visible = ::IsWindowVisible(hwnd) != FALSE;
    out->push_back(f);
    return TRUE;
}

std::vector<FoundTaskbar> EnumerateTaskbars() {
    std::vector<FoundTaskbar> out;
    ::EnumWindows(EnumTaskbarProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

} // namespace

TaskbarControl::~TaskbarControl() {
    Stop();
}

bool TaskbarControl::Start(HWND hwnd, const AppConfig& cfg) {
    m_hwnd = hwnd;
    m_cfg = cfg;
    m_enabled = true;

    // Explorer re-creates its taskbars on restart and broadcasts this; without
    // handling it our hiding silently stops applying.
    m_taskbarCreatedMsg = ::RegisterWindowMessageW(L"TaskbarCreated");
    if (m_taskbarCreatedMsg == 0) {
        DS_WARN(L"taskbar: RegisterWindowMessage(TaskbarCreated) failed - Explorer "
                L"restarts will only be caught by the 2s timer");
    }

    size_t hideCount = 0;
    for (const SegmentConfig& s : cfg.segments) {
        if (!s.showTaskbar) ++hideCount;
    }
    DS_LOG(L"taskbar control: on, %zu of %zu segment(s) want no taskbar "
           L"(primarySegment=%u)", hideCount, cfg.segments.size(),
           cfg.primarySegment);
    Enforce();
    return true;
}

void TaskbarControl::Enforce() {
    if (!m_enabled) return;

    // Resolve our virtual monitors fresh: a display change can move them, and a
    // stale rect would mean touching the wrong monitor's taskbar.
    std::vector<std::wstring> devices;
    devices.reserve(m_cfg.segments.size());
    std::vector<bool> wantShow;
    wantShow.reserve(m_cfg.segments.size());
    for (const SegmentConfig& s : m_cfg.segments) {
        devices.push_back(s.virtualDevice);
        wantShow.push_back(s.showTaskbar);
    }
    std::vector<RECT> segRects;
    FindMonitorRects(devices, segRects);
    if (segRects.size() != devices.size()) {
        // Some virtual monitor is not attached; rect indices would not line up
        // with segment indices, so do nothing rather than guess.
        return;
    }

    for (const FoundTaskbar& tb : EnumerateTaskbars()) {
        const int seg = SegmentForMonitorRect(segRects, tb.monitorRect);
        const TaskbarAction action =
            DecideTaskbarAction(seg, wantShow, tb.visible);
        if (action == TaskbarAction::Leave) continue;

        if (action == TaskbarAction::Hide) {
            ::ShowWindow(tb.hwnd, SW_HIDE);
            if (std::find(m_hidden.begin(), m_hidden.end(), tb.hwnd) ==
                m_hidden.end()) {
                m_hidden.push_back(tb.hwnd);
            }
            DS_VERB(L"taskbar: hid the taskbar on segment %d", seg);
        } else {
            ::ShowWindow(tb.hwnd, SW_SHOWNA);
            m_hidden.erase(std::remove(m_hidden.begin(), m_hidden.end(), tb.hwnd),
                           m_hidden.end());
            DS_VERB(L"taskbar: showed the taskbar on segment %d", seg);
        }
    }
}

void TaskbarControl::RestoreAll() {
    if (m_hidden.empty()) return;
    size_t restored = 0;
    for (HWND h : m_hidden) {
        if (::IsWindow(h)) {
            ::ShowWindow(h, SW_SHOWNA);
            ++restored;
        }
    }
    m_hidden.clear();
    DS_LOG(L"taskbar: restored %zu taskbar(s)", restored);
}

void TaskbarControl::Stop() {
    if (!m_enabled) return;
    RestoreAll();
    m_enabled = false;
    m_hwnd = nullptr;
}

} // namespace ds
