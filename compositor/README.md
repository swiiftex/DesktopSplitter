# compositor/ — `dscomp.exe`

The DesktopSplitter compositor. It captures each virtual monitor with DXGI
Desktop Duplication and composites them onto the physical monitor, so the user
sees only the virtual displays.

C++17, Win32, D3D11 + DXGI 1.5. No external dependencies — the JSON parser is
hand-written and the shaders are compiled at runtime with `D3DCompile`.

## Build

```powershell
# v143 toolset, SDK 10.0.26100
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    compositor\dscomp.vcxproj -p:Configuration=Release -p:Platform=x64
```

Output: `compositor\build\x64\{Release,Debug}\dscomp.exe`.
Configurations: `Debug|x64`, `Release|x64`. Warning level `/W4` (clean),
`UNICODE`/`_UNICODE`, subsystem `Windows`.

### Geometry probe

`tests\GeometryProbe.vcxproj` builds a console test binary from the real
`src\Geometry.h` and asserts the pure math the confiner and rescuer depend on:
edge-slide clamping from all eight approach directions (including corners and
negative-coordinate monitors), the ≥50% overlap trigger, rescue placement with
centring and clamping, and ClipCursor eligibility. Build and run it:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    compositor\tests\GeometryProbe.vcxproj -p:Configuration=Release -p:Platform=x64
compositor\build\x64\Release\GeometryProbe.exe   # exit code 0 = all passed
```

It touches no display state, hooks or windows, so it is safe to run on a machine
in active use. `/W4 /WX`.

## Run

```
dscomp.exe [--verbose] [--config <path>] [--no-confine] [--no-rescue]

  --verbose, -v    Verbose logging (frame/capture rates, duplication events,
                   window rescues).
  --config <path>  Override %ProgramData%\DesktopSplitter\config.json.
  --no-confine     Disable cursor confinement — the covered physical monitor
                   becomes reachable again (useful for debugging, or if the
                   low-level mouse hook is unwanted).
  --no-rescue      Disable the window rescuer — windows that open behind the
                   compositor are left where they are.
  --specialized    Drive the physical monitor directly through
                   Windows.Devices.Display.Core, removing it from the desktop
                   entirely. Requires the monitor to be marked as a specialized
                   display; falls back to the window path with a logged reason
                   otherwise. Also enabled by HKLM\SOFTWARE\DesktopSplitter
                   HidePhysicalDisplay=1 (written by the GUI installer).
                   See docs/HIDING.md.
  --help, -h       Usage.
```

The two features are independent; either can be disabled without affecting the
other or the render path.

`dscomp.exe` is a GUI-subsystem app but attaches to the launching console, so
logs go to stderr (and always to `OutputDebugString`). `2> log.txt` works.

It exits on any of:

- the named event `Global\DeskSplitCompositorStop`
  (`DESKSPLIT_COMPOSITOR_STOP_EVENT` in `shared/DeskSplitProtocol.h`) — opened
  if it exists, otherwise created, and watched on a dedicated thread;
- `WM_CLOSE` on its window (class `DeskSplitCompositorWindow`);
- Ctrl+C / Ctrl+Break / console close when launched from a console.

On exit it releases `ClipCursor(NULL)` and unhooks the low-level mouse hook.

## Config

`%ProgramData%\DesktopSplitter\config.json`, per `docs/ARCHITECTURE.md`:

```json
{
  "physicalDevice": "\\\\.\\DISPLAY1",
  "refreshMillihertz": 164999,
  "segments": [
    { "virtualDevice": "\\\\.\\DISPLAY3", "width": 1280, "height": 1440,
      "physRect": { "x": 0, "y": 0, "w": 1280, "h": 1440 } }
  ]
}
```

`physRect` is in physical-monitor-local pixels. `refreshMillihertz` drives the
present pacing when tearing is supported. `width`/`height` are informational —
the actual capture size comes from the duplication descriptor, so a mode change
on a virtual monitor is picked up automatically.

## Source layout (`src/`)

| File | Responsibility |
| --- | --- |
| `Main.cpp` | Entry point, arg parsing, window, message loop, stop-event watcher, Ctrl+C, startup/shutdown ordering. |
| `Common.h` | Shared Win32 includes, `AppConfig` / `SegmentConfig` / `PhysRect`, UTF-8→UTF-16. |
| `Json.h/.cpp` | Dependency-free recursive-descent JSON parser (objects, arrays, strings with `\uXXXX` + surrogate pairs, numbers, `//` and `/* */` comments). |
| `Config.h/.cpp` | Resolves the config path, reads and validates the schema. |
| `Geometry.h` | Pure, side-effect-free rect/point math: containment, union, overlap fraction, edge sliding, rescue placement. Header-only so `tests/GeometryProbe.cpp` can unit-test it. |
| `MonitorUtil.h/.cpp` | Live display state: `EnumerateMonitors` (device name, rect, work area, primary) and by-name lookups via `EnumDisplayMonitors`/`GetMonitorInfo`. |
| `SegmentShared.h` | `SegmentShared` (capture → render handoff) and `GpuContext` (the shared device + its context mutex). |
| `Renderer.h/.cpp` | D3D11 device, DXGI 1.5 factory, flip-model swapchain, shaders, render thread, frame pacing. |
| `Capture.h/.cpp` | One `IDXGIOutputDuplication` thread per segment; frame publication and cursor shape conversion. |
| `Presenter.h` | `IPresenter`: how a composed frame reaches the display. |
| `PresenterSelect.h` | Pure presenter-selection policy + fallback reasons. Header-only so `tests/PresenterProbe.cpp` can unit-test every failure mode. |
| `WindowPresenter.h/.cpp` | Flip-model DXGI swapchain on the borderless window (the default, unchanged path). |
| `SpecializedPresenter.h/.cpp` | EXPERIMENTAL: `Windows.Devices.Display.Core` acquisition + scanout. See docs/HIDING.md. |
| `CursorConfine.h/.cpp` | Exclusion-based cursor confinement: `WH_MOUSE_LL` edge-slide hook, plus `ClipCursor` only in the rare case where it can express the allowed area exactly. |
| `WindowRescuer.h/.cpp` | `SetWinEventHook` watcher that relocates app windows opening behind the compositor onto the first virtual monitor. Runs on its own thread with its own pump. |

