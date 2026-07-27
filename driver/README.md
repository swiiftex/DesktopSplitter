# DesktopSplitterVdd — IddCx virtual display driver

A UMDF 2 indirect display driver (IddCx ≥ 1.4) that publishes 0–4 virtual
monitors on a root-enumerated software device, under runtime control of the
DesktopSplitter control app. GPU-vendor-neutral: only IddCx, D3D11 and DXGI are
used, no vendor driver APIs.

Modelled on Microsoft's
[IddSampleDriver](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay)
(MIT).

## Files

| File | Purpose |
| --- | --- |
| `Driver.h` / `Driver.cpp` | `DriverEntry`, `EvtWdfDriverDeviceAdd`, the `IDD_CX_CLIENT_CONFIG` callbacks, dynamic monitor management, control IOCTLs, the D3D11 device and the swap-chain drain thread. |
| `Timing.h` / `Timing.cpp` | CVT reduced-blanking (CVT-RB v1) timing generation, fractional-refresh rational conversion, and `DESKSPLIT_CONFIG` validation. |
| `Edid.h` / `Edid.cpp` | Synthesis of a 128-byte EDID 1.4 block per monitor. |
| `Guids.cpp` | The one translation unit that defines `INITGUID`, giving storage to `GUID_DEVINTERFACE_DESKSPLIT`. |
| `Trace.h` | Debug-output logging (WPP deliberately not used). |
| `DesktopSplitterVdd.inf` | Modern minimal UMDF INF: `Class=Display`, hardware ID `Root\DesktopSplitterVdd`, `WUDFRD` service, `IddCx0102` UMDF extension, `IndirectKmd` upper filter. |
| `DesktopSplitterVdd.vcxproj` | WDK UMDF 2 driver project (`WindowsUserModeDriver10.0`, `DynamicLibrary`, UMDF 2.25, IddCx 1.4). |

## Structure

```
DriverEntry
  └─ WdfDriverCreate(DeskSplitDeviceAdd)

DeskSplitDeviceAdd(WDFDEVICE_INIT)
  ├─ WdfDeviceInitSetPnpPowerEventCallbacks (EvtDeviceD0Entry)
  ├─ IddCxDeviceInitConfig(IDD_CX_CLIENT_CONFIG)
  ├─ WdfDeviceCreate  →  IndirectDeviceContext
  ├─ IddCxDeviceInitialize
  └─ WdfDeviceCreateDeviceInterface(GUID_DEVINTERFACE_DESKSPLIT)

EvtDeviceD0Entry
  └─ IndirectDeviceContext::InitAdapter → IddCxAdapterInitAsync   (once)

EvtIddCxAdapterInitFinished
  └─ replay persisted config → IddCxMonitorCreate + IddCxMonitorArrival  (per monitor)

EvtIddCxDeviceIoControl
  ├─ IOCTL_DESKSPLIT_SET_CONFIG  → validate → diff → depart/arrive → persist
  └─ IOCTL_DESKSPLIT_GET_STATUS  → protocol version + active monitor count

EvtIddCxMonitorAssignSwapChain
  └─ Direct3DDevice(RenderAdapterLuid) + SwapChainProcessor thread
       └─ IddCxSwapChainSetDevice
          loop: IddCxSwapChainReleaseAndAcquireBuffer / wait on the acquire
                event / release surface / IddCxSwapChainFinishedProcessingFrame
```

## Design notes

### Control channel

IddCx redirects `IRP_MJ_DEVICE_CONTROL` to an internal queue, so a driver-owned
WDF default queue never sees custom IOCTLs. `IDD_CX_CLIENT_CONFIG::EvtIddCxDeviceIoControl`
is therefore the control path (this is what the IddCx sample's comment
prescribes). A WDF default queue that funnels into the *same* handler is
available behind `DESKSPLIT_USE_WDF_DEFAULT_QUEUE` (off by default) in case a
future IddCx version stops redirecting.

The device interface `GUID_DEVINTERFACE_DESKSPLIT` (from
`shared/DeskSplitProtocol.h`) is registered so the control app can find and
open the device. The INF grants SYSTEM/Administrators full access and
interactive users read/write.

### Dynamic monitors

`ApplyConfig` diffs the incoming `DESKSPLIT_CONFIG` against the live slot array:

* a slot that is active but not wanted, or whose `DESKSPLIT_MONITOR_CONFIG`
  changed byte-for-byte, is departed with `IddCxMonitorDeparture` (IddCx
  destroys the `IDDCX_MONITOR` object; the context cleanup callback stops the
  swap-chain thread);
* a slot that is wanted but not active is created with `IddCxMonitorCreate` and
  announced with `IddCxMonitorArrival`.

A changed mode list forces a re-plug because the EDID changes with it.

`MonitorCount == 0` removes every virtual monitor.

### Persistence

The last accepted `DESKSPLIT_CONFIG` blob is written as `REG_BINARY` under the
driver software key (`WdfDeviceOpenRegistryKey(PLUGPLAY_REGKEY_DRIVER)`, value
name `Config`) and replayed in `EvtIddCxAdapterInitFinished`, so monitors
survive reboot. Persistence is best-effort: a failure to write is logged but
does not stop monitors from being created. If a `SET_CONFIG` arrives before the
adapter finished initialising, it is queued and applied from the init-finished
callback.

