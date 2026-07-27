#include "CursorConfine.h"
#include "MonitorUtil.h"
#include "Geometry.h"
#include "Log.h"

#include <atomic>

namespace ds {

namespace {

// Hook state. Refresh() writes it under the exclusive lock from the UI thread;
// the hook procedure reads it under the shared lock. 'g_lastGood' is hot on
// every mouse move, so it is a lock-free packed atomic instead.
SRWLOCK           g_lock = SRWLOCK_INIT;
RECT              g_coveredRect = {};      // the monitor dscomp hides
bool              g_haveCovered = false;
std::vector<RECT> g_allMonitorRects;       // for validating a slid-to point
RECT              g_clipRect = {};
bool              g_useClip = false;       // only when the allowed area is 1 rect
bool              g_clipApplied = false;
HHOOK             g_hook = nullptr;

std::atomic<uint64_t> g_lastGood{0};
std::atomic<bool>     g_haveLastGood{false};
std::atomic<uint64_t> g_blockedMoves{0};

uint64_t PackPoint(POINT p) {
    return static_cast<uint64_t>(static_cast<uint32_t>(p.x)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(p.y)) << 32);
}

POINT UnpackPoint(uint64_t v) {
    POINT p;
    p.x = static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFFu));
    p.y = static_cast<LONG>(static_cast<int32_t>((v >> 32) & 0xFFFFFFFFu));
    return p;
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return ::CallNextHookEx(g_hook, code, wParam, lParam);

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    // Ignore our own SetCursorPos below (it comes back as an injected move).
    if (ms == nullptr || (ms->flags & LLMHF_INJECTED) != 0 || wParam != WM_MOUSEMOVE) {
        return ::CallNextHookEx(g_hook, code, wParam, lParam);
    }

    const POINT dest = ms->pt;

    // Reused across calls: the hook procedure only ever runs on the thread that
    // installed it, so this needs no synchronisation and no per-move allocation.
    static std::vector<RECT> s_monitors;

    RECT covered = {};
    bool haveCovered = false;
    ::AcquireSRWLockShared(&g_lock);
    haveCovered = g_haveCovered;
    covered = g_coveredRect;
    if (haveCovered && RectContains(covered, dest)) {
        s_monitors.assign(g_allMonitorRects.begin(), g_allMonitorRects.end());
    }
    ::ReleaseSRWLockShared(&g_lock);

    if (!haveCovered || !RectContains(covered, dest)) {
        // Allowed. Remember it as the approach point for the next blocked move.
        g_lastGood.store(PackPoint(dest), std::memory_order_relaxed);
        g_haveLastGood.store(true, std::memory_order_relaxed);
        return ::CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // The destination is inside the covered monitor. Slide along its edge:
    // clamp only the axis the movement entered through, keep the other.
    POINT prev = dest;
    if (g_haveLastGood.load(std::memory_order_relaxed)) {
        prev = UnpackPoint(g_lastGood.load(std::memory_order_relaxed));
    } else {
        ::GetCursorPos(&prev);
    }

    POINT corrected = SlideAlongEdge(covered, prev, dest);

    // Sliding can land in a gap between monitors when displays are not flush.
    // Fall back to holding the other axis, then to the approach point itself.
    if (!s_monitors.empty() && !PointOnAnyRect(s_monitors, corrected)) {
        POINT hold = corrected;
        if (corrected.x != dest.x) hold.y = prev.y;   // X was clamped
        else                       hold.x = prev.x;   // Y was clamped
        if (PointOnAnyRect(s_monitors, hold)) {
            corrected = hold;
        } else if (PointOnAnyRect(s_monitors, prev) && !RectContains(covered, prev)) {
            corrected = prev;
        }
    }

    g_lastGood.store(PackPoint(corrected), std::memory_order_relaxed);
    g_haveLastGood.store(true, std::memory_order_relaxed);
    g_blockedMoves.fetch_add(1, std::memory_order_relaxed);

    ::SetCursorPos(corrected.x, corrected.y);
    return 1;   // swallow the move that would have entered the covered monitor
}

} // namespace

CursorConfiner::~CursorConfiner() {
    Release();
}

bool CursorConfiner::Configure(const std::wstring& physicalDevice) {
    m_physicalDevice = physicalDevice;
    m_configured = true;
    Refresh();
    return true;
}