## Design notes

**Window.** Borderless `WS_POPUP` with `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
WS_EX_TOPMOST` (no taskbar entry, never takes focus, `WM_MOUSEACTIVATE` returns
`MA_NOACTIVATE`). The class cursor is `NULL` and `WM_SETCURSOR` calls
`SetCursor(NULL)` — the visible cursor is the one the renderer draws.
`WM_DISPLAYCHANGE` re-resolves the physical monitor rect, repositions the
window, asks the renderer to `ResizeBuffers`, and refreshes the cursor clip.

**One device for capture and render.** All outputs are normally on the same
adapter, so a single `ID3D11Device` serves both the swapchain and every
`DuplicateOutput` call, and captured textures are shared with the renderer
directly — no keyed mutex, no cross-adapter copy. **`ID3D11Multithread::
SetMultithreadProtected(TRUE)` is required** for this: the duplication object
drives the immediate context internally (`AcquireNextFrame`, `ReleaseFrame`,
`GetFramePointerShape`), which an application-level mutex cannot cover. Without
it the display driver faults within seconds.

**Cross-adapter fallback.** If a segment's output turns out to be on a different
adapter than the render device (LUID mismatch), that capture thread creates its
own device on the output's adapter and moves frames through a staging texture:
`CopyResource` → `Map(READ)` on the capture device, `Map(WRITE_DISCARD)` →
`memcpy` into a `DYNAMIC` texture on the render device. Detected automatically
and logged; the single-GPU path never pays for it.

**Threading.** `SegmentShared::mtx` guards the CPU-side handoff (SRV pointer,
dimensions, cursor position/pixels). `GpuContext::mtx` guards multi-call
immediate-context sequences. The two are never nested, so there is no lock
ordering to get wrong. The render thread snapshots all segment state under the
per-segment locks, then issues GPU work, then `Present`s outside the GPU lock so
a blocking present never stalls a capture thread.

**Rendering.** Flip-model swapchain: `DXGI_SWAP_EFFECT_FLIP_DISCARD`, 2 buffers,
`DXGI_FORMAT_B8G8R8A8_UNORM` end to end. When
`DXGI_FEATURE_PRESENT_ALLOW_TEARING` is reported, the swapchain carries
`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` and presents are
`Present(0, DXGI_PRESENT_ALLOW_TEARING)` paced to `refreshMillihertz` with a
high-resolution waitable timer; otherwise `Present(1, 0)` paces itself. Each
segment is one textured quad drawn with a scissor rect clipped to its
`physRect`, sampled with a linear clamp sampler. The quad needs no vertex
buffer or input layout — positions come from `SV_VertexID` and a constant
buffer holding the NDC rect and UV rect, drawn as a 4-vertex triangle strip.

