#include "InstallEngine.h"

#include "Payload.h"
#include "DriverPackage.h"
#include "Edition.h"
#include "resource.h"
#include "../common/DeviceNode.h"

#include <winioctl.h>          // CTL_CODE for the IOCTLs in DeskSplitProtocol.h
#include "DeskSplitProtocol.h"

#include <cfgmgr32.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    const wchar_t kHardwareId[] = DESKSPLIT_HARDWARE_ID;
    const wchar_t kHardwareIdMultiSz[] = DESKSPLIT_HARDWARE_ID L"\0";
    const wchar_t kDeviceDescription[] = L"DesktopSplitter Virtual Display";
    const wchar_t kCatalogFileName[] = L"DesktopSplitterVdd.cat";

    using DeskSplit::IStepSink;

    void Detailf(IStepSink* pSink, _Printf_format_string_ const wchar_t* Format, ...)
    {
        wchar_t Buffer[1024];
        va_list Args;
        va_start(Args, Format);
        _vsnwprintf_s(Buffer, _countof(Buffer), _TRUNCATE, Format, Args);
        va_end(Args);
        pSink->OnDetail(Buffer);
    }

    void Stepf(IStepSink* pSink, _Printf_format_string_ const wchar_t* Format, ...)
    {
        wchar_t Buffer[1024];
        va_list Args;
        va_start(Args, Format);
        _vsnwprintf_s(Buffer, _countof(Buffer), _TRUNCATE, Format, Args);
        va_end(Args);
        pSink->OnStep(Buffer);
    }

    void Errorf(IStepSink* pSink, _Printf_format_string_ const wchar_t* Format, ...)
    {
        wchar_t Buffer[1024];
        va_list Args;
        va_start(Args, Format);
        _vsnwprintf_s(Buffer, _countof(Buffer), _TRUNCATE, Format, Args);
        va_end(Args);
        pSink->OnError(Buffer);
    }

    void ReportWin32(IStepSink* pSink, const wchar_t* What, DWORD Error)
    {
        wchar_t Message[512] = {};
        DeskSplit::FormatWin32Error(Error, Message, ARRAYSIZE(Message));
        Errorf(pSink, L"%s failed: %s (0x%08X)", What, Message, Error);
    }

    bool WaitForHealthyDeviceNode(DeskSplit::DeviceNodeInfo* pInfo, DWORD TimeoutMs)
    {
        const ULONGLONG Deadline = GetTickCount64() + TimeoutMs;
        for (;;)
        {
            if (DeskSplit::FindDeviceNode(kHardwareId, pInfo))
            {
                if (!pInfo->HasStatus || pInfo->Problem == 0)
                {
                    return true;
                }
            }
            if (GetTickCount64() >= Deadline)
            {
                return false;
            }
            Sleep(250);
        }
    }

    // ---- EDID override detection ------------------------------------------
    // edidoverride.exe stashes the original EDID under this value when it
    // patches a monitor, so its presence means that monitor is currently
    // overridden (hidden from the desktop).
    const wchar_t kEdidBackupValue[] = L"EDID_OVERRIDE_DSBACKUP";

    struct OverriddenMonitors
    {
        static constexpr size_t kMax = 16;
        // Full instance path under Enum\DISPLAY, e.g. "PHLC310\7&224ecef0&0&UID260".
        // The bare enum key ("PHLC310") is ambiguous: stale devnode instances of the
        // same monitor sort first and would make edidoverride target the wrong one.
        wchar_t Ids[kMax][200];
        size_t Count;
    };

    // Walks HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<id>\<instance>\Device Parameters
    // looking for the backup marker.
    void FindOverriddenMonitors(OverriddenMonitors* pOut)
    {
        ZeroMemory(pOut, sizeof(*pOut));

        HKEY DisplayKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY",
                          0, KEY_ENUMERATE_SUB_KEYS, &DisplayKey) != ERROR_SUCCESS)
        {
            return;
        }

        for (DWORD MonitorIndex = 0; pOut->Count < OverriddenMonitors::kMax; ++MonitorIndex)
        {
            wchar_t MonitorId[64] = {};
            DWORD MonitorIdChars = ARRAYSIZE(MonitorId);
            if (RegEnumKeyExW(DisplayKey, MonitorIndex, MonitorId, &MonitorIdChars,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            {
                break;
            }

            HKEY MonitorKey = nullptr;
            if (RegOpenKeyExW(DisplayKey, MonitorId, 0, KEY_ENUMERATE_SUB_KEYS, &MonitorKey) != ERROR_SUCCESS)
            {
                continue;
            }

            bool Overridden = false;
            wchar_t OverriddenInstance[128] = {};
            for (DWORD InstanceIndex = 0; !Overridden; ++InstanceIndex)
            {
                wchar_t Instance[128] = {};
                DWORD InstanceChars = ARRAYSIZE(Instance);
                if (RegEnumKeyExW(MonitorKey, InstanceIndex, Instance, &InstanceChars,
                                  nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                {
                    break;
                }

                wchar_t ParamsPath[256] = {};
                if (FAILED(StringCchPrintfW(ParamsPath, ARRAYSIZE(ParamsPath),
                                            L"%s\\Device Parameters", Instance)))
                {
                    continue;
                }

                HKEY ParamsKey = nullptr;
                if (RegOpenKeyExW(MonitorKey, ParamsPath, 0,
                                  KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &ParamsKey) != ERROR_SUCCESS)
                {
                    continue;
                }

                // edidoverride stores the backup as a SUBKEY of Device Parameters (a copy
                // of the whole EDID_OVERRIDE key); accept a value too, defensively.
                HKEY BackupKey = nullptr;
                if (RegOpenKeyExW(ParamsKey, kEdidBackupValue, 0, KEY_QUERY_VALUE, &BackupKey) == ERROR_SUCCESS)
                {
                    RegCloseKey(BackupKey);
                    Overridden = true;
                }
                else if (RegQueryValueExW(ParamsKey, kEdidBackupValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                {
                    Overridden = true;
                }
                if (Overridden)
                {
                    StringCchCopyW(OverriddenInstance, ARRAYSIZE(OverriddenInstance), Instance);
                }
                RegCloseKey(ParamsKey);
            }

            RegCloseKey(MonitorKey);

            if (Overridden)
            {
                StringCchPrintfW(pOut->Ids[pOut->Count], ARRAYSIZE(pOut->Ids[pOut->Count]),
                                 L"%s\\%s", MonitorId, OverriddenInstance);
                ++pOut->Count;
            }
        }

        RegCloseKey(DisplayKey);
    }

    bool ReadHidePhysicalDisplayFlag()
    {
        HKEY Key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DeskSplit::kProductRegKey, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD Value = 0;
        DWORD Bytes = sizeof(Value);
        DWORD Type = 0;
        const bool Ok = RegQueryValueExW(Key, L"HidePhysicalDisplay", nullptr, &Type,
                                         reinterpret_cast<BYTE*>(&Value), &Bytes) == ERROR_SUCCESS &&
                        Type == REG_DWORD;
        RegCloseKey(Key);
        return Ok && Value != 0;
    }

    // Runs edidoverride.exe from the install directory. Returns false when the
    // helper could not be launched at all.
    bool RunEdidOverride(const wchar_t* InstallDir, const wchar_t* Arguments, DWORD* pExitCode)
    {
        wchar_t Exe[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(Exe, ARRAYSIZE(Exe), L"%s\\edidoverride.exe", InstallDir)))
        {
            return false;
        }
        if (GetFileAttributesW(Exe) == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }

        wchar_t CommandLine[MAX_PATH + 256] = {};
        if (FAILED(StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" %s", Exe, Arguments)))
        {
            return false;
        }

        STARTUPINFOW StartupInfo = { sizeof(STARTUPINFOW) };
        StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        StartupInfo.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION ProcessInfo = {};

        if (!CreateProcessW(Exe, CommandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, InstallDir, &StartupInfo, &ProcessInfo))
        {
            return false;
        }

        WaitForSingleObject(ProcessInfo.hProcess, 120000);
        if (!GetExitCodeProcess(ProcessInfo.hProcess, pExitCode))
        {
            *pExitCode = static_cast<DWORD>(-1);
        }

        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
        return true;
    }

    // Opens the driver's device interface, if the driver is running.
    HANDLE OpenDriverInterface()
    {
        ULONG Length = 0;
        if (CM_Get_Device_Interface_List_SizeW(
                &Length, const_cast<GUID*>(&GUID_DEVINTERFACE_DESKSPLIT), nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || Length < 2)
        {
            return INVALID_HANDLE_VALUE;
        }

        wchar_t* Buffer = static_cast<wchar_t*>(LocalAlloc(LPTR, Length * sizeof(wchar_t)));
        if (Buffer == nullptr)
        {
            return INVALID_HANDLE_VALUE;
        }

        HANDLE Device = INVALID_HANDLE_VALUE;
        if (CM_Get_Device_Interface_ListW(
                const_cast<GUID*>(&GUID_DEVINTERFACE_DESKSPLIT), nullptr, Buffer, Length,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS && Buffer[0] != L'\0')
        {
            Device = CreateFileW(Buffer, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        }

        LocalFree(Buffer);
        return Device;
    }
}

namespace DeskSplit
{
    _Use_decl_annotations_
    void FormatWin32Error(DWORD Error, wchar_t* Buffer, size_t Chars)
    {
        Buffer[0] = L'\0';

        const DWORD Length = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            Buffer, static_cast<DWORD>(Chars), nullptr);

        if (Length == 0)
        {
            StringCchPrintfW(Buffer, Chars, L"(no description)");
            return;
        }

        for (size_t Index = wcslen(Buffer); Index > 0; --Index)
        {
            const wchar_t Ch = Buffer[Index - 1];
            if (Ch == L'\r' || Ch == L'\n' || Ch == L' ' || Ch == L'.')
            {
                Buffer[Index - 1] = L'\0';
            }
            else
            {
                break;
            }
        }
    }

    _Use_decl_annotations_
    bool RevertSplit(IStepSink* pSink)
    {
        HANDLE Device = OpenDriverInterface();
        if (Device == INVALID_HANDLE_VALUE)
        {
            Detailf(pSink, L"Driver not running - no split to revert.");
            return true;
        }

        DESKSPLIT_CONFIG Config = {};
        Config.Version = DESKSPLIT_PROTOCOL_VERSION;
        Config.MonitorCount = 0;

        DWORD Returned = 0;
        const BOOL Ok = DeviceIoControl(Device, IOCTL_DESKSPLIT_SET_CONFIG,
                                        &Config, sizeof(Config), nullptr, 0, &Returned, nullptr);
        const DWORD Error = Ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(Device);

        if (Ok)
        {
            Detailf(pSink, L"Removed all virtual monitors; the desktop is back on the real display.");
            Sleep(1200);   // let the topology settle before the device disappears
            return true;
        }

        ReportWin32(pSink, L"Removing the virtual monitors", Error);
        return false;
    }

    _Use_decl_annotations_
    int RunInstallSequence(const InstallOptions* pOptions, IStepSink* pSink)
    {
        pSink->OnProgress(0);

        // ---- 1. payload ---------------------------------------------------
        Stepf(pSink, L"Checking the installer contents");
        if (!HasEmbeddedPayload())
        {
            Errorf(pSink, L"This installer does not contain a driver package.");
            return ExitPayloadMissing;
        }
        if (!ResourceExists(IDR_APP_EXE) || !ResourceExists(IDR_DSCOMP_EXE) ||
            !ResourceExists(IDR_EDIDOVERRIDE_EXE))
        {
            Errorf(pSink, L"This installer is missing the DesktopSplitter program files.");
            return ExitPayloadMissing;
        }
        Detailf(pSink, L"Driver package and program files present.");
        pSink->OnProgress(5);

        ExtractedPackage Package = {};
        Stepf(pSink, L"Unpacking the driver");
        DWORD Error = ExtractPayload(&Package);
        if (Error != ERROR_SUCCESS)
        {
            ReportWin32(pSink, L"Unpacking the driver", Error);
            return ExitPayloadExtractFailed;
        }
        Detailf(pSink, L"%s", Package.Directory);
        pSink->OnProgress(10);

        int Result = ExitSuccess;

        for (;;)
        {
            if (pSink->IsCancelled()) { Result = ExitCancelled; break; }

            // ---- 2. stop anything running (upgrade case) ------------------
            Stepf(pSink, L"Closing any running DesktopSplitter components");
            int Stopped = 0;
            StopRunningProcesses(&Stopped);
            if (Stopped > 0)
            {
                Detailf(pSink, L"Stopped %d running component(s).", Stopped);
            }
            else
            {
                Detailf(pSink, L"Nothing was running.");
            }
            pSink->OnProgress(15);

            // ---- 3. publisher trust ---------------------------------------
            Stepf(pSink, L"Checking the driver's publisher");
            const LONG TrustStatus = VerifyFileTrust(Package.CatPath);
            Detailf(pSink, L"Signature check: %s", TrustStatusText(TrustStatus));

            if (TrustStatus == TRUST_E_NOSIGNATURE)
            {
                Errorf(pSink, L"The driver package is not signed; it cannot be installed.");
                Result = ExitCatalogUnusable;
                break;
            }

            if (TrustStatus != ERROR_SUCCESS)
            {
                PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
                if (Cert == nullptr)
                {
                    ReportWin32(pSink, L"Reading the signing certificate", GetLastError());
                    Result = ExitCatalogUnusable;
                    break;
                }

                CertificateSummary Summary = {};
                if (!SummarizeCertificate(Cert, &Summary))
                {
                    CertFreeCertificateContext(Cert);
                    Errorf(pSink, L"The signing certificate could not be read.");
                    Result = ExitCatalogUnusable;
                    break;
                }

                if (!pSink->ConfirmCertificateTrust(Summary))
                {
                    CertFreeCertificateContext(Cert);
                    Errorf(pSink, L"Cancelled: the publisher was not trusted. Nothing was changed.");
                    Result = ExitTrustDeclined;
                    break;
                }

                const wchar_t* Stores[] = { L"Root", L"TrustedPublisher" };
                bool CertOk = true;
                for (const wchar_t* StoreName : Stores)
                {
                    const DWORD CertError = AddCertificateToStore(Cert, StoreName);
                    if (CertError == ERROR_SUCCESS)
                    {
                        Detailf(pSink, L"Trusted the publisher in LocalMachine\\%s", StoreName);
                    }
                    else
                    {
                        ReportWin32(pSink, L"Trusting the publisher", CertError);
                        CertOk = false;
                        break;
                    }
                }

                CertFreeCertificateContext(Cert);
                if (!CertOk)
                {
                    Result = ExitCertInstallFailed;
                    break;
                }
            }
            else
            {
                Detailf(pSink, L"Already trusted by this PC - no changes needed.");
            }
            pSink->OnProgress(25);

            // ---- 4. driver package ----------------------------------------
            Stepf(pSink, L"Installing the display driver");

            OemInfList Before = {};
            FindAllOemInfs(kCatalogFileName, &Before);
            for (size_t i = 0; i < Before.Count; ++i)
            {
                Detailf(pSink, L"Existing package: %s (%s)", Before.Names[i], Before.DriverVer[i]);
            }

            InstallMethod Method = InstallMethod::DiInstallDriver;
            bool RebootRequired = false;
            bool AlreadyCurrent = false;
            Error = InstallDriverPackage(Package.InfPath, &Method, &RebootRequired, &AlreadyCurrent);

            if (Error != ERROR_SUCCESS)
            {
                ReportWin32(pSink, L"Installing the display driver", Error);
                Result = ExitDriverInstallFailed;
                break;
            }
            Detailf(pSink, AlreadyCurrent
                        ? L"This version was already installed."
                        : L"Driver package installed.");
            pSink->OnProgress(45);

            // ---- 5. prune superseded packages -----------------------------
            Stepf(pSink, L"Cleaning up older driver versions");
            {
                OemInfList After = {};
                FindAllOemInfs(kCatalogFileName, &After);

                wchar_t KeepName[MAX_PATH] = {};
                for (size_t i = 0; i < After.Count; ++i)
                {
                    bool Existed = false;
                    for (size_t j = 0; j < Before.Count; ++j)
                    {
                        if (_wcsicmp(After.Names[i], Before.Names[j]) == 0) { Existed = true; break; }
                    }
                    if (!Existed)
                    {
                        StringCchCopyW(KeepName, ARRAYSIZE(KeepName), After.Names[i]);
                        Detailf(pSink, L"New package: %s (%s)", After.Names[i], After.DriverVer[i]);
                        break;
                    }
                }
                if (KeepName[0] == L'\0' && After.Count > 0)
                {
                    StringCchCopyW(KeepName, ARRAYSIZE(KeepName), After.Names[0]);
                }

                if (KeepName[0] != L'\0' && After.Count > 1)
                {
                    int Removed = 0, Failed = 0;
                    RemoveSupersededPackages(kCatalogFileName, KeepName, &Removed, &Failed);
                    Detailf(pSink, L"Removed %d older package(s).", Removed);
                }
                else
                {
                    Detailf(pSink, L"Nothing to clean up.");
                }
            }
            pSink->OnProgress(55);

            // ---- 6. device node -------------------------------------------
            Stepf(pSink, L"Creating the virtual display device");
            bool AlreadyExisted = false, NodeCreated = false, NodeReboot = false;
            Error = CreateRootDeviceNode(kHardwareId, kHardwareIdMultiSz,
                                         static_cast<DWORD>(sizeof(kHardwareIdMultiSz)),
                                         kDeviceDescription,
                                         &AlreadyExisted, &NodeCreated, &NodeReboot);

            if (AlreadyExisted)
            {
                Detailf(pSink, L"The device already exists - reused it.");
            }
            else if (Error != ERROR_SUCCESS && !NodeCreated)
            {
                ReportWin32(pSink, L"Creating the device", Error);
                Result = ExitDevNodeFailed;
                break;
            }
            else
            {
                Detailf(pSink, L"Device created.");
            }
            pSink->OnProgress(70);

            // ---- 7. verify -------------------------------------------------
            Stepf(pSink, L"Verifying the driver");
            DeviceNodeInfo Info = {};
            if (!WaitForHealthyDeviceNode(&Info, 10000))
            {
                if (Info.InstanceId[0] == L'\0')
                {
                    Errorf(pSink, L"The virtual display device did not appear.");
                }
                else
                {
                    Errorf(pSink, L"The device reported problem %u: %s",
                           Info.Problem, ProblemCodeText(Info.Problem));
                }
                Result = ExitVerificationFailed;
                break;
            }
            Detailf(pSink, L"Device %s is running.", Info.InstanceId);
            pSink->OnProgress(80);

            // ---- 8. program files ------------------------------------------
            Stepf(pSink, L"Installing DesktopSplitter to %s", pOptions->InstallDir);
            DWORD Bytes = 0;
            Error = InstallProductFiles(pOptions->InstallDir, &Bytes);
            if (Error != ERROR_SUCCESS)
            {
                ReportWin32(pSink, L"Copying program files", Error);
                Result = ExitFileInstallFailed;
                break;
            }
            Detailf(pSink, L"Copied %.1f MB.", static_cast<double>(Bytes) / (1024.0 * 1024.0));
            pSink->OnProgress(92);

            // ---- 9. shortcuts, registry ------------------------------------
            Stepf(pSink, L"Creating shortcuts and registry entries");
            CreateShortcuts(pOptions);
            if (pOptions->DesktopShortcut)   Detailf(pSink, L"Desktop shortcut created.");
            if (pOptions->StartMenuShortcut) Detailf(pSink, L"Start menu shortcut created.");

            WriteUninstallEntry(pOptions, Bytes / 1024);
            Detailf(pSink, L"Registered in Add or remove programs.");

            WriteProductFlags(pOptions);
            if (pOptions->HidePhysicalDisplaySupported)
            {
                Detailf(pSink, L"Hide physical display: %s",
                        pOptions->HidePhysicalDisplay ? L"enabled" : L"disabled");
            }
            else
            {
                Detailf(pSink, L"Hide physical display: not supported on this Windows edition.");
            }

            if (RebootRequired || NodeReboot)
            {
                Detailf(pSink, L"NOTE: a restart is needed to finish setting up the driver.");
            }

            pSink->OnProgress(100);
            break;
        }

        CleanupPayload(&Package);
        return Result;
    }

    _Use_decl_annotations_
    int RunUninstallSequence(const UninstallOptions* pOptions, IStepSink* pSink)
    {
        pSink->OnProgress(0);
        int Result = ExitSuccess;

        wchar_t InstallDir[MAX_PATH] = {};
        wchar_t Version[64] = {};
        const bool Installed = ReadInstalledState(InstallDir, ARRAYSIZE(InstallDir),
                                                  Version, ARRAYSIZE(Version));

        Stepf(pSink, L"Stopping DesktopSplitter");
        int Stopped = 0;
        StopRunningProcesses(&Stopped);
        Detailf(pSink, L"Stopped %d component(s).", Stopped);
        pSink->OnProgress(10);

        Stepf(pSink, L"Restoring the display layout");
        RevertSplit(pSink);
        pSink->OnProgress(20);

        // ---- un-hide any monitor we hid --------------------------------
        // This MUST happen while edidoverride.exe is still on disk, and before
        // the files are deleted. Leaving a monitor overridden after uninstall
        // would hide a real display with no tool left to bring it back.
        Stepf(pSink, L"Restoring hidden displays");
        {
            OverriddenMonitors Overridden = {};
            FindOverriddenMonitors(&Overridden);
            const bool FlagSet = ReadHidePhysicalDisplayFlag();

            if (Overridden.Count == 0)
            {
                Detailf(pSink, FlagSet
                            ? L"The hide-display option was on, but no monitor is currently overridden."
                            : L"No monitor is hidden - nothing to restore.");
            }
            else if (!Installed)
            {
                // No install dir recorded, so no helper to run.
                Errorf(pSink, L"%zu monitor(s) still have an EDID override but the install "
                              L"location is unknown.", Overridden.Count);
                for (size_t i = 0; i < Overridden.Count; ++i)
                {
                    Errorf(pSink, L"  RECOVER MANUALLY: edidoverride.exe guarded-revert %s --yes",
                           Overridden.Ids[i]);
                }
            }
            else
            {
                for (size_t i = 0; i < Overridden.Count; ++i)
                {
                    Detailf(pSink, L"Reverting EDID override on %s", Overridden.Ids[i]);

                    wchar_t Arguments[280] = {};
                    StringCchPrintfW(Arguments, ARRAYSIZE(Arguments),
                                     L"guarded-revert \"%s\" --yes", Overridden.Ids[i]);

                    DWORD ExitCode = 0;
                    if (!RunEdidOverride(InstallDir, Arguments, &ExitCode))
                    {
                        // Non-fatal, but the user must be told exactly how to fix it.
                        Errorf(pSink, L"Could not run edidoverride.exe for %s.", Overridden.Ids[i]);
                        Errorf(pSink, L"  RECOVER MANUALLY: edidoverride.exe guarded-revert %s --yes",
                               Overridden.Ids[i]);
                        Errorf(pSink, L"  or delete HKLM\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s"
                                      L"\\Device Parameters\\EDID_OVERRIDE and reboot.",
                               Overridden.Ids[i]);
                    }
                    else if (ExitCode == 0)
                    {
                        Detailf(pSink, L"%s is visible to Windows again.", Overridden.Ids[i]);
                    }
                    else
                    {
                        Errorf(pSink, L"edidoverride.exe returned %u for %s; the monitor may still be hidden.",
                               ExitCode, Overridden.Ids[i]);
                        Errorf(pSink, L"  RECOVER MANUALLY: edidoverride.exe guarded-revert %s --yes",
                               Overridden.Ids[i]);
                    }
                }
            }
        }
        pSink->OnProgress(28);

        Stepf(pSink, L"Removing the virtual display device");
        int Removed = 0;
        bool RebootRequired = false;
        DWORD Error = RemoveDeviceNodes(kHardwareId, &Removed, &RebootRequired);
        if (Error != ERROR_SUCCESS)
        {
            ReportWin32(pSink, L"Removing the device", Error);
            Result = ExitUninstallFailed;
        }
        Detailf(pSink, L"Removed %d device(s).", Removed);
        pSink->OnProgress(45);

        Stepf(pSink, L"Removing the display driver");
        {
            OemInfList Staged = {};
            if (!FindAllOemInfs(kCatalogFileName, &Staged))
            {
                Detailf(pSink, L"The driver was not present.");
            }
            else
            {
                for (size_t i = 0; i < Staged.Count; ++i)
                {
                    const DWORD PkgError = UninstallOemInf(Staged.Names[i], true);
                    if (PkgError == ERROR_SUCCESS)
                    {
                        Detailf(pSink, L"Removed %s (%s)", Staged.Names[i], Staged.DriverVer[i]);
                    }
                    else
                    {
                        ReportWin32(pSink, L"Removing the driver package", PkgError);
                        Result = ExitUninstallFailed;
                    }
                }
            }
        }
        pSink->OnProgress(65);

        Stepf(pSink, L"Removing shortcuts and program files");
        RemoveShortcuts();
        if (Installed)
        {
            RemoveProductFiles(InstallDir);
            Detailf(pSink, L"Removed %s", InstallDir);
        }
        RemoveUninstallEntry();
        RemoveProductFlags();
        Detailf(pSink, L"Removed registry entries.");
        pSink->OnProgress(85);

        Stepf(pSink, L"Publisher certificate");
        if (!pOptions->RemoveCertificate)
        {
            Detailf(pSink, L"Left in place. Re-run with the certificate option to remove it.");
        }
        else
        {
            ExtractedPackage Package = {};
            if (ExtractPayload(&Package) != ERROR_SUCCESS)
            {
                Errorf(pSink, L"Could not unpack the package to identify the certificate.");
                Result = ExitUninstallFailed;
            }
            else
            {
                PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
                if (Cert == nullptr)
                {
                    Errorf(pSink, L"Could not read the certificate; trust stores untouched.");
                    Result = ExitUninstallFailed;
                }
                else
                {
                    CertificateSummary Summary = {};
                    if (SummarizeCertificate(Cert, &Summary))
                    {
                        const wchar_t* Stores[] = { L"Root", L"TrustedPublisher" };
                        for (const wchar_t* StoreName : Stores)
                        {
                            if (RemoveCertificateFromStore(Summary.Sha1, StoreName) == ERROR_SUCCESS)
                            {
                                Detailf(pSink, L"Removed from LocalMachine\\%s", StoreName);
                            }
                        }
                    }
                    CertFreeCertificateContext(Cert);
                }

                if (!pOptions->KeepTemp)
                {
                    CleanupPayload(&Package);
                }
            }
        }

        pSink->OnProgress(100);
        return Result;
    }
}
