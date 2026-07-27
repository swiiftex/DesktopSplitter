#include "ProductFiles.h"
#include "Payload.h"
#include "resource.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <strsafe.h>

#include "DeskSplitProtocol.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace DeskSplit
{
    const wchar_t kProductRegKey[] = L"SOFTWARE\\DesktopSplitter";
    const wchar_t kUninstallRegKey[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DesktopSplitter";
    const wchar_t kProductName[] = L"DesktopSplitter";
    const wchar_t kPublisher[] = L"DesktopSplitter";
    const wchar_t kProductVersion[] = L"1.0.0";
}

namespace
{
    struct ProductFile
    {
        int Resource;
        const wchar_t* Name;
    };

    const ProductFile kProductFiles[] =
    {
        { IDR_APP_EXE,           L"DesktopSplitter.exe" },
        { IDR_DSCOMP_EXE,        L"dscomp.exe" },
        { IDR_DEVCREATE_EXE,     L"devcreate.exe" },
        // Must sit in the install root: the app probes for it next to its own
        // exe first. Also removed on uninstall (this table drives both).
        { IDR_EDIDOVERRIDE_EXE,  L"edidoverride.exe" },
        { IDR_COMPOSITOR_README, L"compositor-README.md" },
    };

    const wchar_t kSetupCopyName[] = L"DesktopSplitterSetup.exe";
    const wchar_t kStartMenuFolder[] = L"DesktopSplitter";

    DWORD WriteRegString(HKEY Root, const wchar_t* SubKey, const wchar_t* Name, const wchar_t* Value)
    {
        HKEY Key = nullptr;
        DWORD Disposition = 0;
        LONG Result = RegCreateKeyExW(Root, SubKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &Key, &Disposition);
        if (Result != ERROR_SUCCESS)
        {
            return static_cast<DWORD>(Result);
        }

        const DWORD Bytes = static_cast<DWORD>((wcslen(Value) + 1) * sizeof(wchar_t));
        Result = RegSetValueExW(Key, Name, 0, REG_SZ, reinterpret_cast<const BYTE*>(Value), Bytes);
        RegCloseKey(Key);
        return static_cast<DWORD>(Result);
    }

    DWORD WriteRegDword(HKEY Root, const wchar_t* SubKey, const wchar_t* Name, DWORD Value)
    {
        HKEY Key = nullptr;
        DWORD Disposition = 0;
        LONG Result = RegCreateKeyExW(Root, SubKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &Key, &Disposition);
        if (Result != ERROR_SUCCESS)
        {
            return static_cast<DWORD>(Result);
        }

        Result = RegSetValueExW(Key, Name, 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&Value), sizeof(Value));
        RegCloseKey(Key);
        return static_cast<DWORD>(Result);
    }

    // Creates a .lnk. COM is initialised by the caller.
    HRESULT WriteShortcut(const wchar_t* LinkPath, const wchar_t* Target,
                          const wchar_t* WorkingDir, const wchar_t* Description)
    {
        IShellLinkW* Link = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&Link));
        if (FAILED(hr))
        {
            return hr;
        }

        Link->SetPath(Target);
        Link->SetWorkingDirectory(WorkingDir);
        Link->SetDescription(Description);
        Link->SetIconLocation(Target, 0);

        IPersistFile* File = nullptr;
        hr = Link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&File));
        if (SUCCEEDED(hr))
        {
            hr = File->Save(LinkPath, TRUE);
            File->Release();
        }

        Link->Release();
        return hr;
    }

    bool GetShortcutPath(int Csidl, const wchar_t* SubDir, const wchar_t* FileName,
                         wchar_t* Buffer, size_t Chars)
    {
        wchar_t Folder[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, Csidl, nullptr, SHGFP_TYPE_CURRENT, Folder)))
        {
            return false;
        }

        if (SubDir != nullptr && SubDir[0] != L'\0')
        {
            wchar_t WithSub[MAX_PATH] = {};
            if (FAILED(StringCchPrintfW(WithSub, ARRAYSIZE(WithSub), L"%s\\%s", Folder, SubDir)))
            {
                return false;
            }
            CreateDirectoryW(WithSub, nullptr);
            return SUCCEEDED(StringCchPrintfW(Buffer, Chars, L"%s\\%s", WithSub, FileName));
        }

        return SUCCEEDED(StringCchPrintfW(Buffer, Chars, L"%s\\%s", Folder, FileName));
    }

    void TerminateByName(const wchar_t* ImageName, int* pCount)
    {
        HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (Snapshot == INVALID_HANDLE_VALUE)
        {
            return;
        }

        PROCESSENTRY32W Entry = {};
        Entry.dwSize = sizeof(Entry);

        if (Process32FirstW(Snapshot, &Entry))
        {
            do
            {
                if (_wcsicmp(Entry.szExeFile, ImageName) != 0)
                {
                    continue;
                }
                if (Entry.th32ProcessID == GetCurrentProcessId())
                {
                    continue;
                }

                HANDLE Process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Entry.th32ProcessID);
                if (Process != nullptr)
                {
                    if (TerminateProcess(Process, 0))
                    {
                        WaitForSingleObject(Process, 3000);
                        ++(*pCount);
                    }
                    CloseHandle(Process);
                }
            } while (Process32NextW(Snapshot, &Entry));
        }

        CloseHandle(Snapshot);
    }
}

