// ProductFiles.h - everything that is NOT the driver: the installed program
// files, shortcuts, the Add/Remove Programs entry and the feature flags.
#pragma once

#include <windows.h>

namespace DeskSplit
{
    // HKLM\SOFTWARE\DesktopSplitter - feature flags consumed by the app and
    // the compositor.
    extern const wchar_t kProductRegKey[];
    // HKLM\...\Uninstall\DesktopSplitter - Add/Remove Programs.
    extern const wchar_t kUninstallRegKey[];

    extern const wchar_t kProductName[];
    extern const wchar_t kPublisher[];
    extern const wchar_t kProductVersion[];

    struct InstallOptions
    {
        wchar_t InstallDir[MAX_PATH];
        bool    DesktopShortcut;
        bool    StartMenuShortcut;
        bool    HidePhysicalDisplay;
        bool    HidePhysicalDisplaySupported;
    };

    // %ProgramFiles%\DesktopSplitter
    void GetDefaultInstallDir(_Out_writes_z_(Chars) wchar_t* Buffer, _In_ size_t Chars);

    // Creates the directory tree (like SHCreateDirectory, no shell dependency).
    DWORD EnsureDirectory(_In_z_ const wchar_t* Path);

    // Writes DesktopSplitter.exe, dscomp.exe, devcreate.exe and the compositor
    // README into InstallDir, and copies this setup executable in as
    // DesktopSplitterSetup.exe so uninstall works without the original file.
    DWORD InstallProductFiles(_In_z_ const wchar_t* InstallDir, _Out_ DWORD* pBytesWritten);

    // Shortcuts. Desktop = common desktop, Start menu = common programs.
    DWORD CreateShortcuts(_In_ const InstallOptions* pOptions);
    void  RemoveShortcuts();

    // Add/Remove Programs entry.
    DWORD WriteUninstallEntry(_In_ const InstallOptions* pOptions, _In_ DWORD EstimatedSizeKb);
    void  RemoveUninstallEntry();

    // HKLM\SOFTWARE\DesktopSplitter feature flags.
    DWORD WriteProductFlags(_In_ const InstallOptions* pOptions);
    void  RemoveProductFlags();

    // Reads the existing installation, if any. Returns false when not installed.
    bool ReadInstalledState(
        _Out_writes_z_(DirChars) wchar_t* InstallDir, _In_ size_t DirChars,
        _Out_writes_z_(VerChars) wchar_t* Version,    _In_ size_t VerChars);

    // Deletes the installed files and the install directory. The running setup
    // copy inside InstallDir is scheduled for delete-on-reboot if it is us.
    DWORD RemoveProductFiles(_In_z_ const wchar_t* InstallDir);

    // Stops DesktopSplitter.exe / dscomp.exe before an upgrade or uninstall.
    void StopRunningProcesses(_Out_ int* pStoppedCount);

    // Signals the compositor's named stop event, then terminates leftovers.
    void SignalCompositorStop();
}
