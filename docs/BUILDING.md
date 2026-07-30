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

## Versioning

The root **`VERSION`** file is the single source of truth for the product
version (`MAJOR.MINOR.PATCH`).

| Component | Who changes it | How |
| --- | --- | --- |
| **PATCH** | `build.ps1`, automatically | Incremented on every **successful** build |
| **MINOR** | **you, by hand** | New functionality / notable change: `.\build.ps1 -SetVersion 0.6.0` |
| **MAJOR** | **you, by hand** | Large update or breaking change: `.\build.ps1 -SetVersion 1.0.0` |

Never hand-edit the number in `VERSION`: use `-SetVersion` so every generated
artefact is regenerated from it in the same step.

```powershell
.\build.ps1                       # 0.5.3 -> 0.5.4, stamped into everything
.\build.ps1 -NoVersionBump        # build without consuming a version (iterating)
.\build.ps1 -SetVersion 0.6.0     # explicit bump, e.g. raising the minor
```

The version is **only written back after the whole build succeeds**, and via a
temp file + move, so a failed or interrupted build never burns or half-writes a
version.

### How it reaches each artefact

`build.ps1` writes these before anything compiles:

| Generated file | Consumed by |
| --- | --- |
| `build\generated\Version.props` | root `Directory.Build.props` → the .NET app (`Version`, `AssemblyVersion`, `FileVersion`, `InformationalVersion`) |
| `build\generated\Version.h` | C++ projects — `DS_VERSION_MAJOR/MINOR/PATCH`, `DS_VERSION_STR`, `DS_VERSION_WSTR`, `DS_VERSION_COMMA`, `DS_VERSION_STR4`. Plain `#define`s only, so it is includable from both `.cpp` and `.rc`. |
| `build\generated\version.txt` | scripts and CI (bare number, no BOM, no newline) |

The root `Directory.Build.props` is picked up automatically by
`app\DesktopSplitter.csproj` with no edit to that project. Everything in it is
guarded on `'$(MSBuildProjectExtension)' == '.csproj'` so the C++ `.vcxproj`
builds are untouched.

In the installer the version drives the Add/Remove Programs `DisplayVersion`,
the wizard's displayed version, and the exe's own version resource.

CI builds with `-NoVersionBump`: nothing there could commit the change back, so
bumping would only cause version drift and a dirty tree. The workflow reads the
version from the build step's output (falling back to `version.txt`); a `v*`
tag still wins for release naming.

> The driver INF's `DriverVer` is deliberately **not** tied to this version — it
> keeps its date/time stamping, which is what the validated driver upgrade path
> depends on.

## Build

```powershell
# Everything, in dependency order, with versioning (recommended)
.\build.ps1

# --- or individual projects ---
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
