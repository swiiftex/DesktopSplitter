# DesktopSplitterSetup

One file — `DesktopSplitterSetup.exe` (~139 MB) — that installs the whole
DesktopSplitter product on any Windows 10/11 x64 PC, including one with Secure
Boot on and test signing off. No .NET runtime, no WiX/Inno/NSIS, no
"navigate to the project folder".

Double-clicking gives a native Win32 wizard. The same binary also runs headless
for scripted deployment.

## Why it works without test signing

The driver is **UMDF 2** — user-mode code hosted by `WUDFHost.exe`. Kernel-mode
signing policy (test signing, Secure Boot, HVCI) governs what loads into the
*kernel* and does not apply to it. The only thing Windows enforces for a driver
*package* is that its catalog signature chains to a trusted root:

| Package signed by | `WinVerifyTrust` | What the user sees |
| --- | --- | --- |
| Microsoft attestation / WHQL | succeeds | UAC prompt only |
| Our development certificate | `CERT_E_UNTRUSTEDROOT` (0x800B0109) | UAC prompt **+** a publisher-trust dialog |

The installer detects which case it is at run time and only prompts when it
must. Once the package is attestation-signed the extra dialog disappears with no
code change.

## Wizard flow

```
 ┌ Welcome ─────────────┐   version, what will be installed, upgrade notice if
 │                      │   an existing install is detected
 └──────────┬───────────┘
 ┌ Options ─▼───────────┐   install location (editable + Browse)
 │                      │   [x] desktop shortcut   [x] Start menu shortcut
 │                      │   [ ] hide the physical display  (edition-gated)
 └──────────┬───────────┘
 ┌ Progress ▼───────────┐   step text + progress bar + scrolling per-step log,
 │                      │   engine on a worker thread, Cancel honoured
 └──────────┬───────────┘   publisher-trust dialog appears here if needed
 ┌ Finish ──▼───────────┐   result summary, [x] Launch DesktopSplitter
 └──────────────────────┘
```

Uninstall (launched from Add/Remove Programs) reuses the same wizard with an
Options page offering only **"Also remove the publisher certificate"**
(unchecked by default).

Pages are built from child controls in code rather than dialog templates: DPI
scaling stays trivial and `/selftest` can walk them without showing a window.

## Embedded vs staged

Everything ships **inside the exe** as `RT_RCDATA`. Nothing is downloaded.

