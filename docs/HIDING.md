# Truly hiding the physical monitor

By default DesktopSplitter *covers* the physical monitor with a borderless
topmost window. The monitor is still part of the Windows desktop: windows can
open behind the compositor, the cursor can wander onto it, and Windows still
counts it as a display. `dscomp` works around all of that (cursor confinement,
window rescuer, topmost re-assertion), but they are workarounds.

**Specialized display mode removes the monitor from the desktop entirely.**
Windows stops treating it as a desktop display; `dscomp` acquires it directly
through `Windows.Devices.Display.Core` and scans out the composite itself. No
confinement and no window rescuing are needed, because there is no desktop
there to wander onto.

This document is the procedure, and an honest account of what is verified and
what is not.

---

## Status: what is verified vs experimental

| Piece | Status |
| --- | --- |
| OS edition gate | **Verified cleared.** `ProfessionalWorkstation`, build 26200.8875 (25H2). `DisplayManager::Create` succeeds and enumerates 36 targets. |
| Target identification | **Verified.** The Philips 49M2C8900 is found as a `DisplayTarget`, correlated to `\\.\DISPLAY1`. |
| The blocker | **Verified present.** `DisplayMonitorUsageKind` is `Standard` for both attached monitors, and neither EDID contains a Microsoft VSDB. |
| EDID patch logic | **Verified against the real monitor's actual bytes** (in memory, nothing written). See "EDID override" below. |
| EDID override *applied* | **NOT DONE.** Nobody has written the override yet. This is the next live step. |
| Windows accepting the override and flipping `UsageKind` to `SpecialPurpose` | **UNVERIFIED — this is the real unknown.** |
| `dscomp --specialized` acquisition | **UNVERIFIED.** Cannot run until `UsageKind` changes; today it correctly falls back with a logged reason. |
| `dscomp --specialized` presentation loop | **UNVERIFIED AND UNREACHABLE so far.** It compiles and is structured, but no frame has ever been scanned out through it. |
| Guarded transaction (arm / keep / revert / timeout) | **Verified in `--dry-run`**: all three outcomes produce exit 0 / 10 / 11 and the log is written. The registry write and the devnode restart inside it are **unverified** - nobody has restarted a real monitor yet. |
| Fallback to the window path | **Verified.** 14 unit tests over all 64 probe-outcome combinations. |

Treat everything from "EDID override applied" downward as experimental. The
window path is unchanged and remains the default.

---

## Why the EDID has to change

Windows decides whether a display is "specialized" from its EDID. It looks for a
**Microsoft Vendor-Specific Data Block** in a CTA-861 extension: IEEE
registration bytes `5C 12 CA`, version `0x03`, with the **desktop-usage bit
clear**. If it is there, the monitor is marked non-desktop
(`DisplayMonitorUsageKind::SpecialPurpose`) and only a `DisplayManager` client
can drive it.

The Philips 49M2C8900's EDID does not have one — confirmed by probe. So there is
nothing to toggle in Settings; the display never becomes eligible. The only way
in is to override the EDID that Windows reads.

> There is no Settings UI to make an ordinary monitor specialized. "Advanced
> display" only ever *shows* what the EDID already declares. Do not go looking
> for a toggle; there isn't one until the EDID changes.

---

## Procedure — the guided (recommended) path

**Use the GUI.** The control app drives
`edidoverride guarded-apply`, which is a *transaction*: it applies the override,
restarts the monitor, then gives you a countdown to confirm. If you do not
confirm — because the display went dark, or the app crashed, or you changed your
mind — **it reverts itself automatically**. One elevation prompt, and the
safety decision does not depend on the GUI staying alive.

1. Connect a second monitor if you have one. Not required (that is the whole
   point of the auto-revert) but it makes troubleshooting far easier.
2. In the app, tick **Hide physical display** and press Apply.
3. Approve the single UAC prompt.
4. The screen will blink as the monitor is re-enumerated. A countdown dialog
   appears: **press Keep** if you can see it.
