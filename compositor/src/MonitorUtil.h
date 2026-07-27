// MonitorUtil.h - GDI display-device name -> desktop rect resolution.
//
// The pure rect math (containment, union, overlap, edge sliding, placement)
// lives in Geometry.h so it can be unit-tested; this header only covers the
// parts that query live display state.
#pragma once

#include "Common.h"
#include "Geometry.h"

namespace ds {

struct MonitorEntry {
    std::wstring device;        // MONITORINFOEX.szDevice, e.g. "\\.\DISPLAY1"
    RECT         rect = {};     // full monitor rect, virtual-desktop coords
    RECT         work = {};     // work area (minus taskbar / appbars)
    bool         primary = false;
};

// Snapshot of every attached monitor, resolved fresh from EnumDisplayMonitors.
// Virtual-desktop coordinates are re-based whenever the primary monitor
// changes (the control app makes a virtual monitor primary during Apply), so
// callers must re-enumerate rather than cache across a WM_DISPLAYCHANGE.
size_t EnumerateMonitors(std::vector<MonitorEntry>& out);

// Finds the monitor whose GDI device name (MONITORINFOEX.szDevice, e.g.
// "\\.\DISPLAY1") matches 'deviceName' and returns its desktop rect.
bool FindMonitorRect(const std::wstring& deviceName, RECT& rect);

// Same, but returns the work area instead of the full monitor rect.
bool FindMonitorWorkArea(const std::wstring& deviceName, RECT& work);

// Resolves every name, skipping (and logging) the ones that are not present.
// Returns the number of names that resolved.
size_t FindMonitorRects(const std::vector<std::wstring>& deviceNames,
                        std::vector<RECT>& rects);

} // namespace ds