**Cursor.** Position and visibility come from `DXGI_OUTDUPL_FRAME_INFO`
(`LastMouseUpdateTime` / `PointerPosition`); the shape is fetched with
`GetFramePointerShape` only when `PointerShapeBufferSize != 0`.
`_COLOR` is copied as BGRA; `_MONOCHROME` (AND mask over XOR mask, 1bpp) and
`_MASKED_COLOR` are converted to BGRA on the CPU, and only on shape change — a
version counter tells the renderer when to rebuild the immutable cursor
texture. The cursor is drawn as an alpha-blended quad on top of its segment,
scaled by the same `physRect / captureSize` factor.

**Cursor confinement is exclusion-based.** The rule is a single negative one:
*the covered physical monitor's desktop rect is off limits, everything else is
free*. There is deliberately no notion of an "allowed" region to stay inside.

The earlier inclusion-based design — `ClipCursor` to the bounding box of the
virtual monitors, plus a hook that teleported strays to the nearest virtual
point — breaks as soon as a second real monitor exists. A user with an HP
monitor above the virtual row saw the cursor snap back on downward movement:
that monitor lies outside the virtual bounding box, so the clip and the hook
both fought to drag the cursor back into it. A single `ClipCursor` rectangle
simply cannot describe "three virtual monitors in a row plus one real monitor
above one of them, minus the covered one".

So:

- **`ClipCursor` is dropped** unless the union of all *non-covered* monitors is
  exactly one rectangle (`UnionIsSingleRect`, an exact area test because monitor
  rects never overlap). With the physical monitor covered and other real
  monitors free to sit above or below the virtual row, it almost never is. In
  hook-only mode dscomp never calls `ClipCursor` at all, so it cannot fight
  another app's clip, and it only calls `ClipCursor(NULL)` on exit if it was the
  one that set a clip.
- **The `WH_MOUSE_LL` hook is the sole mechanism.** It blocks only non-injected
  `WM_MOUSEMOVE` events whose *destination* lies inside the covered rect.
  Everything else — virtual monitors, the HP, movement between them in any
  direction — passes straight through untouched.
- **Blocked moves slide along the edge.** Entering a rectangle means crossing
  both its slabs, and the wall you actually hit is the one crossed *last*
  (`SlideAlongEdge` compares the parametric crossing times and clamps the axis
  with the larger `t`). Only that axis is clamped to just outside the rect; the
  other keeps the value the movement asked for. Skimming past the covered
  monitor therefore feels like a normal screen edge instead of a snap-back, and
  a diagonal that clips a corner exits through the edge it genuinely reached.
  The hook tracks its own last-allowed point rather than calling
  `GetCursorPos`, so the approach direction is always known.
- **Degenerate cases.** If a slide lands in a gap between non-flush monitors,
  the hook falls back to holding the other axis, then to the approach point. If
  the cursor is somehow already inside the covered rect (startup, or an app
  calling `SetCursorPos`), there is no approach direction to preserve, so it
  exits through the nearest edge — the only teleport left in the design, and
  only as recovery.
- **If the hook cannot be installed**, dscomp logs a warning and runs with no
  confinement at all rather than degrading movement with a clip that does not
  match reality.

The covered rect is resolved fresh from `EnumDisplayMonitors` and re-resolved on
`WM_DISPLAYCHANGE`: the control app makes segment 1's virtual monitor the
Windows *primary* during Apply, which re-bases virtual-desktop coordinates and
can leave the physical monitor at negative coordinates. Nothing is cached across
a display change. `--no-confine` disables all of this.

**Window rescuer.** Windows that open or are restored inside the covered region
are invisible under the compositor, so a watcher moves them out. Three
out-of-context `SetWinEventHook`s — `EVENT_OBJECT_SHOW`,
`EVENT_SYSTEM_MOVESIZEEND` and `EVENT_OBJECT_LOCATIONCHANGE` — feed a filter
that requires the window to be top-level (`GA_ROOT`), visible, not `WS_CHILD`,
not `WS_EX_TOOLWINDOW`, not `WS_EX_NOACTIVATE`, not DWM-cloaked
(`DWMWA_CLOAKED`), at least 100×50, and **≥50% inside the covered rect** by
area. Shell and flyout classes (`Progman`, `WorkerW`, `Shell_TrayWnd`, menus,
tooltips, XAML popups, …) and flyout/logon host processes (`winlogon.exe`,
`LogonUI.exe`, `ShellExperienceHost.exe`, `StartMenuExperienceHost.exe`,
`SearchHost.exe`, `TextInputHost.exe`) are excluded outright, as is our own
process (`WINEVENT_SKIPOWNPROCESS` plus an explicit PID check).