5. Do nothing and it reverts on its own within ~10 seconds.

The countdown you see is mirrored from the tool; the tool's own timer is the
authoritative one. Everything is logged to
`%ProgramData%\DesktopSplitter\edid-guard.log` — read that first if anything
looked wrong, because a display glitch may well have hidden the GUI.

To undo later, use the app's **Restore physical display**, or run
`edidoverride guarded-revert "\\.\DISPLAY1" --yes` from an elevated console.

### Exit codes the GUI acts on

| Code | Meaning |
| --- | --- |
| 0 | kept — `HidePhysicalDisplay=1` |
| 2 | not elevated |
| 3 | apply failed, reverted |
| 4 | monitor did not come back after the restart, reverted |
| 5 | bad arguments |
| 10 | user pressed Revert |
| 11 | countdown expired, auto-reverted |

---

## Procedure — manual CLI path (recovery and debugging)

The steps below do the same thing by hand. They remain supported and are what
you use when the GUI is not available — but prefer the guided path above,
because these steps have **no automatic revert**.

Do this with a **second monitor connected and working**, so you can still see
something if the target display goes dark.

### 0. Read the recovery section first

[`installer/edid-override/README.md`](../installer/edid-override/README.md) —
the section fenced with ⚠️. Copy `edidoverride.exe` somewhere reachable without
the target display, and write down this path:

```
HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<id>\<inst>\Device Parameters\EDID_OVERRIDE
```

Deleting that key (possible from Safe Mode) always undoes everything here.

### 1. Check the starting state

```powershell
compositor\build\x64\Release\DisplayProbe.exe --connected-only
installer\edid-override\build\x64\Release\edidoverride.exe list
```

You want to see your target monitor with `UsageKind: Standard` and
`Microsoft VSDB: absent`. Note its `\\.\DISPLAYn` name — it must match
`physicalDevice` in `%ProgramData%\DesktopSplitter\config.json`.

### 2. Dry-run the patch

```powershell
edidoverride.exe selftest "\\.\DISPLAY1"
edidoverride.exe apply    "\\.\DISPLAY1"     # dry run: no --yes
```

`selftest` must end with `ALL CHECKS PASSED`. `apply` without `--yes` only
prints what it *would* write — confirm the strategy, the size change, and
whether it reports an existing override that it will back up.

### 3. Enable the feature flag

Either tick the box in the GUI installer, or set it by hand:

```powershell
New-Item -Path HKLM:\SOFTWARE\DesktopSplitter -Force | Out-Null
New-ItemProperty -Path HKLM:\SOFTWARE\DesktopSplitter -Name HidePhysicalDisplay `
                 -PropertyType DWord -Value 1 -Force
```

This makes `dscomp` try the specialized path automatically. `--specialized` on
the command line does the same thing for a one-off run.

### 4. Apply the override

From an **elevated** console:

```powershell
edidoverride.exe apply "\\.\DISPLAY1" --yes
```

### 5. Re-enumerate the monitor

The monitor driver only reads the override when the device initialises:

- unplug and replug the monitor (usually enough), **or**
- Device Manager → Monitors → right-click the monitor → *Disable*, then
  *Enable*, **or**
- reboot.

### 6. Confirm Windows took it

```powershell
DisplayProbe.exe --connected-only
```

**Success looks like `UsageKind: SpecialPurpose (SPECIALIZED)`** and
`Microsoft VSDB: FOUND`. The monitor should also vanish from Settings →
Display as a desktop monitor.

If `UsageKind` is still `Standard`, see Troubleshooting.

### 7. Apply the split and start the compositor

Use the control app as normal. Then check the log:

```
[info ] presenter: SPECIALIZED - specialized display acquired
[info ] specialized presentation active: skipping cursor confinement and the
        window rescuer (the display is out of the desktop)