| Resource | Content | Source |
| --- | --- | --- |
| `IDR_DRIVER_INF/DLL/CAT` | signed driver package | `driver\x64\Release\DesktopSplitterVdd\` |
| `IDR_APP_EXE` | `DesktopSplitter.exe`, **self-contained single file** | `dotnet publish -r win-x64 --self-contained -p:PublishSingleFile -p:IncludeNativeLibrariesForSelfExtract` |
| `IDR_DSCOMP_EXE` | `dscomp.exe` | `compositor\build\x64\Release\` |
| `IDR_DEVCREATE_EXE` | `devcreate.exe` | `installer\devcreate\bin\x64\Release\` |
| `IDR_COMPOSITOR_README` | `compositor-README.md` | `compositor\README.md` |

`stage-payload.ps1` runs as an MSBuild prebuild step, gathers all of the above
into `payload\` (git-ignored), and **fails the build with an actionable
message** when the driver package is missing/incomplete, the catalog is
unsigned, `dotnet publish` fails, or the compositor/devcreate are unbuilt. It
records what it embedded in `payload\payload.json`.

`IncludeNativeLibrariesForSelfExtract` folds the WPF native DLLs into the single
file, so exactly one app binary is embedded and installed.

## Installed layout

Everything lands flat in one directory (default `%ProgramFiles%\DesktopSplitter`):

```
DesktopSplitter.exe        the control app (self-contained; no .NET needed)
dscomp.exe                 the compositor
devcreate.exe              device node helper
compositor-README.md
DesktopSplitterSetup.exe   a copy of this installer, for Add/Remove Programs
```

This layout needs **no app changes**: `CompositorLauncher` already probes
`dscomp.exe` next to the app *first* ("deployed layout — always tried first")
before walking up to the repo build tree.

Registered in **HKLM** `...\Uninstall\DesktopSplitter` with `DisplayName`,
`DisplayVersion`, `Publisher`, `InstallLocation`, `UninstallString`,
`QuietUninstallString`, `DisplayIcon`, `EstimatedSize`, `NoModify`, `NoRepair`.

## Feature flag: hide the physical display

`HKLM\SOFTWARE\DesktopSplitter`:

| Value | Meaning |
| --- | --- |
| `HidePhysicalDisplaySupported` (DWORD) | the OS edition can honour a specialized display |
| `HidePhysicalDisplay` (DWORD) | the user opted in |
| `InstallLocation`, `Version` (SZ) | for the app |

The installer **only records intent** — it never touches EDIDs or display
config. The app/compositor consume the flag.

The checkbox is enabled only when `EditionID` is one of
`ProfessionalWorkstation`, `Enterprise`, `IoTEnterprise` (Microsoft gates
user-designated specialized displays to those editions). Otherwise it is
disabled and reads *"Requires Windows Pro for Workstations / Enterprise"*, and
`HidePhysicalDisplay` is forced to 0.

## Command line

| Invocation | Behaviour |
| --- | --- |
| *(none)* | GUI wizard |
| `/uninstall` | GUI uninstaller |
| `/quiet` | install, no UI, implies accepting the publisher prompt |
| `/uninstall /quiet` | remove, no UI |
| `/verify` | report embedded payload, signature and install state; **changes nothing, needs no admin** |
| `/selftest` | headless wizard test (see below) |
| `/?` | usage |

Console options: `/dir <path>`, `/noshortcut`, `/hidedisplay`, `/removecert`,
`/keeptemp`. `-` works in place of `/`.

The exe is **GUI subsystem**, so double-clicking never flashes a console.
Console modes call `AttachConsole(ATTACH_PARENT_PROCESS)` so output lands in the
launching shell, and honour redirection (`setup.exe /verify > log.txt`).

## Exit codes

| Code | Meaning | | Code | Meaning |
| ---: | --- | --- | ---: | --- |
| 0 | success | | 7 | certificate installation failed |
| 1 | bad command line | | 8 | driver package installation failed |
| 2 | not elevated | | 9 | device node creation failed |
| 3 | embedded package missing | | 10 | post-install verification failed |
| 4 | package extraction failed | | 11 | uninstall failed |
| 5 | catalog unusable | | 12 | copying program files failed |
| 6 | trust declined by user | | 13 | cancelled |

## Upgrade and uninstall

**Upgrade** — running setup over an existing install detects it from the ARP
key, pre-selects the existing directory, stops running components, overwrites
the files, installs the new driver package and prunes every superseded
`oemNN.inf` (identified by diffing the staged set before/after, not by parsing
`DriverVer`). The device node and settings are kept.

**Uninstall** — stops the app and compositor, sends `monitorCount = 0` through
the device interface so any active split is undone *before* the device
disappears, removes the device node, removes all matching driver packages,
deletes files/shortcuts/registry, and optionally removes the publisher
certificate.

## Architecture

| File | Role |
| --- | --- |
| `Setup.cpp` | entry point, argument parsing, mode dispatch, console sink, `/verify` |
| `Wizard.cpp/.h` | the Win32 wizard, GUI sink, publisher-trust dialog, `/selftest` |
| `InstallEngine.cpp/.h` | the install/uninstall sequence, UI-independent |
| `ProductFiles.cpp/.h` | program files, shortcuts, ARP entry, feature flags, process control |
| `Edition.cpp/.h` | edition detection + test override |
| `Payload.cpp/.h` | RCDATA extraction |
| `Trust.cpp/.h` | `WinVerifyTrust`, signer certificate, machine trust stores |
| `DriverPackage.cpp/.h` | `DiInstallDriverW` + pnputil fallback, `oemNN.inf` lookup/pruning |
| `../common/DeviceNode.cpp/.h` | root device node create/find/remove/status |

Console and GUI drive the **same** `InstallEngine` and differ only in the
`IStepSink` they pass, so they cannot drift apart. The engine runs on a worker
thread in GUI mode and marshals every UI touch with `SendMessage`.

## Testing without elevation

`requireAdministrator` means the process cannot start unelevated, so the project
has a build-time test hook:

```
msbuild DesktopSplitterSetup.vcxproj /p:Configuration=Release /p:Platform=x64 ^
        /p:DeskSplitNoManifest=true /p:DeskSplitSkipAppPublish=true ^
        /p:OutDir=<scratch>\
```

`DeskSplitNoManifest` omits the manifest (defaults to including it — **never
ship a build made with that flag**); `DeskSplitSkipAppPublish` reuses the last
app publish for quick rebuilds.

`/selftest` then builds every wizard page **without showing a window**, walks
them, and asserts control states — including the hide-display checkbox under the
real registry edition and under mocked `ProfessionalWorkstation`, `Enterprise`,
`IoTEnterprise`, `Professional` and `Core`. It exits non-zero on any failure.

Note it must test the control's own `WS_VISIBLE` bit: `IsWindowVisible()` also
requires every ancestor to be visible, which is never true for a hidden wizard.

## How to test the real install

On a machine you are willing to disturb (it installs a display driver):

1. `.\build.ps1` — produces
   `installer\setup\bin\x64\Release\DesktopSplitterSetup.exe`.
2. Copy that single file anywhere (ideally a *clean* VM with no .NET).
3. `DesktopSplitterSetup.exe /verify` first — confirms what is inside and what
   is already installed, without changing anything or needing admin.
4. Double-click it. Accept UAC. On a PC that does not yet trust the development
   certificate you also get the publisher dialog — check the SHA-1 matches the
   one printed by `/verify`.
5. Walk the wizard, leave **Launch DesktopSplitter** ticked, Finish.
6. Verify: the app starts; Add/Remove Programs lists DesktopSplitter; Device
   Manager shows *DesktopSplitter Virtual Display* with no warning icon.
7. Uninstall from Add/Remove Programs and confirm the desktop returns to the
   real monitor and the device disappears.

For unattended deployment: `DesktopSplitterSetup.exe /quiet` (exit code 0 = ok)
and `DesktopSplitterSetup.exe /uninstall /quiet`.
