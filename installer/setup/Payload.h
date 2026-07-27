// Payload.h - extraction of the embedded driver package to a temp directory.
#pragma once

#include <windows.h>

namespace DeskSplit
{
    struct ExtractedPackage
    {
        wchar_t Directory[MAX_PATH];
        wchar_t InfPath[MAX_PATH];
        wchar_t DllPath[MAX_PATH];
        wchar_t CatPath[MAX_PATH];
    };

    // Creates %TEMP%\DesktopSplitterSetup\<random>\ and writes the three
    // embedded resources into it. Returns a Win32 error code.
    DWORD ExtractPayload(_Out_ ExtractedPackage* pPackage);

    // Best-effort removal of the directory created by ExtractPayload.
    void CleanupPayload(_In_ const ExtractedPackage* pPackage);

    // True when all three driver payload resources are present in this executable.
    bool HasEmbeddedPayload();

    // ---- generic resource helpers (used for the product files too) --------

    // True when the RCDATA resource exists and is non-empty.
    bool ResourceExists(_In_ int ResourceId);

    // Size in bytes of an embedded RCDATA resource (0 when absent).
    DWORD ResourceSize(_In_ int ResourceId);

    // Writes an embedded RCDATA resource to Path, creating/overwriting it.
    // Returns a Win32 error code.
    DWORD WriteResourceToFile(_In_ int ResourceId, _In_z_ const wchar_t* Path);
}
