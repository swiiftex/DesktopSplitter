# Building DesktopSplitter

## Prerequisites

| Component | Needs | Status on a stock dev box |
|-----------|-------|---------------------------|
| `compositor/` | VS2022 (C++ v143), Windows SDK | builds out of the box |
| `installer/devcreate/` | VS2022 (C++ v143) | builds out of the box |
| `app/` | .NET 8 SDK | `winget install Microsoft.DotNet.SDK.8` |
| `driver/` | **WDK 10.0.26100** + VS2022 WDK extension | must be installed manually |

### Installing the WDK (required for the driver only)

The WDK version must match an installed Windows SDK (this repo assumes
10.0.26100):

```powershell
winget install Microsoft.WindowsWDK.10.0.26100
```

Then install the **WDK Visual Studio extension** (offered at the end of WDK
setup, or run the VSIX from
`C:\Program Files (x86)\Windows Kits\10\Vsix\VS2022\10.0.26100.x\WDK.vsix`).

## Build

```powershell
# Compositor + devcreate (plain VS2022)
msbuild compositor\dscomp.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild installer\devcreate\devcreate.vcxproj /p:Configuration=Release /p:Platform=x64

# Control app
dotnet build app\DesktopSplitter.csproj -c Release

# Driver (requires WDK)
msbuild driver\DesktopSplitterVdd.vcxproj /p:Configuration=Release /p:Platform=x64
```

Use a "Developer PowerShell for VS 2022" prompt so `msbuild` is on PATH.

## Install (dev machine)

No test-signing mode and no Secure Boot changes are needed:
DesktopSplitterVdd is a UMDF (user-mode) driver, so Windows' kernel-mode
signing policy does not apply. The package only needs a signature the machine
trusts, which `sign-dev.ps1` arranges locally.

1. Sign the driver with a dev cert and trust it machine-wide (elevated):
   ```powershell
   installer\sign-dev.ps1
   ```
2. Install driver + create the virtual device (elevated):
   ```powershell
   installer\install-driver.ps1
   ```
3. Run `app\bin\Release\net8.0-windows\DesktopSplitter.exe`, pick a layout,
   Apply.

## Uninstall

```powershell
installer\uninstall-driver.ps1
```

## Production signing (shipping to other machines)

The dev-cert route works only on machines where the certificate was installed.
To distribute properly — silent install on any Windows 10/11 machine, Secure
Boot on, nothing added to certificate stores — use **Microsoft attestation
signing** via the Hardware Dev Center:

1. Register for the [Windows Hardware Developer Program](https://learn.microsoft.com/windows-hardware/drivers/dashboard/register-for-the-hardware-program)
   in Partner Center (identity verification requires an EV code-signing
   certificate or Microsoft's current accepted equivalent — check the page).
2. Package `DesktopSplitterVdd.inf/.dll/.cat` into a `.cab`, sign the cab with
   the EV certificate.
3. Submit on the hardware dashboard, select **attestation signing** and the
   target Windows versions (Windows 10 1809+ / Windows 11 x64).
4. Download the Microsoft-signed package and ship it; `pnputil /add-driver`
   then works everywhere with no cert or boot-config changes.

Attestation signing requires the driver to pass the "Universal" signability
checks — this build already does (verified in the build output). Full WHQL
(HLK testing) is only needed for Windows Update distribution.