void CursorConfiner::Refresh() {
    if (!m_configured) return;

    std::vector<MonitorEntry> all;
    EnumerateMonitors(all);

    RECT covered = {};
    bool haveCovered = false;
    std::vector<RECT> allRects;
    std::vector<RECT> allowed;      // everything except the covered monitor
    allRects.reserve(all.size());
    allowed.reserve(all.size());

    for (const MonitorEntry& e : all) {
        allRects.push_back(e.rect);
        if (::_wcsicmp(e.device.c_str(), m_physicalDevice.c_str()) == 0) {
            covered = e.rect;
            haveCovered = true;
        } else {
            allowed.push_back(e.rect);
        }
    }

    // ClipCursor is a single rectangle, so it can only ever express the allowed
    // area when that area IS a single rectangle. With the physical monitor
    // covered and other real monitors free to sit above or below the virtual
    // row, it almost never is - and a clip that does not match reality fights
    // the hook and snaps the cursor back. Drop it unless it fits exactly.
    RECT clip = {};
    bool useClip = false;
    if (!allowed.empty() && UnionIsSingleRect(allowed)) {
        useClip = UnionOfRects(allowed, clip);
    }

    bool hadClip = false;
    ::AcquireSRWLockExclusive(&g_lock);
    g_coveredRect = covered;
    g_haveCovered = haveCovered;
    g_allMonitorRects = allRects;
    g_clipRect = clip;
    g_useClip = useClip;
    hadClip = g_clipApplied;
    ::ReleaseSRWLockExclusive(&g_lock);

    if (!haveCovered) {
        DS_WARN(L"covered monitor '%s' not found; cursor confinement disabled",
                m_physicalDevice.c_str());
    } else {
        DS_LOG(L"confinement: excluding '%s' (%ld,%ld)-(%ld,%ld); %zu monitor(s) "
               L"reachable; ClipCursor %s",
               m_physicalDevice.c_str(), covered.left, covered.top, covered.right,
               covered.bottom, allowed.size(),
               useClip ? L"in use (allowed area is one rect)" : L"not used");
    }

    if (useClip) {
        Reapply();
    } else if (hadClip) {
        ::ClipCursor(nullptr);
        ::AcquireSRWLockExclusive(&g_lock);
        g_clipApplied = false;
        ::ReleaseSRWLockExclusive(&g_lock);
        DS_VERB(L"released a previously applied cursor clip");
    }

    // Re-seed the approach point; a re-base invalidates the old coordinates.
    POINT cur = {};
    if (::GetCursorPos(&cur)) {
        if (haveCovered && RectContains(covered, cur)) {
            cur = NearestOutside(covered, cur);
            ::SetCursorPos(cur.x, cur.y);
            DS_VERB(L"cursor was inside the covered monitor; moved to (%ld,%ld)",
                    cur.x, cur.y);
        }
        g_lastGood.store(PackPoint(cur), std::memory_order_relaxed);
        g_haveLastGood.store(true, std::memory_order_relaxed);
    }
}

void CursorConfiner::Reapply() {
    RECT r = {};
    bool use = false;
    ::AcquireSRWLockShared(&g_lock);
    r = g_clipRect;
    use = g_useClip;
    ::ReleaseSRWLockShared(&g_lock);
    if (!use) return;   // hook-only mode: never touch another app's clip

    RECT current = {};
    if (::GetClipCursor(&current) && current.left == r.left &&
        current.top == r.top && current.right == r.right &&
        current.bottom == r.bottom) {
        return;   // already ours
    }
    if (::ClipCursor(&r)) {
        ::AcquireSRWLockExclusive(&g_lock);
        g_clipApplied = true;
        ::ReleaseSRWLockExclusive(&g_lock);
    } else {
        DS_VERB(L"ClipCursor failed: %s",
                HrString(HRESULT_FROM_WIN32(::GetLastError())));
    }
}

bool CursorConfiner::InstallHook() {
    if (g_hook) return true;
    g_hook = ::SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc,
                                 ::GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        DS_WARN(L"SetWindowsHookEx(WH_MOUSE_LL) failed: %s - running WITHOUT "
                L"cursor confinement (the covered monitor stays reachable)",
                HrString(HRESULT_FROM_WIN32(::GetLastError())));
        return false;
    }
    DS_VERB(L"low-level mouse hook installed");
    return true;
}

bool CursorConfiner::HookInstalled() const {
    return g_hook != nullptr;
}

void CursorConfiner::Release() {
    if (g_hook) {
        ::UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    bool hadClip = false;
    ::AcquireSRWLockExclusive(&g_lock);
    hadClip = g_clipApplied;
    g_clipApplied = false;
    g_useClip = false;
    g_haveCovered = false;
    g_allMonitorRects.clear();
    ::ReleaseSRWLockExclusive(&g_lock);

    if (hadClip) ::ClipCursor(nullptr);

    const uint64_t blocked = g_blockedMoves.exchange(0, std::memory_order_relaxed);
    if (blocked) DS_VERB(L"confinement blocked %llu move(s)", blocked);
    m_configured = false;
}

} // namespace ds
