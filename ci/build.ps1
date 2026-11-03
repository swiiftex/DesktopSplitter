# DesktopSplitter cloud build driver v5 (lives outside .github/workflows so
# it can be updated without editing the workflow file).
# Mirrors build.ps1 step by step, but every native build runs with output
# redirected to logs\<step>.log, the driver catalog signature is checked
# (and fixed with the repo's own installer\sign-dev.ps1 when unsigned),
# and log tails are appended to ci-log.txt, which the workflow commits
# to the ci-logs branch after every run.
$ErrorActionPreference = 'Continue'

$logPath = Join-Path $PWD 'ci-log.txt'
New-Item -ItemType Directory -Path (Join-Path $PWD 'logs') -Force | Out-Null
try { Start-Transcript -Path $logPath -Force | Out-Null } catch { }

function Section([string]$t) { Write-Host "`n########## $t ##########" }

# NOTE: do NOT wrap builds in Start-Process -Wait with redirected streams:
# dotnet/msbuild leave build-server processes alive that inherit the
# redirected handles, and Start-Process -Wait then hangs forever (v6 bug).
# Direct invocation + Tee returns when the direct process exits.
$env:MSBUILDDISABLENODEREUSE = '1'
function Invoke-Logged([string]$Name, [string]$Exe, [string[]]$ArgList) {
    $out = Join-Path $PWD "logs\$Name.log"
    Write-Host ">> $Name : $Exe $($ArgList -join ' ')"
    try {
        & $Exe @ArgList *>&1 | Tee-Object -FilePath $out | Out-Null
        $code = $LASTEXITCODE
    } catch {
        $code = 1
        Write-Host "!! $Name threw: $($_.Exception.Message)"
        Add-Content -Path $out -Value "threw: $($_.Exception.Message)"
    }
    if ($null -eq $code) { $code = 0 }
    Write-Host "<< $Name exit code: $code"
    return $code
}

Section 'ENVIRONMENT'
Write-Host "PSVersion: $($PSVersionTable.PSVersion)"
Write-Host "OS: $([Environment]::OSVersion.VersionString)"
Write-Host "Elevated: $(([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) { & $vswhere -all -products * -format json } else { Write-Host 'vswhere NOT FOUND' }
if (Get-Command dotnet -ErrorAction SilentlyContinue) { dotnet --list-sdks } else { Write-Host 'dotnet NOT FOUND' }

function Get-Winget {
    $w = Get-Command winget -ErrorAction SilentlyContinue
    if ($w) { return $w.Source }
    $candidate = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path $candidate) { return $candidate }
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    $w = Get-Command winget -ErrorAction SilentlyContinue
    if ($w) { return $w.Source }
    return $null
}
$winget = Get-Winget
Write-Host "winget: $winget"

Section 'INSTALL WINDOWS SDK 10.0.26100 (if missing)'
if (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Include\10.0.26100.0") {
    Write-Host 'SDK 26100 already present, skipping'
} elseif ($winget) {
    & $winget install --id Microsoft.WindowsSDK.10.0.26100 --exact --accept-source-agreements --accept-package-agreements --disable-interactivity
    Write-Host "winget SDK exit code: $LASTEXITCODE"
} else {
    Write-Host 'winget NOT FOUND - cannot install SDK (will fail later if truly missing)'
}

Section 'INSTALL WDK 10.0.26100 (if IddCx missing)'
$iddcxPresent = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\iddcx\*\IddCx.h" -ErrorAction SilentlyContinue
if ($iddcxPresent) {
    Write-Host 'IddCx headers already present, skipping WDK install'
} elseif ($winget) {
    & $winget install --id Microsoft.WindowsWDK.10.0.26100 --exact --accept-source-agreements --accept-package-agreements --disable-interactivity
    Write-Host "winget WDK exit code: $LASTEXITCODE"
} else {
    Write-Host 'winget NOT FOUND - cannot install WDK (will fail later if truly missing)'
}

