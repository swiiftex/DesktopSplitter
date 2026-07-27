<#
.SYNOPSIS
    Creates a self-signed code-signing certificate and signs the
    DesktopSplitterVdd driver package with it, for development / test-signing
    use only.

.DESCRIPTION
    1. Creates (or reuses) a self-signed code-signing certificate in
       Cert:\CurrentUser\My.
    2. Installs it into LocalMachine\Root and LocalMachine\TrustedPublisher so
       Windows trusts driver packages signed with it.
    3. Signs DesktopSplitterVdd.dll and DesktopSplitterVdd.cat with signtool
       (SHA-256, RFC3161 timestamp optional).
    4. Prints the commands needed to enable test signing.

    DesktopSplitterVdd is a UMDF (user-mode) driver, so the kernel-mode
    signing policy does NOT apply: no test-signing mode and no Secure Boot
    changes are needed. The package installs because this script places the
    signing certificate in LocalMachine\Root and LocalMachine\TrustedPublisher
    on THIS machine. To distribute to machines without installing a cert,
    use Microsoft attestation signing (see docs/BUILDING.md).

.PARAMETER PackageDir
    Directory containing DesktopSplitterVdd.inf, .dll and .cat. Defaults to
    searching ..\driver for the most recently built package.

.PARAMETER Subject
    Certificate subject. Default: "CN=DesktopSplitter Development".

.PARAMETER Timestamp
    Add an RFC3161 timestamp (requires internet access).

.EXAMPLE
    .\sign-dev.ps1
    .\sign-dev.ps1 -PackageDir ..\driver\x64\Release\DesktopSplitterVdd
#>
[CmdletBinding()]
param(
    [string] $PackageDir,
    [string] $Subject = 'CN=DesktopSplitter Development',
    [switch] $Timestamp
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script must be run from an elevated (Run as administrator) PowerShell prompt: it writes to the LocalMachine certificate stores.'
    }
}

function Find-SignTool {
    $roots = @(
        'C:\Program Files (x86)\Windows Kits\10\bin',
        'C:\Program Files\Windows Kits\10\bin'
    ) | Where-Object { Test-Path $_ }

    $candidates = foreach ($root in $roots) {
        Get-ChildItem -Path $root -Filter 'signtool.exe' -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -match '\\x64$' }
    }

    if (-not $candidates) { throw 'signtool.exe not found. Install the Windows SDK.' }
    return ($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}

function Find-PackageDir {
    if ($PackageDir) {
        if (-not (Test-Path $PackageDir)) { throw "Package directory not found: $PackageDir" }
        return (Resolve-Path $PackageDir).Path
    }

    $driverDir = Join-Path $repoRoot 'driver'
    $inf = Get-ChildItem -Path $driverDir -Filter 'DesktopSplitterVdd.inf' -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName 'DesktopSplitterVdd.dll') } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $inf) { throw "No built driver package found under $driverDir. Build the driver with the WDK first." }
    return $inf.DirectoryName
}

Assert-Admin

# ---- certificate ----------------------------------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1

if ($cert) {
    Write-Host "Reusing existing certificate: $($cert.Thumbprint)" -ForegroundColor Green
} else {
    Write-Host "Creating self-signed code-signing certificate: $Subject" -ForegroundColor Cyan
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(5) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')   # EKU: code signing
    Write-Host "Created certificate: $($cert.Thumbprint)" -ForegroundColor Green
}

# Trust it machine-wide so the signed package validates.
$publicCer = Join-Path $env:TEMP 'DesktopSplitterDev.cer'
Export-Certificate -Cert $cert -FilePath $publicCer -Force | Out-Null

foreach ($store in @('Root', 'TrustedPublisher')) {
    Write-Host "Importing certificate into LocalMachine\$store" -ForegroundColor Cyan
    Import-Certificate -FilePath $publicCer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}
Remove-Item $publicCer -Force -ErrorAction SilentlyContinue

# ---- sign -----------------------------------------------------------------
$signtool = Find-SignTool
$package = Find-PackageDir
Write-Host "signtool: $signtool"
Write-Host "package : $package"

$targets = @('DesktopSplitterVdd.dll', 'DesktopSplitterVdd.cat') |
    ForEach-Object { Join-Path $package $_ } |
    Where-Object { Test-Path $_ }

if (-not $targets) { throw "Nothing to sign in $package (expected DesktopSplitterVdd.dll and DesktopSplitterVdd.cat)." }

foreach ($target in $targets) {
    Write-Host "Signing $target" -ForegroundColor Cyan

    $signArgs = @('sign', '/v', '/fd', 'sha256', '/sha1', $cert.Thumbprint, '/s', 'My')
    if ($Timestamp) { $signArgs += @('/tr', 'http://timestamp.digicert.com', '/td', 'sha256') }
    $signArgs += $target

    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $target (exit $LASTEXITCODE)." }
}

Write-Host ''
Write-Host 'Signing complete.' -ForegroundColor Green
Write-Host ''
Write-Host 'No test-signing mode is required: this is a user-mode (UMDF) driver and the' -ForegroundColor Yellow
Write-Host 'signing certificate is now trusted machine-wide. Secure Boot can stay enabled.'
Write-Host 'If you previously enabled test signing for this driver, you can turn it off:'
Write-Host '    bcdedit /set testsigning off   (then reboot)'
Write-Host ''
Write-Host 'Note: the certificate trust is per-machine. To ship to other machines without'
Write-Host 'installing a certificate, use Microsoft attestation signing (docs/BUILDING.md).'
