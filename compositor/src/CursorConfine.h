// CursorConfine.h - keeps the OS cursor off the covered physical monitor.
//
// Exclusion-based, not inclusion-based: the only forbidden area is the covered
// physical monitor's desktop rect. Every other monitor - virtual or real, in
// any direction - stays freely reachable. See README.md for the rationale.
#pragma once

#include "Common.h"

namespace ds {

class CursorConfiner {
public:
    CursorConfiner() = default;
    ~CursorConfiner();

    CursorConfiner(const CursorConfiner&) = delete;
    CursorConfiner& operator=(const CursorConfiner&) = delete;

    // 'physicalDevice' is the monitor the compositor covers - the single
    // excluded region. Resolves display state and applies the policy.
    bool Configure(const std::wstring& physicalDevice);

    // Re-resolves monitor rects and re-evaluates the ClipCursor decision.
    // Must be called on WM_DISPLAYCHANGE: making a virtual monitor primary
    // re-bases virtual-desktop coordinates, so every cached rect moves.
    void Refresh();

    // Cheap re-assert from the 2 s timer. No-op unless we are in the rare
    // single-rect ClipCursor mode (other apps can steal the clip).
    void Reapply();

    // Installs the WH_MOUSE_LL hook. Must be called on a thread that pumps
    // messages; the hook procedure runs on that thread. Returns false if the
    // hook could not be installed - the caller carries on unconfined.
    bool InstallHook();

    bool HookInstalled() const;

    // Unhooks and drops any clip we applied.
    void Release();

private:
    std::wstring m_physicalDevice;
    bool         m_configured = false;
};

} // namespace ds
