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

        wprintf(L"\nPress Enter to close this window...");
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
        void OnError(const wchar_t* Text) override { Out(L"    ERROR: %s\n", Text); }
        void OnProgress(int) override {}

        bool ConfirmCertificateTrust(const CertificateSummary& Summary) override
        {
            Out(L"\n");
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     This driver is signed by a publisher your PC does not yet trust.\n");
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     Publisher       : %s\n", Summary.SubjectName);
            Out(L"     Issued by       : %s%s\n", Summary.IssuerName, Summary.SelfSigned ? L"  (self-signed)" : L"");
            Out(L"     Valid           : %s  ->  %s\n", Summary.NotBefore, Summary.NotAfter);
            Out(L"     SHA-1 thumbprint: %s\n", Summary.Thumbprint);
            Out(L"    ------------------------------------------------------------------\n");
            Out(L"     Continuing adds this certificate to LocalMachine Root and\n");
            Out(L"     TrustedPublisher. This PC will then trust ANY software signed by\n");
            Out(L"     that publisher - not just this driver.\n");
            Out(L"    ------------------------------------------------------------------\n\n");

            if (g_Quiet)
            {
                Out(L"    /quiet specified - proceeding without asking.\n");
                return true;
            }

            for (;;)
            {
                Out(L"    Type Y to continue, N to cancel: ");
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
            L"DesktopSplitterSetup - installs DesktopSplitter and its virtual display driver\n"
            L"\n"
            L"Run with no arguments for the graphical installer.\n"
            L"\n"
            L"Usage:\n"
            L"  DesktopSplitterSetup.exe                 graphical installer (wizard)\n"
            L"  DesktopSplitterSetup.exe /uninstall      graphical uninstaller\n"
            L"  DesktopSplitterSetup.exe /quiet          install with no UI at all\n"
            L"  DesktopSplitterSetup.exe /uninstall /quiet   remove with no UI\n"
            L"  DesktopSplitterSetup.exe /verify         report what is embedded and what is\n"
            L"                                           installed, change nothing (no admin)\n"
            L"  DesktopSplitterSetup.exe /?              this help\n"
            L"\n"
            L"Options (console modes):\n"
            L"  /dir <path>   install location (default %%ProgramFiles%%\\DesktopSplitter)\n"
            L"  /noshortcut   do not create desktop or Start menu shortcuts\n"
            L"  /hidedisplay  enable the experimental \"hide the physical display\" flag\n"
            L"                (ignored unless the Windows edition supports it)\n"
            L"  /removecert   with /uninstall, also remove the publisher certificate\n"
            L"  /keeptemp     keep the unpacked driver package for diagnostics\n"
            L"\n"
            L"Exit codes:\n"
            L"   0 success                    7 certificate installation failed\n"
            L"   1 bad command line           8 driver package installation failed\n"
            L"   2 not elevated               9 device node creation failed\n"
            L"   3 embedded package missing  10 post-install verification failed\n"
            L"   4 package extraction failed 11 uninstall failed\n"
            L"   5 catalog unusable          12 copying program files failed\n"
            L"   6 trust declined by user    13 cancelled\n");
    }

    // ---- /verify ----------------------------------------------------------
    int RunVerify()
    {
        Out(L"DesktopSplitter setup - report\n");
        Out(L"==============================\n\n");

        Out(L"Installer      : DesktopSplitter %s\n", kProductVersion);

        EditionInfo Edition = {};
        GetEditionInfo(&Edition);
        Out(L"Windows        : %s (EditionID=%s) %s build %u\n",
            Edition.ProductName, Edition.EditionId, Edition.DisplayVersion, Edition.Build);
        Out(L"Hide-display   : %s\n\n",
            Edition.SupportsSpecializedDisplay
                ? L"supported on this edition"
                : L"NOT supported (needs Pro for Workstations / Enterprise / IoT Enterprise)");

        Out(L"Embedded payload:\n");
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
            Out(L"  %-24s %12u bytes%s\n", It.Name, Size, Size ? L"" : L"   *** MISSING ***");
            if (Size == 0) AllPresent = false;
        }
        Out(L"\n");

        if (!AllPresent)
        {
            Out(L"This installer is incomplete.\n");
            return ExitPayloadMissing;
        }

        ExtractedPackage Package = {};
        const DWORD Error = ExtractPayload(&Package);
        if (Error != ERROR_SUCCESS)
        {
            Out(L"Could not unpack the driver package (0x%08X).\n", Error);
            return ExitPayloadExtractFailed;
        }

        Out(L"Driver signature:\n");
        const LONG Trust = VerifyFileTrust(Package.CatPath);
        Out(L"  WinVerifyTrust 0x%08X - %s\n", static_cast<unsigned>(Trust), TrustStatusText(Trust));

        PCCERT_CONTEXT Cert = LoadSignerCertificate(Package.CatPath);
        if (Cert != nullptr)
        {
            CertificateSummary Summary = {};
            if (SummarizeCertificate(Cert, &Summary))
            {
                Out(L"  Publisher : %s\n", Summary.SubjectName);
                Out(L"  Issued by : %s%s\n", Summary.IssuerName, Summary.SelfSigned ? L"  (self-signed)" : L"");
                Out(L"  Valid     : %s -> %s\n", Summary.NotBefore, Summary.NotAfter);
                Out(L"  SHA-1     : %s\n", Summary.Thumbprint);
            }
            CertFreeCertificateContext(Cert);
        }
        Out(L"\n");

        Out(L"Installed state:\n");
        wchar_t InstallDir[MAX_PATH] = {};
        wchar_t Version[64] = {};
        if (ReadInstalledState(InstallDir, ARRAYSIZE(InstallDir), Version, ARRAYSIZE(Version)))
        {
            Out(L"  DesktopSplitter %s is installed in %s\n", Version, InstallDir);
            Out(L"  Running setup will upgrade it in place.\n");
        }
        else
        {
            Out(L"  Not installed (no Add/Remove Programs entry).\n");
        }

        OemInfList Staged = {};
        if (FindAllOemInfs(kCatalogFileName, &Staged))
        {
            for (size_t i = 0; i < Staged.Count; ++i)
            {
                Out(L"  Driver package staged: %s  DriverVer=%s\n", Staged.Names[i], Staged.DriverVer[i]);
            }
            if (Staged.Count > 1)
            {
                Out(L"  (%zu copies present; installing prunes all but the new one)\n", Staged.Count);
            }
        }
        else
        {
            Out(L"  Driver package: not in the driver store.\n");
        }

        DeviceNodeInfo Info = {};
        if (FindDeviceNode(kHardwareId, &Info))
        {
            Out(L"  Device present: %s  problem=%u (%s)\n",
                Info.InstanceId, Info.Problem, ProblemCodeText(Info.Problem));
        }
        else
        {
            Out(L"  Device: not present.\n");
        }

        CleanupPayload(&Package);
        Out(L"\nNothing was changed on this PC.\n");
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
            Out(L"Unrecognised option: %s\n\n", Args[Index]);
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
            Out(L"This installer must run as administrator.\n");
        }
        else
        {
            MessageBoxW(nullptr,
                        L"DesktopSplitter Setup must be run as administrator.\n\n"
                        L"Right-click the installer and choose \"Run as administrator\".",
                        L"DesktopSplitter Setup", MB_ICONERROR | MB_OK);
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

        Out(L"\n%s (exit %d)\n", Result == ExitSuccess ? L"Completed." : L"Failed.", Result);
        return Result;
    }

    // ---- graphical wizard --------------------------------------------------
    return RunWizard(Instance, WantUninstall ? WizardMode::Uninstall : WizardMode::Install);
}
