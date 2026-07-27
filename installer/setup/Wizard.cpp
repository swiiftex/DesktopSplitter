#include "Wizard.h"
#include "InstallEngine.h"
#include "Edition.h"
#include "Payload.h"
#include "resource.h"

#include <commctrl.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>
#include <process.h>

#pragma comment(lib, "comctl32.lib")

namespace
{
    using namespace DeskSplit;

    // Layout is authored at 96 DPI and scaled at runtime.
    constexpr int kBaseWidth = 500;
    constexpr int kBaseHeight = 360;
    constexpr int kBaseHeaderHeight = 58;
    constexpr int kBaseButtonBarHeight = 46;
    constexpr int kBaseMargin = 16;

    const wchar_t kWindowClass[] = L"DesktopSplitterSetupWizard";

    int Scale(int Value, UINT Dpi) { return MulDiv(Value, static_cast<int>(Dpi), 96); }

    // /selftest can force a DPI so the layout is checked at 125% / 150% too.
    UINT g_TestDpiOverride = 0;

    struct Wizard
    {
        HINSTANCE Instance = nullptr;
        HWND Main = nullptr;
        UINT Dpi = 96;
        HFONT FontNormal = nullptr;
        HFONT FontHeader = nullptr;
        HFONT FontTitle = nullptr;

        WizardMode Mode = WizardMode::Install;
        WizardPage Page = WizardPage::Welcome;

        InstallOptions Options = {};
        EditionInfo Edition = {};

        bool Headless = false;      // /selftest: never show, never run the engine
        bool Running = false;       // engine thread active
        bool Finished = false;
        bool Cancelled = false;
        int  Result = 0;
        bool LaunchApp = true;
        bool RemoveCert = false;

        // Controls
        HWND Header = nullptr, SubHeader = nullptr, Body = nullptr;
        HWND EditDir = nullptr, BtnBrowse = nullptr;
        HWND ChkDesktop = nullptr, ChkStartMenu = nullptr, ChkHide = nullptr, HideNote = nullptr;
        HWND List = nullptr, Progress = nullptr, StepText = nullptr;
        HWND ChkLaunch = nullptr, ChkRemoveCert = nullptr;
        HWND BtnBack = nullptr, BtnNext = nullptr, BtnCancel = nullptr;
    };

    Wizard* g_Wizard = nullptr;