Section 'ADD WDK COMPONENT TO VISUAL STUDIO'
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1 }
Write-Host "VS path: $vs"
if ($vs) {
    $setupExeVs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
    $p = Start-Process -FilePath $setupExeVs -Wait -PassThru -ArgumentList 'modify', '--installPath', "`"$vs`"", '--add', 'Component.Microsoft.Windows.DriverKit', '--quiet', '--norestart'
    Write-Host "VS modify exit code: $($p.ExitCode)"
} else { Write-Host 'No VS installation found!' }

$msbuild = $null
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1 }
}
Write-Host "msbuild: $msbuild"
if (-not $msbuild) { try { Stop-Transcript | Out-Null } catch { }; exit 1 }

Section 'GENERATE VERSION FILES (same as build.ps1, NoVersionBump)'
$versionLine = Get-Content .\VERSION | Where-Object { $_.Trim() -ne '' -and -not $_.TrimStart().StartsWith('#') } | Select-Object -Last 1
$version = $versionLine.Trim()
$vp = $version.Split('.'); $vMajor = [int]$vp[0]; $vMinor = [int]$vp[1]; $vPatch = [int]$vp[2]
Write-Host "version: $version"
$genDir = Join-Path $PWD 'build\generated'
New-Item -ItemType Directory -Path $genDir -Force | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-Gen([string]$Path, [string]$Content, [switch]$NoTrailingNewline) {
    if (-not $NoTrailingNewline -and -not $Content.EndsWith("`n")) { $Content += "`r`n" }
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}
Write-Gen (Join-Path $genDir 'Version.props') @"
<?xml version="1.0" encoding="utf-8"?>
<!-- GENERATED by ci/build.ps1 from the root VERSION file. Do not edit. -->
<Project>
  <PropertyGroup>
    <DeskSplitVersion>$version</DeskSplitVersion>
    <DeskSplitVersionMajor>$vMajor</DeskSplitVersionMajor>
    <DeskSplitVersionMinor>$vMinor</DeskSplitVersionMinor>
    <DeskSplitVersionPatch>$vPatch</DeskSplitVersionPatch>
  </PropertyGroup>
</Project>
"@
Write-Gen (Join-Path $genDir 'Version.h') @"
/* GENERATED by ci/build.ps1 from the root VERSION file. Do not edit. */
#ifndef DESKSPLIT_VERSION_H
#define DESKSPLIT_VERSION_H

#define DS_VERSION_MAJOR $vMajor
#define DS_VERSION_MINOR $vMinor
#define DS_VERSION_PATCH $vPatch

#define DS_VERSION_STR   "$version"
#define DS_VERSION_WSTR  L"$version"

/* For VERSIONINFO FILEVERSION / PRODUCTVERSION (4 parts). */
#define DS_VERSION_COMMA $vMajor,$vMinor,$vPatch,0
#define DS_VERSION_STR4  "$version.0"

#endif /* DESKSPLIT_VERSION_H */
"@
Write-Gen (Join-Path $genDir 'version.txt') $version -NoTrailingNewline

$failAt = $null

Section 'BUILD compositor'
if (-not $failAt) { if ((Invoke-Logged 'compositor' $msbuild @('compositor\dscomp.vcxproj','/p:Configuration=Release','/p:Platform=x64','/v:m','/nologo')) -ne 0) { $failAt = 'compositor' } }

Section 'BUILD devcreate'
if (-not $failAt) { if ((Invoke-Logged 'devcreate' $msbuild @('installer\devcreate\devcreate.vcxproj','/p:Configuration=Release','/p:Platform=x64','/v:m','/nologo')) -ne 0) { $failAt = 'devcreate' } }

Section 'BUILD edidoverride'
if (-not $failAt) { if ((Invoke-Logged 'edidoverride' $msbuild @('installer\edid-override\edidoverride.vcxproj','/p:Configuration=Release','/p:Platform=x64','/v:m','/nologo')) -ne 0) { $failAt = 'edidoverride' } }

