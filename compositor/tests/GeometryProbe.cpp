// GeometryProbe.cpp - unit tests for the pure geometry the compositor runs.
//
// Includes the real src/Geometry.h, so these exercise exactly the functions
// CursorConfine.cpp and WindowRescuer.cpp call. Nothing here touches display
// state, hooks or windows, so it is safe to run on a machine in active use.
//
// Exit code 0 = all passed, 1 = at least one failure.

#include "../src/Geometry.h"

#include <cstdio>
#include <cwchar>

using namespace ds;

namespace {

int g_passed = 0;
int g_failed = 0;

RECT R(LONG l, LONG t, LONG r, LONG b) {
    RECT v;
    v.left = l;
    v.top = t;
    v.right = r;
    v.bottom = b;
    return v;
}

POINT P(LONG x, LONG y) {
    POINT v;
    v.x = x;
    v.y = y;
    return v;
}

void Check(bool ok, const wchar_t* name, const wchar_t* detail) {
    if (ok) {
        ++g_passed;
        ::wprintf(L"  pass  %s\n", name);
    } else {
        ++g_failed;
        ::wprintf(L"  FAIL  %s  --  %s\n", name, detail);
    }
}

void CheckPoint(POINT got, POINT want, const wchar_t* name) {
    wchar_t detail[160];
    ::_snwprintf_s(detail, _countof(detail), _TRUNCATE,
                   L"got (%ld,%ld) want (%ld,%ld)", got.x, got.y, want.x, want.y);
    Check(got.x == want.x && got.y == want.y, name, detail);
}

void CheckRect(RECT got, RECT want, const wchar_t* name) {
    wchar_t detail[220];
    ::_snwprintf_s(detail, _countof(detail), _TRUNCATE,
                   L"got (%ld,%ld,%ld,%ld) want (%ld,%ld,%ld,%ld)", got.left,
                   got.top, got.right, got.bottom, want.left, want.top,
                   want.right, want.bottom);
    Check(got.left == want.left && got.top == want.top &&
              got.right == want.right && got.bottom == want.bottom,
          name, detail);
}

void CheckBool(bool got, bool want, const wchar_t* name) {
    wchar_t detail[80];
    ::_snwprintf_s(detail, _countof(detail), _TRUNCATE, L"got %s want %s",
                   got ? L"true" : L"false", want ? L"true" : L"false");
    Check(got == want, name, detail);
}

// ---------------------------------------------------------------------------
// Edge sliding
// ---------------------------------------------------------------------------
//
// Blocked (covered physical monitor) rect: x in [100,300), y in [100,200).
// "Just outside" is therefore x=99 / x=300 / y=99 / y=200.

void TestEdgeSlideCardinal() {
    ::wprintf(L"\nedge slide - 4 cardinal approaches\n");
    const RECT blocked = R(100, 100, 300, 200);

    CheckPoint(SlideAlongEdge(blocked, P(50, 150), P(150, 150)), P(99, 150),
               L"from W: x clamped to 99, y kept");
    CheckPoint(SlideAlongEdge(blocked, P(350, 150), P(250, 150)), P(300, 150),
               L"from E: x clamped to 300, y kept");
    CheckPoint(SlideAlongEdge(blocked, P(200, 50), P(200, 150)), P(200, 99),
               L"from N: y clamped to 99, x kept");
    CheckPoint(SlideAlongEdge(blocked, P(200, 250), P(200, 150)), P(200, 200),
               L"from S: y clamped to 200, x kept");
}

void TestEdgeSlideCorners() {
    ::wprintf(L"\nedge slide - 4 diagonal/corner approaches\n");
    const RECT blocked = R(100, 100, 300, 200);

    // Shallow from NW: crosses y=100 first (t=0.167) while still left of the
    // rect, so the wall actually hit is the LEFT edge -> clamp x.
    CheckPoint(SlideAlongEdge(blocked, P(50, 90), P(150, 150)), P(99, 150),
               L"from NW shallow: enters through left edge");
    // Steep from NW: crosses x=100 first while still above, so the wall hit is
    // the TOP edge -> clamp y, x preserved.
    CheckPoint(SlideAlongEdge(blocked, P(90, 50), P(150, 150)), P(150, 99),
               L"from NW steep: enters through top edge");
    CheckPoint(SlideAlongEdge(blocked, P(350, 90), P(250, 150)), P(300, 150),
               L"from NE: enters through right edge");
    CheckPoint(SlideAlongEdge(blocked, P(310, 50), P(250, 150)), P(250, 99),
               L"from NE steep: enters through top edge");
    CheckPoint(SlideAlongEdge(blocked, P(50, 210), P(150, 150)), P(99, 150),
               L"from SW: enters through left edge");
    CheckPoint(SlideAlongEdge(blocked, P(90, 250), P(150, 150)), P(150, 200),
               L"from SW steep: enters through bottom edge");
    CheckPoint(SlideAlongEdge(blocked, P(350, 210), P(250, 150)), P(300, 150),
               L"from SE: enters through right edge");
    CheckPoint(SlideAlongEdge(blocked, P(310, 250), P(250, 150)), P(250, 200),
               L"from SE steep: enters through bottom edge");

    // Exact diagonal into a corner is a tie; documented to clamp X.
    CheckPoint(SlideAlongEdge(blocked, P(50, 50), P(150, 150)), P(99, 150),
               L"exact NW diagonal tie clamps X");
}

void TestEdgeSlidePreservesFreeAxis() {
    ::wprintf(L"\nedge slide - the free axis really slides (no snap-back)\n");
    const RECT blocked = R(100, 100, 300, 200);

    // Approaching from the west while also moving down: y must follow the
    // movement, not stay at the previous y.
    CheckPoint(SlideAlongEdge(blocked, P(50, 120), P(150, 180)), P(99, 180),
               L"W approach moving down: y follows to 180");
    CheckPoint(SlideAlongEdge(blocked, P(50, 180), P(150, 120)), P(99, 120),
               L"W approach moving up: y follows to 120");
    // Approaching from the north while also moving right.
    CheckPoint(SlideAlongEdge(blocked, P(120, 50), P(280, 150)), P(280, 99),
               L"N approach moving right: x follows to 280");
    CheckPoint(SlideAlongEdge(blocked, P(280, 50), P(120, 150)), P(120, 99),
               L"N approach moving left: x follows to 120");
    // From below, sliding sideways along the bottom edge.
    CheckPoint(SlideAlongEdge(blocked, P(150, 250), P(260, 150)), P(260, 200),
               L"S approach moving right: x follows to 260");
}

void TestEdgeSlidePassThrough() {
    ::wprintf(L"\nedge slide - destinations that must NOT be touched\n");
    const RECT blocked = R(100, 100, 300, 200);

    CheckPoint(SlideAlongEdge(blocked, P(50, 150), P(80, 150)), P(80, 150),
               L"destination left of blocked rect passes through");
    CheckPoint(SlideAlongEdge(blocked, P(200, 50), P(200, 80)), P(200, 80),
               L"destination above blocked rect passes through");
    CheckPoint(SlideAlongEdge(blocked, P(200, 250), P(400, 400)), P(400, 400),
               L"far destination passes through");
    // Boundary: right/bottom are exclusive, so these are already outside.
    CheckPoint(SlideAlongEdge(blocked, P(350, 150), P(300, 150)), P(300, 150),
               L"x == right is outside (half-open rect)");
    CheckPoint(SlideAlongEdge(blocked, P(200, 250), P(200, 200)), P(200, 200),
               L"y == bottom is outside (half-open rect)");
    // Movement between two other monitors that never touches the blocked rect.
    CheckPoint(SlideAlongEdge(blocked, P(-500, -400), P(-100, 900)),
               P(-100, 900), L"negative-coordinate move is untouched");
}

void TestEdgeSlideRecovery() {
    ::wprintf(L"\nedge slide - recovery when the previous point is inside too\n");
    const RECT blocked = R(100, 100, 300, 200);

    // No approach direction to preserve: exit through the nearest edge.
    CheckPoint(SlideAlongEdge(blocked, P(150, 150), P(150, 140)), P(150, 99),
               L"inside->inside exits through nearest (top) edge");
    CheckPoint(SlideAlongEdge(blocked, P(110, 150), P(110, 150)), P(99, 150),
               L"inside near left exits left");
    CheckPoint(SlideAlongEdge(blocked, P(290, 150), P(290, 150)), P(300, 150),
               L"inside near right exits right");
    CheckPoint(SlideAlongEdge(blocked, P(200, 190), P(200, 190)), P(200, 200),
               L"inside near bottom exits bottom");
}

void TestEdgeSlideNegativeCoords() {
    ::wprintf(L"\nedge slide - covered monitor at negative coordinates\n");
    // After the control app makes a virtual monitor primary, the physical
    // monitor can sit at negative coordinates.
    const RECT blocked = R(-5120, -1440, 0, 0);

    CheckPoint(SlideAlongEdge(blocked, P(100, -700), P(-200, -700)), P(0, -700),
               L"approach from the right of a negative rect");
    CheckPoint(SlideAlongEdge(blocked, P(-2000, 100), P(-2000, -300)),
               P(-2000, 0), L"approach from below a negative rect");
    CheckPoint(SlideAlongEdge(blocked, P(-6000, -700), P(-4000, -700)),
               P(-5121, -700), L"approach from the left of a negative rect");
    CheckPoint(SlideAlongEdge(blocked, P(-2000, -2000), P(-2000, -700)),
               P(-2000, -1441), L"approach from above a negative rect");
}

// ---------------------------------------------------------------------------
// Overlap detection for the window rescuer
// ---------------------------------------------------------------------------

void TestOverlap() {
    ::wprintf(L"\nrescue trigger - >= 50%% of the window inside the covered rect\n");
    const RECT covered = R(0, 0, 1000, 1000);

    CheckBool(IsMostlyInside(R(100, 100, 300, 300), covered), true,
              L"fully inside -> rescue");
    CheckBool(IsMostlyInside(R(2000, 2000, 2200, 2200), covered), false,
              L"fully outside -> no rescue");
    // 200x200 window straddling the right edge, exactly half in.
    CheckBool(IsMostlyInside(R(900, 100, 1100, 300), covered), true,
              L"exactly 50% -> rescue (inclusive)");
    // Same window nudged one pixel out: 99/200 inside.
    CheckBool(IsMostlyInside(R(901, 100, 1101, 300), covered), false,
              L"just under 50% -> no rescue");
    // Straddling a corner: 100x100 of a 200x200 window = 25%.
    CheckBool(IsMostlyInside(R(900, 900, 1100, 1100), covered), false,
              L"corner overlap 25% -> no rescue");
    CheckBool(IsMostlyInside(R(0, 0, 0, 0), covered), false,
              L"zero-area window -> no rescue");
    CheckBool(IsMostlyInside(R(500, 500, 400, 400), covered), false,
              L"inverted window rect -> no rescue");

    // Negative-coordinate covered monitor (re-based desktop).
    const RECT negCovered = R(-5120, -1440, 0, 0);
    CheckBool(IsMostlyInside(R(-3000, -800, -2000, -200), negCovered), true,
              L"window inside a negative-coordinate covered rect");
    CheckBool(IsMostlyInside(R(200, 200, 900, 700), negCovered), false,
              L"window on another monitor is left alone");
}

// ---------------------------------------------------------------------------
// Rescue placement
// ---------------------------------------------------------------------------

void TestRescuePlacement() {
    ::wprintf(L"\nrescue placement - centre, preserve size, clamp to work area\n");
    const RECT work = R(0, 0, 2560, 1400);   // 2560x1440 minus a taskbar

    CheckRect(ComputeRescuePlacement(work, R(4000, 4000, 4800, 4600)),
              R(880, 400, 1680, 1000),
              L"800x600 window fits: size preserved, centred");
    CheckRect(ComputeRescuePlacement(work, R(0, 0, 2560, 1400)),
              R(0, 0, 2560, 1400), L"exact fit lands at the work origin");
    CheckRect(ComputeRescuePlacement(work, R(0, 0, 4000, 3000)),
              R(0, 0, 2560, 1400), L"oversized in both axes shrinks to work");
    CheckRect(ComputeRescuePlacement(work, R(0, 0, 4000, 600)),
              R(0, 400, 2560, 1000), L"oversized width only: height preserved");
    CheckRect(ComputeRescuePlacement(work, R(0, 0, 800, 3000)),
              R(880, 0, 1680, 1400), L"oversized height only: width preserved");
    CheckRect(ComputeRescuePlacement(work, R(0, 0, 1, 1)), R(1279, 699, 1280, 700),
              L"1x1 window is centred");

    // Work area not at the origin - the normal case once segments are laid out.
    const RECT offsetWork = R(2560, 0, 5120, 1400);
    CheckRect(ComputeRescuePlacement(offsetWork, R(0, 0, 800, 600)),
              R(3440, 400, 4240, 1000),
              L"offset work area: centred within it, not at 0,0");

    // Re-based desktop: the target virtual monitor at negative coordinates.
    const RECT negWork = R(-2560, -1400, 0, 0);
    CheckRect(ComputeRescuePlacement(negWork, R(0, 0, 800, 600)),
              R(-1680, -1000, -880, -400),
              L"negative work area: centred within it");
    CheckRect(ComputeRescuePlacement(negWork, R(0, 0, 9000, 9000)),
              R(-2560, -1400, 0, 0),
              L"negative work area: oversized clamps to the whole work area");
}

// ---------------------------------------------------------------------------
// ClipCursor eligibility
// ---------------------------------------------------------------------------

void TestUnionIsSingleRect() {
    ::wprintf(L"\nClipCursor eligibility - allowed area must be exactly one rect\n");
    std::vector<RECT> v;

    CheckBool(UnionIsSingleRect(v), false, L"no monitors -> no clip");

    v = { R(0, 0, 100, 100) };
    CheckBool(UnionIsSingleRect(v), true, L"one monitor -> clip is expressible");

    v = { R(0, 0, 100, 100), R(100, 0, 200, 100) };
    CheckBool(UnionIsSingleRect(v), true, L"two flush side-by-side -> one rect");

    v = { R(0, 0, 100, 100), R(150, 0, 250, 100) };
    CheckBool(UnionIsSingleRect(v), false, L"gap between monitors -> not one rect");

    v = { R(0, 0, 100, 100), R(100, 0, 200, 100), R(0, -100, 100, 0) };
    CheckBool(UnionIsSingleRect(v), false, L"L shape (monitor above) -> not one rect");

    // The reported live layout: three virtual monitors in a row plus an HP
    // above one of them. This must NOT take the ClipCursor path.
    v = { R(0, 0, 1706, 1440), R(1706, 0, 3413, 1440), R(3413, 0, 5120, 1440),
          R(1000, -1080, 2920, 0) };
    CheckBool(UnionIsSingleRect(v), false,
              L"3 virtual + 1 real above -> hook-only, no clip");

    v = { R(0, 0, 1706, 1440), R(1706, 0, 3413, 1440), R(3413, 0, 5120, 1440) };
    CheckBool(UnionIsSingleRect(v), true,
              L"3 virtual monitors alone tile one rect");
}

void TestRectBasics() {
    ::wprintf(L"\nrect basics\n");
    const RECT r = R(10, 10, 20, 20);
    CheckBool(RectContains(r, P(10, 10)), true, L"top-left corner is inside");
    CheckBool(RectContains(r, P(19, 19)), true, L"bottom-right-1 is inside");
    CheckBool(RectContains(r, P(20, 15)), false, L"right edge is outside");
    CheckBool(RectContains(r, P(15, 20)), false, L"bottom edge is outside");
    CheckBool(RectContains(r, P(9, 15)), false, L"left-1 is outside");

    std::vector<RECT> mons = { R(0, 0, 100, 100), R(100, 0, 200, 100) };
    CheckBool(PointOnAnyRect(mons, P(150, 50)), true, L"point on second monitor");
    CheckBool(PointOnAnyRect(mons, P(150, 150)), false, L"point in the void");
}

} // namespace

int wmain() {
    ::wprintf(L"dscomp geometry probe\n");
    ::wprintf(L"=====================\n");

    TestRectBasics();
    TestEdgeSlideCardinal();
    TestEdgeSlideCorners();
    TestEdgeSlidePreservesFreeAxis();
    TestEdgeSlidePassThrough();
    TestEdgeSlideRecovery();
    TestEdgeSlideNegativeCoords();
    TestOverlap();
    TestRescuePlacement();
    TestUnionIsSingleRect();

    ::wprintf(L"\n=====================\n");
    ::wprintf(L"%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
