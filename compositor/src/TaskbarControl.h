// TaskbarControl.h - per-segment taskbar visibility.
//
// Windows 11 has no per-monitor taskbar toggle: there is one global "show my
// taskbar on all displays" switch. The control app turns that on and picks
// which segment is primary; this enforces the per-segment choice by hiding the
// secondary taskbar windows on the segments that asked for no taskbar.
//
// SAFETY RULE: only ever touch a taskbar that sits on one of OUR virtual
// monitors. The physical monitor's taskbar and any unrelated real display are
// never touched, and everything we hid is restored on every exit path.
#pragma once

#include "Common.h"
#include "Geometry.h"

#include <vector>

namespace ds {

// --- pure decision logic (unit-tested by tests/TaskbarProbe.cpp) -----------

enum class TaskbarAction {
    Leave,   // not one of our monitors, or already correct - do nothing
    Hide,
    Show,
};

inline const wchar_t* TaskbarActionName(TaskbarAction a) {
    switch (a) {
    case TaskbarAction::Hide: return L"hide";
    case TaskbarAction::Show: return L"show";
    default:                  return L"leave";
    }
}

// Which segment owns the monitor a taskbar is on. Monitor rects are compared by
// exact identity because each segment maps to exactly one virtual monitor.
// Returns -1 when the monitor is not one of ours - the physical monitor and the
// user's other real displays land here and are then always left alone.
inline int SegmentForMonitorRect(const std::vector<RECT>& segmentMonitorRects,
                                 const RECT& monitorRect) {
    for (size_t i = 0; i < segmentMonitorRects.size(); ++i) {
        const RECT& r = segmentMonitorRects[i];
        if (RectIsEmpty(r)) continue;
        if (r.left == monitorRect.left && r.top == monitorRect.top &&
            r.right == monitorRect.right && r.bottom == monitorRect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The full decision table. 'segIndex' comes from SegmentForMonitorRect,
// 'currentlyVisible' from IsWindowVisible.
inline TaskbarAction DecideTaskbarAction(int segIndex,
                                         const std::vector<bool>& showTaskbar,
                                         bool currentlyVisible) {
    if (segIndex < 0) return TaskbarAction::Leave;            // not ours
    if (static_cast<size_t>(segIndex) >= showTaskbar.size()) {
        return TaskbarAction::Leave;                          // out of range
    }
    const bool want = showTaskbar[static_cast<size_t>(segIndex)];
    if (want == currentlyVisible) return TaskbarAction::Leave;  // already right
    return want ? TaskbarAction::Show : TaskbarAction::Hide;
}

// --- live enforcement -----------------------------------------------------

class TaskbarControl {
public:
    TaskbarControl() = default;
    ~TaskbarControl();

    TaskbarControl(const TaskbarControl&) = delete;
    TaskbarControl& operator=(const TaskbarControl&) = delete;

    // 'hwnd' receives the registered TaskbarCreated broadcast.
    bool Start(HWND hwnd, const AppConfig& cfg);

    // Restores every taskbar we hid, then stops enforcing.
    void Stop();

    // Cheap; safe to call from the 2 s timer, WM_DISPLAYCHANGE and on resume.
    void Enforce();

    // While the split is suspended nothing of ours is on screen, so put every
    // taskbar back; Enforce() re-hides on resume.
    void RestoreAll();

    // True when 'msg' is the registered TaskbarCreated message.
    bool IsTaskbarCreatedMessage(UINT msg) const {
        return m_taskbarCreatedMsg != 0 && msg == m_taskbarCreatedMsg;
    }

    bool Enabled() const { return m_enabled; }

private:
    struct Hidden {
        HWND hwnd;
    };

    bool  m_enabled = false;
    HWND  m_hwnd = nullptr;
    UINT  m_taskbarCreatedMsg = 0;
    AppConfig m_cfg;
    std::vector<HWND> m_hidden;   // taskbars WE hid, for exact restoration
};

} // namespace ds