Section 'BUILD app (dotnet)'
if (-not $failAt) { if ((Invoke-Logged 'app' 'dotnet' @('build','app\DesktopSplitter.csproj','-c','Release','--nologo','--disable-build-servers')) -ne 0) { $failAt = 'app' } }

Section 'BUILD driver'
if (-not $failAt) { if ((Invoke-Logged 'driver' $msbuild @('driver\DesktopSplitterVdd.vcxproj','/p:Configuration=Release','/p:Platform=x64','/p:Inf2CatUseLocalTime=true','/v:m','/nologo')) -ne 0) { $failAt = 'driver' } }

Section 'DRIVER CATALOG SIGNATURE'
$pkgDir = Join-Path $PWD 'driver\x64\Release\DesktopSplitterVdd'
Get-ChildItem $pkgDir -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.Name) $($_.Length)" }
$catPath = Join-Path $pkgDir 'desktopsplittervdd.cat'
if (-not $failAt) {
    $sig = Get-AuthenticodeSignature $catPath
    Write-Host "catalog signature status: $($sig.Status)"
    if ($sig.SignerCertificate) { Write-Host "signer: $($sig.SignerCertificate.Subject)" }
    # Always dev-sign with the repo's own sign-dev.ps1 (explicit package dir):
    # the raw driver build signs the catalog with the build machine's ephemeral
    # "WDKTestCert <user>" which looks like gibberish in the installer's trust
    # prompt. Re-signing makes the prompt show "DesktopSplitter Development".
    Write-Host 'Dev-signing the driver package with installer\sign-dev.ps1 (explicit -PackageDir)...'
    try {
        & .\installer\sign-dev.ps1 -PackageDir $pkgDir *>&1 | Tee-Object -FilePath (Join-Path $PWD 'logs\sign-dev.log')
        Write-Host "sign-dev completed"
    } catch {
        Write-Host "sign-dev threw: $($_.Exception.Message)"
    }
    $sig2 = Get-AuthenticodeSignature $catPath
    Write-Host "catalog signature after sign-dev: $($sig2.Status)"
    if ($sig2.SignerCertificate) { Write-Host "signer: $($sig2.SignerCertificate.Subject)" }
    if ($sig2.Status -eq 'NotSigned' -or -not $sig2.SignerCertificate) { $failAt = 'sign-dev' }
}

Section 'BUILD setup (DesktopSplitterSetup)'
# stage-payload.ps1 runs inside a powershell.exe (5.1) child spawned by msbuild;
# it cannot load Microsoft.PowerShell.Security when PSModulePath is polluted by
# this pwsh 7 session. Clear it so the child rebuilds its own default path.
[Environment]::SetEnvironmentVariable('PSModulePath', $null, 'Process')
if (-not $failAt) { if ((Invoke-Logged 'setup' $msbuild @('installer\setup\DesktopSplitterSetup.vcxproj','/p:Configuration=Release','/p:Platform=x64','/v:m','/nologo')) -ne 0) { $failAt = 'setup' } }

