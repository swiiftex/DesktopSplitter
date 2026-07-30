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

> # ⛔ THE EDID-OVERRIDE APPROACH IS DEAD — AND UNNECESSARY
>
> The override was applied for real on 2026-07-28. It wrote cleanly, the devnode
> restarted healthy, the monitor kept working — and the display did **not**
> become specialized. Investigation found the reason, and it is not a bug we can
> fix. Microsoft states it directly:
>
> > "Displays may **not** be designated as HMDs or specialized displays by
> > overriding the EDID in software."
> > — [Specialized monitors: custom compositor](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/specialized-monitors-compositor)
>
> **The good news: the supported route is available on this exact machine right
> now, and needs no EDID work at all.** A read-only probe of
> `DisplayConfigGetDeviceInfo(GET_MONITOR_SPECIALIZATION)` returns `0x00000006`
> for the Philips: *available for this monitor* **YES**, *available for this
> system* **YES**.
>
> **Use `Settings > System > Display > Advanced display > Remove display from
> desktop`.** `installer/edid-override` is retained only as a recovery tool for
> machines where it was already applied. Do not use it to try to hide a display.
>
> Full evidence in "Field failure analysis" at the end of this document.

## Status

| Piece | Status |
| --- | --- |
| OS edition gate | **Verified cleared.** `ProfessionalWorkstation`, build 26200.8875 (25H2). |
| Specialization available for this monitor | **VERIFIED YES.** `GET_MONITOR_SPECIALIZATION` returns `0x6` for both attached monitors: available for monitor *and* for system. |
| EDID-override route | **DEAD — confirmed unsupported by Microsoft.** Applied for real; display did not become specialized. Do not pursue. |
| `installer/edid-override` | **Deprecated for hiding.** Retained as a recovery tool only. Its EDID patching, guarded transaction and auto-revert are all correct and tested — they were simply aimed at the wrong mechanism. |
| Settings toggle / `SET_MONITOR_SPECIALIZATION` | **Not yet implemented.** This is the replacement work item. |
| `dscomp --specialized` acquisition | **Unverified**, but no longer blocked on `UsageKind` (see below). |
| `dscomp --specialized` presentation loop | **Unverified and never exercised.** Compiles; no frame has been scanned out. |
| Fallback to the window path | **Verified.** 12 unit tests across all 64 probe-outcome combinations. |

The window path is unchanged and remains the default.

---

## Why the EDID route fails (corrected)

An earlier version of this document said the EDID was the only way in and that
no Settings toggle existed. **Both claims were wrong** — they are the exact
inverse of the truth, and they cost a night of field debugging. The record:

- Windows *does* read a Microsoft VSDB from a display's EDID to mark it
  specialized — but only from the **firmware** EDID. The docs are explicit that
  the designation may **not** be made by overriding the EDID in software.
- `EDID_OVERRIDE` is consumed by **monitor.sys**, which reads the EDID *after*
  the graphics stack already has its own copy: the display port driver calls
  `DxgkDdiQueryDeviceDescriptor` during initialisation, and monitor.sys calls it
  later. A monitor-devnode override therefore never reaches the decision.
- **A Settings toggle does exist**, on Enterprise / **Pro for Workstations** /
  IoT Enterprise — which is this machine — and for an off-the-shelf panel it is
  the *only* supported route.

## The supported route

`Settings > System > Display > Advanced display > Remove display from desktop`.

**This works on the Philips today — verified in the field.** When the user
clicks it the display leaves the desktop and goes black, which is correct: the
compositor window that had been covering it dies with the desktop, so nothing
is driving the panel any more.

That makes **start order the whole game**.

> ### The rule
>
> **Start `dscomp --wait-for-target` FIRST. Toggle the display off the desktop
> SECOND.**
>
> Started first, dscomp is already polling and acquires the display the instant
> it becomes available — the black period is a blink. Toggled first, the panel
> sits dark until something acquires it, and dscomp cannot even *find* it any
> more (a non-desktop display is gone from `EnumDisplayDevices`, so
> `physicalDevice` no longer resolves and dscomp exits 3).

