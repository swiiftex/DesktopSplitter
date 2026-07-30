# DesktopSplitter — Architecture

Split a single physical monitor into 2–4 virtual monitors that Windows treats as
completely separate displays. Windows-only. GPU-vendor-neutral (works on AMD,
NVIDIA, Intel) because it is built on IddCx, Microsoft's Indirect Display Driver
class extension — no vendor driver APIs are used anywhere.

## How it works (data flow)

```
 apps render to virtual monitors (normal Windows desktop)
        │
        ▼
 [driver] IddCx virtual display driver creates N virtual monitors
        │  (Windows composites desktops onto them like any real monitor)
        ▼
 [compositor] captures each virtual monitor via DXGI Desktop Duplication
        │  and draws each into its segment rect
        ▼
 borderless fullscreen flip-model window on the PHYSICAL monitor,
 presented at the physical monitor's max refresh rate
```

The physical monitor keeps its normal desktop, but the compositor covers it
entirely and the cursor is confined to the virtual monitors' area of the
virtual desktop, so the user only ever sees and interacts with the virtual
monitors. Input needs no forwarding: the OS cursor genuinely lives on the
virtual monitors; the compositor renders the cursor image supplied by the
duplication API at its true position.

## Components

### 1. driver/ — DesktopSplitterVdd (C++ UMDF 2, IddCx ≥ 1.4)
- Root-enumerated software device, hardware ID `Root\DesktopSplitterVdd`.
- On adapter init, reads persisted config from its registry software key and
  creates that many monitors (0 on first install).
- Exposes a device interface (GUID in `shared/DeskSplitProtocol.h`). Control
  app opens it and sends `IOCTL_DESKSPLIT_SET_CONFIG` to change monitor
  count/modes at runtime → driver performs IddCxMonitorDeparture/Arrival.
- Per-monitor mode list is supplied in the config (width, height, refresh in
  millihertz). Each monitor advertises exactly the modes it is given, with the
  first mode preferred. Monitor description via synthesized EDID with unique
  serial per monitor index (so Windows persists per-monitor settings).
- Swapchain processing: standard IddCx swapchain drain on a D3D11 device on
  the default adapter (frames acquired and released; presentation to the user
  happens via the compositor's desktop duplication, not in the driver).
- Modeled on Microsoft's IddSampleDriver (MIT).

### 2. compositor/ — dscomp.exe (C++17, D3D11 + DXGI 1.5)
- Reads `%ProgramData%\DesktopSplitter\config.json` (written by control app).
- For each segment: finds the virtual monitor's DXGI output by device name
  (`\\.\DISPLAYn`), starts an `IDXGIOutputDuplication` capture thread.
- Window: borderless, `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, topmost, covers
  the physical monitor. Flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`,
  tearing allowed when supported); presents on every v-blank of the physical
  output → max refresh.
- Scales each captured virtual monitor into its segment rect (linear
  filtering when captured size ≠ segment size).
- Cursor: uses the pointer shape/position from desktop duplication and draws
  it composited; supports monochrome and masked color shapes.
- Cursor confinement: `ClipCursor` to the bounding rect of the virtual
  monitors' union in virtual-desktop space, plus a `WH_MOUSE_LL` hook that
  rejects movements into the physical monitor's own desktop rect (covers
  non-rectangular unions).
- Exits cleanly on `WM_CLOSE` / named event `Global\DeskSplitCompositorStop`.

### 3. app/ — DesktopSplitter.exe (C# .NET 8 WPF)
- Enumerates physical monitors (`QueryDisplayConfig`) with max refresh rate.
- UI: choose physical monitor → choose layout → set each segment's resolution
  (defaults = physical resolution subdivided) → Apply / Revert.
- Layouts: 2-way (vertical split, horizontal split), 3-way (3 columns,
  1 big + 2 stacked), 4-way (2×2 grid, 4 columns).
- Apply sequence:
  1. Write config.json (see schema below).
  2. Open driver device interface, send `IOCTL_DESKSPLIT_SET_CONFIG`.
  3. Wait for the virtual monitors to appear (poll display enumeration).
  4. Position virtual monitors contiguously in virtual desktop space to the
     RIGHT of the physical monitor via `SetDisplayConfig`, matching the
     layout's relative geometry; set each to its configured mode.
  5. Launch dscomp.exe.
- Revert: stop compositor, send config with monitorCount=0, restore clip.
- Tray icon with Apply last config / Revert / Exit.

### 4. installer/
- `install-driver.ps1`: enable test signing check, `pnputil /add-driver
  DesktopSplitterVdd.inf /install`, create the root-enumerated device via
  SWDevice API helper (`devgen` or bundled `nefconc`-style createdevice tool —
  use `pnputil /add-driver` + a small C++ `devcreate.exe` calling
  `SwDeviceCreate`).