Section 'SELF-TEST (sabotaged WUDFRD.inf, silent install)'
# Reproduce the target PC's defect on this throwaway runner: hide the inbox
# WUDFRD.inf, run the installer with /quiet, and require that (a) the
# installer restores WUDFRD.inf itself and (b) the virtual display device
# ends up bound to the driver (Class = Display). If this fails, the build
# fails and no release is published.
if (-not $failAt) {
    $setupExeT = 'installer\setup\bin\x64\Release\DesktopSplitterSetup.exe'
    $namedT = "DesktopSplitterSetup-$version.exe"
    Copy-Item $setupExeT $namedT -Force
    $wufrdT = Join-Path $env:WINDIR 'INF\WUDFRD.inf'
    $wufrdBakT = "$wufrdT.citest.bak"
    try {
        if (-not (Test-Path $wufrdT)) { throw "runner unexpectedly lacks $wufrdT" }
        & takeown /f $wufrdT | Out-Null
        & icacls $wufrdT /grant '*S-1-5-32-544:F' | Out-Null
        Rename-Item $wufrdT $wufrdBakT -Force
        if (Test-Path $wufrdT) { throw 'sabotage failed: WUDFRD.inf still present' }
        Write-Host 'SELF-TEST: WUDFRD.inf hidden (simulating the target PC)'

        $procT = Start-Process -FilePath (Join-Path $PWD $namedT) -ArgumentList '/quiet' -PassThru
        if (-not $procT.WaitForExit(600000)) { $procT.Kill(); throw 'setup /quiet timed out after 10 minutes' }
        Write-Host "SELF-TEST: setup exit code = $($procT.ExitCode)"
        if ($procT.ExitCode -ne 0) { throw "setup /quiet failed with exit $($procT.ExitCode)" }

        if (-not (Test-Path $wufrdT)) { throw 'WUDFRD.inf was NOT restored by the installer' }
        Write-Host 'SELF-TEST: WUDFRD.inf restored by the installer'

        $devT = Get-CimInstance Win32_PnPEntity -Filter "Name='DesktopSplitter Virtual Display'" -ErrorAction Stop | Select-Object -First 1
        if (-not $devT) { throw 'device not found via CIM after install' }
        Write-Host "SELF-TEST: device PNPClass=$($devT.PNPClass) Status=$($devT.Status) ConfigError=$($devT.ConfigManagerErrorCode) DeviceID=$($devT.DeviceID)"
        if ($devT.PNPClass -ne 'Display') { throw "device class is '$($devT.PNPClass)', expected 'Display' (driver not bound)" }
        if ($devT.ConfigManagerErrorCode -ne 0) { throw "device problem code $($devT.ConfigManagerErrorCode), expected 0" }
        Write-Host 'SELF-TEST: PASSED - driver bound on a machine missing WUDFRD.inf'
    } catch {
        Write-Host "SELF-TEST FAILED: $($_.Exception.Message)"
        $failAt = 'self-test'
    } finally {
        try {
            $unT = Start-Process -FilePath (Join-Path $PWD $namedT) -ArgumentList '/uninstall','/quiet' -PassThru
            if (-not $unT.WaitForExit(300000)) { $unT.Kill() }
        } catch { }
        if (Test-Path $wufrdBakT) { Rename-Item $wufrdBakT $wufrdT -Force -ErrorAction SilentlyContinue }
    }
}

Section 'RESULT'
$finalExit = 1
$setupExe = 'installer\setup\bin\x64\Release\DesktopSplitterSetup.exe'
if (-not $failAt -and (Test-Path $setupExe)) {
    $named = "DesktopSplitterSetup-$version.exe"
    Copy-Item $setupExe $named -Force
    Write-Host "Setup exe ready: $named ($([math]::Round((Get-Item $named).Length/1MB,1)) MB)"
    & ".\$named" /verify | Write-Host
    Write-Host "verify exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -eq 0) { $finalExit = 0 }
} else {
    Write-Host "FAILED at step: $failAt"
    Write-Host 'Setup exe NOT produced.'
}

try { Stop-Transcript | Out-Null } catch { }

# Append per-step log tails to the committed log.
try {
    foreach ($name in @('compositor','devcreate','edidoverride','app','driver','sign-dev','setup')) {
        $f = Join-Path $PWD "logs\$name.log"
        if (Test-Path $f) {
            Add-Content -Path $logPath -Value "`n########## STEP LOG: $name (last 300 lines) ##########"
            Get-Content $f -Tail 300 | Add-Content -Path $logPath
        }
        $fe = Join-Path $PWD "logs\$name.err"
        if ((Test-Path $fe) -and (Get-Item $fe).Length -gt 0) {
            Add-Content -Path $logPath -Value "`n########## STEP LOG: $name STDERR (last 100 lines) ##########"
            Get-Content $fe -Tail 100 | Add-Content -Path $logPath
        }
    }
} catch { }

exit $finalExit