Survivors are moved into the **work area of segment 1's virtual monitor**
(`config.segments[0].virtualDevice`, resolved fresh at rescue time), keeping
their size when it fits and otherwise shrinking to the work area, then centred
and clamped. Maximized windows cannot be moved, so they are `SW_RESTORE`d,
positioned, and `SW_MAXIMIZE`d again — which maximizes them onto whichever
monitor they now sit on. `EVENT_OBJECT_LOCATIONCHANGE` is debounced with a
300 ms coalescing timer (and filtered to `OBJID_WINDOW`/`CHILDID_SELF`, without
which it would fire for every caret and cursor movement), and each `HWND` is
rescued at most once per 5 s so dscomp never ping-pongs with an app that
repositions itself. Rescues are logged under `--verbose`. `--no-rescue`
disables it.

The rescuer runs on **its own thread with its own message pump**, not the UI
thread. Relocating another process's window is a cross-process `SetWindowPos`
that can block on a hung app, and the UI thread also services the `WH_MOUSE_LL`
confinement hook — stalling it would stall system-wide mouse input.

## Known runtime risks

- **Recursive capture.** If the compositor's own window ever ends up on a
  monitor that is being duplicated, the output feeds back. The design relies on
  the physical monitor being distinct from every virtual monitor; a config that
  lists the physical device as a segment source will hall-of-mirrors.
- **XOR cursors are approximated.** `_MONOCHROME` invert pixels render as white
  and `_MASKED_COLOR` XOR pixels render as the inverted source colour, since
  true XOR would require reading back the composed frame. Affects only I-beam
  and a few legacy cursors over certain backgrounds.
- **Secure desktop / session switch.** `DuplicateOutput` and `AcquireNextFrame`
  fail with `E_ACCESSDENIED` / `DXGI_ERROR_ACCESS_LOST` across UAC prompts,
  lock screen and fast user switching. The capture threads tear down and retry
  every 250 ms, so the affected segment goes black until the desktop returns.
- **`Global\` event namespace.** Creating `Global\DeskSplitCompositorStop`
  needs `SeCreateGlobalPrivilege`. A non-elevated compositor can still *open*
  the event if the control app created it first; if neither can, dscomp logs a
  warning and remains stoppable only via `WM_CLOSE` / Ctrl+C.
- **Duplication limits.** Each output supports a limited number of concurrent
  duplications. If another capture tool holds one, `DuplicateOutput` returns
  `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` and that segment retries indefinitely.
- **Topmost is not absolute.** Another topmost window (or an exclusive-fullscreen
  app) can cover the compositor. The 2 s timer re-asserts `HWND_TOPMOST` but
  cannot win against every case.
- **Latency.** One composition hop: duplication → present, roughly one frame,
  as noted in `docs/ARCHITECTURE.md`.
- **Low-level hooks are cooperative.** A `WH_MOUSE_LL` procedure that overruns
  `LowLevelHooksTimeout` is skipped by Windows for that event, so a momentarily
  busy UI thread shows up as the cursor briefly slipping onto the covered
  monitor. The hook itself does no I/O and no allocation on the hot path for
  exactly this reason. It also cannot see input from an elevated process's
  window while dscomp runs unelevated (UIPI).
- **The rescuer can surprise a well-behaved app.** Anything that deliberately
  positions a window on the physical monitor gets moved once and then left
  alone for 5 s. Apps that re-assert their position every frame will visibly
  fight for that window; `--no-rescue` is the escape hatch. Restoring a
  maximized window to move it also activates it, which can steal focus.
- **Rescue heuristics are not exhaustive.** The class/process exclusion lists
  cover the shell surfaces seen so far; an unusual splash screen, overlay or
  launcher may still be relocated, and a genuinely borderless-fullscreen app on
  the covered monitor will be treated as a window to rescue.
- **Not yet exercised against the real driver.** The render path was validated
  live; the confinement and rescuer changes are verified by build plus the
  geometry probe only, and still need a live pass with the HP monitor attached.
