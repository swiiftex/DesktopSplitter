# installer/edid-override — `edidoverride.exe`

> # ⛔ DEPRECATED FOR HIDING DISPLAYS
>
> This tool adds a Microsoft VSDB to a monitor's EDID via a registry override,
> to mark the display "specialized". **That does not work, and cannot be made to
> work.** Microsoft: *"Displays may not be designated as HMDs or specialized
> displays by overriding the EDID in software."*
> ([source](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/specialized-monitors-compositor))
>
> `EDID_OVERRIDE` is read by monitor.sys, which sees the EDID only *after* the
> display port driver already obtained its own copy — so the override never
> reaches the decision. Verified in the field on 2026-07-28: a structurally
> perfect override went live and the display stayed `Standard`.
>
> **The supported route is `Settings > System > Display > Advanced display >
> Remove display from desktop`** (Enterprise / Pro for Workstations / IoT
> Enterprise), programmatically `DisplayConfigSetDeviceInfo` with
> `SET_MONITOR_SPECIALIZATION` (type 13). See [`docs/HIDING.md`](../../docs/HIDING.md).
>
> **This tool is retained only to REVERT an override on a machine where one was
> already applied** — `guarded-revert` / `revert`. Its EDID patching, guarded
> transaction and auto-revert are correct and well tested; they were simply
> aimed at a mechanism that Windows does not honour. Do not use `apply` or
> `guarded-apply` expecting a display to disappear.

Inspects and patches a monitor's **EDID override** so Windows will treat the
display as a **specialized (non-desktop) display**. This is the prerequisite for
`dscomp --specialized`. The full user procedure is in [`docs/HIDING.md`](../../docs/HIDING.md).

---

# ⚠️ READ THIS BEFORE RUNNING `apply` ⚠️

**A bad EDID override can leave a monitor showing nothing at all.** The override
is stored in the registry and is applied every time the monitor is enumerated,
including at boot — so a broken one survives reboots.

### RECOVERY — how to undo it if the display goes dark

The override lives here (`<id>` and `<inst>` are shown by `edidoverride list`):

```
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\DISPLAY\<id>\<inst>\Device Parameters\EDID_OVERRIDE
```

**Deleting that key and rebooting always restores the monitor's real EDID.**

Pick whichever you can reach:

1. **Another monitor still works** — run `edidoverride revert <target> --yes`
   from an elevated console, then replug the monitor or reboot. This restores a
   backed-up previous override if there was one.
2. **Remote access works** — same thing over RDP/SSH, or delete the key with
   `reg delete`.
3. **Nothing is visible: Safe Mode.** Interrupt boot with the power button 2–3
   times to reach the recovery menu → *Troubleshoot* → *Advanced options* →
   *Startup Settings* → *Restart* → press **4** for Safe Mode. Safe Mode uses a
   basic display driver and ignores the override. Then run `regedit`, delete the
   `EDID_OVERRIDE` key above, and reboot normally.
4. **Last resort** — in Device Manager, uninstall the monitor device. Windows
   recreates its registry key clean on the next enumeration, which discards the
   override.

**Before you run `apply` for the first time**, put a copy of `edidoverride.exe`
somewhere you can reach without the affected display (a USB stick, or a second
machine), and write down the registry path above. Do not rely on being able to
read this file afterwards.

---

## Commands

```
edidoverride list                       Enumerate monitors, EDID sizes, overrides
edidoverride dump     <target> [--hex]  Current vs override EDID, parsed + checksums
edidoverride verify   <target>          Validate checksums, structure and VSDB
edidoverride selftest <target>          Exercise the patch logic in memory (SAFE)
edidoverride apply    <target> [--yes]  Write the override (dry run without --yes)
edidoverride revert   <target> [--yes]  Restore backup / delete override

edidoverride guarded-apply  <target> [--timeout-seconds N] [--dry-run] --yes
edidoverride guarded-revert <target> [--dry-run] --yes
```

---

## Transaction-guarded mode (what the GUI drives)

`guarded-apply` is the **recommended** way to apply the override. It exists so a
GUI can do the whole risky change with **one elevation prompt** and a
**guaranteed auto-revert**: this process is the safety authority, not the GUI.
If the GUI crashes, is killed, or the user simply cannot see anything, the
countdown still expires here and the monitor still goes back.

### Sequence

