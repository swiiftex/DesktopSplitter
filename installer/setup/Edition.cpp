#include "Edition.h"

#include <strsafe.h>
#include <stdlib.h>

namespace
{
    wchar_t g_EditionOverride[64] = {};
    bool g_HaveOverride = false;

    bool ReadStringValue(HKEY Key, const wchar_t* Name, wchar_t* Buffer, DWORD BufferChars)
    {
        Buffer[0] = L'\0';

        DWORD Type = 0;
        DWORD Bytes = BufferChars * sizeof(wchar_t);
        if (RegQueryValueExW(Key, Name, nullptr, &Type, reinterpret_cast<BYTE*>(Buffer), &Bytes) != ERROR_SUCCESS)
        {
            return false;
        }
        if (Type != REG_SZ && Type != REG_EXPAND_SZ)
        {
            Buffer[0] = L'\0';
            return false;
        }

        // RegQueryValueEx does not guarantee NUL termination.
        const DWORD Chars = Bytes / sizeof(wchar_t);
        Buffer[(Chars < BufferChars) ? Chars : (BufferChars - 1)] = L'\0';
        return true;
    }
}

namespace DeskSplit
{
    bool EditionSupportsSpecializedDisplay(const wchar_t* EditionId)
    {
        if (EditionId == nullptr || EditionId[0] == L'\0')
        {
            return false;
        }

        static const wchar_t* const kSupported[] =
        {
            L"ProfessionalWorkstation",   // Windows Pro for Workstations
            L"Enterprise",
            L"IoTEnterprise",
        };

        for (const wchar_t* Candidate : kSupported)
        {
            if (_wcsicmp(EditionId, Candidate) == 0)
            {
                return true;
            }
        }

        return false;
    }

    void SetEditionOverrideForTest(const wchar_t* EditionId)
    {
        if (EditionId == nullptr)
        {
            g_HaveOverride = false;
            g_EditionOverride[0] = L'\0';
            return;
        }

        StringCchCopyW(g_EditionOverride, ARRAYSIZE(g_EditionOverride), EditionId);
        g_HaveOverride = true;
    }

    _Use_decl_annotations_
    void GetEditionInfo(EditionInfo* pInfo)
    {
        ZeroMemory(pInfo, sizeof(*pInfo));

        HKEY Key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
        {
            ReadStringValue(Key, L"EditionID", pInfo->EditionId, ARRAYSIZE(pInfo->EditionId));
            ReadStringValue(Key, L"ProductName", pInfo->ProductName, ARRAYSIZE(pInfo->ProductName));
            ReadStringValue(Key, L"DisplayVersion", pInfo->DisplayVersion, ARRAYSIZE(pInfo->DisplayVersion));

            wchar_t BuildText[32] = {};
            if (ReadStringValue(Key, L"CurrentBuild", BuildText, ARRAYSIZE(BuildText)))
            {
                pInfo->Build = static_cast<DWORD>(_wtoi(BuildText));
            }

            RegCloseKey(Key);
        }

        if (g_HaveOverride)
        {
            StringCchCopyW(pInfo->EditionId, ARRAYSIZE(pInfo->EditionId), g_EditionOverride);
        }

        pInfo->SupportsSpecializedDisplay = EditionSupportsSpecializedDisplay(pInfo->EditionId);
    }
}