    // ---- window helpers ---------------------------------------------------
    HWND MakeControl(Wizard* W, const wchar_t* Class, const wchar_t* Text, DWORD Style,
                     int X, int Y, int Cx, int Cy, int Id, HFONT Font = nullptr)
    {
        HWND Control = CreateWindowExW(0, Class, Text, WS_CHILD | Style,
                                       Scale(X, W->Dpi), Scale(Y, W->Dpi),
                                       Scale(Cx, W->Dpi), Scale(Cy, W->Dpi),
                                       W->Main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Id)),
                                       W->Instance, nullptr);
        if (Control != nullptr)
        {
            SendMessageW(Control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(Font ? Font : W->FontNormal), TRUE);
        }
        return Control;
    }

    void SetShown(HWND Control, bool Shown)
    {
        if (Control != nullptr)
        {
            ShowWindow(Control, Shown ? SW_SHOW : SW_HIDE);
        }
    }

    // The control's OWN visibility bit. IsWindowVisible() additionally requires
    // every ancestor to be visible, which is never true in the headless
    // /selftest (the wizard is deliberately never shown), so it cannot be used
    // to assert which page is up.
    bool IsShown(HWND Control)
    {
        return Control != nullptr &&
               (GetWindowLongPtrW(Control, GWL_STYLE) & WS_VISIBLE) != 0;
    }

    HFONT MakeFont(UINT Dpi, int PointSize, bool Bold)
    {
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(PointSize, static_cast<int>(Dpi), 72);
        lf.lfWeight = Bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI");
        return CreateFontIndirectW(&lf);
    }

    void AppendLog(Wizard* W, const wchar_t* Text)
    {
        if (W->List == nullptr) return;
        const int Index = static_cast<int>(SendMessageW(W->List, LB_ADDSTRING, 0,
                                                        reinterpret_cast<LPARAM>(Text)));
        SendMessageW(W->List, LB_SETTOPINDEX, static_cast<WPARAM>(Index), 0);
    }

    // ---- page content -----------------------------------------------------
    const wchar_t* PageTitle(Wizard* W, WizardPage Page)
    {
        const bool Un = (W->Mode == WizardMode::Uninstall);
        switch (Page)
        {
        case WizardPage::Welcome:  return Un ? L"Remove DesktopSplitter" : L"Welcome to DesktopSplitter";
        case WizardPage::Options:  return Un ? L"Removal options" : L"Installation options";
        case WizardPage::Progress: return Un ? L"Removing" : L"Installing";
        case WizardPage::Finish:   return Un ? L"Removal complete" : L"Installation complete";
        default:                   return L"";
        }
    }

    const wchar_t* PageSubtitle(Wizard* W, WizardPage Page)
    {
        const bool Un = (W->Mode == WizardMode::Uninstall);
        switch (Page)
        {
        case WizardPage::Welcome:
            return Un ? L"This will remove DesktopSplitter and its display driver."
                      : L"Split one monitor into several that Windows treats as separate displays.";
        case WizardPage::Options:
            return Un ? L"Choose what to remove." : L"Choose where to install and what to create.";
        case WizardPage::Progress:
            return L"This can take a minute. Your screen may flicker.";
        case WizardPage::Finish:
            return Un ? L"DesktopSplitter has been removed." : L"DesktopSplitter is ready to use.";
        default:
            return L"";
        }
    }

    void BuildControls(Wizard* W)
    {
        const int Width = kBaseWidth;
        const int ContentTop = kBaseHeaderHeight + kBaseMargin;
        const int M = kBaseMargin;
        const int InnerW = Width - 2 * M;

        // The header and the button bar are on EVERY page, so they are created
        // visible. Page-specific controls below are created hidden and switched
        // by ShowPage(). Forgetting WS_VISIBLE here is what made the shipped
        // wizard render with no buttons at all.
        W->Header = MakeControl(W, L"STATIC", L"", SS_LEFT | WS_VISIBLE, M, 12, InnerW, 22, IDC_STATIC_HEADER, W->FontTitle);
        W->SubHeader = MakeControl(W, L"STATIC", L"", SS_LEFT | WS_VISIBLE, M, 34, InnerW, 18, IDC_STATIC_SUBHEADER);

        // --- Welcome
        W->Body = MakeControl(W, L"STATIC", L"", SS_LEFT, M, ContentTop, InnerW, 150, IDC_STATIC_BODY);

        // --- Options
        int Y = ContentTop;
        MakeControl(W, L"STATIC", L"Install location:", SS_LEFT, M, Y, InnerW, 16, 0);
        Y += 20;
        W->EditDir = MakeControl(W, L"EDIT", W->Options.InstallDir,
                                 WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, M, Y, InnerW - 84, 24,
                                 IDC_EDIT_INSTALLDIR);
        W->BtnBrowse = MakeControl(W, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON,
                                   M + InnerW - 78, Y, 78, 24, IDC_BTN_BROWSE);
        Y += 38;
        W->ChkDesktop = MakeControl(W, L"BUTTON", L"Create a desktop shortcut",
                                    WS_TABSTOP | BS_AUTOCHECKBOX, M, Y, InnerW, 20, IDC_CHK_DESKTOP_SHORTCUT);
        Y += 24;
        W->ChkStartMenu = MakeControl(W, L"BUTTON", L"Create a Start menu shortcut",
                                      WS_TABSTOP | BS_AUTOCHECKBOX, M, Y, InnerW, 20, IDC_CHK_STARTMENU_SHORTCUT);
        Y += 30;
        W->ChkHide = MakeControl(W, L"BUTTON", L"Hide the physical display when splitting (experimental)",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, M, Y, InnerW, 20, IDC_CHK_HIDE_PHYSICAL);
        Y += 22;
        W->HideNote = MakeControl(W, L"STATIC", L"", SS_LEFT, M + 18, Y, InnerW - 18, 32, IDC_STATIC_HIDE_NOTE);

        // Uninstall-only option shares the Options page.
        W->ChkRemoveCert = MakeControl(W, L"BUTTON",
                                       L"Also remove the publisher certificate from this PC",
                                       WS_TABSTOP | BS_AUTOCHECKBOX, M, ContentTop + 20, InnerW, 20,
                                       IDC_CHK_REMOVE_CERT);

        // --- Progress
        W->StepText = MakeControl(W, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, M, ContentTop, InnerW, 18, IDC_STATIC_STEP);
        W->Progress = MakeControl(W, PROGRESS_CLASSW, L"", 0, M, ContentTop + 22, InnerW, 16, IDC_PROGRESS);
        W->List = MakeControl(W, L"LISTBOX", L"",
                              WS_BORDER | WS_VSCROLL | LBS_NOSEL | LBS_NOINTEGRALHEIGHT,
                              M, ContentTop + 46, InnerW, 150, IDC_LIST_LOG);
        SendMessageW(W->Progress, PBM_SETRANGE32, 0, 100);

        // --- Finish
        W->ChkLaunch = MakeControl(W, L"BUTTON", L"Launch DesktopSplitter",
                                   WS_TABSTOP | BS_AUTOCHECKBOX, M, ContentTop + 110, InnerW, 20, IDC_CHK_LAUNCH);

        // --- buttons (always visible; LayoutChrome positions them from the
        //     REAL client rect, so these coordinates are only a starting point)
        const int BtnY = kBaseHeight - kBaseButtonBarHeight + 10;
        W->BtnBack = MakeControl(W, L"BUTTON", L"< Back", WS_TABSTOP | BS_PUSHBUTTON | WS_VISIBLE,
                                 Width - 3 * 88 - M, BtnY, 84, 26, IDC_BTN_BACK);
        W->BtnNext = MakeControl(W, L"BUTTON", L"Next >", WS_TABSTOP | BS_DEFPUSHBUTTON | WS_VISIBLE,
                                 Width - 2 * 88 - M, BtnY, 84, 26, IDC_BTN_NEXT);
        W->BtnCancel = MakeControl(W, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_VISIBLE,
                                   Width - 88 - M, BtnY, 84, 26, IDC_BTN_CANCEL);

        CheckDlgButton(W->Main, IDC_CHK_DESKTOP_SHORTCUT, W->Options.DesktopShortcut ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(W->Main, IDC_CHK_STARTMENU_SHORTCUT, W->Options.StartMenuShortcut ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(W->Main, IDC_CHK_LAUNCH, BST_CHECKED);

        // The feature gate: only editions that can actually honour a
        // specialized display may enable this.
        if (W->Options.HidePhysicalDisplaySupported)
        {
            EnableWindow(W->ChkHide, TRUE);
            SetWindowTextW(W->HideNote,
                           L"Supported on this edition. The app hides the real monitor while a split is active.");
        }
        else
        {
            EnableWindow(W->ChkHide, FALSE);
            CheckDlgButton(W->Main, IDC_CHK_HIDE_PHYSICAL, BST_UNCHECKED);
            SetWindowTextW(W->HideNote,
                           L"Requires Windows Pro for Workstations / Enterprise. "
                           L"This PC runs an edition that cannot hide a display.");
        }
    }

    // Positions the always-visible chrome from the ACTUAL client rect rather
    // than from the size we asked CreateWindow for. AdjustWindowRect can be off
    // (DPI, theme, accessibility settings), and a button bar computed from an
    // assumed height is exactly how buttons end up below the visible area.
    void LayoutChrome(Wizard* W)
    {
        if (W->Main == nullptr) return;

        RECT Client = {};
        GetClientRect(W->Main, &Client);
        if (Client.right <= 0 || Client.bottom <= 0) return;

        const int Margin = Scale(kBaseMargin, W->Dpi);
        const int BarHeight = Scale(kBaseButtonBarHeight, W->Dpi);
        const int ButtonWidth = Scale(84, W->Dpi);
        const int ButtonHeight = Scale(26, W->Dpi);
        const int Gap = Scale(6, W->Dpi);

        int Y = Client.bottom - BarHeight + (BarHeight - ButtonHeight) / 2;
        if (Y + ButtonHeight > Client.bottom) Y = Client.bottom - ButtonHeight;
        if (Y < 0) Y = 0;

        int X = Client.right - Margin - ButtonWidth;
        MoveWindow(W->BtnCancel, X, Y, ButtonWidth, ButtonHeight, TRUE);
        X -= ButtonWidth + Gap;
        MoveWindow(W->BtnNext, X, Y, ButtonWidth, ButtonHeight, TRUE);
        X -= ButtonWidth + Gap;
        MoveWindow(W->BtnBack, X, Y, ButtonWidth, ButtonHeight, TRUE);

        const int InnerWidth = Client.right - 2 * Margin;
        MoveWindow(W->Header, Margin, Scale(12, W->Dpi), InnerWidth, Scale(22, W->Dpi), TRUE);
        MoveWindow(W->SubHeader, Margin, Scale(34, W->Dpi), InnerWidth, Scale(18, W->Dpi), TRUE);
    }

    void ShowPage(Wizard* W, WizardPage Page)
    {
        W->Page = Page;

        const bool Welcome = (Page == WizardPage::Welcome);
        const bool Options = (Page == WizardPage::Options);
        const bool Progress = (Page == WizardPage::Progress);
        const bool Finish = (Page == WizardPage::Finish);
        const bool Install = (W->Mode == WizardMode::Install);

        SetWindowTextW(W->Header, PageTitle(W, Page));
        SetWindowTextW(W->SubHeader, PageSubtitle(W, Page));

        SetShown(W->Body, Welcome || Finish);
        SetShown(W->EditDir, Options && Install);
        SetShown(W->BtnBrowse, Options && Install);
        SetShown(W->ChkDesktop, Options && Install);
        SetShown(W->ChkStartMenu, Options && Install);
        SetShown(W->ChkHide, Options && Install);
        SetShown(W->HideNote, Options && Install);
        SetShown(W->ChkRemoveCert, Options && !Install);
        SetShown(W->StepText, Progress);
        SetShown(W->Progress, Progress);
        SetShown(W->List, Progress);
        SetShown(W->ChkLaunch, Finish && Install && W->Result == ExitSuccess);

        if (Welcome)
        {
            wchar_t Text[1024];
            wchar_t Installed[MAX_PATH] = {};
            wchar_t Version[64] = {};
            const bool Have = ReadInstalledState(Installed, ARRAYSIZE(Installed), Version, ARRAYSIZE(Version));

            if (W->Mode == WizardMode::Uninstall)
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text),
                    L"This will remove:\r\n\r\n"
                    L"    \x2022  The DesktopSplitter app and compositor\r\n"
                    L"    \x2022  The virtual display driver and its device\r\n"
                    L"    \x2022  Shortcuts and settings\r\n\r\n"
                    L"Any active split is undone first, so your desktop returns to the real monitor.\r\n\r\n"
                    L"Installed version: %s", Have ? Version : L"(unknown)");
            }
            else if (Have)
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text),
                    L"DesktopSplitter %s is already installed in\r\n%s\r\n\r\n"
                    L"This will upgrade it to version %s, keeping your settings.\r\n\r\n"
                    L"Administrator rights are required because a display driver is installed.",
                    Version, Installed, kProductVersion);
            }
            else
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text),
                    L"Version %s\r\n\r\n"
                    L"DesktopSplitter turns one monitor into up to four that Windows treats as\r\n"
                    L"completely separate displays, so windows snap and maximise to each part.\r\n\r\n"
                    L"This installs a virtual display driver, so Windows will ask for administrator\r\n"
                    L"rights and may show a driver warning.\r\n\r\n"
                    L"Click Next to continue.", kProductVersion);
            }
            SetWindowTextW(W->Body, Text);
        }

        if (Finish)
        {
            wchar_t Text[1024];
            if (W->Result == ExitSuccess)
            {
                if (W->Mode == WizardMode::Uninstall)
                {
                    StringCchCopyW(Text, ARRAYSIZE(Text),
                        L"DesktopSplitter has been removed from this PC.\r\n\r\n"
                        L"If a restart was mentioned above, please restart to finish cleaning up "
                        L"the display driver.");
                }
                else
                {
                    StringCchPrintfW(Text, ARRAYSIZE(Text),
                        L"DesktopSplitter has been installed to\r\n%s\r\n\r\n"
                        L"Open it to pick a monitor and a layout, then click Apply.",
                        W->Options.InstallDir);
                }
            }
            else
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text),
                    L"Setup did not complete (code %d).\r\n\r\n"
                    L"The log on the previous page shows which step failed. Nothing else was changed.",
                    W->Result);
            }
            SetWindowTextW(W->Body, Text);
        }

        EnableWindow(W->BtnBack, (Options && !W->Running) ? TRUE : FALSE);
        EnableWindow(W->BtnNext, (!Progress || W->Finished) ? TRUE : FALSE);
        EnableWindow(W->BtnCancel, Finish ? FALSE : TRUE);

        SetWindowTextW(W->BtnNext,
                       Finish ? L"Finish"
                              : (Options ? (W->Mode == WizardMode::Uninstall ? L"Remove" : L"Install")
                                         : L"Next >"));

        InvalidateRect(W->Main, nullptr, TRUE);
    }

    // ---- engine sink ------------------------------------------------------
    // The engine runs on a worker thread; every UI touch is marshalled to the
    // wizard thread with SendMessage so the controls stay single-threaded.
    enum : UINT
    {
        WM_WIZ_STEP = WM_APP + 1,
        WM_WIZ_DETAIL,
        WM_WIZ_ERROR,
        WM_WIZ_PROGRESS,
        WM_WIZ_CONFIRMCERT,
        WM_WIZ_DONE,
    };

    struct GuiSink : IStepSink
    {
        Wizard* W;
        explicit GuiSink(Wizard* Wiz) : W(Wiz) {}

        void OnStep(const wchar_t* Text) override
        {
            SendMessageW(W->Main, WM_WIZ_STEP, 0, reinterpret_cast<LPARAM>(Text));
        }
        void OnDetail(const wchar_t* Text) override
        {
            SendMessageW(W->Main, WM_WIZ_DETAIL, 0, reinterpret_cast<LPARAM>(Text));
        }
        void OnError(const wchar_t* Text) override
        {
            SendMessageW(W->Main, WM_WIZ_ERROR, 0, reinterpret_cast<LPARAM>(Text));
        }
        void OnProgress(int Percent) override
        {
            SendMessageW(W->Main, WM_WIZ_PROGRESS, static_cast<WPARAM>(Percent), 0);
        }
        bool ConfirmCertificateTrust(const CertificateSummary& Summary) override
        {
            return SendMessageW(W->Main, WM_WIZ_CONFIRMCERT, 0,
                                reinterpret_cast<LPARAM>(&Summary)) != 0;
        }
        bool IsCancelled() override { return W->Cancelled; }
    };

    unsigned __stdcall EngineThread(void* Param)
    {
        Wizard* W = static_cast<Wizard*>(Param);
        GuiSink Sink(W);

        int Result;
        if (W->Mode == WizardMode::Uninstall)
        {
            UninstallOptions Options = {};
            Options.RemoveCertificate = W->RemoveCert;
            Result = RunUninstallSequence(&Options, &Sink);
        }
        else
        {
            Result = RunInstallSequence(&W->Options, &Sink);
        }

        PostMessageW(W->Main, WM_WIZ_DONE, static_cast<WPARAM>(Result), 0);
        return 0;
    }

    // Certificate consent, as a proper dialog.
    bool AskCertificateTrust(Wizard* W, const CertificateSummary& Summary)
    {
        wchar_t Body[1400];
        StringCchPrintfW(Body, ARRAYSIZE(Body),
            L"Windows does not trust the publisher of this display driver yet.\n\n"
            L"Publisher:\t%s\n"
            L"Issued by:\t%s%s\n"
            L"Valid:\t%s  to  %s\n"
            L"SHA-1:\t%s\n\n"
            L"To install the driver, this PC must be told to trust that publisher. "
            L"Continuing adds the certificate above to the machine's Trusted Root "
            L"Certification Authorities and Trusted Publishers stores.\n\n"
            L"This PC will then accept ANY software signed by that publisher \x2014 not just "
            L"this driver. Only continue if you obtained this installer from a source you "
            L"trust and the SHA-1 above matches the one they published.\n\n"
            L"You can undo this later by removing DesktopSplitter and ticking "
            L"\"Also remove the publisher certificate\".",
            Summary.SubjectName,
            Summary.IssuerName,
            Summary.SelfSigned ? L"  (self-signed)" : L"",
            Summary.NotBefore, Summary.NotAfter,
            Summary.Thumbprint);

        const int Answer = MessageBoxW(W->Main, Body, L"Trust this publisher?",
                                       MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        return Answer == IDYES;
    }

    void StartEngine(Wizard* W)
    {
        // Freeze the options exactly as the user left them.
        if (W->Mode == WizardMode::Install)
        {
            GetWindowTextW(W->EditDir, W->Options.InstallDir, ARRAYSIZE(W->Options.InstallDir));
            W->Options.DesktopShortcut = IsDlgButtonChecked(W->Main, IDC_CHK_DESKTOP_SHORTCUT) == BST_CHECKED;
            W->Options.StartMenuShortcut = IsDlgButtonChecked(W->Main, IDC_CHK_STARTMENU_SHORTCUT) == BST_CHECKED;
            W->Options.HidePhysicalDisplay =
                W->Options.HidePhysicalDisplaySupported &&
                IsDlgButtonChecked(W->Main, IDC_CHK_HIDE_PHYSICAL) == BST_CHECKED;
        }
        else
        {
            W->RemoveCert = IsDlgButtonChecked(W->Main, IDC_CHK_REMOVE_CERT) == BST_CHECKED;
        }

        W->Running = true;
        W->Finished = false;
        ShowPage(W, WizardPage::Progress);

        _beginthreadex(nullptr, 0, EngineThread, W, 0, nullptr);
    }

    void BrowseForFolder(Wizard* W)
    {
        wchar_t Current[MAX_PATH] = {};
        GetWindowTextW(W->EditDir, Current, ARRAYSIZE(Current));

        BROWSEINFOW Info = {};
        Info.hwndOwner = W->Main;
        Info.lpszTitle = L"Choose where to install DesktopSplitter";
        Info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        LPITEMIDLIST List = SHBrowseForFolderW(&Info);
        if (List == nullptr) return;

        wchar_t Chosen[MAX_PATH] = {};
        if (SHGetPathFromIDListW(List, Chosen))
        {
            wchar_t Full[MAX_PATH] = {};
            // Append the product name unless the user already picked it.
            const wchar_t* Leaf = wcsrchr(Chosen, L'\\');
            if (Leaf != nullptr && _wcsicmp(Leaf + 1, kProductName) == 0)
            {
                StringCchCopyW(Full, ARRAYSIZE(Full), Chosen);
            }
            else
            {
                StringCchPrintfW(Full, ARRAYSIZE(Full), L"%s\\%s", Chosen, kProductName);
            }
            SetWindowTextW(W->EditDir, Full);
        }
        CoTaskMemFree(List);
    }

    LRESULT CALLBACK WizardProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam)
    {
        Wizard* W = g_Wizard;

        switch (Message)
        {
        case WM_ERASEBKGND:
        {
            // Classic wizard look: white content area, themed button bar.
            HDC Dc = reinterpret_cast<HDC>(wParam);
            RECT Client = {};
            GetClientRect(Window, &Client);

            const int BarTop = Client.bottom - Scale(kBaseButtonBarHeight, W->Dpi);
            RECT Content = { 0, 0, Client.right, BarTop };
            FillRect(Dc, &Content, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

            RECT Bar = { 0, BarTop, Client.right, Client.bottom };
            FillRect(Dc, &Bar, GetSysColorBrush(COLOR_BTNFACE));

            RECT Line = { 0, BarTop, Client.right, BarTop + 1 };
            FillRect(Dc, &Line, GetSysColorBrush(COLOR_3DSHADOW));
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        {
            HDC Dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(Dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_BTN_NEXT:
                if (W->Page == WizardPage::Welcome)      ShowPage(W, WizardPage::Options);
                else if (W->Page == WizardPage::Options) StartEngine(W);
                else if (W->Page == WizardPage::Finish)
                {
                    W->LaunchApp = IsDlgButtonChecked(Window, IDC_CHK_LAUNCH) == BST_CHECKED;
                    DestroyWindow(Window);
                }
                else if (W->Finished)                    ShowPage(W, WizardPage::Finish);
                return 0;

            case IDC_BTN_BACK:
                if (W->Page == WizardPage::Options) ShowPage(W, WizardPage::Welcome);
                return 0;

            case IDC_BTN_BROWSE:
                BrowseForFolder(W);
                return 0;

            case IDC_BTN_CANCEL:
                if (W->Running)
                {
                    if (MessageBoxW(Window, L"Setup is part-way through. Cancel anyway?",
                                    L"DesktopSplitter Setup",
                                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES)
                    {
                        W->Cancelled = true;
                    }
                }
                else
                {
                    W->Result = ExitCancelled;
                    DestroyWindow(Window);
                }
                return 0;
            default:
                break;
            }
            break;

        case WM_WIZ_STEP:
            SetWindowTextW(W->StepText, reinterpret_cast<const wchar_t*>(lParam));
            AppendLog(W, reinterpret_cast<const wchar_t*>(lParam));
            return 0;

        case WM_WIZ_DETAIL:
        {
            wchar_t Line[1100];
            StringCchPrintfW(Line, ARRAYSIZE(Line), L"      %s", reinterpret_cast<const wchar_t*>(lParam));
            AppendLog(W, Line);
            return 0;
        }

        case WM_WIZ_ERROR:
        {
            wchar_t Line[1100];
            StringCchPrintfW(Line, ARRAYSIZE(Line), L"      ERROR: %s", reinterpret_cast<const wchar_t*>(lParam));
            AppendLog(W, Line);
            return 0;
        }

        case WM_WIZ_PROGRESS:
            SendMessageW(W->Progress, PBM_SETPOS, wParam, 0);
            return 0;

        case WM_WIZ_CONFIRMCERT:
            return AskCertificateTrust(W, *reinterpret_cast<const CertificateSummary*>(lParam)) ? 1 : 0;

        case WM_WIZ_DONE:
            W->Result = static_cast<int>(wParam);
            W->Running = false;
            W->Finished = true;
            ShowPage(W, WizardPage::Finish);
            return 0;

        case WM_CLOSE:
            SendMessageW(Window, WM_COMMAND, IDC_BTN_CANCEL, 0);
            return 0;

        case WM_SIZE:
            LayoutChrome(W);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(Window, Message, wParam, lParam);
    }

    bool CreateWizardWindow(Wizard* W, bool Visible)
    {
        WNDCLASSEXW Class = {};
        Class.cbSize = sizeof(Class);
        Class.lpfnWndProc = WizardProc;
        Class.hInstance = W->Instance;
        Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        Class.lpszClassName = kWindowClass;
        Class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        RegisterClassExW(&Class);

        const DWORD Style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

        RECT Rect = { 0, 0, Scale(kBaseWidth, W->Dpi), Scale(kBaseHeight, W->Dpi) };
        AdjustWindowRect(&Rect, Style, FALSE);

        const int Width = Rect.right - Rect.left;
        const int Height = Rect.bottom - Rect.top;
        const int X = (GetSystemMetrics(SM_CXSCREEN) - Width) / 2;
        const int Y = (GetSystemMetrics(SM_CYSCREEN) - Height) / 2;

        W->Main = CreateWindowExW(0, kWindowClass,
                                  W->Mode == WizardMode::Uninstall
                                      ? L"Remove DesktopSplitter"
                                      : L"DesktopSplitter Setup",
                                  Style, X, Y, Width, Height,
                                  nullptr, nullptr, W->Instance, nullptr);
        if (W->Main == nullptr)
        {
            return false;
        }

        BuildControls(W);
        LayoutChrome(W);
        ShowPage(W, WizardPage::Welcome);

        if (Visible)
        {
            ShowWindow(W->Main, SW_SHOW);
            UpdateWindow(W->Main);
        }
        return true;
    }

    void InitWizard(Wizard* W, HINSTANCE Instance, WizardMode Mode)
    {
        W->Instance = Instance;
        W->Mode = Mode;

        W->Dpi = 96;
        if (g_TestDpiOverride != 0)
        {
            W->Dpi = g_TestDpiOverride;
        }
        else
        {
            HDC Screen = GetDC(nullptr);
            if (Screen != nullptr)
            {
                W->Dpi = static_cast<UINT>(GetDeviceCaps(Screen, LOGPIXELSX));
                ReleaseDC(nullptr, Screen);
            }
        }

        W->FontNormal = MakeFont(W->Dpi, 9, false);
        W->FontHeader = MakeFont(W->Dpi, 9, true);
        W->FontTitle = MakeFont(W->Dpi, 12, true);

        GetEditionInfo(&W->Edition);

        GetDefaultInstallDir(W->Options.InstallDir, ARRAYSIZE(W->Options.InstallDir));

        // An existing install pre-selects its own location (upgrade in place).
        wchar_t Existing[MAX_PATH] = {};
        wchar_t Version[64] = {};
        if (ReadInstalledState(Existing, ARRAYSIZE(Existing), Version, ARRAYSIZE(Version)))
        {
            StringCchCopyW(W->Options.InstallDir, ARRAYSIZE(W->Options.InstallDir), Existing);
        }

        W->Options.DesktopShortcut = true;
        W->Options.StartMenuShortcut = true;
        W->Options.HidePhysicalDisplay = false;
        W->Options.HidePhysicalDisplaySupported = W->Edition.SupportsSpecializedDisplay;
    }

    void DestroyWizard(Wizard* W)
    {
        if (W->FontNormal) DeleteObject(W->FontNormal);
        if (W->FontHeader) DeleteObject(W->FontHeader);
        if (W->FontTitle)  DeleteObject(W->FontTitle);
    }
}

namespace DeskSplit
{
    _Use_decl_annotations_
    int RunWizard(HINSTANCE Instance, WizardMode Mode)
    {
        INITCOMMONCONTROLSEX Controls = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&Controls);

        Wizard W;
        g_Wizard = &W;
        InitWizard(&W, Instance, Mode);

        if (!CreateWizardWindow(&W, true))
        {
            g_Wizard = nullptr;
            return ExitUsage;
        }

        MSG Message = {};
        while (GetMessageW(&Message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(W.Main, &Message))
            {
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
            }
        }

        const int Result = W.Result;
        const bool Launch = W.LaunchApp && Mode == WizardMode::Install && Result == ExitSuccess;

        wchar_t AppPath[MAX_PATH] = {};
        StringCchPrintfW(AppPath, ARRAYSIZE(AppPath), L"%s\\DesktopSplitter.exe", W.Options.InstallDir);

        DestroyWizard(&W);
        g_Wizard = nullptr;

        if (Launch)
        {
            // Launch de-elevated would need the shell; starting it directly is
            // acceptable here and matches what most installers do.
            STARTUPINFOW StartupInfo = { sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION ProcessInfo = {};
            if (CreateProcessW(AppPath, nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                               W.Options.InstallDir, &StartupInfo, &ProcessInfo))
            {
                CloseHandle(ProcessInfo.hThread);
                CloseHandle(ProcessInfo.hProcess);
            }
        }

        return Result;
    }

    _Use_decl_annotations_
    int RunWizardSelfTest(HINSTANCE Instance)
    {
        INITCOMMONCONTROLSEX Controls = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&Controls);

        int Failures = 0;

        auto Check = [&Failures](const wchar_t* What, bool Condition)
        {
            wprintf(L"  [%s] %s\n", Condition ? L"PASS" : L"FAIL", What);
            if (!Condition) ++Failures;
        };

        // ---- real edition on this machine ---------------------------------
        EditionInfo Real = {};
        SetEditionOverrideForTest(nullptr);
        GetEditionInfo(&Real);

        wprintf(L"Machine edition: EditionID=%s ProductName=%s %s build %u\n",
                Real.EditionId, Real.ProductName, Real.DisplayVersion, Real.Build);
        wprintf(L"Specialized display supported: %s\n\n",
                Real.SupportsSpecializedDisplay ? L"yes" : L"no");

        // Asserts the always-present chrome: the three wizard buttons must be
        // visible, non-degenerate, inside the client area, and correctly
        // labelled for the page.
        auto CheckChrome = [&Check](Wizard* W, WizardPage Page, auto& CheckFn)
        {
            (void)CheckFn;

            RECT Client = {};
            GetClientRect(W->Main, &Client);

            struct Button { HWND Handle; const wchar_t* Name; };
            const Button Buttons[] =
            {
                { W->BtnBack,   L"Back" },
                { W->BtnNext,   L"Next" },
                { W->BtnCancel, L"Cancel" },
            };

            for (const Button& Btn : Buttons)
            {
                wchar_t Message[256];

                StringCchPrintfW(Message, ARRAYSIZE(Message),
                                 L"page %d: %s button exists and is WS_VISIBLE",
                                 static_cast<int>(Page), Btn.Name);
                Check(Message, Btn.Handle != nullptr && IsShown(Btn.Handle));

                if (Btn.Handle == nullptr) continue;

                RECT Window = {};
                GetWindowRect(Btn.Handle, &Window);
                POINT TopLeft = { Window.left, Window.top };
                POINT BottomRight = { Window.right, Window.bottom };
                ScreenToClient(W->Main, &TopLeft);
                ScreenToClient(W->Main, &BottomRight);

                const int ButtonWidth = BottomRight.x - TopLeft.x;
                const int ButtonHeight = BottomRight.y - TopLeft.y;

                StringCchPrintfW(Message, ARRAYSIZE(Message),
                                 L"page %d: %s button has a non-zero size (%dx%d)",
                                 static_cast<int>(Page), Btn.Name, ButtonWidth, ButtonHeight);
                Check(Message, ButtonWidth > 0 && ButtonHeight > 0);

                const bool Inside = TopLeft.x >= 0 && TopLeft.y >= 0 &&
                                    BottomRight.x <= Client.right &&
                                    BottomRight.y <= Client.bottom;
                StringCchPrintfW(Message, ARRAYSIZE(Message),
                                 L"page %d: %s button lies inside the client rect "
                                 L"(%d,%d)-(%d,%d) within %dx%d",
                                 static_cast<int>(Page), Btn.Name,
                                 TopLeft.x, TopLeft.y, BottomRight.x, BottomRight.y,
                                 Client.right, Client.bottom);
                Check(Message, Inside);
            }

            wchar_t NextLabel[64] = {};
            GetWindowTextW(W->BtnNext, NextLabel, ARRAYSIZE(NextLabel));
            const wchar_t* Expected =
                (Page == WizardPage::Finish) ? L"Finish"
                : (Page == WizardPage::Options)
                      ? (W->Mode == WizardMode::Uninstall ? L"Remove" : L"Install")
                      : L"Next >";

            wchar_t Message[256];
            StringCchPrintfW(Message, ARRAYSIZE(Message),
                             L"page %d: Next button reads '%s' (expected '%s')",
                             static_cast<int>(Page), NextLabel, Expected);
            Check(Message, wcscmp(NextLabel, Expected) == 0);

            // Header text must be present too - it is chrome, not page content.
            wchar_t Header[256] = {};
            GetWindowTextW(W->Header, Header, ARRAYSIZE(Header));
            StringCchPrintfW(Message, ARRAYSIZE(Message),
                             L"page %d: header is visible and non-empty",
                             static_cast<int>(Page));
            Check(Message, IsShown(W->Header) && Header[0] != L'\0');
        };

        struct Case
        {
            const wchar_t* Edition;
            bool ExpectEnabled;
        };
        const Case Cases[] =
        {
            { nullptr,                    Real.SupportsSpecializedDisplay },  // real registry value
            { L"ProfessionalWorkstation", true  },
            { L"Enterprise",              true  },
            { L"IoTEnterprise",           true  },
            { L"Professional",            false },
            { L"Core",                    false },
        };

        for (const Case& TestCase : Cases)
        {
            SetEditionOverrideForTest(TestCase.Edition);

            Wizard W;
            g_Wizard = &W;
            InitWizard(&W, Instance, WizardMode::Install);
            W.Headless = true;

            if (!CreateWizardWindow(&W, false))
            {
                wprintf(L"  [FAIL] could not create the hidden wizard window\n");
                g_Wizard = nullptr;
                return 1;
            }

            const wchar_t* Label = TestCase.Edition ? TestCase.Edition : L"(real registry value)";
            wprintf(L"Edition '%s':\n", Label);

            const bool Enabled = IsWindowEnabled(W.ChkHide) != FALSE;
            wchar_t Note[256] = {};
            GetWindowTextW(W.HideNote, Note, ARRAYSIZE(Note));

            wchar_t Message[256];
            StringCchPrintfW(Message, ARRAYSIZE(Message),
                             L"hide-physical-display checkbox %s (expected %s)",
                             Enabled ? L"ENABLED" : L"disabled",
                             TestCase.ExpectEnabled ? L"ENABLED" : L"disabled");
            Check(Message, Enabled == TestCase.ExpectEnabled);

            if (!TestCase.ExpectEnabled)
            {
                Check(L"disabled note mentions the required editions",
                      wcsstr(Note, L"Pro for Workstations") != nullptr);
                Check(L"checkbox is forced unchecked when unsupported",
                      IsDlgButtonChecked(W.Main, IDC_CHK_HIDE_PHYSICAL) == BST_UNCHECKED);
            }

            // ---- walk every page, assert the expected controls are up -----
            const WizardPage Pages[] = { WizardPage::Welcome, WizardPage::Options,
                                         WizardPage::Progress, WizardPage::Finish };
            for (WizardPage Page : Pages)
            {
                ShowPage(&W, Page);

                wchar_t Header[256] = {};
                GetWindowTextW(W.Header, Header, ARRAYSIZE(Header));

                const bool BodyUp = IsShown(W.Body);
                const bool OptionsUp = IsShown(W.EditDir);
                const bool ProgressUp = IsShown(W.Progress);

                // The chrome must be usable on EVERY page. The shipped build
                // rendered with no buttons at all because they were created
                // without WS_VISIBLE and never shown, and the old selftest only
                // looked at page-specific controls - so it passed regardless.
                CheckChrome(&W, Page, Check);

                wprintf(L"    page %d '%s' body=%d options=%d progress=%d\n",
                        static_cast<int>(Page), Header, BodyUp, OptionsUp, ProgressUp);

                switch (Page)
                {
                case WizardPage::Welcome:
                    Check(L"welcome shows the body text", BodyUp && !OptionsUp && !ProgressUp);
                    break;
                case WizardPage::Options:
                    Check(L"options shows the install-dir controls", OptionsUp && !ProgressUp);
                    break;
                case WizardPage::Progress:
                    Check(L"progress shows the progress bar and log",
                          ProgressUp && IsShown(W.List));
                    break;
                case WizardPage::Finish:
                    Check(L"finish shows the body text", BodyUp && !ProgressUp);
                    break;
                default:
                    break;
                }
            }

            // Install dir must be non-empty and absolute.
            wchar_t Dir[MAX_PATH] = {};
            GetWindowTextW(W.EditDir, Dir, ARRAYSIZE(Dir));
            Check(L"install directory defaults to an absolute path",
                  Dir[0] != L'\0' && Dir[1] == L':');

            Check(L"desktop shortcut is checked by default",
                  IsDlgButtonChecked(W.Main, IDC_CHK_DESKTOP_SHORTCUT) == BST_CHECKED);
            Check(L"start menu shortcut is checked by default",
                  IsDlgButtonChecked(W.Main, IDC_CHK_STARTMENU_SHORTCUT) == BST_CHECKED);

            DestroyWindow(W.Main);
            MSG Message2 = {};
            while (PeekMessageW(&Message2, nullptr, 0, 0, PM_REMOVE)) { /* drain */ }

            DestroyWizard(&W);
            g_Wizard = nullptr;
            wprintf(L"\n");
        }

        SetEditionOverrideForTest(nullptr);

        // ---- layout across display scalings --------------------------------
        // The user's report was at an unknown scaling on a 5120x1440 desktop,
        // so the button bar is checked at every common factor.
        const UINT DpiCases[] = { 96, 120, 144, 192 };   // 100%, 125%, 150%, 200%

        for (UINT Dpi : DpiCases)
        {
            g_TestDpiOverride = Dpi;

            Wizard W;
            g_Wizard = &W;
            InitWizard(&W, Instance, WizardMode::Install);
            W.Headless = true;

            if (!CreateWizardWindow(&W, false))
            {
                wprintf(L"  [FAIL] could not create the wizard at %u DPI\n", Dpi);
                g_Wizard = nullptr;
                g_TestDpiOverride = 0;
                return 1;
            }

            RECT Client = {};
            GetClientRect(W.Main, &Client);
            wprintf(L"DPI %u (%u%% scaling), client %dx%d:\n",
                    Dpi, MulDiv(Dpi, 100, 96), Client.right, Client.bottom);

            const WizardPage Pages[] = { WizardPage::Welcome, WizardPage::Options,
                                         WizardPage::Progress, WizardPage::Finish };
            for (WizardPage Page : Pages)
            {
                ShowPage(&W, Page);
                CheckChrome(&W, Page, Check);
            }

            // Uninstall mode relabels Next on the Options page.
            ShowPage(&W, WizardPage::Welcome);
            W.Mode = WizardMode::Uninstall;
            ShowPage(&W, WizardPage::Options);
            CheckChrome(&W, WizardPage::Options, Check);
            W.Mode = WizardMode::Install;

            DestroyWindow(W.Main);
            MSG Drain = {};
            while (PeekMessageW(&Drain, nullptr, 0, 0, PM_REMOVE)) { /* drain */ }

            DestroyWizard(&W);
            g_Wizard = nullptr;
            wprintf(L"\n");
        }

        g_TestDpiOverride = 0;

        wprintf(L"%s (%d failure%s)\n", Failures == 0 ? L"SELFTEST PASSED" : L"SELFTEST FAILED",
                Failures, Failures == 1 ? L"" : L"s");
        return Failures == 0 ? 0 : 1;
    }
}
