// Wizard.h - the native Win32 wizard UI.
//
// Classic 4-page installer: Welcome -> Options -> Progress -> Finish. No
// external frameworks and no dialog templates: pages are built from child
// controls in code, which keeps DPI scaling straightforward and lets the
// /selftest mode walk the pages programmatically without showing a window.
#pragma once

#include <windows.h>

#include "ProductFiles.h"

namespace DeskSplit
{
    enum class WizardPage
    {
        Welcome = 0,
        Options,
        Progress,
        Finish,
        Count
    };

    enum class WizardMode
    {
        Install,
        Uninstall
    };

    // Runs the wizard modally. Returns an ExitCode.
    int RunWizard(_In_ HINSTANCE Instance, _In_ WizardMode Mode);

    // ---- /selftest support ------------------------------------------------
    // Creates the whole wizard WITHOUT showing it, walks every page, and writes
    // a description of each page's controls (text, enabled, checked) to stdout.
    // Returns 0 when every assertion passed.
    int RunWizardSelfTest(_In_ HINSTANCE Instance);
}
