# DesktopSplitter build script.
# Builds everything that the installed toolchain supports:
#   - compositor (dscomp.exe)        : VS C++ (v143+)
#   - installer/devcreate            : VS C++ (v143+)
#   - app (DesktopSplitter.exe)      : .NET SDK 8+
#   - driver (DesktopSplitterVdd)    : only if the WDK is installed
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio 2022+" }
# Use 64-bit MSBuild: the WDK's InfVerif step ships no x86 binary and fails under 32-bit MSBuild.
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) { $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1 }
if (-not $msbuild) { throw "MSBuild not found" }

Write-Host "== compositor (dscomp) ==" -ForegroundColor Cyan
& $msbuild "$root\compositor\dscomp.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw "compositor build failed" }

Write-Host "== installer/devcreate ==" -ForegroundColor Cyan
& $msbuild "$root\installer\devcreate\devcreate.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw "devcreate build failed" }

# Must build before the installer: its exe is embedded by the setup stage step.
Write-Host "== installer/edid-override ==" -ForegroundColor Cyan
& $msbuild "$root\installer\edid-override\edidoverride.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw "edidoverride build failed" }

Write-Host "== app (DesktopSplitter) ==" -ForegroundColor Cyan
dotnet build "$root\app\DesktopSplitter.csproj" -c $Configuration --nologo
if ($LASTEXITCODE -ne 0) { throw "app build failed" }

# Driver: needs the WDK (IddCx headers + WindowsUserModeDriver toolset)
$wdkFound = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\iddcx\*\IddCx.h" -ErrorAction SilentlyContinue
if ($wdkFound) {
    Write-Host "== driver (DesktopSplitterVdd) ==" -ForegroundColor Cyan
    # Inf2CatUseLocalTime: stampinf writes the local date into DriverVer but inf2cat
    # validates against UTC; near midnight (UTC behind local) that reads as "postdated".
    & $msbuild "$root\driver\DesktopSplitterVdd.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /p:Inf2CatUseLocalTime=true /v:m /nologo
    if ($LASTEXITCODE -ne 0) { throw "driver build failed" }
} else {
    Write-Warning "WDK not installed - skipping driver build. See docs\BUILDING.md (winget install Microsoft.WindowsWDK.10.0.26100)"
}

# End-user installer. It embeds the SIGNED driver package plus the product
# files (the app published self-contained, the compositor and devcreate), so it
# can only be built once the driver package exists. It always embeds the Release
# artefacts - that is what ships - independently of $Configuration.
#
# The app publish happens inside the project's staging step
# (installer\setup\stage-payload.ps1), which fails the build with an actionable
# message if anything it needs is missing.
$setupBuilt = $false
$pkgDir = Join-Path $root 'driver\x64\Release\DesktopSplitterVdd'
$pkgFiles = @('DesktopSplitterVdd.inf', 'DesktopSplitterVdd.dll', 'desktopsplittervdd.cat')
$pkgMissing = $pkgFiles | Where-Object { -not (Test-Path (Join-Path $pkgDir $_)) }

if ($pkgMissing) {
    Write-Warning ("No complete driver package at driver\x64\Release\DesktopSplitterVdd " +
                   "(missing: $($pkgMissing -join ', ')) - skipping DesktopSplitterSetup.")
    Write-Warning "  Build the driver Release|x64 first; sign it with installer\sign-dev.ps1 if needed."
} else {
    Write-Host "== installer/setup (DesktopSplitterSetup) ==" -ForegroundColor Cyan
    Write-Host "   (publishes the app self-contained and embeds ~140 MB of payload - this is slow)"
    & $msbuild "$root\installer\setup\DesktopSplitterSetup.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /v:m /nologo
    if ($LASTEXITCODE -ne 0) { throw "setup build failed" }
    $setupBuilt = $true
}

Write-Host "`nBuild complete ($Configuration)." -ForegroundColor Green
Write-Host "  compositor : compositor\build\x64\$Configuration\dscomp.exe"
Write-Host "  devcreate  : installer\devcreate\bin\x64\$Configuration\devcreate.exe"
Write-Host "  edidoverr. : installer\edid-override\build\x64\$Configuration\edidoverride.exe"

# The app's target framework moves (net8.0-windows -> net8.0-windows10.0.19041.0
# for WinRT), so resolve the real output rather than hardcoding a TFM.
$appExe = Get-ChildItem -Path (Join-Path $root "app\bin\$Configuration") -Filter 'DesktopSplitter.exe' `
    -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($appExe) {
    Write-Host ("  app        : " + $appExe.FullName.Substring($root.Length + 1))
} else {
    Write-Host "  app        : app\bin\$Configuration\<tfm>\DesktopSplitter.exe"
}
if ($setupBuilt) {
    Write-Host "  setup      : installer\setup\bin\x64\$Configuration\DesktopSplitterSetup.exe" -ForegroundColor Green
    Write-Host "               ^ this single file is what you give an end user."
}
