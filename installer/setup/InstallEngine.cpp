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
        Errorf(pSink, L"%s 失败: %s (0x%08X)", What, Message, Error);
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
            StringCchPrintfW(Buffer, Chars, L"(无说明)");
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
            Detailf(pSink, L"驱动未运行 - 没有需要撤销的拆分。");
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
            Detailf(pSink, L"已移除所有虚拟显示器；桌面已恢复到真实显示器。");
            Sleep(1200);   // let the topology settle before the device disappears
            return true;
        }

        ReportWin32(pSink, L"移除虚拟显示器", Error);
        return false;
    }

    _Use_decl_annotations_
    // WUDFRD.inf is the inbox INF that gives every UMDF device its reflector
    // service association; this driver package Includes it. On machines where
    // the file has gone missing from %WINDIR%\INF, device installation fails
    // with 0xe0000219 ("a function driver was not specified") even though the
    // driver package itself is fine. The installer embeds a copy of the build
    // machine's inbox WUDFRD.inf and restores it when - and only when - the
    // target machine lacks it.
    bool EnsureWufrdInf(IStepSink* pSink)
    {
        wchar_t InfPath[MAX_PATH];
        if (GetWindowsDirectoryW(InfPath, ARRAYSIZE(InfPath)) == 0)
        {
            ReportWin32(pSink, L"定位系统目录", GetLastError());
            return false;
        }
        if (FAILED(StringCchCatW(InfPath, ARRAYSIZE(InfPath), L"\\INF\\WUDFRD.inf")))
        {
            return false;
        }

        if (GetFileAttributesW(InfPath) != INVALID_FILE_ATTRIBUTES)
        {
            Detailf(pSink, L"WUDFRD.inf 已存在，无需修复。");
            return true;
        }

        Detailf(pSink, L"系统缺少 WUDFRD.inf（UMDF 驱动必需的系统文件），正在补回…");
        const DWORD Error = WriteResourceToFile(IDR_WUDFRD_INF, InfPath);
        if (Error != ERROR_SUCCESS)
        {
            ReportWin32(pSink, L"补回 WUDFRD.inf", Error);
            return false;
        }
        Detailf(pSink, L"已补回 %s", InfPath);
        return true;
    }

    int RunInstallSequence(const InstallOptions* pOptions, IStepSink* pSink)
    {
        pSink->OnProgress(0);

        // ---- 1. payload ---------------------------------------------------
        Stepf(pSink, L"检查安装程序内容");
        if (!HasEmbeddedPayload())
        {
            Errorf(pSink, L"此安装程序不包含驱动包。");
            return ExitPayloadMissing;
        }
        if (!ResourceExists(IDR_APP_EXE) || !ResourceExists(IDR_DSCOMP_EXE) ||
            !ResourceExists(IDR_EDIDOVERRIDE_EXE))
        {
            Errorf(pSink, L"此安装程序缺少 DesktopSplitter 程序文件。");
            return ExitPayloadMissing;
        }
        Detailf(pSink, L"驱动包和程序文件齐全。");
        pSink->OnProgress(5);

        ExtractedPackage Package = {};
        Stepf(pSink, L"解压驱动程序");
        DWORD Error = ExtractPayload(&Package);
        if (Error != ERROR_SUCCESS)
        {
            ReportWin32(pSink, L"解压驱动程序", Error);
            return ExitPayloadExtractFailed;
        }
        Detailf(pSink, L"%s", Package.Directory);
        pSink->OnProgress(10);

        int Result = ExitSuccess;

        for (;;)
        {
            if (pSink->IsCancelled()) { Result = ExitCancelled; break; }

            // ---- 2. stop anything running (upgrade case) ------------------
            Stepf(pSink, L"关闭正在运行的 DesktopSplitter 组件");
            int Stopped = 0;
            StopRunningProcesses(&Stopped);
            if (Stopped > 0)
            {
                Detailf(pSink, L"已停止 %d 个正在运行的组件。", Stopped);
            }
            else
            {
                Detailf(pSink, L"没有组件在运行。");
            }
            pSink->OnProgress(15);

            // ---- 3. publisher trust ---------------------------------------
            Stepf(pSink, L"检查驱动的发布者");
            const LONG TrustStatus = VerifyFileTrust(Package.CatPath);
            Detailf(pSink, L"签名检查: %s", TrustStatusText(TrustStatus));

            if (TrustStatus == TRUST_E_NOSIGNATURE)
            {
                Errorf(pSink, L"驱动包未签名，无法安装。");
                Result = ExitCatalogUnusable;
                break;
            }

            if (TrustStatus != ERROR_SUCCESS)
            {
                PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
                if (Cert == nullptr)
                {
                    ReportWin32(pSink, L"读取签名证书", GetLastError());
                    Result = ExitCatalogUnusable;
                    break;
                }

                CertificateSummary Summary = {};
                if (!SummarizeCertificate(Cert, &Summary))
                {
                    CertFreeCertificateContext(Cert);
                    Errorf(pSink, L"无法读取签名证书。");
                    Result = ExitCatalogUnusable;
                    break;
                }

                if (!pSink->ConfirmCertificateTrust(Summary))
                {
                    CertFreeCertificateContext(Cert);
                    Errorf(pSink, L"已取消：未信任该发布者，未做任何更改。");
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
                        Detailf(pSink, L"已在 LocalMachine\\%s 中信任该发布者", StoreName);
                    }
                    else
                    {
                        ReportWin32(pSink, L"信任发布者", CertError);
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
                Detailf(pSink, L"此电脑已信任该发布者 - 无需更改。");
            }
            pSink->OnProgress(25);

            // ---- 3.5 inbox UMDF support file ------------------------------
            Stepf(pSink, L"检查系统组件");
            if (!EnsureWufrdInf(pSink))
            {
                Errorf(pSink, L"系统组件 WUDFRD.inf 缺失且无法自动补回，驱动无法安装。");
                Result = ExitDriverInstallFailed;
                break;
            }
            pSink->OnProgress(28);

            // ---- 4. driver package ----------------------------------------
            Stepf(pSink, L"安装显示驱动");

            OemInfList Before = {};
            FindAllOemInfs(kCatalogFileName, &Before);
            for (size_t i = 0; i < Before.Count; ++i)
            {
                Detailf(pSink, L"已有驱动包: %s (%s)", Before.Names[i], Before.DriverVer[i]);
            }

            InstallMethod Method = InstallMethod::DiInstallDriver;
            bool RebootRequired = false;
            bool AlreadyCurrent = false;
            Error = InstallDriverPackage(Package.InfPath, &Method, &RebootRequired, &AlreadyCurrent);

            if (Error != ERROR_SUCCESS)
            {
                ReportWin32(pSink, L"安装显示驱动", Error);
                Result = ExitDriverInstallFailed;
                break;
            }
            Detailf(pSink, AlreadyCurrent
                        ? L"此版本已安装。"
                        : L"驱动包已安装。");
            pSink->OnProgress(45);

            // ---- 5. prune superseded packages -----------------------------
            Stepf(pSink, L"清理旧版本驱动");
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
                        Detailf(pSink, L"新驱动包: %s (%s)", After.Names[i], After.DriverVer[i]);
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
                    Detailf(pSink, L"已删除 %d 个旧驱动包。", Removed);
                }
                else
                {
                    Detailf(pSink, L"没有需要清理的内容。");
                }
            }
            pSink->OnProgress(55);

            // ---- 6. device node -------------------------------------------
            Stepf(pSink, L"创建虚拟显示设备");
            bool AlreadyExisted = false, NodeCreated = false, NodeReboot = false;
            Error = CreateRootDeviceNode(kHardwareId, kHardwareIdMultiSz,
                                         static_cast<DWORD>(sizeof(kHardwareIdMultiSz)),
                                         kDeviceDescription,
                                         &AlreadyExisted, &NodeCreated, &NodeReboot);

            if (AlreadyExisted)
            {
                Detailf(pSink, L"设备已存在 - 直接复用。");
            }
            else if (Error != ERROR_SUCCESS && !NodeCreated)
            {
                ReportWin32(pSink, L"创建设备", Error);
                Result = ExitDevNodeFailed;
                break;
            }
            else
            {
                Detailf(pSink, L"设备已创建。");
            }
            pSink->OnProgress(70);

            // ---- 7. verify -------------------------------------------------
            Stepf(pSink, L"验证驱动");
            DeviceNodeInfo Info = {};
            if (!WaitForHealthyDeviceNode(&Info, 10000))
            {
                if (Info.InstanceId[0] == L'\0')
                {
                    Errorf(pSink, L"虚拟显示设备未出现。");
                }
                else
                {
                    Errorf(pSink, L"设备报告问题 %u: %s",
                           Info.Problem, ProblemCodeText(Info.Problem));
                }
                Result = ExitVerificationFailed;
                break;
            }
            Detailf(pSink, L"设备 %s 正在运行。", Info.InstanceId);
            pSink->OnProgress(80);

            // ---- 8. program files ------------------------------------------
            Stepf(pSink, L"将 DesktopSplitter 安装到 %s", pOptions->InstallDir);
            DWORD Bytes = 0;
            Error = InstallProductFiles(pOptions->InstallDir, &Bytes);
            if (Error != ERROR_SUCCESS)
            {
                ReportWin32(pSink, L"复制程序文件", Error);
                Result = ExitFileInstallFailed;
                break;
            }
            Detailf(pSink, L"已复制 %.1f MB。", static_cast<double>(Bytes) / (1024.0 * 1024.0));
            pSink->OnProgress(92);

            // ---- 9. shortcuts, registry ------------------------------------
            Stepf(pSink, L"创建快捷方式和注册表项");
            CreateShortcuts(pOptions);
            if (pOptions->DesktopShortcut)   Detailf(pSink, L"桌面快捷方式已创建。");
            if (pOptions->StartMenuShortcut) Detailf(pSink, L"开始菜单快捷方式已创建。");

            WriteUninstallEntry(pOptions, Bytes / 1024);
            Detailf(pSink, L"已注册到“添加或删除程序”。");

            WriteProductFlags(pOptions);
            if (pOptions->HidePhysicalDisplaySupported)
            {
                Detailf(pSink, L"隐藏物理显示器: %s",
                        pOptions->HidePhysicalDisplay ? L"已启用" : L"已禁用");
            }
            else
            {
                Detailf(pSink, L"隐藏物理显示器: 当前 Windows 版本不支持。");
            }

            if (RebootRequired || NodeReboot)
            {
                Detailf(pSink, L"注意：需要重启电脑才能完成驱动的设置。");
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

        Stepf(pSink, L"停止 DesktopSplitter");
        int Stopped = 0;
        StopRunningProcesses(&Stopped);
        Detailf(pSink, L"已停止 %d 个组件。", Stopped);
        pSink->OnProgress(10);

        Stepf(pSink, L"恢复显示布局");
        RevertSplit(pSink);
        pSink->OnProgress(20);

        // ---- un-hide any monitor we hid --------------------------------
        // This MUST happen while edidoverride.exe is still on disk, and before
        // the files are deleted. Leaving a monitor overridden after uninstall
        // would hide a real display with no tool left to bring it back.
        Stepf(pSink, L"恢复被隐藏的显示器");
        {
            OverriddenMonitors Overridden = {};
            FindOverriddenMonitors(&Overridden);
            const bool FlagSet = ReadHidePhysicalDisplayFlag();

            if (Overridden.Count == 0)
            {
                Detailf(pSink, FlagSet
                            ? L"隐藏显示器选项曾开启，但目前没有显示器被覆盖。"
                            : L"没有显示器被隐藏 - 无需恢复。");
            }
            else if (!Installed)
            {
                // No install dir recorded, so no helper to run.
                Errorf(pSink, L"仍有 %zu 台显示器存在 EDID 覆盖，但安装"
                              L"位置未知。", Overridden.Count);
                for (size_t i = 0; i < Overridden.Count; ++i)
                {
                    Errorf(pSink, L"  请手动恢复: edidoverride.exe guarded-revert %s --yes",
                           Overridden.Ids[i]);
                }
            }
            else
            {
                for (size_t i = 0; i < Overridden.Count; ++i)
                {
                    Detailf(pSink, L"正在撤销 %s 的 EDID 覆盖", Overridden.Ids[i]);

                    wchar_t Arguments[280] = {};
                    StringCchPrintfW(Arguments, ARRAYSIZE(Arguments),
                                     L"guarded-revert \"%s\" --yes", Overridden.Ids[i]);

                    DWORD ExitCode = 0;
                    if (!RunEdidOverride(InstallDir, Arguments, &ExitCode))
                    {
                        // Non-fatal, but the user must be told exactly how to fix it.
                        Errorf(pSink, L"无法为 %s 运行 edidoverride.exe。", Overridden.Ids[i]);
                        Errorf(pSink, L"  请手动恢复: edidoverride.exe guarded-revert %s --yes",
                               Overridden.Ids[i]);
                        Errorf(pSink, L"  或删除 HKLM\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s"
                                      L"\\Device Parameters\\EDID_OVERRIDE 并重启。",
                               Overridden.Ids[i]);
                    }
                    else if (ExitCode == 0)
                    {
                        Detailf(pSink, L"%s 已重新对 Windows 可见。", Overridden.Ids[i]);
                    }
                    else
                    {
                        Errorf(pSink, L"edidoverride.exe 返回 %u（针对 %s）；该显示器可能仍被隐藏。",
                               ExitCode, Overridden.Ids[i]);
                        Errorf(pSink, L"  请手动恢复: edidoverride.exe guarded-revert %s --yes",
                               Overridden.Ids[i]);
                    }
                }
            }
        }
        pSink->OnProgress(28);

        Stepf(pSink, L"删除虚拟显示设备");
        int Removed = 0;
        bool RebootRequired = false;
        DWORD Error = RemoveDeviceNodes(kHardwareId, &Removed, &RebootRequired);
        if (Error != ERROR_SUCCESS)
        {
            ReportWin32(pSink, L"删除设备", Error);
            Result = ExitUninstallFailed;
        }
        Detailf(pSink, L"已删除 %d 个设备。", Removed);
        pSink->OnProgress(45);

        Stepf(pSink, L"删除显示驱动");
        {
            OemInfList Staged = {};
            if (!FindAllOemInfs(kCatalogFileName, &Staged))
            {
                Detailf(pSink, L"驱动不存在。");
            }
            else
            {
                for (size_t i = 0; i < Staged.Count; ++i)
                {
                    const DWORD PkgError = UninstallOemInf(Staged.Names[i], true);
                    if (PkgError == ERROR_SUCCESS)
                    {
                        Detailf(pSink, L"已删除 %s (%s)", Staged.Names[i], Staged.DriverVer[i]);
                    }
                    else
                    {
                        ReportWin32(pSink, L"删除驱动包", PkgError);
                        Result = ExitUninstallFailed;
                    }
                }
            }
        }
        pSink->OnProgress(65);

        Stepf(pSink, L"删除快捷方式和程序文件");
        RemoveShortcuts();
        if (Installed)
        {
            RemoveProductFiles(InstallDir);
            Detailf(pSink, L"已删除 %s", InstallDir);
        }
        RemoveUninstallEntry();
        RemoveProductFlags();
        Detailf(pSink, L"注册表项已删除。");
        pSink->OnProgress(85);

        Stepf(pSink, L"发布者证书");
        if (!pOptions->RemoveCertificate)
        {
            Detailf(pSink, L"已保留。如需删除，请勾选证书选项重新运行。");
        }
        else
        {
            ExtractedPackage Package = {};
            if (ExtractPayload(&Package) != ERROR_SUCCESS)
            {
                Errorf(pSink, L"无法解压驱动包以识别证书。");
                Result = ExitUninstallFailed;
            }
            else
            {
                PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
                if (Cert == nullptr)
                {
                    Errorf(pSink, L"无法读取证书；信任存储未做更改。");
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
                                Detailf(pSink, L"已从 LocalMachine\\%s 中删除", StoreName);
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
