# DesktopSplitter

Split a single physical monitor into **2, 3, or 4 real virtual monitors** on
Windows. Unlike window managers (FancyZones etc.), the segments are genuine
monitors as far as Windows and every application is concerned — each has its
own resolution, taskbar, fullscreen support, and DPI, and each runs at the
physical monitor's maximum refresh rate.

Works on any GPU vendor (AMD, NVIDIA, Intel): it is built on **IddCx**,
Microsoft's Indirect Display Driver framework, with no vendor-specific APIs.

## How it works

1. **Virtual display driver** (`driver/`) — an IddCx UMDF driver creates up to
   4 virtual monitors with the resolutions/refresh rate you configure.
2. **Compositor** (`compositor/dscomp.exe`) — captures each virtual monitor
   via DXGI Desktop Duplication and renders them into their segments on the
   physical monitor, fullscreen, at max refresh.
3. **Control app** (`app/DesktopSplitter.exe`) — pick a monitor, pick a
   layout (2/3/4-way), set per-segment resolutions, hit Apply.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details and
[docs/BUILDING.md](docs/BUILDING.md) for build/install instructions.

## Requirements

- Windows 10 1903+ (Windows 11 recommended)
- Building the driver requires the Windows Driver Kit (see BUILDING.md)
- No test-signing mode or Secure Boot changes needed: the driver is
  user-mode (UMDF). Dev installs trust a local certificate
  (`installer\sign-dev.ps1`); distribution uses Microsoft attestation
  signing (see BUILDING.md)

## Layout support

| Split | Layouts |
|-------|---------|
| 2-way | side-by-side, stacked |
| 3-way | 3 columns, 1 large + 2 stacked |
| 4-way | 2×2 grid, 4 columns |

## Status

Early development. Core pipeline (driver → duplication → compositor) is
implemented; hardware validation pending.
