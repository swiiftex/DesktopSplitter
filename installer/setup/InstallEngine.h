// InstallEngine.h - the install / uninstall sequence, independent of UI.
//
// Both the console mode and the GUI wizard drive the same engine and differ
// only in the sink they pass, so the two can never drift apart.
#pragma once

#include <windows.h>

#include "ProductFiles.h"
#include "Trust.h"

namespace DeskSplit
{
    enum ExitCode : int
    {
        ExitSuccess = 0,
        ExitUsage = 1,
        ExitNotElevated = 2,
        ExitPayloadMissing = 3,
        ExitPayloadExtractFailed = 4,
        ExitCatalogUnusable = 5,
        ExitTrustDeclined = 6,
        ExitCertInstallFailed = 7,
        ExitDriverInstallFailed = 8,
        ExitDevNodeFailed = 9,
        ExitVerificationFailed = 10,
        ExitUninstallFailed = 11,
        ExitFileInstallFailed = 12,
        ExitCancelled = 13,
    };

    // UI callbacks. Every implementation must be safe to call from the thread
    // the engine runs on (the GUI sink marshals to the UI thread).
    struct IStepSink
    {
        virtual ~IStepSink() = default;

        // A new top-level step began.
        virtual void OnStep(_In_z_ const wchar_t* Text) = 0;
        // A detail line under the current step.
        virtual void OnDetail(_In_z_ const wchar_t* Text) = 0;
        // Something went wrong (the engine may still continue).
        virtual void OnError(_In_z_ const wchar_t* Text) = 0;
        // 0..100 overall progress.
        virtual void OnProgress(_In_ int Percent) = 0;

        // The package is signed by a publisher this PC does not trust yet.
        // Return true to add the certificate to the machine trust stores.
        virtual bool ConfirmCertificateTrust(_In_ const CertificateSummary& Summary) = 0;

        // Return true to abort at the next checkpoint.
        virtual bool IsCancelled() { return false; }
    };

    struct UninstallOptions
    {
        bool RemoveCertificate;
        bool KeepTemp;
    };

    // Full product install: driver package + device node + program files +
    // shortcuts + registry. Returns an ExitCode.
    int RunInstallSequence(_In_ const InstallOptions* pOptions, _In_ IStepSink* pSink);

    // Full product removal.
    int RunUninstallSequence(_In_ const UninstallOptions* pOptions, _In_ IStepSink* pSink);

    // Best-effort: tell the driver to drop all virtual monitors, so uninstall
    // never leaves the desktop spread across displays that are about to vanish.
    bool RevertSplit(_In_ IStepSink* pSink);

    // Formats a Win32 error the way the sinks present it.
    void FormatWin32Error(_In_ DWORD Error,
                          _Out_writes_z_(Chars) wchar_t* Buffer,
                          _In_ size_t Chars);
}
