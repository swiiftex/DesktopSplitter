// devcreate.exe - creates / removes the root-enumerated software device node
// that DesktopSplitterVdd binds to (hardware ID Root\DesktopSplitterVdd).
//
// Usage:
//   devcreate create              create a PERSISTENT root device node (default)
//   devcreate create --swdevice   create a transient node with SwDeviceCreate
//                                 and stay resident until Ctrl+C
//   devcreate remove              remove every device node with our hardware ID
//   devcreate status              report whether the node exists
//
// Why two creation paths
// ----------------------
// SwDeviceCreate (swdevice.h, exported from cfgmgr32.dll) is the documented way
// to create a software device, and it is what Microsoft's IddSampleApp uses.
// However a device created that way lives exactly as long as the HSWDEVICE
// handle: as soon as the creating process exits, the node - and therefore all
// virtual monitors - disappears. DesktopSplitter needs the node to survive
// process exit and reboot (the driver persists its monitor configuration and
// replays it at adapter start), so the default `create` path registers a
// persistent root-enumerated node through SetupAPI, the same way `devcon
// install` does. `--swdevice` is kept for quick testing and for parity with the
// IddCx sample.

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <swdevice.h>
#include <devguid.h>

#include <stdio.h>
#include <wchar.h>

#include "DeskSplitProtocol.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "swdevice.lib")   // SwDeviceCreate / SwDeviceClose
#pragma comment(lib, "ole32.lib")

namespace
{
    // Hardware ID as a REG_MULTI_SZ (the trailing L"\0" plus the implicit
    // terminator of the literal give the required double NUL).
    const wchar_t kHardwareIdMultiSz[] = DESKSPLIT_HARDWARE_ID L"\0";
    const wchar_t kHardwareId[] = DESKSPLIT_HARDWARE_ID;

    const wchar_t kInstanceId[] = L"DesktopSplitterVdd";
    const wchar_t kDeviceDescription[] = L"DesktopSplitter Virtual Display";

    HANDLE g_StopEvent = nullptr;

    void PrintLastError(const wchar_t* What)
    {
        const DWORD Error = GetLastError();
        wprintf(L"  %s failed: 0x%08X (%u)\n", What, Error, Error);
    }

    bool IsElevated()
    {
        HANDLE Token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        {
            return false;
        }

        TOKEN_ELEVATION Elevation = {};
        DWORD Size = sizeof(Elevation);
        const bool Ok = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Size) != FALSE;
        CloseHandle(Token);

