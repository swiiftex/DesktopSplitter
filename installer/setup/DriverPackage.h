// DriverPackage.h - installing / removing the driver package itself
// (as opposed to the device node, which lives in common\DeviceNode.h).
#pragma once

#include <windows.h>

namespace DeskSplit
{
    enum class InstallMethod
    {
        DiInstallDriver,    // newdev.dll!DiInstallDriverW
        PnpUtil             // pnputil.exe /add-driver <inf> /install
    };

    // Installs (stages + binds) the driver package identified by InfPath.
    // Returns a Win32 error code; ERROR_SUCCESS on success.
    //
    //   *pMethod         - which mechanism actually performed the install
    //   *pRebootRequired - a reboot is needed to finish
    //   *pAlreadyCurrent - the same or a newer package was already present
    DWORD InstallDriverPackage(
        _In_z_ const wchar_t* InfPath,
        _Out_ InstallMethod* pMethod,
        _Out_ bool* pRebootRequired,
        _Out_ bool* pAlreadyCurrent);

    // Finds the oemNN.inf name in %WINDIR%\INF that corresponds to our package,
    // by matching the [Version] CatalogFile entry. Returns false if not staged.
    bool FindOemInfName(
        _In_z_ const wchar_t* CatalogFileName,
        _Out_writes_z_(NameChars) wchar_t* OemName,
        _In_ size_t NameChars);

    // Every staged oemNN.inf whose [Version] CatalogFile matches ours.
    // Windows keeps each installed version as its own oemNN.inf, so after an
    // upgrade there is more than one and the old ones must be pruned.
    struct OemInfList
    {
        static constexpr size_t kMax = 32;
        wchar_t Names[kMax][MAX_PATH];
        wchar_t DriverVer[kMax][128];
        size_t  Count;
    };

    bool FindAllOemInfs(_In_z_ const wchar_t* CatalogFileName, _Out_ OemInfList* pList);

    // Removes every staged package matching CatalogFileName EXCEPT KeepOemName.
    // Used after a successful install so exactly one package remains.
    DWORD RemoveSupersededPackages(
        _In_z_ const wchar_t* CatalogFileName,
        _In_z_ const wchar_t* KeepOemName,
        _Out_ int* pRemovedCount,
        _Out_ int* pFailedCount);

    // SetupUninstallOEMInf on the given oemNN.inf. Returns a Win32 error code.
    DWORD UninstallOemInf(_In_z_ const wchar_t* OemName, _In_ bool Force);
}
