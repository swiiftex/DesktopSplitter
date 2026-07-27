// Geometry.h - pure, side-effect-free rect/point math.
//
// Everything here is deterministic and depends only on RECT/POINT, so it can be
// unit-tested off-machine. tests/GeometryProbe.cpp includes this header
// directly and exercises the same functions the compositor runs.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <vector>

namespace ds {

// Rects are half-open: [left, right) x [top, bottom), matching Win32 monitor
// rects. "Just outside" a rect on the left is therefore left-1, and on the
// right it is right (the first column that is no longer covered).

inline bool RectIsEmpty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

inline bool RectContains(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

inline long long RectArea(const RECT& r) {
    if (RectIsEmpty(r)) return 0;
    return static_cast<long long>(r.right - r.left) *
           static_cast<long long>(r.bottom - r.top);
}

inline long long IntersectArea(const RECT& a, const RECT& b) {
    const LONG l = (a.left > b.left) ? a.left : b.left;
    const LONG t = (a.top > b.top) ? a.top : b.top;
    const LONG r = (a.right < b.right) ? a.right : b.right;
    const LONG bo = (a.bottom < b.bottom) ? a.bottom : b.bottom;
    if (r <= l || bo <= t) return 0;
    return static_cast<long long>(r - l) * static_cast<long long>(bo - t);
}

// True when at least 'numerator/denominator' of 'window' lies inside 'region'.
// Default is "at least half".
inline bool IsMostlyInside(const RECT& window, const RECT& region,
                           long long numerator = 1, long long denominator = 2) {
    const long long area = RectArea(window);
    if (area <= 0) return false;
    return IntersectArea(window, region) * denominator >= area * numerator;
}

inline bool UnionOfRects(const std::vector<RECT>& rects, RECT& out) {
    bool any = false;
    for (const RECT& r : rects) {
        if (RectIsEmpty(r)) continue;
        if (!any) {
            out = r;
            any = true;
        } else {
            if (r.left < out.left)     out.left = r.left;
            if (r.top < out.top)       out.top = r.top;
            if (r.right > out.right)   out.right = r.right;
            if (r.bottom > out.bottom) out.bottom = r.bottom;
        }
    }
    return any;
}

// True when the rects exactly tile their bounding box, i.e. the union is one
// solid rectangle with no gaps. Exact for non-overlapping monitor rects (an
// area comparison suffices because monitor rects never overlap).
inline bool UnionIsSingleRect(const std::vector<RECT>& rects) {
    RECT bounds = {};
    if (!UnionOfRects(rects, bounds)) return false;
    long long covered = 0;
    for (const RECT& r : rects) covered += RectArea(r);
    return covered == RectArea(bounds);
}

inline bool PointOnAnyRect(const std::vector<RECT>& rects, POINT p) {
    for (const RECT& r : rects) {
        if (RectContains(r, p)) return true;
    }
    return false;
}

// Nearest point outside 'r', moving straight out through the closest edge.
// Only used to recover when the cursor is already inside the blocked region.
inline POINT NearestOutside(const RECT& r, POINT p) {
    if (!RectContains(r, p)) return p;
    const long long dLeft   = static_cast<long long>(p.x) - (r.left - 1);
    const long long dRight  = static_cast<long long>(r.right) - p.x;
    const long long dTop    = static_cast<long long>(p.y) - (r.top - 1);
    const long long dBottom = static_cast<long long>(r.bottom) - p.y;

    POINT q = p;
    long long best = dLeft;
    q.x = r.left - 1;
    if (dRight < best) {
        best = dRight;
        q = p;
        q.x = r.right;
    }
    if (dTop < best) {
        best = dTop;
        q = p;
        q.y = r.top - 1;
    }
    if (dBottom < best) {
        q = p;
        q.y = r.bottom;
    }
    return q;
}

// How a movement along one axis enters the blocked slab [lo, hi).
struct SlabEntry {
    bool   crosses = false;  // the movement actually enters the slab on this axis
    double t = -1.0;         // parametric position of the crossing, in [0, 1]
    LONG   outside = 0;      // coordinate just outside the slab, on the approach side
};

// 'prev' -> 'dest' along one axis against the blocked slab [lo, hi).
// When prev already lies inside the slab the constraint is satisfied at t=0 and
// this axis cannot be the one that blocks, so 'crosses' is false.
inline SlabEntry AxisSlabEntry(LONG prev, LONG dest, LONG lo, LONG hi) {
    SlabEntry e;
    if (prev >= lo && prev < hi) return e;             // already inside this slab
    if (prev < lo) {
        if (dest < lo) return e;                       // never reaches the slab
        e.outside = lo - 1;
        e.t = (dest == prev) ? 0.0
                             : static_cast<double>(lo - prev) /
                                   static_cast<double>(dest - prev);
    } else {                                           // prev >= hi
        if (dest >= hi) return e;                      // never reaches the slab
        e.outside = hi;
        e.t = (dest == prev) ? 0.0
                             : static_cast<double>(hi - prev) /
                                   static_cast<double>(dest - prev);
    }
    e.crosses = true;
    return e;
}

// Edge-slide: 'dest' lands inside the blocked rect, so clamp ONLY the axis the
// movement actually entered through and keep the other axis untouched. That is
// what makes skimming past the blocked monitor feel like a normal screen edge
// instead of a snap-back.
//
// Entering a rect means crossing both slabs; the wall you actually hit is the
// one crossed LAST (max t), because that is the crossing that completes the
// containment. A pure diagonal into a corner is a tie and clamps X.
//
// If 'prev' is itself inside the blocked rect (shouldn't happen once tracking
// is warm, but can right after startup or an external SetCursorPos) there is no
// approach direction to preserve, so exit through the nearest edge.
inline POINT SlideAlongEdge(const RECT& blocked, POINT prev, POINT dest) {
    if (RectIsEmpty(blocked)) return dest;
    if (!RectContains(blocked, dest)) return dest;

    const SlabEntry ex = AxisSlabEntry(prev.x, dest.x, blocked.left, blocked.right);
    const SlabEntry ey = AxisSlabEntry(prev.y, dest.y, blocked.top, blocked.bottom);

    POINT q = dest;
    if (!ex.crosses && !ey.crosses) {
        return NearestOutside(blocked, dest);          // prev was inside too
    }
    if (ex.crosses && ey.crosses) {
        if (ex.t >= ey.t) q.x = ex.outside;
        else              q.y = ey.outside;
    } else if (ex.crosses) {
        q.x = ex.outside;
    } else {
        q.y = ey.outside;
    }
    return q;
}

// Where to put a rescued window inside 'work': keep its size when it fits,
// otherwise shrink to the work area, then centre and clamp.
inline RECT ComputeRescuePlacement(const RECT& work, const RECT& windowRect) {
    const LONG workW = work.right - work.left;
    const LONG workH = work.bottom - work.top;
    LONG w = windowRect.right - windowRect.left;
    LONG h = windowRect.bottom - windowRect.top;
    if (w > workW) w = workW;
    if (h > workH) h = workH;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    LONG x = work.left + (workW - w) / 2;
    LONG y = work.top + (workH - h) / 2;
    if (x < work.left)         x = work.left;
    if (x > work.right - w)    x = work.right - w;
    if (y < work.top)          y = work.top;
    if (y > work.bottom - h)   y = work.bottom - h;

    RECT out;
    out.left = x;
    out.top = y;
    out.right = x + w;
    out.bottom = y + h;
    return out;
}

} // namespace ds