        return Ok && Elevation.TokenIsElevated != 0;
    }

    // Does this device info element carry our hardware ID?
    bool HasOurHardwareId(HDEVINFO DevInfo, SP_DEVINFO_DATA* pDevInfoData)
    {
        BYTE Buffer[1024] = {};
        DWORD RequiredSize = 0;
        DWORD PropertyType = 0;

        if (!SetupDiGetDeviceRegistryPropertyW(
                DevInfo,
                pDevInfoData,
                SPDRP_HARDWAREID,
                &PropertyType,
                Buffer,
                sizeof(Buffer),
                &RequiredSize))
        {
            return false;
        }

        for (const wchar_t* Id = reinterpret_cast<const wchar_t*>(Buffer); *Id != L'\0'; Id += wcslen(Id) + 1)
        {
            if (_wcsicmp(Id, kHardwareId) == 0)
            {
                return true;
            }
        }

        return false;
    }

    // Walks the ROOT enumerator and invokes Visit() for every node carrying our
    // hardware ID. Returns the number of matches.
    template <typename TVisitor>
    int ForEachMatchingDevice(TVisitor Visit)
    {
        HDEVINFO DevInfo = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            PrintLastError(L"SetupDiGetClassDevs");
            return -1;
        }

        int Matches = 0;
        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        for (DWORD Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData); ++Index)
        {
            if (!HasOurHardwareId(DevInfo, &DevInfoData))
            {
                continue;
            }

            ++Matches;
            Visit(DevInfo, &DevInfoData);
        }

        SetupDiDestroyDeviceInfoList(DevInfo);
        return Matches;
    }

    int CommandStatus()
    {
        const int Matches = ForEachMatchingDevice([](HDEVINFO DevInfo, SP_DEVINFO_DATA* pDevInfoData)
        {
            wchar_t InstanceId[MAX_DEVICE_ID_LEN] = {};
            if (SetupDiGetDeviceInstanceIdW(DevInfo, pDevInfoData, InstanceId, ARRAYSIZE(InstanceId), nullptr))
            {
                wprintf(L"  %s\n", InstanceId);
            }
        });

        if (Matches < 0)
        {
            return 1;
        }

        wprintf(L"%d device node(s) with hardware ID %s\n", Matches, kHardwareId);
        return (Matches > 0) ? 0 : 2;
    }

    // ---- Persistent creation (SetupAPI, devcon-style) --------------------
    int CommandCreatePersistent()
    {
        // Refuse to create a second node.
        const int Existing = ForEachMatchingDevice([](HDEVINFO, SP_DEVINFO_DATA*) {});
        if (Existing > 0)
        {
            wprintf(L"Device node already exists (%d); nothing to do.\n", Existing);
            return 0;
        }

        GUID ClassGuid = GUID_DEVCLASS_DISPLAY;

        HDEVINFO DevInfo = SetupDiCreateDeviceInfoList(&ClassGuid, nullptr);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            PrintLastError(L"SetupDiCreateDeviceInfoList");
            return 1;
        }

        int Result = 1;

        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        if (!SetupDiCreateDeviceInfoW(
                DevInfo,
                L"Display",
                &ClassGuid,
                kDeviceDescription,
                nullptr,
                DICD_GENERATE_ID,
                &DevInfoData))
        {
            PrintLastError(L"SetupDiCreateDeviceInfo");
            goto Cleanup;
        }

        if (!SetupDiSetDeviceRegistryPropertyW(
                DevInfo,
                &DevInfoData,
                SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(kHardwareIdMultiSz),
                static_cast<DWORD>(sizeof(kHardwareIdMultiSz))))
        {
            PrintLastError(L"SetupDiSetDeviceRegistryProperty(SPDRP_HARDWAREID)");
            goto Cleanup;
        }

        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
        {
            PrintLastError(L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
            goto Cleanup;
        }

        wprintf(L"Root device node registered.\n");

        {
            // Bind the best matching driver from the driver store. If the
            // package has not been staged yet this fails harmlessly; PnP binds
            // the driver as soon as pnputil /add-driver /install runs.
            BOOL RebootRequired = FALSE;
            if (DiInstallDevice(nullptr, DevInfo, &DevInfoData, nullptr, 0, &RebootRequired))
            {
                wprintf(L"Driver installed on the new node.%s\n", RebootRequired ? L" (reboot required)" : L"");
            }
            else
            {
                const DWORD Error = GetLastError();
                wprintf(L"  DiInstallDevice returned 0x%08X - the node exists but has no driver yet.\n", Error);
                wprintf(L"  Run: pnputil /add-driver DesktopSplitterVdd.inf /install\n");
            }
        }

        Result = 0;

    Cleanup:
        SetupDiDestroyDeviceInfoList(DevInfo);
        return Result;
    }

    // ---- Transient creation (SwDeviceCreate) ----------------------------
    void WINAPI SwDeviceCreationCallback(
        HSWDEVICE hSwDevice,
        HRESULT CreateResult,
        PVOID pContext,
        PCWSTR pszDeviceInstanceId)
    {
        UNREFERENCED_PARAMETER(hSwDevice);

        if (SUCCEEDED(CreateResult))
        {
            wprintf(L"Software device created: %s\n", (pszDeviceInstanceId != nullptr) ? pszDeviceInstanceId : L"?");
        }
        else
        {
            wprintf(L"SwDeviceCreate callback reported 0x%08X\n", CreateResult);
        }

        SetEvent(*reinterpret_cast<HANDLE*>(pContext));
    }

    BOOL WINAPI ConsoleCtrlHandler(DWORD CtrlType)
    {
        UNREFERENCED_PARAMETER(CtrlType);

        if (g_StopEvent != nullptr)
        {
            SetEvent(g_StopEvent);
        }

        return TRUE;
    }

    int CommandCreateSwDevice()
    {
        HANDLE CreatedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (CreatedEvent == nullptr)
        {
            PrintLastError(L"CreateEvent");
            return 1;
        }

        g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_StopEvent == nullptr)
        {
            PrintLastError(L"CreateEvent");
            CloseHandle(CreatedEvent);
            return 1;
        }

        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

        SW_DEVICE_CREATE_INFO CreateInfo = {};
        CreateInfo.cbSize = sizeof(CreateInfo);
        CreateInfo.pszInstanceId = kInstanceId;
        CreateInfo.pszzHardwareIds = kHardwareIdMultiSz;
        CreateInfo.pszzCompatibleIds = kHardwareIdMultiSz;
        CreateInfo.pszDeviceDescription = kDeviceDescription;
        CreateInfo.CapabilityFlags =
            SWDeviceCapabilitiesRemovable |
            SWDeviceCapabilitiesSilentInstall |
            SWDeviceCapabilitiesDriverRequired;

        HSWDEVICE hSwDevice = nullptr;
        const HRESULT hr = SwDeviceCreate(
            L"DesktopSplitterVdd",
            L"HTREE\\ROOT\\0",
            &CreateInfo,
            0,
            nullptr,
            SwDeviceCreationCallback,
            &CreatedEvent,
            &hSwDevice);

        if (FAILED(hr))
        {
            wprintf(L"SwDeviceCreate failed: 0x%08X\n", hr);
            CloseHandle(CreatedEvent);
            CloseHandle(g_StopEvent);
            return 1;
        }

        if (WaitForSingleObject(CreatedEvent, 30000) != WAIT_OBJECT_0)
        {
            wprintf(L"Timed out waiting for the software device to be created.\n");
        }

        wprintf(L"Software device is alive. It disappears when this process exits.\n");
        wprintf(L"Press Ctrl+C to remove it.\n");

        WaitForSingleObject(g_StopEvent, INFINITE);

        SwDeviceClose(hSwDevice);
        CloseHandle(CreatedEvent);
        CloseHandle(g_StopEvent);

        wprintf(L"Software device removed.\n");
        return 0;
    }

    int CommandRemove()
    {
        bool AnyFailure = false;

        const int Matches = ForEachMatchingDevice([&AnyFailure](HDEVINFO DevInfo, SP_DEVINFO_DATA* pDevInfoData)
        {
            wchar_t InstanceId[MAX_DEVICE_ID_LEN] = {};
            SetupDiGetDeviceInstanceIdW(DevInfo, pDevInfoData, InstanceId, ARRAYSIZE(InstanceId), nullptr);

            BOOL RebootRequired = FALSE;
            if (DiUninstallDevice(nullptr, DevInfo, pDevInfoData, 0, &RebootRequired))
            {
                wprintf(L"Removed %s%s\n", InstanceId, RebootRequired ? L" (reboot required)" : L"");
            }
            else
            {
                AnyFailure = true;
                wprintf(L"Failed to remove %s: 0x%08X\n", InstanceId, GetLastError());
            }
        });

        if (Matches < 0)
        {
            return 1;
        }
        if (Matches == 0)
        {
            wprintf(L"No device node with hardware ID %s found.\n", kHardwareId);
            return 0;
        }

        return AnyFailure ? 1 : 0;
    }

    void PrintUsage()
    {
        wprintf(L"devcreate - DesktopSplitter virtual display device node helper\n\n");
        wprintf(L"  devcreate create [--swdevice]   create the root device node\n");
        wprintf(L"  devcreate remove                remove the root device node\n");
        wprintf(L"  devcreate status                list matching device nodes\n\n");
        wprintf(L"  --swdevice uses SwDeviceCreate and keeps the node alive only while\n");
        wprintf(L"  this process runs. Without it a persistent node is registered.\n");
    }
}

int __cdecl wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    const bool WantSwDevice = (argc > 2) && (_wcsicmp(argv[2], L"--swdevice") == 0);

    if (_wcsicmp(argv[1], L"status") == 0)
    {
        return CommandStatus();
    }

    if (!IsElevated())
    {
        wprintf(L"devcreate must be run from an elevated (administrator) prompt.\n");
        return 1;
    }

    if (_wcsicmp(argv[1], L"create") == 0)
    {
        return WantSwDevice ? CommandCreateSwDevice() : CommandCreatePersistent();
    }

    if (_wcsicmp(argv[1], L"remove") == 0)
    {
        return CommandRemove();
    }

    PrintUsage();
    return 1;
}
