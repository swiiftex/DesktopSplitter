#include "Payload.h"
#include "resource.h"

#include <strsafe.h>

namespace
{
    struct PayloadItem
    {
        int Id;
        const wchar_t* FileName;
    };

    const PayloadItem kPayloadItems[] =
    {
        { IDR_DRIVER_INF, L"DesktopSplitterVdd.inf" },
        { IDR_DRIVER_DLL, L"DesktopSplitterVdd.dll" },
        { IDR_DRIVER_CAT, L"DesktopSplitterVdd.cat" },
    };

    bool LockPayloadResource(int Id, const void** ppData, DWORD* pSize)
    {
        HMODULE Module = GetModuleHandleW(nullptr);

        HRSRC Resource = FindResourceW(Module, MAKEINTRESOURCEW(Id), RT_RCDATA);
        if (Resource == nullptr)
        {
            return false;
        }

        const DWORD Size = SizeofResource(Module, Resource);
        if (Size == 0)
        {
            return false;
        }

        HGLOBAL Loaded = LoadResource(Module, Resource);
        if (Loaded == nullptr)
        {
            return false;
        }

        const void* Data = LockResource(Loaded);
        if (Data == nullptr)
        {
            return false;
        }

        *ppData = Data;
        *pSize = Size;
        return true;
    }

    DWORD WriteFileBytes(const wchar_t* Path, const void* Data, DWORD Size)
    {
        HANDLE File = CreateFileW(
            Path,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (File == INVALID_HANDLE_VALUE)
        {
            return GetLastError();
        }

        DWORD Written = 0;
        const BOOL Ok = WriteFile(File, Data, Size, &Written, nullptr);
        const DWORD Error = Ok ? ERROR_SUCCESS : GetLastError();

        CloseHandle(File);

        if (Ok && Written != Size)
        {
            return ERROR_WRITE_FAULT;
        }

        return Error;
    }
}

namespace DeskSplit
{
    bool HasEmbeddedPayload()
    {
        for (const PayloadItem& Item : kPayloadItems)
        {
            const void* Data = nullptr;
            DWORD Size = 0;
            if (!LockPayloadResource(Item.Id, &Data, &Size))
            {
                return false;
            }
        }

        return true;
    }

    _Use_decl_annotations_
    DWORD ExtractPayload(ExtractedPackage* pPackage)
    {
        ZeroMemory(pPackage, sizeof(*pPackage));

        wchar_t TempRoot[MAX_PATH] = {};
        if (GetTempPathW(static_cast<DWORD>(ARRAYSIZE(TempRoot)), TempRoot) == 0)
        {
            return GetLastError();
        }

        wchar_t BaseDir[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(BaseDir, ARRAYSIZE(BaseDir), L"%sDesktopSplitterSetup", TempRoot)))
        {
            return ERROR_BUFFER_OVERFLOW;
        }

        if (!CreateDirectoryW(BaseDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return GetLastError();
        }

        // Unique per-run subdirectory so concurrent or repeated runs never
        // collide and a stale file can never be picked up by mistake.
        DWORD LastError = ERROR_SUCCESS;
        bool Created = false;

        for (unsigned Attempt = 0; Attempt < 32 && !Created; ++Attempt)
        {
            const ULONGLONG Tick = GetTickCount64();
            const DWORD Pid = GetCurrentProcessId();

            if (FAILED(StringCchPrintfW(
                    pPackage->Directory,
                    ARRAYSIZE(pPackage->Directory),
                    L"%s\\%08X%04X",
                    BaseDir,
                    static_cast<unsigned>(Tick & 0xFFFFFFFFull) + Attempt * 7919u,
                    static_cast<unsigned>(Pid & 0xFFFFu))))
            {
                return ERROR_BUFFER_OVERFLOW;
            }

            if (CreateDirectoryW(pPackage->Directory, nullptr))
            {
                Created = true;
            }
            else
            {
                LastError = GetLastError();
                if (LastError != ERROR_ALREADY_EXISTS)
                {
                    return LastError;
                }
                Sleep(1);
            }
        }

        if (!Created)
        {
            return (LastError != ERROR_SUCCESS) ? LastError : ERROR_ALREADY_EXISTS;
        }

        for (const PayloadItem& Item : kPayloadItems)
        {
            const void* Data = nullptr;
            DWORD Size = 0;
            if (!LockPayloadResource(Item.Id, &Data, &Size))
            {
                return ERROR_RESOURCE_DATA_NOT_FOUND;
            }

            wchar_t Path[MAX_PATH] = {};
            if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", pPackage->Directory, Item.FileName)))
            {
                return ERROR_BUFFER_OVERFLOW;
            }

            const DWORD Error = WriteFileBytes(Path, Data, Size);
            if (Error != ERROR_SUCCESS)
            {
                return Error;
            }

            switch (Item.Id)
            {
            case IDR_DRIVER_INF:
                StringCchCopyW(pPackage->InfPath, ARRAYSIZE(pPackage->InfPath), Path);
                break;
            case IDR_DRIVER_DLL:
                StringCchCopyW(pPackage->DllPath, ARRAYSIZE(pPackage->DllPath), Path);
                break;
            case IDR_DRIVER_CAT:
                StringCchCopyW(pPackage->CatPath, ARRAYSIZE(pPackage->CatPath), Path);
                break;
            default:
                break;
            }
        }

        return ERROR_SUCCESS;
    }

    bool ResourceExists(int ResourceId)
    {
        const void* Data = nullptr;
        DWORD Size = 0;
        return LockPayloadResource(ResourceId, &Data, &Size);
    }

    DWORD ResourceSize(int ResourceId)
    {
        const void* Data = nullptr;
        DWORD Size = 0;
        if (!LockPayloadResource(ResourceId, &Data, &Size))
        {
            return 0;
        }
        return Size;
    }

    _Use_decl_annotations_
    DWORD WriteResourceToFile(int ResourceId, const wchar_t* Path)
    {
        const void* Data = nullptr;
        DWORD Size = 0;
        if (!LockPayloadResource(ResourceId, &Data, &Size))
        {
            return ERROR_RESOURCE_DATA_NOT_FOUND;
        }

        return WriteFileBytes(Path, Data, Size);
    }

    _Use_decl_annotations_
    void CleanupPayload(const ExtractedPackage* pPackage)
    {
        if (pPackage->Directory[0] == L'\0')
        {
            return;
        }

        for (const PayloadItem& Item : kPayloadItems)
        {
            wchar_t Path[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", pPackage->Directory, Item.FileName)))
            {
                DeleteFileW(Path);
            }
        }

        RemoveDirectoryW(pPackage->Directory);
    }
}
