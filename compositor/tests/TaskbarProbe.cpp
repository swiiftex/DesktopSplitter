// TaskbarProbe.cpp - unit tests for the pure taskbar decision logic.
// Includes the real src/TaskbarControl.h. Touches no windows and no taskbars.
#include "../src/TaskbarControl.h"

#include <cstdio>
#include <vector>

using namespace ds;

namespace {
int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const wchar_t* name) {
    if (ok) {
        ++g_pass;
        ::wprintf(L"  pass  %s\n", name);
    } else {
        ++g_fail;
        ::wprintf(L"  FAIL  %s\n", name);
    }
}
RECT R(LONG l, LONG t, LONG r, LONG b) {
    RECT v;
    v.left = l; v.top = t; v.right = r; v.bottom = b;
    return v;
}

// Three virtual monitors in a row, plus the physical one and the user's HP.
const RECT kSeg0 = { 0, 0, 1280, 1440 };
const RECT kSeg1 = { 1280, 0, 3840, 1440 };
const RECT kSeg2 = { 3840, 0, 5120, 1440 };
const RECT kPhysical = { -5120, 0, 0, 1440 };
const RECT kOtherReal = { 1000, -1080, 2920, 0 };

void TestMapping() {
    ::wprintf(L"\nmonitor -> segment mapping\n");
    const std::vector<RECT> segs = { kSeg0, kSeg1, kSeg2 };
    Check(SegmentForMonitorRect(segs, kSeg0) == 0, L"segment 0 maps to index 0");
    Check(SegmentForMonitorRect(segs, kSeg1) == 1, L"segment 1 maps to index 1");
    Check(SegmentForMonitorRect(segs, kSeg2) == 2, L"segment 2 maps to index 2");
    Check(SegmentForMonitorRect(segs, kPhysical) == -1,
          L"the PHYSICAL monitor is never ours");
    Check(SegmentForMonitorRect(segs, kOtherReal) == -1,
          L"an unrelated real monitor (the HP) is never ours");
    Check(SegmentForMonitorRect(segs, R(0, 0, 1281, 1440)) == -1,
          L"a near-miss rect does not match");
    Check(SegmentForMonitorRect(std::vector<RECT>(), kSeg0) == -1,
          L"no segments -> no match");
    const std::vector<RECT> withEmpty = { R(0, 0, 0, 0), kSeg1 };
    Check(SegmentForMonitorRect(withEmpty, kSeg1) == 1,
          L"an empty segment rect is skipped, not matched");
}

void TestDecision() {
    ::wprintf(L"\ndecision table\n");
    const std::vector<bool> show = { true, false, true };

    Check(DecideTaskbarAction(-1, show, true) == TaskbarAction::Leave,
          L"not our monitor, visible -> LEAVE (never touch the HP)");
    Check(DecideTaskbarAction(-1, show, false) == TaskbarAction::Leave,
          L"not our monitor, hidden -> LEAVE");
    Check(DecideTaskbarAction(0, show, true) == TaskbarAction::Leave,
          L"want show, is visible -> leave");
    Check(DecideTaskbarAction(0, show, false) == TaskbarAction::Show,
          L"want show, is hidden -> SHOW");
    Check(DecideTaskbarAction(1, show, true) == TaskbarAction::Hide,
          L"want hide, is visible -> HIDE");
    Check(DecideTaskbarAction(1, show, false) == TaskbarAction::Leave,
          L"want hide, already hidden -> leave (idempotent)");
    Check(DecideTaskbarAction(2, show, false) == TaskbarAction::Show,
          L"third segment wants show -> SHOW");
    Check(DecideTaskbarAction(7, show, true) == TaskbarAction::Leave,
          L"segment index past the end -> LEAVE, never guess");
    Check(DecideTaskbarAction(0, std::vector<bool>(), true) == TaskbarAction::Leave,
          L"empty policy -> LEAVE");
}

void TestDefaults() {
    ::wprintf(L"\nsafe defaults\n");
    const std::vector<bool> allShow = { true, true, true };
    bool anyTouch = false;
    for (int i = 0; i < 3; ++i) {
        if (DecideTaskbarAction(i, allShow, true) != TaskbarAction::Leave) {
            anyTouch = true;
        }
    }
    Check(!anyTouch, L"default config (all showTaskbar=true) touches nothing");

    // Repeated enforcement must be a no-op once the state is right.
    const std::vector<bool> mixed = { true, false };
    Check(DecideTaskbarAction(1, mixed, false) == TaskbarAction::Leave &&
              DecideTaskbarAction(0, mixed, true) == TaskbarAction::Leave,
          L"re-enforcing a correct state is a no-op (2s timer is idempotent)");
}
} // namespace

int wmain() {
    ::wprintf(L"dscomp taskbar probe\n");
    ::wprintf(L"====================\n");
    TestMapping();
    TestDecision();
    TestDefaults();
    ::wprintf(L"\n====================\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
