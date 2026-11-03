// Setup.cpp - DesktopSplitterSetup entry point and mode dispatch.
//
// ONE shipping executable serves both audiences:
//   * double-clicked / launched from Add-Remove Programs -> native Win32 wizard
//   * launched with /quiet, /verify, /uninstall /quiet, /? -> console mode
//
// The exe is GUI-subsystem so double-clicking never flashes a console window;
// console mode attaches to the launching console (AttachConsole) so output
// still lands in the shell that started it.
//
// Both modes drive the SAME InstallEngine sequence and differ only in the
// IStepSink they pass, so they cannot drift apart.

#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <shellapi.h>
#include <strsafe.h>

#include "InstallEngine.h"
#include "Payload.h"
#include "Trust.h"
#include "DriverPackage.h"
#include "ProductFiles.h"
#include "Edition.h"
#include "Wizard.h"
#include "resource.h"
#include "../common/DeviceNode.h"
#include "DeskSplitProtocol.h"

using namespace DeskSplit;

namespace
{
    const wchar_t kHardwareId[] = DESKSPLIT_HARDWARE_ID;
    const wchar_t kCatalogFileName[] = L"DesktopSplitterVdd.cat";

    bool g_Quiet = false;
    bool g_HaveConsole = false;
    int  g_StepNumber = 0;

    // ---- console plumbing -------------------------------------------------
    void EnsureConsole()
    {
        if (g_HaveConsole) return;

        // Respect redirection: when the caller did "setup.exe /verify > log.txt"
        // (or piped us), the std handles are already valid and must not be
        // replaced with CONOUT$ - that would throw the output away.
        const HANDLE ExistingOut = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool OutRedirected = (ExistingOut != nullptr && ExistingOut != INVALID_HANDLE_VALUE);

        // Reuse the launching console when there is one; otherwise make our own
        // so /verify from Explorer still shows something.
        if (!AttachConsole(ATTACH_PARENT_PROCESS) && !OutRedirected)
        {
            AllocConsole();
        }

        FILE* Stream = nullptr;
        if (!OutRedirected)
        {
            freopen_s(&Stream, "CONOUT$", "w", stdout);
            freopen_s(&Stream, "CONOUT$", "w", stderr);
        }
        else if (_fileno(stdout) < 0)
        {
            // GUI subsystem + redirection: the CRT has no stdout yet, so bind
            // it to the handle the shell gave us.
            freopen_s(&Stream, "CONOUT$", "w", stdout);
        }

        const HANDLE ExistingIn = GetStdHandle(STD_INPUT_HANDLE);
        if (ExistingIn == nullptr || ExistingIn == INVALID_HANDLE_VALUE)
        {
            freopen_s(&Stream, "CONIN$", "r", stdin);
        }

        SetConsoleOutputCP(CP_UTF8);
        g_HaveConsole = true;
    }

    void Out(_Printf_format_string_ const wchar_t* Format, ...)
    {
        va_list Args;
        va_start(Args, Format);
        vwprintf(Format, Args);
        va_end(Args);
        fflush(stdout);
    }

    // True when this process owns its console (double-clicked from Explorer).
    bool OwnsConsole()
    {
        DWORD ProcessIds[4] = {};
        return GetConsoleProcessList(ProcessIds, static_cast<DWORD>(ARRAYSIZE(ProcessIds))) == 1;
    }

    void PauseIfInteractive()
    {
        if (g_Quiet || !g_HaveConsole || !OwnsConsole()) return;

        wprintf(L"\n按 Enter 键关闭此窗口...");
        fflush(stdout);

        int Ch = 0;
        while ((Ch = _getwch()) != L'\r' && Ch != L'\n' && Ch != EOF)
        {
            if (Ch == 0 || Ch == 0xE0) _getwch();
        }
        wprintf(L"\n");
    }

    bool IsElevated()
    {
        HANDLE Token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) return false;