| Step | Action | On failure |
| --- | --- | --- |
| a | Write the override (backing up any existing one, exactly as `apply --yes`) | revert, exit **3** |
| b | Disable + enable the monitor devnode, then wait ≤5 s for problem code 0 | revert + restart, exit **4** |
| c | Signal `Global\DeskSplitEdidArmed` — **the countdown starts here** | revert, exit 3 |
| d | Wait ≤ N seconds (default 10) for `Global\DeskSplitEdidKeep` or `Global\DeskSplitEdidRevert` | — |

Outcomes:

- **Keep** signalled → override stays, `HidePhysicalDisplay=1`, exit **0**
- **Revert** signalled → restore backup, restart, `HidePhysicalDisplay=0`, exit **10**
- **Timeout** → same as revert, exit **11**

The countdown clock starts at step c, *after* the restart. The GUI mirrors it
visually once it sees `Armed`, but **this process's timeout is the
authoritative one** — the GUI's is only cosmetic.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | kept — user confirmed |
| 2 | not elevated |
| 3 | apply failed; any partial change was reverted |
| 4 | devnode restart failed or the monitor did not return; reverted |
| 5 | bad arguments |
| 6 | nothing to do — the override already carries the VSDB |
| 10 | user asked to revert; reverted |
| 11 | countdown expired; auto-reverted |

### Which EDID a patch is computed from

There are **two** registry EDIDs and they can disagree:

| Value | What it is |
| --- | --- |
| `Device Parameters\EDID_OVERRIDE\0,1,…` | what we install — **authoritative** for what Windows will apply |
| `Device Parameters\EDID` | monitor.sys's **cache** of the effective EDID, refreshed when the monitor is enumerated |

The tool patches from **the override when one exists**, falling back to the cache
only when there is none (in which case the cache *is* the hardware EDID).

This matters because the cache lags. Observed in the field: after a
`guarded-revert` restored the original 512-byte CRU override, `Device
Parameters\EDID` still held the previously applied 640-byte VSDB EDID for the
rest of the session. Reading the cache made every retry conclude "a Microsoft
VSDB is already present" and exit — a permanent wedge from a value that was
merely stale. The tool now compares the two, logs the discrepancy explicitly,
and proceeds from the override. The stale cache clears itself on the next
devnode enumeration.

"Already applied" is also its own exit code (**6**) rather than being folded
into the generic failure 3, so a GUI can say "already done" instead of
"something went wrong".

### Event security (DACL)

The three events are created with an explicit DACL so an **unelevated** GUI can
take part while the guard itself runs elevated:

```
D:P(A;;0x1f0003;;;BA)(A;;0x1f0003;;;SY)(A;;0x00120002;;;AU)
```

- **Administrators / SYSTEM** — `EVENT_ALL_ACCESS`.
- **Authenticated Users** — `EVENT_MODIFY_STATE | SYNCHRONIZE | READ_CONTROL`
  (`0x00120002`): enough to wait on `Armed`, set `Keep`/`Revert`, and read the
  DACL back for diagnostics — and nothing more. It cannot rewrite the DACL or
  delete the objects.

Two details worth knowing if you write another client:

- `CreateEvent` always requests `EVENT_ALL_ACCESS`. If the objects already
  exist and your process is unelevated, that is **denied** (an unelevated token
  carries Administrators as deny-only). Open with
  `EVENT_MODIFY_STATE | SYNCHRONIZE` instead. The guard does exactly this.