namespace DeskSplit
{
    _Use_decl_annotations_
    void GetDefaultInstallDir(wchar_t* Buffer, size_t Chars)
    {
        wchar_t ProgramFiles[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, ProgramFiles)))
        {
            StringCchCopyW(ProgramFiles, ARRAYSIZE(ProgramFiles), L"C:\\Program Files");
        }

        StringCchPrintfW(Buffer, Chars, L"%s\\%s", ProgramFiles, kProductName);
    }

    _Use_decl_annotations_
    DWORD EnsureDirectory(const wchar_t* Path)
    {
        if (CreateDirectoryW(Path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return ERROR_SUCCESS;
        }

        if (GetLastError() != ERROR_PATH_NOT_FOUND)
        {
            return GetLastError();
        }

        // Create the parent first.
        wchar_t Parent[MAX_PATH] = {};
        StringCchCopyW(Parent, ARRAYSIZE(Parent), Path);

        wchar_t* Slash = wcsrchr(Parent, L'\\');
        if (Slash == nullptr || Slash == Parent)
        {
            return ERROR_PATH_NOT_FOUND;
        }
        *Slash = L'\0';

        const DWORD Error = EnsureDirectory(Parent);
        if (Error != ERROR_SUCCESS)
        {
            return Error;
        }

        if (CreateDirectoryW(Path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return ERROR_SUCCESS;
        }
        return GetLastError();
    }

    _Use_decl_annotations_
    DWORD InstallProductFiles(const wchar_t* InstallDir, DWORD* pBytesWritten)
    {
        *pBytesWritten = 0;

        DWORD Error = EnsureDirectory(InstallDir);
        if (Error != ERROR_SUCCESS)
        {
            return Error;
        }

        for (const ProductFile& File : kProductFiles)
        {
            wchar_t Path[MAX_PATH] = {};
            if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", InstallDir, File.Name)))
            {
                return ERROR_BUFFER_OVERFLOW;
            }

            Error = WriteResourceToFile(File.Resource, Path);
            if (Error != ERROR_SUCCESS)
            {
                return Error;
            }

            *pBytesWritten += ResourceSize(File.Resource);
        }

        // Copy ourselves in so Add/Remove Programs works even if the original
        // download is deleted.
        wchar_t SelfPath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, SelfPath, ARRAYSIZE(SelfPath)) == 0)
        {
            return GetLastError();
        }

        wchar_t SetupTarget[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(SetupTarget, ARRAYSIZE(SetupTarget), L"%s\\%s", InstallDir, kSetupCopyName)))
        {
            return ERROR_BUFFER_OVERFLOW;
        }

        if (_wcsicmp(SelfPath, SetupTarget) != 0)
        {
            if (!CopyFileW(SelfPath, SetupTarget, FALSE))
            {
                return GetLastError();
            }

            LARGE_INTEGER Size = {};
            HANDLE File = CreateFileW(SetupTarget, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File != INVALID_HANDLE_VALUE)
            {
                if (GetFileSizeEx(File, &Size))
                {
                    *pBytesWritten += static_cast<DWORD>(Size.QuadPart / 1024) * 1024;
                }
                CloseHandle(File);
            }
        }

        return ERROR_SUCCESS;
    }

    _Use_decl_annotations_
    DWORD CreateShortcuts(const InstallOptions* pOptions)
    {
        wchar_t AppPath[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(AppPath, ARRAYSIZE(AppPath), L"%s\\DesktopSplitter.exe", pOptions->InstallDir)))
        {
            return ERROR_BUFFER_OVERFLOW;
        }

        const HRESULT ComInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        DWORD Result = ERROR_SUCCESS;

        if (pOptions->DesktopShortcut)
        {
            wchar_t Link[MAX_PATH] = {};
            if (GetShortcutPath(CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, L"DesktopSplitter.lnk", Link, ARRAYSIZE(Link)))
            {
                const HRESULT hr = WriteShortcut(Link, AppPath, pOptions->InstallDir, L"Split one monitor into several");
                if (FAILED(hr))
                {
                    Result = static_cast<DWORD>(hr);
                }
            }
        }

        if (pOptions->StartMenuShortcut)
        {
            wchar_t Link[MAX_PATH] = {};
            if (GetShortcutPath(CSIDL_COMMON_PROGRAMS, kStartMenuFolder, L"DesktopSplitter.lnk", Link, ARRAYSIZE(Link)))
            {
                const HRESULT hr = WriteShortcut(Link, AppPath, pOptions->InstallDir, L"Split one monitor into several");
                if (FAILED(hr))
                {
                    Result = static_cast<DWORD>(hr);
                }
            }

            wchar_t Uninstall[MAX_PATH] = {};
            wchar_t SetupPath[MAX_PATH] = {};
            StringCchPrintfW(SetupPath, ARRAYSIZE(SetupPath), L"%s\\%s", pOptions->InstallDir, kSetupCopyName);
            if (GetShortcutPath(CSIDL_COMMON_PROGRAMS, kStartMenuFolder, L"Uninstall DesktopSplitter.lnk",
                                Uninstall, ARRAYSIZE(Uninstall)))
            {
                WriteShortcut(Uninstall, SetupPath, pOptions->InstallDir, L"Remove DesktopSplitter");
            }
        }

        if (SUCCEEDED(ComInit))
        {
            CoUninitialize();
        }

        return Result;
    }

    void RemoveShortcuts()
    {
        wchar_t Link[MAX_PATH] = {};

        if (GetShortcutPath(CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, L"DesktopSplitter.lnk", Link, ARRAYSIZE(Link)))
        {
            DeleteFileW(Link);
        }
        if (GetShortcutPath(CSIDL_COMMON_PROGRAMS, kStartMenuFolder, L"DesktopSplitter.lnk", Link, ARRAYSIZE(Link)))
        {
            DeleteFileW(Link);
        }
        if (GetShortcutPath(CSIDL_COMMON_PROGRAMS, kStartMenuFolder, L"Uninstall DesktopSplitter.lnk", Link, ARRAYSIZE(Link)))
        {
            DeleteFileW(Link);
        }

        wchar_t Folder[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, Folder)))
        {
            wchar_t Sub[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"%s\\%s", Folder, kStartMenuFolder)))
            {
                RemoveDirectoryW(Sub);
            }
        }
    }

    _Use_decl_annotations_
    DWORD WriteUninstallEntry(const InstallOptions* pOptions, DWORD EstimatedSizeKb)
    {
        wchar_t SetupPath[MAX_PATH] = {};
        StringCchPrintfW(SetupPath, ARRAYSIZE(SetupPath), L"%s\\%s", pOptions->InstallDir, kSetupCopyName);

        wchar_t UninstallCommand[MAX_PATH + 32] = {};
        StringCchPrintfW(UninstallCommand, ARRAYSIZE(UninstallCommand), L"\"%s\" /uninstall", SetupPath);

        wchar_t QuietCommand[MAX_PATH + 32] = {};
        StringCchPrintfW(QuietCommand, ARRAYSIZE(QuietCommand), L"\"%s\" /uninstall /quiet", SetupPath);

        wchar_t AppPath[MAX_PATH] = {};
        StringCchPrintfW(AppPath, ARRAYSIZE(AppPath), L"%s\\DesktopSplitter.exe", pOptions->InstallDir);

        DWORD Error = WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"DisplayName", kProductName);
        if (Error != ERROR_SUCCESS) return Error;

        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"DisplayVersion", kProductVersion);
        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"Publisher", kPublisher);
        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"InstallLocation", pOptions->InstallDir);
        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"UninstallString", UninstallCommand);
        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"QuietUninstallString", QuietCommand);
        WriteRegString(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"DisplayIcon", AppPath);
        WriteRegDword(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"EstimatedSize", EstimatedSizeKb);
        WriteRegDword(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"NoModify", 1);
        WriteRegDword(HKEY_LOCAL_MACHINE, kUninstallRegKey, L"NoRepair", 1);

        return ERROR_SUCCESS;
    }

    void RemoveUninstallEntry()
    {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallRegKey, KEY_WOW64_64KEY, 0);
    }

    _Use_decl_annotations_
    DWORD WriteProductFlags(const InstallOptions* pOptions)
    {
        DWORD Error = WriteRegString(HKEY_LOCAL_MACHINE, kProductRegKey, L"InstallLocation", pOptions->InstallDir);
        if (Error != ERROR_SUCCESS) return Error;

        WriteRegString(HKEY_LOCAL_MACHINE, kProductRegKey, L"Version", kProductVersion);

        // Consumed by the app / compositor; the installer only records intent.
        WriteRegDword(HKEY_LOCAL_MACHINE, kProductRegKey, L"HidePhysicalDisplaySupported",
                      pOptions->HidePhysicalDisplaySupported ? 1u : 0u);
        WriteRegDword(HKEY_LOCAL_MACHINE, kProductRegKey, L"HidePhysicalDisplay",
                      (pOptions->HidePhysicalDisplaySupported && pOptions->HidePhysicalDisplay) ? 1u : 0u);

        return ERROR_SUCCESS;
    }

    void RemoveProductFlags()
    {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kProductRegKey, KEY_WOW64_64KEY, 0);
    }

    _Use_decl_annotations_
    bool ReadInstalledState(wchar_t* InstallDir, size_t DirChars, wchar_t* Version, size_t VerChars)
    {
        if (DirChars) InstallDir[0] = L'\0';
        if (VerChars) Version[0] = L'\0';

        HKEY Key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallRegKey, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD Type = 0;
        DWORD Bytes = static_cast<DWORD>(DirChars * sizeof(wchar_t));
        RegQueryValueExW(Key, L"InstallLocation", nullptr, &Type, reinterpret_cast<BYTE*>(InstallDir), &Bytes);

        Type = 0;
        Bytes = static_cast<DWORD>(VerChars * sizeof(wchar_t));
        RegQueryValueExW(Key, L"DisplayVersion", nullptr, &Type, reinterpret_cast<BYTE*>(Version), &Bytes);

        RegCloseKey(Key);
        return InstallDir[0] != L'\0';
    }

    _Use_decl_annotations_
    DWORD RemoveProductFiles(const wchar_t* InstallDir)
    {
        if (InstallDir == nullptr || InstallDir[0] == L'\0')
        {
            return ERROR_SUCCESS;
        }

        wchar_t SelfPath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, SelfPath, ARRAYSIZE(SelfPath));

        for (const ProductFile& File : kProductFiles)
        {
            wchar_t Path[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", InstallDir, File.Name)))
            {
                DeleteFileW(Path);
            }
        }

        wchar_t SetupPath[MAX_PATH] = {};
        if (SUCCEEDED(StringCchPrintfW(SetupPath, ARRAYSIZE(SetupPath), L"%s\\%s", InstallDir, kSetupCopyName)))
        {
            if (_wcsicmp(SelfPath, SetupPath) == 0)
            {
                // We are running from the copy being removed: it cannot be
                // deleted now, so let the OS drop it on the next boot.
                MoveFileExW(SetupPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
            else
            {
                DeleteFileW(SetupPath);
            }
        }

        // Only succeeds when empty, which is what we want: never delete files
        // the user put there.
        RemoveDirectoryW(InstallDir);
        return ERROR_SUCCESS;
    }

    void SignalCompositorStop()
    {
        HANDLE Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, DESKSPLIT_COMPOSITOR_STOP_EVENT);
        if (Event != nullptr)
        {
            SetEvent(Event);
            CloseHandle(Event);
            Sleep(700);   // give it a moment to exit cleanly
        }
    }

    _Use_decl_annotations_
    void StopRunningProcesses(int* pStoppedCount)
    {
        *pStoppedCount = 0;

        SignalCompositorStop();

        TerminateByName(L"dscomp.exe", pStoppedCount);
        TerminateByName(L"DesktopSplitter.exe", pStoppedCount);
    }
}
