// DeviceNode.h - root-enumerated device node helpers shared by the installer
// tools (installer\setup\DesktopSplitterSetup and, optionally, devcreate).
//
// Creating the node is the same sequence devcon uses:
//   SetupDiCreateDeviceInfoList -> SetupDiCreateDeviceInfo ->
//   SetupDiSetDeviceRegistryProperty(SPDRP_HARDWAREID) ->
//   SetupDiCallClassInstaller(DIF_REGISTERDEVICE) -> DiInstallDevice
//
// which produces a node that survives process exit and reboot (unlike
// SwDeviceCreate, whose node lives only as long as the HSWDEVICE handle).
#pragma once

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

namespace DeskSplit
{
    struct DeviceNodeInfo
    {
        wchar_t InstanceId[MAX_DEVICE_ID_LEN];
        DEVINST DevInst;
        ULONG   Status;         // CM_Get_DevNode_Status output
        ULONG   Problem;        // CM_PROB_* (0 when healthy)
        bool    HasStatus;      // false if CM_Get_DevNode_Status failed
    };

    // Number of device nodes under the ROOT enumerator carrying HardwareId.
    // Returns -1 if the enumeration itself failed.
    int CountDeviceNodes(_In_z_ const wchar_t* HardwareId);

    // Finds the first present node carrying HardwareId and fills pInfo
    // (including its CM_Get_DevNode_Status result). Returns false if no node
    // with that hardware ID is present.
    bool FindDeviceNode(_In_z_ const wchar_t* HardwareId, _Out_ DeviceNodeInfo* pInfo);

    // Registers a persistent root-enumerated node and binds the best matching
    // driver from the driver store.
    //
    // Returns a Win32 error code (ERROR_SUCCESS on success).
    //   *pAlreadyExisted - a node was already present; nothing was created.
    //   *pNodeCreated    - the node was registered (even if driver binding then
    //                      failed, in which case the return value is non-zero
    //                      but the node still exists and PnP may bind later).
    //   *pRebootRequired - the class installer asked for a reboot.
    DWORD CreateRootDeviceNode(
        _In_z_ const wchar_t* HardwareId,
        _In_reads_bytes_(HardwareIdMultiSzBytes) const wchar_t* HardwareIdMultiSz,
        _In_ DWORD HardwareIdMultiSzBytes,
        _In_z_ const wchar_t* Description,
        _Out_ bool* pAlreadyExisted,
        _Out_ bool* pNodeCreated,
        _Out_ bool* pRebootRequired);

    // Uninstalls every node carrying HardwareId. ERROR_SUCCESS when all removals
    // succeeded (including the "there were none" case).
    DWORD RemoveDeviceNodes(
        _In_z_ const wchar_t* HardwareId,
        _Out_ int* pRemovedCount,
        _Out_ bool* pRebootRequired);

    // Human-readable text for a CM_PROB_* code.
    const wchar_t* ProblemCodeText(_In_ ULONG Problem);
}