### Modes and fractional refresh

Each monitor advertises exactly its configured mode list, preferred mode
flagged, from two callbacks:

* `EvtIddCxParseMonitorDescription` → `IDDCX_MONITOR_MODE` with
  `IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR`, plus `PreferredMonitorModeIdx`;
* `EvtIddCxMonitorQueryTargetModes` → the same list as `IDDCX_TARGET_MODE`.

Windows shows the intersection, so the monitor ends up with exactly the
configured modes.

`DeskSplitComputeTiming` (Timing.cpp) is the single source of truth for the
`DISPLAYCONFIG_VIDEO_SIGNAL_INFO` fields and for the EDID detailed timing
descriptor. It uses CVT-RB v1 blanking (160 px horizontal blanking split
48/32/80, vertical front porch 3, aspect-derived sync width, back porch 6, a
460 µs minimum vertical blanking interval) and derives

```
pixelRate = hTotal * vTotal * refreshMillihertz / 1000
vSyncFreq = refreshMillihertz / 1000, reduced by GCD   (164999 mHz → 164999/1000)
hSyncFreq = vSyncFreq * vTotal,       reduced by GCD
```

The active size is *not* rounded to the CVT 8-pixel cell — the advertised
resolution must be exactly what was configured; only the blanking comes from
CVT.

### EDID

128 bytes, EDID 1.4, checksum correct:

* manufacturer ID `DSP` (0x12 0x70), product code = monitor index, serial
  `0xD5000001 + index` (unique per index so Windows persists per-monitor
  settings);
* digital input, 8 bpc, sRGB chromaticity, gamma 2.2, physical size derived
  from the preferred mode at 96 DPI;
* descriptor 1: detailed timing for the preferred mode (or the first mode that
  fits — EDID DTD fields are 12 bits, so a mode wider than 4095 px cannot be
  expressed; a generic 1920×1080@60 DTD is used if no mode fits);
* descriptor 2: display range limits derived from the mode list;
* descriptors 3/4: monitor name (`DeskSplit N`) and serial text;
* no established/standard timings — the real mode list comes from the
  callbacks, and legacy timings would only add unwanted modes.

Because `EvtIddCxParseMonitorDescription` receives only the description bytes
(no monitor handle) and the OS may call it re-entrantly from inside
`IddCxMonitorArrival`, live EDIDs and their mode lists are published to a small
table guarded by its own SRW lock that is never held across an IddCx call. The
device context lock is never taken on that path, which is what keeps arrival
from deadlocking.

### Swap-chain processing

One `SwapChainProcessor` thread per assigned swap-chain, running at MMCSS
"Distribution" priority. It calls `IddCxSwapChainSetDevice` with a D3D11 device
created on the render adapter LUID the OS supplied (falling back to the default
DXGI adapter if that LUID cannot be resolved), then loops
`IddCxSwapChainReleaseAndAcquireBuffer`, waiting on the acquire event when the
call returns `E_PENDING`. Frames are released immediately —
**no encoding and no copy-out** — because what the user sees is captured from
the virtual monitors by the compositor through DXGI desktop duplication.

## Building

Requires the WDK plus the matching Visual Studio driver workload. Build
`DesktopSplitterVdd.vcxproj` for `x64` (an `ARM64` configuration is included).
The output package (`.inf`, `.dll`, `.cat`) lands under
`driver\<Platform>\<Configuration>\DesktopSplitterVdd\`.

**Use the 64-bit MSBuild.** The WDK's INF verification MSBuild task probes for
`x86\InfVerif.dll`, which the WDK does not ship (only `x64` and `arm64` exist),
so a 32-bit MSBuild fails the `InfVerif` target with
`Unable to load DLL 'x86\InfVerif.dll'`. Build with `Bin\amd64\MSBuild.exe`:

```
& "…\MSBuild\Current\Bin\amd64\MSBuild.exe" driver\DesktopSplitterVdd.vcxproj `
    /p:Configuration=Release /p:Platform=x64
```

Verified against WDK/SDK 10.0.28000 with UMDF 2.25, the IddCx **1.4** headers
(`Include\<ver>\um\iddcx\1.4`) and `IddCxStub.lib` from
`Lib\<ver>\um\x64\iddcx\1.4`. Release and Debug x64 both build with **zero
warnings** at `/W4` with warnings-as-errors, PREfast code analysis on, and
Spectre mitigation enabled; InfVerif and the Inf2Cat signability test both pass
with no errors.

Compile-time knobs:

| Macro | Default | Effect |
| --- | --- | --- |
| `IDDCX_VERSION_MAJOR/MINOR` | 1 / 4 | IddCx API surface |
| `IDDCX_MINIMUM_VERSION_REQUIRED` | 4 | driver refuses to load below IddCx 1.4 |
| `DESKSPLIT_USE_WDF_DEFAULT_QUEUE` | unset | also create a WDF default queue for the control IOCTLs |
| `DESKSPLIT_TRACE_ENABLED` | 1 | `OutputDebugString` logging |

## Installing

See `installer/install-driver.ps1`. The driver is unsigned unless
`installer/sign-dev.ps1` has been run, and a self-signed driver only loads with
`bcdedit /set testsigning on` (Secure Boot off) until an attestation- or
EV-signed release exists.
