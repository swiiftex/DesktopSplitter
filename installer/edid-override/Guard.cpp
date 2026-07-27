#include "Guard.h"

#include <sddl.h>
#include <aclapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <cstdarg>
#include <vector>

namespace guard {

const wchar_t* const kEventArmed = L"DeskSplitEdidArmed";
const wchar_t* const kEventKeep = L"DeskSplitEdidKeep";
const wchar_t* const kEventRevert = L"DeskSplitEdidRevert";

namespace {

HANDLE g_logFile = INVALID_HANDLE_VALUE;

std::wstring ProgramDataDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::ExpandEnvironmentStringsW(L"%ProgramData%\\DesktopSplitter", buf,
                                                _countof(buf));
    if (n == 0 || n > _countof(buf)) return L"C:\\ProgramData\\DesktopSplitter";
    return buf;
}

void WriteLogLine(const std::wstring& line) {
    ::fputws(line.c_str(), stdout);
    ::fflush(stdout);
    if (g_logFile == INVALID_HANDLE_VALUE) return;

    // The log is read by humans after a display failure; UTF-8 keeps it
    // openable in anything.
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                           static_cast<int>(line.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (need <= 0) return;
    std::vector<char> utf8(static_cast<size_t>(need));
    ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                          utf8.data(), need, nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                nullptr);
    ::FlushFileBuffers(g_logFile);
}

std::wstring Timestamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t buf[64];
    ::_snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds);
    return buf;
}

bool ProcessIsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION el = {};
    DWORD cb = sizeof(el);
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &el, sizeof(el), &cb);
    ::CloseHandle(token);
    return ok && el.TokenIsElevated != 0;
}

bool SidsEqualToWellKnown(PSID sid, WELL_KNOWN_SID_TYPE type) {
    BYTE  buf[SECURITY_MAX_SID_SIZE] = {};
    DWORD cb = sizeof(buf);
    if (!::CreateWellKnownSid(type, nullptr, buf, &cb)) return false;
    return ::EqualSid(sid, buf) != FALSE;
}

} // namespace

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

std::wstring LogPath() {
    return ProgramDataDir() + L"\\edid-guard.log";
}

void LogOpen(const wchar_t* command) {
    const std::wstring dir = ProgramDataDir();
    ::CreateDirectoryW(dir.c_str(), nullptr);   // harmless when it exists

    g_logFile = ::CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        ::wprintf(L"warning: cannot open log file %s (continuing, stdout only)\n",
                  LogPath().c_str());
    }
    WriteLogLine(L"\n");
    Log(L"===== %s (pid %lu) =====", command, ::GetCurrentProcessId());
    Log(L"log file: %s", LogPath().c_str());
    Log(L"elevated: %s", ProcessIsElevated() ? L"yes" : L"no");
}

void Log(const wchar_t* fmt, ...) {
    wchar_t body[1024];
    va_list args;
    va_start(args, fmt);
    ::_vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);

    std::wstring line = L"[" + Timestamp() + L"] " + body + L"\n";
    WriteLogLine(line);
}

void LogClose(int exitCode) {
    Log(L"exit code %d", exitCode);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------------------------------------------------------
// Event security
// ---------------------------------------------------------------------------

bool IsProcessElevated() { return ProcessIsElevated(); }

PSECURITY_DESCRIPTOR BuildEventSecurityDescriptor() {
    // Admins + SYSTEM: full control (this process, and any recovery tooling).
    // Authenticated Users: EVENT_MODIFY_STATE | SYNCHRONIZE only - exactly
    // enough for the unelevated GUI to wait on Armed and set Keep / Revert,
    // and nothing more (it cannot change the DACL or delete the objects).
    static const wchar_t* const kSddl =
        L"D:P"
        L"(A;;0x1f0003;;;BA)"      // Built-in Administrators: EVENT_ALL_ACCESS
        L"(A;;0x1f0003;;;SY)"      // Local System: EVENT_ALL_ACCESS
        L"(A;;0x00120002;;;AU)";   // Authenticated Users: MODIFY_STATE|SYNCHRONIZE|READ_CONTROL

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return nullptr;
    }
    return sd;
}

