<#
.SYNOPSIS
    Removes the DesktopSplitterVdd device node and deletes the driver package
    from the driver store.

.DESCRIPTION
    1. Verifies the shell is elevated.
    2. Runs `devcreate remove` to uninstall every Root\DesktopSplitterVdd node.
    3. Finds the staged oem*.inf for DesktopSplitterVdd and deletes it with
       pnputil /delete-driver /uninstall.

.PARAMETER Force
    Pass /force to pnputil /delete-driver.
#>
[CmdletBinding()]
param(
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script must be run from an elevated (Run as administrator) PowerShell prompt.'
    }
}

Assert-Admin

# ---- remove the device node ----------------------------------------------
$devcreate = Get-ChildItem -Path (Join-Path $scriptRoot 'devcreate') -Filter 'devcreate.exe' -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($devcreate) {
    Write-Host "Removing device node with: $($devcreate.FullName)" -ForegroundColor Cyan
    & $devcreate.FullName remove
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "devcreate remove returned exit code $LASTEXITCODE; continuing."
    }
} else {
    Write-Warning 'devcreate.exe not found; skipping device node removal.'
    Write-Warning 'Remove the "DesktopSplitter Virtual Display" device from Device Manager manually if it is still present.'
}

# ---- delete the driver package -------------------------------------------
Write-Host 'Looking for the staged driver package...' -ForegroundColor Cyan

$enum = & pnputil.exe /enum-drivers | Out-String
$publishedName = $null
$currentName = $null

foreach ($line in ($enum -split "`r?`n")) {
    if ($line -match '^\s*Published Name\s*:\s*(\S+)') {
        $currentName = $Matches[1]
    } elseif ($line -match '^\s*Original Name\s*:\s*(\S+)') {
        if ($Matches[1] -ieq 'DesktopSplitterVdd.inf') {
            $publishedName = $currentName
        }
    }
}

if (-not $publishedName) {
    Write-Host 'No staged DesktopSplitterVdd package found in the driver store.'
} else {
    Write-Host "Deleting driver package: $publishedName" -ForegroundColor Cyan
    $pnpArgs = @('/delete-driver', $publishedName, '/uninstall')
    if ($Force) { $pnpArgs += '/force' }

    & pnputil.exe @pnpArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil /delete-driver returned exit code $LASTEXITCODE."
        Write-Warning 'Re-run with -Force if the package is still in use.'
    }
}

Write-Host ''
Write-Host 'DesktopSplitterVdd uninstalled.' -ForegroundColor Green
