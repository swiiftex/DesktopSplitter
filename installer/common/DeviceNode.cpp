#include "DeviceNode.h"

#include <newdev.h>
#include <devguid.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace
{
    // Does this device info element carry the given hardware ID?
    bool HasHardwareId(HDEVINFO DevInfo, SP_DEVINFO_DATA* pDevInfoData, const wchar_t* HardwareId)
    {
        BYTE Buffer[2048] = {};
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

        // REG_MULTI_SZ: walk each ID in the list.
        for (const wchar_t* Id = reinterpret_cast<const wchar_t*>(Buffer);
             *Id != L'\0';
             Id += wcslen(Id) + 1)
        {
            if (_wcsicmp(Id, HardwareId) == 0)
            {
                return true;
            }
        }

        return false;
    }
}

namespace DeskSplit
{
    int CountDeviceNodes(const wchar_t* HardwareId)
    {
        HDEVINFO DevInfo = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            return -1;
        }

        int Matches = 0;
        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        for (DWORD Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData); ++Index)
        {
            if (HasHardwareId(DevInfo, &DevInfoData, HardwareId))
            {
                ++Matches;
            }
        }

        SetupDiDestroyDeviceInfoList(DevInfo);
        return Matches;
    }

    _Use_decl_annotations_
    bool FindDeviceNode(const wchar_t* HardwareId, DeviceNodeInfo* pInfo)
    {
        ZeroMemory(pInfo, sizeof(*pInfo));

        HDEVINFO DevInfo = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool Found = false;
        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        for (DWORD Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData); ++Index)
        {
            if (!HasHardwareId(DevInfo, &DevInfoData, HardwareId))
            {
                continue;
            }

            SetupDiGetDeviceInstanceIdW(
                DevInfo,
                &DevInfoData,
                pInfo->InstanceId,
                static_cast<DWORD>(ARRAYSIZE(pInfo->InstanceId)),
                nullptr);

            pInfo->DevInst = DevInfoData.DevInst;

            ULONG Status = 0;
            ULONG Problem = 0;
            if (CM_Get_DevNode_Status(&Status, &Problem, DevInfoData.DevInst, 0) == CR_SUCCESS)
            {
                pInfo->Status = Status;
                pInfo->Problem = Problem;
                pInfo->HasStatus = true;
            }

            Found = true;
            break;
        }

        SetupDiDestroyDeviceInfoList(DevInfo);
        return Found;
    }

    _Use_decl_annotations_
    DWORD CreateRootDeviceNode(
        const wchar_t* HardwareId,
        const wchar_t* HardwareIdMultiSz,
        DWORD HardwareIdMultiSzBytes,
        const wchar_t* Description,
        bool* pAlreadyExisted,
        bool* pNodeCreated,
        bool* pRebootRequired)
    {
        *pAlreadyExisted = false;
        *pNodeCreated = false;
        *pRebootRequired = false;

        const int Existing = CountDeviceNodes(HardwareId);
        if (Existing > 0)
        {
            *pAlreadyExisted = true;
            return ERROR_SUCCESS;
        }

        GUID ClassGuid = GUID_DEVCLASS_DISPLAY;

        HDEVINFO DevInfo = SetupDiCreateDeviceInfoList(&ClassGuid, nullptr);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            return GetLastError();
        }

        DWORD Result = ERROR_SUCCESS;

        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        if (!SetupDiCreateDeviceInfoW(
                DevInfo,
                L"Display",
                &ClassGuid,
                Description,
                nullptr,
                DICD_GENERATE_ID,
                &DevInfoData))
        {
            Result = GetLastError();
        }
        else if (!SetupDiSetDeviceRegistryPropertyW(
                     DevInfo,
                     &DevInfoData,
                     SPDRP_HARDWAREID,
                     reinterpret_cast<const BYTE*>(HardwareIdMultiSz),
                     HardwareIdMultiSzBytes))
        {
            Result = GetLastError();
        }
        else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
        {
            Result = GetLastError();
        }
        else
        {
            // The node now exists even if driver binding below fails.
            *pNodeCreated = true;

            BOOL Reboot = FALSE;
            if (!DiInstallDevice(nullptr, DevInfo, &DevInfoData, nullptr, 0, &Reboot))
            {
                Result = GetLastError();
            }

            *pRebootRequired = (Reboot != FALSE);
        }

        SetupDiDestroyDeviceInfoList(DevInfo);
        return Result;
    }

    _Use_decl_annotations_
    DWORD RemoveDeviceNodes(const wchar_t* HardwareId, int* pRemovedCount, bool* pRebootRequired)
    {
        *pRemovedCount = 0;
        *pRebootRequired = false;

        HDEVINFO DevInfo = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
        if (DevInfo == INVALID_HANDLE_VALUE)
        {
            return GetLastError();
        }

        DWORD Result = ERROR_SUCCESS;

        SP_DEVINFO_DATA DevInfoData = {};
        DevInfoData.cbSize = sizeof(DevInfoData);

        for (DWORD Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData); ++Index)
        {
            if (!HasHardwareId(DevInfo, &DevInfoData, HardwareId))
            {
                continue;
            }

            BOOL Reboot = FALSE;
            if (DiUninstallDevice(nullptr, DevInfo, &DevInfoData, 0, &Reboot))
            {
                ++(*pRemovedCount);
                if (Reboot)
                {
                    *pRebootRequired = true;
                }
            }
            else
            {
                Result = GetLastError();
            }
        }

        SetupDiDestroyDeviceInfoList(DevInfo);
        return Result;
    }

    const wchar_t* ProblemCodeText(ULONG Problem)
    {
        switch (Problem)
        {
        case 0:                       return L"none";
        case CM_PROB_NOT_CONFIGURED:  return L"CM_PROB_NOT_CONFIGURED - no driver bound to the device";
        case CM_PROB_FAILED_START:    return L"CM_PROB_FAILED_START - the driver failed to start";
        case CM_PROB_NORMAL_CONFLICT: return L"CM_PROB_NORMAL_CONFLICT - resource conflict";
        case CM_PROB_NEED_RESTART:    return L"CM_PROB_NEED_RESTART - a restart is required";
        case CM_PROB_REINSTALL:       return L"CM_PROB_REINSTALL - the device must be reinstalled";
        case CM_PROB_DISABLED:        return L"CM_PROB_DISABLED - the device is disabled";
        case CM_PROB_FAILED_INSTALL:  return L"CM_PROB_FAILED_INSTALL - driver installation failed";
        case CM_PROB_FAILED_ADD:      return L"CM_PROB_FAILED_ADD - a driver in the stack failed AddDevice";
        case CM_PROB_DRIVER_FAILED_LOAD:
                                      return L"CM_PROB_DRIVER_FAILED_LOAD - the driver could not be loaded "
                                             L"(unsigned/untrusted package, or a missing dependency)";
        case CM_PROB_DRIVER_BLOCKED:  return L"CM_PROB_DRIVER_BLOCKED - the driver is blocked from loading";
        case CM_PROB_UNSIGNED_DRIVER: return L"CM_PROB_UNSIGNED_DRIVER - the driver package is not trusted";
        default:                      return L"see CM_PROB_* in cfgmgr32.h";
        }
    }
}