bool GuardEvents::Create() {
    PSECURITY_DESCRIPTOR sd = BuildEventSecurityDescriptor();
    if (!sd) {
        Log(L"ERROR: could not build the event security descriptor (0x%08X)",
            ::GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    auto makeOne = [&](const wchar_t* prefix, const wchar_t* name) -> HANDLE {
        const std::wstring full = std::wstring(prefix) + name;
        // Manual reset, initially unsignalled.
        HANDLE h = ::CreateEventW(&sa, TRUE, FALSE, full.c_str());
        if (h) return h;
        if (::GetLastError() != ERROR_ACCESS_DENIED) return nullptr;

        // The object already exists and we are not elevated. CreateEvent always
        // asks for EVENT_ALL_ACCESS, which the DACL only grants to
        // Administrators - and an unelevated token carries that group as
        // deny-only. Re-open with just the rights we actually need instead of
        // silently changing namespace, which would leave a GUI waiting on the
        // other name forever.
        h = ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE | READ_CONTROL, FALSE,
                         full.c_str());
        if (h) {
            Log(L"opened existing event %s with reduced rights (unelevated)",
                full.c_str());
        }
        return h;
    };

    const wchar_t* prefix = L"Global\\";
    armed = makeOne(prefix, kEventArmed);
    if (!armed) {
        const DWORD err = ::GetLastError();
        if (IsProcessElevated()) {
            // Elevated runs MUST use Global\: that is the contract the GUI waits
            // on. Silently dropping to Local\ would leave it waiting forever, so
            // fail loudly instead.
            Log(L"ERROR: cannot create Global\\%s (0x%08lX) while elevated - "
                L"refusing to fall back, the GUI would never see it",
                kEventArmed, err);
            ::LocalFree(sd);
            return false;
        }
        // Unelevated --dry-run has no SeCreateGlobalPrivilege. Fall back to the
        // session-local namespace so the GUI can still integration-test the
        // whole dance harmlessly.
        Log(L"Global\\ namespace unavailable (0x%08lX) and this process is not "
            L"elevated - using the session-local namespace for this dry run",
            err);
        prefix = L"Local\\";
        usedGlobalNamespace = false;
        armed = makeOne(prefix, kEventArmed);
    }
    keep = makeOne(prefix, kEventKeep);
    revert = makeOne(prefix, kEventRevert);

    ::LocalFree(sd);

    if (!armed || !keep || !revert) {
        Log(L"ERROR: could not create the guard events (0x%08X)", ::GetLastError());
        Close();
        return false;
    }
    Log(L"events created in the %s namespace: %s%s, %s%s, %s%s",
        usedGlobalNamespace ? L"Global" : L"Local (unelevated fallback)", prefix,
        kEventArmed, prefix, kEventKeep, prefix, kEventRevert);

    // Clear any stale signal left by an earlier run before we arm.
    ::ResetEvent(armed);
    ::ResetEvent(keep);
    ::ResetEvent(revert);
    return true;
}

void GuardEvents::Close() {
    if (armed) { ::CloseHandle(armed); armed = nullptr; }
    if (keep) { ::CloseHandle(keep); keep = nullptr; }
    if (revert) { ::CloseHandle(revert); revert = nullptr; }
}

bool VerifyEventDacl(HANDLE eventHandle, std::wstring& detail) {
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD r = ::GetSecurityInfo(eventHandle, SE_KERNEL_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &dacl, nullptr, &sd);
    if (r != ERROR_SUCCESS || dacl == nullptr) {
        detail = L"GetSecurityInfo failed";
        if (sd) ::LocalFree(sd);
        return false;
    }

    bool authUsersOk = false;
    bool adminsOk = false;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        LPVOID raw = nullptr;
        if (!::GetAce(dacl, i, &raw)) continue;
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;

        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        PSID sid = reinterpret_cast<PSID>(&ace->SidStart);
        const ACCESS_MASK mask = ace->Mask;

        if (SidsEqualToWellKnown(sid, WinAuthenticatedUserSid)) {
            if ((mask & EVENT_MODIFY_STATE) && (mask & SYNCHRONIZE)) authUsersOk = true;
        } else if (SidsEqualToWellKnown(sid, WinBuiltinAdministratorsSid)) {
            if ((mask & EVENT_MODIFY_STATE) && (mask & SYNCHRONIZE)) adminsOk = true;
        }
    }
    ::LocalFree(sd);

    if (!authUsersOk) {
        detail = L"no Authenticated Users ACE granting EVENT_MODIFY_STATE|SYNCHRONIZE";
        return false;
    }
    if (!adminsOk) {
        detail = L"no Administrators ACE granting EVENT_MODIFY_STATE|SYNCHRONIZE";
        return false;
    }
    detail = L"Authenticated Users and Administrators ACEs present";
    return true;
}