- `uninstall-driver.ps1`, `sign-dev.ps1` (self-signed cert + signtool for
  test-signing mode).

## Shared contracts
- `shared/DeskSplitProtocol.h` — device interface GUID, IOCTLs, config structs
  (used by driver C++ and P/Invoked from C#).
- `%ProgramData%\DesktopSplitter\config.json` — control app → compositor:

```json
{
  "physicalDevice": "\\\\.\\DISPLAY1",
  "refreshMillihertz": 164999,
  "primarySegment": 0,
  "launchSegment": -1,
  "segments": [
    { "virtualDevice": "\\\\.\\DISPLAY3", "width": 1280, "height": 1440,
      "physRect": { "x": 0, "y": 0, "w": 1280, "h": 1440 }, "showTaskbar": true },
    { "virtualDevice": "\\\\.\\DISPLAY4", "width": 1280, "height": 1440,
      "physRect": { "x": 1280, "y": 0, "w": 1280, "h": 1440 }, "showTaskbar": false }
  ]
}
```
`physRect` is in physical-monitor-local pixels. The compositor maps each
virtual monitor's captured frames into its physRect.

`primarySegment` (0-based) is the segment the control app promotes to Windows'
primary display — where the taskbar, Start menu, Alt-Tab and the system tray
go. The app performs the promotion; the field is informational for the
compositor. Absent means 0.

`launchSegment` (0-based, or `-1` for off) is the segment new application
windows should be steered to. This exists so a *side* segment can hold the
primary display — and therefore the full taskbar, system tray and notification
centre — while apps and games still open on the centre segment.

`showTaskbar` per segment (absent means `true`, for backward compatibility)
asks for Windows' taskbar on that segment. Note the measured Windows 11 (build
26200) behaviour: a **non-primary** taskbar shows app buttons and a clock, but
**not** the system tray icons and **not** the notification centre — those are
anchored to the primary display. Taskbars on non-primary displays additionally
require `HKCU\...\Explorer\Advanced\MMTaskbarEnabled = 1`, which Explorer only
reads at startup, so it needs a sign-out to take effect.

## Implementation notes (deviations from the original plan, both intentional)

- **Driver IOCTLs** are handled in `EvtIddCxDeviceIoControl`, not a WDF
  default queue: IddCx redirects `IRP_MJ_DEVICE_CONTROL` to its own queue, so
  a driver-owned default queue would never see our IOCTLs.
- **`devcreate create`** registers a *persistent* devnode via SetupAPI
  (`DIF_REGISTERDEVICE` + `DiInstallDevice`); a plain `SwDeviceCreate` node
  dies with the creating process, which would defeat reboot persistence.
  `devcreate create --swdevice` keeps the original behavior.
- **Segment → virtual monitor mapping** is by EDID identity, not position:
  the driver stamps each monitor's EDID with mfg `DSP`, product code = index,
  serial `0xD5000001 + index`; the control app reads the EDID back from the
  monitor devnode registry and maps segment i to serial `0xD5000001+i`
  (positional DISPLAYn-order fallback if EDID reads fail).

## Not yet verified on hardware (review focus)

- Driver has never been compiled (WDK absent on the dev machine). First-build
  knobs flagged by the author: `DISPLAYCONFIG_VIDEO_SIGNAL_INFO.totalSize`
  uses real CVT-RB blanking totals (MS sample uses activeSize — flip if
  Windows rejects modes); `UmdfExtensions=IddCx0102` INF directive;
  `UMDF_VERSION_MINOR=2.25` must be ≤ installed WDK; registry persistence
  assumes the WUDF host can write `PLUGPLAY_REGKEY_DRIVER`; INF interface
  SDDL grants `GRGW` to interactive users — verify unelevated CreateFile.
- Compositor: XOR cursor pixels approximated; secure desktop/lock screen
  blanks segments until duplication is reacquired; no guard against a config
  that lists the physical monitor as a segment source (feedback loop).
- App: exact fractional refresh (e.g. 164999 mHz) is only detectable when the
  physical monitor is already running at max rate, otherwise `Hz*1000` is
  used; `DM_DISPLAYFREQUENCY` on virtual monitors retries without the flag if
  the mode is rejected.

## Constraints and notes
- Driver build requires WDK matching SDK 10.0.26100 + VS2022 WDK extension.
- Unsigned driver → machine must run in test-signing mode (`bcdedit /set
  testsigning on`) until an EV-signed/attestation-signed release exists.
- IddCx swapchains render on the system's render GPU — vendor-neutral.
- Refresh: virtual monitors advertise the physical max refresh so apps/games
  on them run at full rate; compositor presents at physical max refresh.
- Latency: one extra composition hop (duplication → present), comparable to
  streaming-app local loopback (~1 frame).