- An **elevated** guard will never fall back to another namespace: if
  `Global\` cannot be used it fails loudly, because silently switching to
  `Local\` would leave the GUI waiting on a name that is never signalled. Only
  an unelevated `--dry-run` (which has no `SeCreateGlobalPrivilege`) may fall
  back, and it says so in the log.

### `--dry-run`

Walks the entire sequence, **fires the same events**, and honours the same
timeout and exit codes — but writes no registry and restarts nothing. It does
not require elevation. This is what the GUI uses for its preview, and it lets
the whole Armed/Keep/Revert dance be integration-tested harmlessly.

### `guarded-revert`

Undoes DesktopSplitter's change and restarts the devnode. It is deliberately
conservative about what counts as "ours":

- a `EDID_OVERRIDE_DSBACKUP` exists → restore it (this is the user's original
  override, CRU's included) and delete the backup;
- no backup, but the current override **contains a Microsoft VSDB** → it is
  ours, delete it;
- otherwise → **no-op**. An override we did not create (a plain CRU one, say)
  is the user's and is never touched. Exit 0 with a message either way.

### Logging

Every step goes to stdout **and** appends to
`%ProgramData%\DesktopSplitter\edid-guard.log`, with timestamps, the elevation
state, and the exit code. When a display change goes wrong the GUI is often the
last thing that can tell you anything — read the log first.

### Known limitation

The guard reverts on timeout, on request, and on its own failures. It cannot
revert if **the guard process itself** is killed between steps a and d (e.g.
`taskkill /f`, or a hard power loss). In that case the override survives to the
next boot and you recover with `guarded-revert`, `revert`, or the registry
deletion described above.

`<target>` is a GDI device name (`\\.\DISPLAY1`) or any substring of the monitor
devnode (`PHLC310`).

- `apply` and `revert` are **dry runs unless `--yes` is given**, and require an
  elevated console.
- `selftest` and `dump`/`verify`/`list` never write anything and need no
  elevation.
- `--use-case N` sets the VSDB primary use case (default `2` = generic display).

## What `apply` actually writes

It reads the monitor's current **effective** EDID from
`Device Parameters\EDID`, adds a Microsoft Vendor-Specific Data Block, fixes
every checksum, and writes the result back as `EDID_OVERRIDE\0`, `\1`, `\2`, …
(one `REG_BINARY` value per 128-byte block — this is the documented Windows
override format, the same one monitor INFs use via `HKR,EDID_OVERRIDE,"0",1,…`
from a `.HW` section).

### The Microsoft VSDB

22 bytes, appended to a CTA-861 extension block:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `75` | CTA header: tag 3 (Vendor Specific), payload length 21 |
| 1–3 | `5C 12 CA` | Microsoft IEEE registration id, in this sequential byte order |
| 4 | `03` | Version 3 — specialized, non-HMD display |
| 5 | see below | bit 6 = desktop usage, bit 5 = third-party usage, bits 4–0 = primary use case |
| 6–21 | 16 bytes | Container ID (UUID) |

**Bit 6 must be CLEAR.** Windows treats version 3 with desktop-usage *clear* as
non-desktop. This is the opposite of what intuition suggests, and it is what
makes the display "specialized". The tool always writes it clear and the
self-test asserts it.

The Container ID is derived deterministically from the monitor's devnode id, so
re-running `apply` produces byte-identical output.

### Two patch strategies

- **Insert into the existing CTA block** when it has ≥22 unused bytes after its
  detailed timing descriptors. The EDID length is unchanged; the DTD run shifts
  down and byte 2 (the DTD offset) is updated.
- **Append a new CTA extension block** otherwise. The EDID grows by 128 bytes,
  the base block's extension count (byte 126) is incremented, and both
  checksums are fixed. **Only two bytes of the original EDID change.**

`selftest` reports which strategy your monitor will take. For the Philips
49M2C8900 it is *append*: that EDID's CTA block has only 15 free bytes.

### It never destroys an existing override

If the monitor already has an `EDID_OVERRIDE` — very common, e.g. anything set
up with **CRU (Custom Resolution Utility)** — `apply` copies the whole key
(including CRU's own marker values) to `EDID_OVERRIDE_DSBACKUP` first, and
`revert` restores it. The patch is computed from the *effective* EDID, so
existing customisations are carried forward rather than lost.

## Making the override take effect

The monitor driver reads the override when the device initialises, so you must
re-enumerate the monitor:

- unplug and replug the monitor (enough for external displays), **or**
- disable + enable the monitor in Device Manager, **or**
- reboot.

## Build

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    installer\edid-override\edidoverride.vcxproj -p:Configuration=Release -p:Platform=x64
```

C++17, `/W4 /WX`, no dependencies beyond `advapi32`/`user32`. The patch logic is
header-only in `EdidPatch.h` so `selftest` runs exactly the code `apply` runs.

## Caveats

- Whether Windows validates an override's checksums is **not documented**. The
  tool always computes them correctly and refuses to write if its own
  round-trip check fails, but do not rely on Windows rejecting a bad one — the
  observed community failure mode is that a malformed override is accepted and
  the display goes dark.
- The literal `Enum\...\Device Parameters` path is well-established but
  Microsoft formally recommends `CM_Open_DevNode_Key`/`SetupDiOpenDevRegKey`
  instead. The literal path is used here deliberately: it is the same path the
  recovery instructions above tell you to delete.
- Marking a display specialized does **not** by itself make anything render to
  it. It removes it from the desktop; something must then acquire and drive it
  (that is `dscomp --specialized`).
