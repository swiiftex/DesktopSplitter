#include "MonitorUtil.h"
#include "Log.h"

namespace ds {

namespace {

BOOL CALLBACK EnumAllProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(hMon, &mi)) {
        MonitorEntry e;
        e.device = mi.szDevice;
        e.rect = mi.rcMonitor;
        e.work = mi.rcWork;
        e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        out->push_back(std::move(e));
    }
    return TRUE;
}

const MonitorEntry* FindEntry(const std::vector<MonitorEntry>& all,
                              const std::wstring& deviceName) {
    for (const MonitorEntry& e : all) {
        if (::_wcsicmp(e.device.c_str(), deviceName.c_str()) == 0) return &e;
    }
    return nullptr;
}

} // namespace

size_t EnumerateMonitors(std::vector<MonitorEntry>& out) {
    out.clear();
    ::EnumDisplayMonitors(nullptr, nullptr, EnumAllProc,
                          reinterpret_cast<LPARAM>(&out));
    return out.size();
}

bool FindMonitorRect(const std::wstring& deviceName, RECT& rect) {
    std::vector<MonitorEntry> all;
    EnumerateMonitors(all);
    const MonitorEntry* e = FindEntry(all, deviceName);
    if (!e) return false;
    rect = e->rect;
    return true;
}

bool FindMonitorWorkArea(const std::wstring& deviceName, RECT& work) {
    std::vector<MonitorEntry> all;
    EnumerateMonitors(all);
    const MonitorEntry* e = FindEntry(all, deviceName);
    if (!e) return false;
    work = e->work;
    return true;
}

size_t FindMonitorRects(const std::vector<std::wstring>& deviceNames,
                        std::vector<RECT>& rects) {
    std::vector<MonitorEntry> all;
    EnumerateMonitors(all);

    rects.clear();
    size_t found = 0;
    for (const std::wstring& name : deviceNames) {
        const MonitorEntry* e = FindEntry(all, name);
        if (e) {
            rects.push_back(e->rect);
            ++found;
        } else {
            DS_WARN(L"display device '%s' is not currently attached", name.c_str());
        }
    }
    return found;
}

} // namespace ds
