<#
.SYNOPSIS
    Installs the DesktopSplitterVdd indirect display driver and creates its
    root-enumerated device node.

.DESCRIPTION
    1. Verifies the shell is elevated.
    2. Warns if the machine is not in test-signing mode (an unsigned or
       self-signed driver will not load otherwise).
    3. Stages and installs the driver package with pnputil.
    4. Builds devcreate.exe if it is not present, then runs `devcreate create`
       to register the Root\DesktopSplitterVdd device node.

.PARAMETER InfPath
    Path to DesktopSplitterVdd.inf. Defaults to searching ..\driver for the
    most recently built package.

.PARAMETER SkipTestSigningCheck
    Continue without prompting even if test signing is disabled.

.EXAMPLE
    .\install-driver.ps1
    .\install-driver.ps1 -InfPath ..\driver\x64\Release\DesktopSplitterVdd\DesktopSplitterVdd.inf
#>
[CmdletBinding()]
param(
    [string] $InfPath,
    [switch] $SkipTestSigningCheck
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script must be run from an elevated (Run as administrator) PowerShell prompt.'
    }
}

function Test-TestSigning {
    # bcdedit reports "testsigning  Yes" when test signing is on.
    try {
        $output = & bcdedit /enum '{current}' 2>&1 | Out-String
    } catch {
        Write-Warning "Could not query bcdedit: $_"
        return $null
    }

    if ($output -match '(?im)^\s*testsigning\s+Yes\s*$') { return $true }
    return $false
}

function Find-Inf {
    if ($InfPath) {
        if (-not (Test-Path $InfPath)) { throw "INF not found: $InfPath" }
        return (Resolve-Path $InfPath).Path
    }

    # A usable package is an INF with DesktopSplitterVdd.dll next to it. That
    # rules out both the source INF and the intermediate stamped copy the WDK
    # leaves in x64\<Config>\ (the real package is x64\<Config>\DesktopSplitterVdd\).
    $driverDir = Join-Path $repoRoot 'driver'
    $candidates = Get-ChildItem -Path $driverDir -Filter 'DesktopSplitterVdd.inf' -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName 'DesktopSplitterVdd.dll') } |
        Sort-Object @{ Expression = { Test-Path (Join-Path $_.DirectoryName 'desktopsplittervdd.cat') }; Descending = $true },
                    @{ Expression = { $_.DirectoryName -match '\\Release\\' }; Descending = $true },
                    @{ Expression = { $_.LastWriteTime }; Descending = $true }

    if (-not $candidates) {
        throw "No built driver package found under $driverDir (expected DesktopSplitterVdd.inf with DesktopSplitterVdd.dll beside it). Build DesktopSplitterVdd.vcxproj with the WDK first."
    }

    $chosen = $candidates[0]
    if (-not (Test-Path (Join-Path $chosen.DirectoryName 'desktopsplittervdd.cat'))) {
        Write-Warning "No catalog file next to $($chosen.FullName); the package is unsigned and will not install without test signing."
    }

    return $chosen.FullName
}

function Get-DevCreate {
    $existing = Get-ChildItem -Path (Join-Path $scriptRoot 'devcreate') -Filter 'devcreate.exe' -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($existing) { return $existing[0].FullName }

    Write-Host 'devcreate.exe not found; building it...' -ForegroundColor Cyan

    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio (Desktop development with C++) or build installer\devcreate\devcreate.vcxproj manually.' }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $vsPath) { throw 'No Visual Studio installation with MSBuild was found.' }

    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuild)) { throw "MSBuild.exe not found at $msbuild" }

    $proj = Join-Path $scriptRoot 'devcreate\devcreate.vcxproj'
    & $msbuild $proj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Building devcreate failed (exit $LASTEXITCODE)." }

    $built = Join-Path $scriptRoot 'devcreate\bin\x64\Release\devcreate.exe'
    if (-not (Test-Path $built)) { throw "devcreate.exe was not produced at $built" }
    return $built
}

Assert-Admin

# ---- test signing ---------------------------------------------------------
$testSigning = Test-TestSigning
if ($testSigning -eq $false -and -not $SkipTestSigningCheck) {
    Write-Warning 'Test signing is DISABLED on this machine.'
    Write-Warning 'A self-signed / unsigned driver will NOT load until you run:'
    Write-Warning '    bcdedit /set testsigning on'
    Write-Warning 'and reboot. (Secure Boot must be off for test signing to take effect.)'
    $answer = Read-Host 'Continue with installation anyway? [y/N]'
    if ($answer -notmatch '^(y|Y)') {
        Write-Host 'Aborted.'
        exit 1
    }
} elseif ($testSigning -eq $true) {
    Write-Host 'Test signing is enabled.' -ForegroundColor Green
}

# ---- stage + install the package -----------------------------------------
$inf = Find-Inf
Write-Host "Installing driver package: $inf" -ForegroundColor Cyan

& pnputil.exe /add-driver "$inf" /install
$pnputilExit = $LASTEXITCODE
# pnputil returns 259 (ERROR_NO_MORE_ITEMS) when nothing needed updating.
if ($pnputilExit -ne 0 -and $pnputilExit -ne 259) {
    throw "pnputil /add-driver failed with exit code $pnputilExit."
}

# ---- create the device node ----------------------------------------------
$devcreate = Get-DevCreate
Write-Host "Creating device node with: $devcreate" -ForegroundColor Cyan

& $devcreate create
if ($LASTEXITCODE -ne 0) {
    throw "devcreate create failed with exit code $LASTEXITCODE."
}

Write-Host ''
Write-Host 'DesktopSplitterVdd installed.' -ForegroundColor Green
Write-Host 'The driver starts with 0 virtual monitors; the control app creates them'
Write-Host 'by sending IOCTL_DESKSPLIT_SET_CONFIG.'