```

If instead you see `presenter: falling back to the window path - …`, the reason
is printed on the same line and everything still works exactly as before.

---

## Troubleshooting

**`UsageKind` is still `Standard` after re-enumerating.**
The override did not take, or Windows rejected it.
- `edidoverride.exe verify "\\.\DISPLAY1"` — is the override present, are the
  checksums OK, is the VSDB found with desktop-usage clear?
- `edidoverride.exe dump "\\.\DISPLAY1" --hex` — does the *current* EDID now
  equal the override? If not, the monitor was not really re-enumerated: reboot.
- Some GPU drivers cache EDIDs. A full reboot is more reliable than replugging.

**The display went dark after applying.**
Go to the recovery section in
[`installer/edid-override/README.md`](../installer/edid-override/README.md).
Short version: `edidoverride.exe revert <target> --yes` from another monitor, or
delete the `EDID_OVERRIDE` key from Safe Mode, then reboot.

**`dscomp` logs `TryAcquireTarget did not succeed` / `TargetAccessDenied`.**
The monitor is marked specialized but something else already owns it. Make sure
no other DisplayManager client is running, and that the target is not still
attached to the desktop.

**The monitor disappeared and I want it back as a normal display.**
```powershell
edidoverride.exe revert "\\.\DISPLAY1" --yes
```
then replug or reboot. Also set `HidePhysicalDisplay=0` (or delete it) so
`dscomp` stops trying the specialized path.

**Everything works but I want the old behaviour for a moment.**
Run `dscomp.exe --no-... ` without `--specialized` and with
`HidePhysicalDisplay=0`, or just start it with the window path — the two are
independent and the window path is untouched by any of this.

---

## What changes inside `dscomp`

Presentation sits behind an `IPresenter` interface:

- **`WindowPresenter`** — the original flip-model DXGI swapchain on the
  borderless topmost window. Unchanged behaviour, still the default.
- **`SpecializedPresenter`** — `DisplayManager` → match the target to
  `physicalDevice` → check `UsageKind` → `TryAcquireTargetsAndCreateEmptyState`
  → `ConnectTarget` → pick the highest-refresh mode → `TryApply` →
  `CreateDisplayDevice` / `CreateTaskPool` / `CreateScanoutSource` → two
  `DisplaySurface` primaries shared into the D3D11 device via
  `IDisplayDeviceInterop::CreateSharedHandle` + `OpenSharedResource1`, presented
  with `CreateSimpleScanout` + `DisplayTaskPool::ExecuteTask`.

The renderer draws **exactly the same composite** either way; only `BeginFrame`
(which render target) and `EndFrame` (how it is presented) differ.

Selection is a pure function (`PresenterSelect.h`) fed by probe results, so the
fallback ladder is unit-tested against mocked failures rather than needing a
display. Any failure at any step logs a specific reason and falls back to the
window path — the compositor never fails to start because specialized mode was
unavailable.

In specialized mode, cursor confinement and the window rescuer are **not
installed**: with the monitor out of the desktop there is nowhere for the cursor
to stray and nothing can open behind the compositor.

---

## Tools reference

| Tool | Purpose | Safe to run? |
| --- | --- | --- |
| `compositor\tests\DisplayProbe` | Enumerate targets/monitors, `UsageKind`, EDID + VSDB summary | Yes, read-only. `--try-acquire` is opt-in and skips non-`Standard` monitors |
| `compositor\tests\PresenterProbe` | Unit tests for presenter selection and fallback | Yes, no display access at all |
| `compositor\tests\GeometryProbe` | Unit tests for confinement/rescuer geometry | Yes |
| `installer\edid-override\edidoverride` | Inspect / patch / revert the EDID override | `list`/`dump`/`verify`/`selftest` yes; `apply`/`revert` need `--yes` + elevation |
| `edidoverride guarded-apply --dry-run` | Walk the whole guarded transaction, firing the real events, writing nothing | Yes, and needs no elevation |
