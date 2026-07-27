# DesktopSplitter

Split a single physical monitor into **2, 3, or 4 real virtual monitors** on
Windows. Unlike window managers (FancyZones etc.), the segments are genuine
monitors as far as Windows and every application is concerned — each has its
own resolution, taskbar behavior, fullscreen support, and DPI, and each runs
at the physical monitor's maximum refresh rate (verified at 240 Hz end to
end).

Works on any GPU vendor (AMD, NVIDIA, Intel): it is built on **IddCx**,
Microsoft's Indirect Display Driver framework, with no vendor-specific APIs.

## Installing

Run **`DesktopSplitterSetup.exe`** — a single-file, dependency-free installer
(no .NET required on the target machine):

- Traditional wizard: install location, desktop / Start-menu shortcuts,
  optional experimental display hiding (see below).
- Installs the virtual display driver, creates the virtual display device,
  and registers a normal Add/Remove Programs entry with GUI uninstall.
- Re-running it over an existing install performs an in-place upgrade and
  removes superseded driver packages.
- Uninstall safely reverts any active split, restores any hidden monitor,
  and removes the device and driver.
- Command line: `/verify` (inspect contents and install state, no admin, no
  changes), `/quiet`, `/uninstall [/quiet]`.

Driver signing: the driver is **user-mode (UMDF)** — no test-signing mode and
no Secure Boot changes are ever needed. On PCs that don't trust the signing
certificate yet, the installer shows the publisher and thumbprint and asks
for explicit consent before trusting it. (A Microsoft attestation-signed
release would skip that prompt entirely — see
[docs/BUILDING.md](docs/BUILDING.md).)

## Using it

1. Pick the monitor to split. Every entry shows its name, device, current
   mode, and maximum refresh rate.
2. Pick a layout and shape it in the **visual editor**:
   - Drag the borders between segments — neighbors auto-fill so the layout
     always covers the monitor exactly, with live size labels.
   - Or type exact per-segment resolutions — manual entry never fights you
     (no auto-fit while typing); if sizes don't sum to the monitor, segments
     are scaled into place and the editor says so.
   - Zones show aspect-ratio badges (16:9, 21:9, 32:9, 16:10, 4:3) when
     within 1% of a clean ratio. Reset restores the layout's defaults.
3. **Apply.** The virtual monitors appear, are arranged contiguously in the
   Windows display layout, and the split renders on the physical monitor at
   its max refresh rate. **Segment 1 becomes the Windows primary display**,
   so the Start menu, Alt-Tab, and new windows land inside the split.
   **Revert** restores everything, including whichever monitor was really
   primary before.

### Layouts

| Split | Layouts |
|-------|---------|
| 2-way | side-by-side, stacked |
| 3-way | 3 columns, 1 large + 2 stacked, **16:9 Center** |
| 4-way | 2×2 grid, 4 columns |

**16:9 Center** is built for ultrawide gaming: a true 16:9 center segment
sized to the monitor's height (e.g. 2560×1440 on a 5120×1440 panel) with the
wings filling the sides. The center is segment 1, so games and the shell
default to it.

### Quality-of-life built in

- **Exact segment identity**: virtual monitors are matched to segments by a
  serial number embedded in each virtual monitor's EDID — never by guesswork
  — so segments cannot swap.
- **Cursor behavior**: only the covered physical monitor's area is off
  limits (it feels like a normal screen edge); all other monitors, real or
  virtual, are unrestricted.
- **Window rescuer**: any app window that opens into the covered area is
  automatically moved into segment 1. (`--no-rescue` / `--no-confine` flags
  on the compositor disable these.)
- **Multi-monitor friendly**: additional real monitors keep working
  normally alongside the split.
- Tray icon with Apply-last / Revert; settings persist across restarts;
  the driver's monitor configuration survives reboots.

## Hiding the physical monitor (experimental)

By default the physical monitor technically remains a (covered) Windows
display, which leaves a few shell quirks: Win+Tab spans it, Ctrl+Alt+Del
renders invisibly on it, and it stays in display lists. The **Display
hiding** panel in the app can remove it from the desktop entirely using
Windows' *specialized display* mechanism, which requires:

- Windows **Pro for Workstations, Enterprise, or IoT Enterprise**
- An EDID addition the app manages for you (existing CRU overrides are
  preserved and backed up)

The whole flow is GUI-driven with a safety net modeled on display-settings
dialogs: after one UAC prompt, changes apply, the monitor restarts, and a
countdown dialog appears **on another monitor** — keep or revert, with
**automatic revert after 10 seconds** if you do nothing (enforced by the
elevated helper itself, so it works even if the app dies). A **Test run**
button rehearses everything without changing anything. When hiding is
active, the compositor presents directly to the acquired display and the
shell quirks above disappear. Full procedure and recovery documentation:
[docs/HIDING.md](docs/HIDING.md).

Status: the transaction machinery is fully tested; the final
acquisition/scanout step is newly implemented and still being validated on
real hardware.

## How it works

1. **Virtual display driver** (`driver/`) — IddCx UMDF driver creating up to
   4 virtual monitors with the exact modes you configure (fractional
   refresh preserved, e.g. 164.999 Hz), reconfigurable at runtime via IOCTL.
2. **Compositor** (`compositor/dscomp.exe`) — captures each virtual monitor
   via DXGI Desktop Duplication and composites them onto the physical
   monitor with a tearing-capable flip-model swapchain at max refresh
   (measured 240 fps end-to-end), or scans out directly in hidden mode.
3. **Control app** (`app/DesktopSplitter.exe`) — WPF control center: monitor
   picker, layout editor, apply/revert orchestration, display hiding panel,
   tray icon.
4. **Installer** (`installer/`) — the GUI setup wizard plus `devcreate`
   (device node tool) and `edidoverride` (EDID override helper with the
   guarded keep/revert transaction).

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design and
[docs/BUILDING.md](docs/BUILDING.md) for building from source (driver needs
the WDK; everything else builds with VS2022+ and the .NET SDK).

## Requirements

- Windows 10 1903+ (Windows 11 recommended); x64
- Any GPU (AMD, NVIDIA, Intel)
- Display hiding additionally requires Pro for Workstations / Enterprise
- No test-signing mode, no Secure Boot changes

## Known limitations

- Without display hiding: Win+Tab task view spans the covered monitor, and
  the Ctrl+Alt+Del screen renders on displays the covered monitor cannot
  show (it still works — press Esc — but is invisible on that panel).
- Lock screen / UAC secure desktop briefly blanks the split (Desktop
  Duplication cannot capture the secure desktop); it recovers automatically.
- XOR-style cursors are approximated.
- One extra composition hop adds roughly a frame of latency on the split
  monitor (windowed mode).

## Status

Working on real hardware (AMD, 5120×1440 @ 240 Hz + secondary monitor):
driver, compositor, control app, installer, upgrades, and uninstall all
exercised live. Display hiding is the one experimental area. Feedback on
other GPUs, resolutions, and multi-monitor topologies is welcome.