        TOKEN_ELEVATION Elevation = {};
        DWORD Size = sizeof(Elevation);
        const bool Ok = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Size) != FALSE;
        CloseHandle(Token);
        return Ok && Elevation.TokenIsElevated != 0;
    }

    // ---- console sink -----------------------------------------------------
    struct ConsoleSink : IStepSink
    {
        void OnStep(const wchar_t* Text) override
        {
            ++g_StepNumber;
            Out(L"\n[%d] %s\n", g_StepNumber, Text);
        }
        void OnDetail(const wchar_t* Text) override { Out(L"    %s\n", Text); }
        void OnError(const wchar_t* Text) override { Out(L"    错误: %s\n", Text); }
        void OnProgress(int) override {}

        bool ConfirmCertificateTrust(const CertificateSummary& Summary) override
        {
            Out(L"\n");
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     此驱动由您的电脑尚未信任的发布者签名。\n");
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     发布者：%s\n", Summary.SubjectName);
            Out(L"     颁发者：%s%s\n", Summary.IssuerName, Summary.SelfSigned ? L"  （自签名）" : L"");
            Out(L"     有效期：%s  ->  %s\n", Summary.NotBefore, Summary.NotAfter);
            Out(L"     SHA-1 指纹：%s\n", Summary.Thumbprint);
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     继续操作会把此证书添加到 LocalMachine 的 Root 和\n");
            Out(L"     TrustedPublisher 存储中。此后，本电脑将信任该发布者签名的\n");
            Out(L"     所有软件，而不仅仅是此驱动。\n");
            Out(L"    ------------------------------------------------------------------\n\n");

            if (g_Quiet)
            {
                Out(L"    已指定 /quiet - 不再询问，直接继续。\n");
                return true;
            }

            for (;;)
            {
                Out(L"    输入 Y 继续，N 取消: ");
                wchar_t Line[64] = {};
                if (fgetws(Line, static_cast<int>(ARRAYSIZE(Line)), stdin) == nullptr)
                {
                    Out(L"\n");
                    return false;
                }
                for (const wchar_t* C = Line; *C; ++C)
                {
                    if (*C == L'Y' || *C == L'y') return true;
                    if (*C == L'N' || *C == L'n') return false;
                }
            }
        }
    };

    void PrintUsage()
    {
        Out(
            L"DesktopSplitterSetup - 安装 DesktopSplitter 及其虚拟显示驱动\n"
            L"\n"
            L"不带参数运行时打开图形安装程序。\n"
            L"\n"
            L"用法:\n"
            L"  DesktopSplitterSetup.exe                 图形安装程序（向导）\n"
            L"  DesktopSplitterSetup.exe /uninstall      图形卸载程序\n"
            L"  DesktopSplitterSetup.exe /quiet          完全无界面安装\n"
            L"  DesktopSplitterSetup.exe /uninstall /quiet   无界面卸载\n"
            L"  DesktopSplitterSetup.exe /verify         报告内嵌内容与已安装状态，\n"
            L"                                           不做任何更改（无需管理员）\n"
            L"  DesktopSplitterSetup.exe /?              本帮助\n"
            L"\n"
            L"选项（控制台模式）:\n"
            L"  /dir <path>   安装位置（默认 %%ProgramFiles%%\\DesktopSplitter）\n"
            L"  /noshortcut   不创建桌面和开始菜单快捷方式\n"
            L"  /hidedisplay  启用实验性的“隐藏物理显示器”标志\n"
            L"                （仅当 Windows 版本支持时才生效）\n"
            L"  /removecert   与 /uninstall 一起使用，同时删除发布者证书\n"
            L"  /keeptemp     保留解压出的驱动包以便诊断\n"
            L"\n"
            L"退出代码:\n"
            L"   0 成功                       7 证书安装失败\n"
            L"   1 命令行错误                 8 驱动包安装失败\n"
            L"   2 未以管理员身份运行         9 设备节点创建失败\n"
            L"   3 内嵌驱动包缺失            10 安装后验证失败\n"
            L"   4 驱动包解压失败            11 卸载失败\n"
            L"   5 目录文件不可用            12 复制程序文件失败\n"
            L"   6 用户拒绝信任              13 已取消\n");
    }

    // ---- /verify ----------------------------------------------------------
    int RunVerify()
    {
        Out(L"DesktopSplitter 安装程序 - 报告\n");
        Out(L"==============================\n\n");

        Out(L"安装程序：DesktopSplitter %s\n", kProductVersion);

        EditionInfo Edition = {};
        GetEditionInfo(&Edition);
        Out(L"Windows：%s (EditionID=%s) %s 内部版本 %u\n",
            Edition.ProductName, Edition.EditionId, Edition.DisplayVersion, Edition.Build);
        Out(L"隐藏显示器：%s\n\n",
            Edition.SupportsSpecializedDisplay
                ? L"此版本支持"
                : L"不支持（需要 Pro for Workstations / Enterprise / IoT Enterprise）");

        Out(L"内嵌文件:\n");
        struct Item { int Id; const wchar_t* Name; };
        const Item Items[] = {
            { IDR_DRIVER_INF, L"DesktopSplitterVdd.inf" },
            { IDR_DRIVER_DLL, L"DesktopSplitterVdd.dll" },
            { IDR_DRIVER_CAT, L"DesktopSplitterVdd.cat" },
            { IDR_APP_EXE, L"DesktopSplitter.exe" },
            { IDR_DSCOMP_EXE, L"dscomp.exe" },
            { IDR_DEVCREATE_EXE, L"devcreate.exe" },
            { IDR_EDIDOVERRIDE_EXE, L"edidoverride.exe" },
            { IDR_COMPOSITOR_README, L"compositor-README.md" },
        };
        bool AllPresent = true;
        for (const Item& It : Items)
        {
            const DWORD Size = ResourceSize(It.Id);
            Out(L"  %-24s %12u 字节%s\n", It.Name, Size, Size ? L"" : L"   *** 缺失 ***");
            if (Size == 0) AllPresent = false;
        }
        Out(L"\n");

        if (!AllPresent)
        {
            Out(L"此安装程序不完整。\n");
            return ExitPayloadMissing;
        }

        ExtractedPackage Package = {};
        const DWORD Error = ExtractPayload(&Package);
        if (Error != ERROR_SUCCESS)
        {
            Out(L"无法解压驱动包 (0x%08X)。\n", Error);
            return ExitPayloadExtractFailed;
        }

        Out(L"驱动签名:\n");
        const LONG Trust = VerifyFileTrust(Package.CatPath);
        Out(L"  WinVerifyTrust 0x%08X - %s\n", static_cast<unsigned>(Trust), TrustStatusText(Trust));

        PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
        if (Cert != nullptr)
        {
            CertificateSummary Summary = {};
            if (SummarizeCertificate(Cert, &Summary))
            {
                Out(L"  发布者：%s\n", Summary.SubjectName);
                Out(L"  颁发者：%s%s\n", Summary.IssuerName, Summary.SelfSigned ? L"  （自签名）" : L"");
                Out(L"  有效期：%s -> %s\n", Summary.NotBefore, Summary.NotAfter);
                Out(L"  SHA-1：%s\n", Summary.Thumbprint);
            }
            CertFreeCertificateContext(Cert);
        }
        Out(L"\n");

        Out(L"安装状态:\n");
        wchar_t InstallDir[MAX_PATH] = {};
        wchar_t Version[64] = {};
        if (ReadInstalledState(InstallDir, ARRAYSIZE(InstallDir), Version, ARRAYSIZE(Version)))
        {
            Out(L"  DesktopSplitter %s 已安装在 %s\n", Version, InstallDir);
            Out(L"  再次运行安装程序将原地升级。\n");
        }
        else
        {
            Out(L"  未安装（没有“添加/删除程序”条目）。\n");
        }

        OemInfList Staged = {};
        if (FindAllOemInfs(kCatalogFileName, &Staged))
        {
            for (size_t i = 0; i < Staged.Count; ++i)
            {
                Out(L"  已暂存的驱动包: %s  DriverVer=%s\n", Staged.Names[i], Staged.DriverVer[i]);
            }
            if (Staged.Count > 1)
            {
                Out(L"  （共有 %zu 份；安装时会只保留新的一份）\n", Staged.Count);
            }
        }
        else
        {
            Out(L"  驱动包：不在驱动程序存储中。\n");
        }

        DeviceNodeInfo Info = {};
        if (FindDeviceNode(kHardwareId, &Info))
        {
            Out(L"  设备存在: %s  problem=%u (%s)\n",
                Info.InstanceId, Info.Problem, ProblemCodeText(Info.Problem));
        }
        else
        {
            Out(L"  设备：不存在。\n");
        }

        CleanupPayload(&Package);
        Out(L"\n未对这台电脑做任何更改。\n");
        return ExitSuccess;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE Instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    int ArgCount = 0;
    LPWSTR* Args = CommandLineToArgvW(GetCommandLineW(), &ArgCount);
    if (Args == nullptr)
    {
        return ExitUsage;
    }

    bool WantUninstall = false, WantVerify = false, WantHelp = false, WantSelfTest = false;
    bool RemoveCert = false, KeepTemp = false, NoShortcut = false, HideDisplay = false;
    const wchar_t* DirOverride = nullptr;

    for (int Index = 1; Index < ArgCount; ++Index)
    {
        const wchar_t* Arg = Args[Index];
        if (Arg[0] == L'/' || Arg[0] == L'-')
        {
            ++Arg;
            if (*Arg == L'-') ++Arg;
        }

        if (!_wcsicmp(Arg, L"?") || !_wcsicmp(Arg, L"h") || !_wcsicmp(Arg, L"help")) WantHelp = true;
        else if (!_wcsicmp(Arg, L"uninstall") || !_wcsicmp(Arg, L"remove"))          WantUninstall = true;
        else if (!_wcsicmp(Arg, L"verify") || !_wcsicmp(Arg, L"verify-package"))     WantVerify = true;
        else if (!_wcsicmp(Arg, L"quiet") || !_wcsicmp(Arg, L"silent"))              g_Quiet = true;
        else if (!_wcsicmp(Arg, L"removecert"))                                      RemoveCert = true;
        else if (!_wcsicmp(Arg, L"keeptemp"))                                        KeepTemp = true;
        else if (!_wcsicmp(Arg, L"noshortcut"))                                      NoShortcut = true;
        else if (!_wcsicmp(Arg, L"hidedisplay"))                                     HideDisplay = true;
        else if (!_wcsicmp(Arg, L"selftest"))                                        WantSelfTest = true;
        else if (!_wcsicmp(Arg, L"dir") && Index + 1 < ArgCount)                     DirOverride = Args[++Index];
        else
        {
            EnsureConsole();
            Out(L"无法识别的选项: %s\n\n", Args[Index]);
            PrintUsage();
            PauseIfInteractive();
            return ExitUsage;
        }
    }

    // ---- console-only modes (handled before anything else) ----------------
    if (WantHelp)
    {
        EnsureConsole();
        PrintUsage();
        PauseIfInteractive();
        return ExitSuccess;
    }

    if (WantSelfTest)
    {
        // Headless UI test: builds every wizard page without showing a window.
        EnsureConsole();
        const int Result = RunWizardSelfTest(Instance);
        PauseIfInteractive();
        return Result;
    }

    if (WantVerify)
    {
        // Read-only: deliberately does not require elevation.
        EnsureConsole();
        const int Result = RunVerify();
        PauseIfInteractive();
        return Result;
    }

    // ---- everything below changes the machine -----------------------------
    if (!IsElevated())
    {
        // The manifest requests requireAdministrator, so this is belt and
        // braces; without it the failure would surface as a confusing
        // access-denied deep inside SetupAPI.
        if (g_Quiet)
        {
            EnsureConsole();
            Out(L"此安装程序必须以管理员身份运行。\n");
        }
        else
        {
            MessageBoxW(nullptr,
                        L"DesktopSplitter 安装程序必须以管理员身份运行。\n\n"
                        L"请右键点击安装程序，选择“以管理员身份运行”。",
                        L"DesktopSplitter 安装程序", MB_ICONERROR | MB_OK);
        }
        return ExitNotElevated;
    }

    if (g_Quiet)
    {
        EnsureConsole();
        ConsoleSink Sink;
        int Result;

        if (WantUninstall)
        {
            UninstallOptions Options = {};
            Options.RemoveCertificate = RemoveCert;
            Options.KeepTemp = KeepTemp;
            Result = RunUninstallSequence(&Options, &Sink);
        }
        else
        {
            EditionInfo Edition = {};
            GetEditionInfo(&Edition);

            InstallOptions Options = {};
            if (DirOverride != nullptr)
            {
                StringCchCopyW(Options.InstallDir, ARRAYSIZE(Options.InstallDir), DirOverride);
            }
            else
            {
                GetDefaultInstallDir(Options.InstallDir, ARRAYSIZE(Options.InstallDir));
            }
            Options.DesktopShortcut = !NoShortcut;
            Options.StartMenuShortcut = !NoShortcut;
            Options.HidePhysicalDisplaySupported = Edition.SupportsSpecializedDisplay;
            Options.HidePhysicalDisplay = HideDisplay && Edition.SupportsSpecializedDisplay;

            Result = RunInstallSequence(&Options, &Sink);
        }

        Out(L"\n%s（退出代码 %d）\n", Result == ExitSuccess ? L"已完成。" : L"失败。", Result);
        return Result;
    }

    // ---- graphical wizard --------------------------------------------------
    return RunWizard(Instance, WantUninstall ? WizardMode::Uninstall : WizardMode::Install);
}