// ---------------------------------------------------------------------------
// Device node restart
// ---------------------------------------------------------------------------

namespace {

bool ChangeDevnodeState(const std::wstring& instanceId, DWORD stateChange,
                        std::wstring& error) {
    HDEVINFO set = ::SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        error = L"SetupDiCreateDeviceInfoList failed";
        return false;
    }
    SP_DEVINFO_DATA dd = {};
    dd.cbSize = sizeof(dd);
    if (!::SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &dd)) {
        error = L"SetupDiOpenDeviceInfo failed (device instance not found)";
        ::SetupDiDestroyDeviceInfoList(set);
        return false;
    }

    SP_PROPCHANGE_PARAMS pcp = {};
    pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pcp.StateChange = stateChange;
    pcp.Scope = DICS_FLAG_GLOBAL;
    pcp.HwProfile = 0;

    bool ok = false;
    if (::SetupDiSetClassInstallParamsW(set, &dd, &pcp.ClassInstallHeader,
                                        sizeof(pcp))) {
        ok = ::SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dd) != FALSE;
        if (!ok) error = L"SetupDiCallClassInstaller(DIF_PROPERTYCHANGE) failed";
    } else {
        error = L"SetupDiSetClassInstallParams failed";
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return ok;
}

} // namespace

bool RestartDevnode(const std::wstring& instanceId, std::wstring& error) {
    Log(L"restarting devnode %s", instanceId.c_str());
    if (!ChangeDevnodeState(instanceId, DICS_DISABLE, error)) {
        Log(L"  disable failed: %s (0x%08X)", error.c_str(), ::GetLastError());
        return false;
    }
    Log(L"  disabled");
    ::Sleep(400);   // let the display stack settle before bringing it back
    if (!ChangeDevnodeState(instanceId, DICS_ENABLE, error)) {
        Log(L"  enable failed: %s (0x%08X)", error.c_str(), ::GetLastError());
        // Try once more: leaving a monitor disabled is the worst outcome here.
        ::Sleep(500);
        if (!ChangeDevnodeState(instanceId, DICS_ENABLE, error)) {
            Log(L"  second enable attempt failed too");
            return false;
        }
    }
    Log(L"  enabled");
    return true;
}

bool WaitDevnodeHealthy(const std::wstring& instanceId, DWORD timeoutMs,
                        std::wstring& error) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        DEVINST inst = 0;
        const CONFIGRET cr = ::CM_Locate_DevNodeW(
            &inst, const_cast<DEVINSTID_W>(instanceId.c_str()),
            CM_LOCATE_DEVNODE_NORMAL);
        if (cr == CR_SUCCESS) {
            ULONG status = 0;
            ULONG problem = 0;
            if (::CM_Get_DevNode_Status(&status, &problem, inst, 0) == CR_SUCCESS) {
                if (problem == 0) {
                    Log(L"  devnode healthy (status 0x%08lX, problem 0)", status);
                    return true;
                }
                error = L"device reports a problem code";
                Log(L"  devnode problem code %lu", problem);
            }
        }
        if (::GetTickCount64() >= deadline) {
            if (error.empty()) error = L"device did not return within the timeout";
            return false;
        }
        ::Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// Feature flag
// ---------------------------------------------------------------------------

bool SetHidePhysicalDisplay(DWORD value) {
    HKEY key = nullptr;
    const LONG r = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DesktopSplitter",
                                     0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                     nullptr);
    if (r != ERROR_SUCCESS) {
        Log(L"ERROR: cannot open HKLM\\SOFTWARE\\DesktopSplitter (0x%08lX)", r);
        return false;
    }
    const LONG w = ::RegSetValueExW(key, L"HidePhysicalDisplay", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&value),
                                    sizeof(value));
    ::RegCloseKey(key);
    if (w != ERROR_SUCCESS) {
        Log(L"ERROR: cannot write HidePhysicalDisplay (0x%08lX)", w);
        return false;
    }
    Log(L"HidePhysicalDisplay = %lu", value);
    return true;
}

} // namespace guard