Programmatically the toggle is `DisplayConfigSetDeviceInfo` with
`SET_MONITOR_SPECIALIZATION` (type 13) — currently returning
`ERROR_INVALID_PARAMETER`, under investigation by the app. The Settings toggle
is the working path today.

`GET` (type 12) is free and read-only; `DisplayProbe` reports its three bits.
On this machine both "available" bits are set for both monitors (`0x6`).

Two caveats that matter:

- **`DisplayMonitorUsageKind` is not a reliable success signal.** A display
  specialized through the toggle is still reported as `Standard`
  ([open Windows bug](https://github.com/microsoft/Windows-classic-samples/issues/191)).
  `dscomp` no longer gates on it — acquisition succeeding is the real test.
- The toggle is offered only for **non-primary** displays, and there are field
  reports of users unable to restore a specialized display. Keep a guarded,
  auto-reverting wrapper around any automated use of `SET`.

---

## MANUAL TEST RECIPE

> ### Run 1 first: `--test-pattern`
>
> The 2026-07-28 live test proved acquisition works and the **first present**
> failed. So validate presentation in isolation before involving capture at all.
> `--test-pattern` draws a cycling solid colour with **no capture threads**:
>
> * panel shows shifting colour -> presentation works, any black screen is a
>   capture problem;
> * panel stays black -> the fault is in the scanout loop, and the log says
>   which call returned what.

What to run, in what order, and what output proves it worked. Have a second
monitor connected so you can always see the console.

### 1. Check eligibility (read-only, no side effects)

```powershell
compositoruildd\Release\DisplayProbe.exe --connected-only
```

Confirm `SPECIALIZATION raw value 0x00000006` on the target: eligible for both
monitor and system, not yet enabled.

### 2. RUN 1 - prove presentation, no capture

From a console **on your other monitor**:

```powershell
compositoruildd\Release\dscomp.exe --verbose --test-pattern --wait-for-target 60
```

Wait for the countdown, **then** turn on
`Settings > System > Display > Advanced display > Remove display from desktop`.

Success looks like:

```
specialized: render device and target share adapter LUID ...
specialized: shared scanout fence created
specialized: acquired '\.\DISPLAY1' at 5120x1440 with 2 primaries
--test-pattern: drawing a cycling solid colour, NO capture threads.
specialized: FIRST FRAME SCANNED OUT - signalled DeskSplitSpecializedLive
```

**and the panel slowly cycling through colours.** That is the scanout loop
proven end to end.

If it fails instead, the log now names the failing call and, critically, whether
*our* device is healthy:

```
presenter 'specialized' returned 0x887A0005, but OUR D3D11 device is healthy
(GetDeviceRemovedReason = S_OK). The failure is inside the presentation path.
```

### 3. RUN 2 - the real thing

Only once run 1 shows colour. Turn the toggle back **off**, then:

```powershell
compositoruildd\Release\dscomp.exe --verbose --wait-for-target 60
```

Wait for the countdown, toggle on. Success is the same
`FIRST FRAME SCANNED OUT` line plus the panel showing your virtual monitors.

### 4. If it does not work

| Symptom | Meaning |
| --- | --- |
| `ADAPTER MISMATCH` | The render device is on a different GPU than the acquired target. Report the two LUIDs. |
| `could not create a shared scanout fence` | Fence interop failed; presentation is running unsupported. Report it. |
| `no frame was scanned out within 20 second(s)` then exit **20** | Acquired but never presented. Panel is specialized and black - **turn the toggle back off.** |
| Countdown runs out, `falling back to the window path` | The toggle never took. dscomp exits 0, nothing is broken. |
| Exit **3**, "must be started BEFORE the display is removed" | You toggled first. Toggle off, start again from step 2. |

### 5. Cleanup

Stop dscomp (Ctrl+C or `Global\DeskSplitCompositorStop`) - it releases the
display on every exit path - then turn the toggle **off**.

## DEPRECATED: the EDID-override procedures

> Everything from here to "Tools reference" describes the **EDID-override**
> mechanism, which is now known not to work for hiding a display. It is kept
> because the guarded transaction is still the right way to *undo* an override
> on a machine where one was applied. Do not follow it to hide a display — use
> the Settings toggle above.

### Guided path (EDID override — deprecated)

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

### Manual CLI path (EDID override — recovery and debugging)

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


---

## Field failure analysis (2026-07-28)

Two independent problems, found from the guard log plus a read-only dump of
every EDID source.

### Problem 1 — a stale cache wedged every retry (FIXED)

`guarded-apply` refused to run seven times in a row with *"a Microsoft VSDB is
already present"* (exit 3), while the override key held the original CRU bytes
and no backup existed.

There are two registry EDIDs and they had diverged:

| Value | Content at 01:45 |
| --- | --- |
| `EDID_OVERRIDE ..3` | 512 bytes, CRU original, **no VSDB** — the revert had worked |
| `Device Parameters\EDID` | still the previously applied 640-byte EDID, **VSDB present** |

`Device Parameters\EDID` is monitor.sys's **cache**; it lagged a full session
behind the override. The tool read the cache, saw a VSDB that no longer existed
anywhere authoritative, and refused forever.

Fixed: patches are now computed from the **override when one exists** (the cache
only when there is none, where it *is* the hardware EDID). A disagreement
between the two is logged explicitly as stale rather than treated as a veto, and
"already applied" is now its own exit code (**6**) instead of the generic
failure 3. Verified: all sources now agree at 512 bytes with no VSDB — the stale
cache cleared itself on the next devnode enumeration, as expected.

### Problem 2 — the mechanism itself (NOT FIXABLE)

The override *was* live and correct, and the display still did not become
specialized.

Structural audit of the exact 640-byte EDID the tool produced, rebuilt from the
current CRU bytes:

- Base header valid, **all five block checksums valid**, extension count
  correctly incremented 3 → 4.
- All seven original CTA data blocks and all three DTDs preserved byte for byte;
  only bytes 126 and 127 of the base block changed.
- The VSDB is well-formed: `75 5C 12 CA 03 02` — tag 3, length 21, Microsoft
  OUI, version 3, **desktop-usage bit clear**.
- **The block-map hypothesis is false**: block 1 is a CTA extension (`0x02`),
  not a Block Map (`0xF0`). This EDID has no block map to update. (The patcher
  now handles block maps correctly anyway — see below.)
- One genuine oddity: the VSDB lands in the **second** CTA extension (block 4);
  the first is block 1. That was the last remaining structural suspect.

It turned out not to matter, because the mechanism is closed by design:

> "Displays may **not** be designated as HMDs or specialized displays by
> overriding the EDID in software."

`EDID_OVERRIDE` is consumed by monitor.sys, which reads the EDID *after* the
display port driver has already obtained its own copy via
`DxgkDdiQueryDeviceDescriptor`. The override never reaches the decision. No
registry knob (`SpecializedDisplays`, `NonDesktop`, …) exists as an alternative,
and no one has ever reported success with this approach.

### Collateral fixes made anyway

- **Block-map correctness.** Appending a block to an EDID that *does* have a
  block map would have left the map describing one block fewer than exists,
  making the appended block invisible to any parser that trusts it. The patcher
  now rewrites and re-checksums the map, and `VerifyRoundTrip` rejects an
  inconsistent one. Covered by five synthetic-EDID selftest cases (block map
  present, map rewritten, inconsistent map detected, in-place insert preferred,
  multi-CTA finder) that the real monitor cannot exercise.
- **`UsageKind` is no longer a gate.** `dscomp` used to refuse unless
  `UsageKind == SpecialPurpose`. A display specialized through the *supported*
  route is still reported as `Standard`, so that check would have rejected a
  perfectly acquirable display. It is now advisory and logged; acquisition
  succeeding is the real test.
