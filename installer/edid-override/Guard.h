// Guard.h - support for the transaction-guarded EDID apply.
//
// The guarded flow exists so a GUI can drive a risky EDID change with ONE
// elevation and a guaranteed auto-revert: this process is the safety authority.
// It applies the override, restarts the monitor, arms a countdown, and reverts
// on its own if nobody confirms - including when the GUI has died or the user
// simply cannot see anything.
#pragma once

#include <Windows.h>

#include <string>

namespace guard {

// --- exit codes (contract with the GUI; do not renumber) --------------------
constexpr int kExitKept = 0;    // user confirmed: override kept
constexpr int kExitNotElevated = 2;    // elevation required but absent
constexpr int kExitApplyFailedReverted = 3;    // write/patch failed, reverted
constexpr int kExitRestartFailedReverted = 4;    // devnode restart failed, reverted
constexpr int kExitBadArgs = 5;    // argument error
constexpr int kExitUserRevert = 10;   // user pressed revert
constexpr int kExitTimeoutRevert = 11;   // countdown expired, auto-reverted

// --- named events (contract with the GUI) -----------------------------------
// The guard signals Armed once the override is live and the countdown starts;
// the GUI then sets exactly one of Keep or Revert.
extern const wchar_t* const kEventArmed;    // DeskSplitEdidArmed
extern const wchar_t* const kEventKeep;     // DeskSplitEdidKeep
extern const wchar_t* const kEventRevert;   // DeskSplitEdidRevert

// --- logging ---------------------------------------------------------------
// Everything goes to stdout AND %ProgramData%\DesktopSplitter\edid-guard.log.
// The GUI can be blind if the display glitches, so the log is the post-mortem.
void LogOpen(const wchar_t* command);
void Log(const wchar_t* fmt, ...);
void LogClose(int exitCode);
std::wstring LogPath();

// --- events ----------------------------------------------------------------
struct GuardEvents {
    HANDLE armed = nullptr;
    HANDLE keep = nullptr;
    HANDLE revert = nullptr;
    bool   usedGlobalNamespace = true;

    // Creates all three manual-reset events with a DACL that lets the
    // unelevated interactive user open and set them. Falls back to the
    // session-local namespace when Global\ is unavailable (an unelevated
    // --dry-run has no SeCreateGlobalPrivilege).
    bool Create();
    void Close();
};

// Builds the shared security descriptor. Caller frees with LocalFree.
// Admins/SYSTEM: full control. Authenticated Users: EVENT_MODIFY_STATE |
// SYNCHRONIZE, which is exactly enough to wait on Armed and set Keep/Revert.
bool IsProcessElevated();

PSECURITY_DESCRIPTOR BuildEventSecurityDescriptor();

// Reads a live event's DACL back and confirms the intended ACEs are present.
// Used by `selftest`, since a real unelevated open cannot be simulated here.
bool VerifyEventDacl(HANDLE eventHandle, std::wstring& detail);

// --- device node -----------------------------------------------------------
// Disable + enable the monitor devnode so the new EDID is re-read without a
// reboot. 'instanceId' is e.g. L"DISPLAY\\PHLC310\\7&224ecef0&0&UID260".
bool RestartDevnode(const std::wstring& instanceId, std::wstring& error);

// Polls CM_Get_DevNode_Status until the device reports problem code 0.
bool WaitDevnodeHealthy(const std::wstring& instanceId, DWORD timeoutMs,
                        std::wstring& error);

// --- feature flag ----------------------------------------------------------
// HKLM\SOFTWARE\DesktopSplitter\HidePhysicalDisplay
bool SetHidePhysicalDisplay(DWORD value);

} // namespace guard
