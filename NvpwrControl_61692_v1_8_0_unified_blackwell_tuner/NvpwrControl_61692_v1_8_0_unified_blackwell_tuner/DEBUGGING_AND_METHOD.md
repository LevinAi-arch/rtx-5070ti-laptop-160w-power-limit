# Nvpwr 1.5.0 Experimental — logging + “where / what / why” review notes

This file describes the existing 1.5.0 experimental control path. The logging/comment pass does **not** intentionally expand power ranges or change the target formula.

## 1. High-level control path

```text
NvpwrControl.exe
    ↓  profile selection + IOCTL request
Nvpwr.sys
    ↓  exact nvlddmkm 616.92 PE/signature validation
ResolveContext
    ↓  live NVIDIA GPU → Major → power Root → Board
SaveBaseline
    ↓  exact OEM state captured for same-session rollback
PHASE A: StageTargetAtStockCeiling
    ↓  base = target - 25 W
    ↓  amount = 25 W
    ↓  eligibility = 1 → native NVIDIA F7 generator
    ↓  MAX/UPPER still held at captured OEM ceiling
PHASE B
    ↓  Board selector2 / source FE = target (MAX)
    ↓  root+3D24 = target (UPPER)
PHASE C
    ↓  native eligibility setter re-runs NVIDIA generator
    ↓  final readback: MAX / CURRENT / F7 / UPPER / predicted F7
SUCCESS or rollback to captured baseline
```

## 2. Where the important fields are

For the exact NVIDIA 616.92 image guarded by this project:

- `root+0x3D14` — base / cTGP input used by the relevant generator branch.
- `root+0x3D18` — dynamic amount. The controlled 1.5.0 model uses `25,000 mW`.
- `root+0x3D24` — UPPER, the saturation ceiling used by the F7 generator.
- Board `selector2`, source `FE` — MAX-like policy ceiling.
- Board `selector3`, source `F7` — current/applied result produced by the NVIDIA generator.

The project does not patch NVIDIA executable code. It resolves these runtime objects and invokes NVIDIA's existing native setter/generator paths.

## 3. Why the transition is staged

Changing only one number is not treated as success.

**Phase A** proves that the requested `base + 25 W` model is coherent while the saved OEM ceiling is still in force. This keeps the first transition bounded by the original ceiling.

**Phase B** raises both places that matter for the proven path: Board MAX and root UPPER.

**Phase C** re-runs the real NVIDIA generator and requires readback agreement. A target is accepted only when the runtime state is classified as `NvpwrStateApplied` and the applied target matches the request.

## 4. Why baseline is saved

1.5.0 does not assume every laptop has the same OEM wattage. On the first mutation in a driver session it stores:

- Root and Board object identity
- eligibility
- amount-active flag
- base (`+3D14`)
- amount (`+3D18`)
- UPPER (`+3D24`)

Rollback refuses to use that saved state if the live Root/Board identity has changed.

## 5. Kernel logging

`driver/driver.c` now emits `DbgPrintEx` messages prefixed with:

```text
[NVPWR][INFO]
[NVPWR][WARN]
[NVPWR][ERROR]
```

Important messages include:

```text
ValidateBuild: ...
ResolveContext: ...
SaveBaseline: ...
Phase A: ...
BoardSet: selector=2 source=0xFE ...
Phase B: writing root+3D24 UPPER=...
PHASE C ...
Final verify: ...
Rollback: ...
RestoreStock: ...
```

Kernel messages require a kernel-debug/DbgPrint capture setup. They are complementary to the GUI log below.

## 6. GUI log — easiest log to send for review

`NvpwrControl.exe` writes UTF-8 text to:

```text
C:\ProgramData\NvpwrControl\nvpwr-control.log
```

It records:

- program startup / exit
- detected GPU name and profile
- exact `Nvpwr.sys` path selected
- service restart/start state
- device-open result
- Apply / Restore / Refresh actions
- status snapshots containing state, detail, NTSTATUS, NVIDIA status, OEM baseline, base, amount, UPPER, MAX, CURRENT, F7 and predicted F7

A typical useful sequence is:

```text
GPU detection: ...
EnsureDriverService: using driver path: ...
Refresh: state=OEM STOCK ...
ApplyDesired preflight: ...
SendTarget: request 160 W ...
Refresh: state=APPLIED ...
```

## 7. What to send if something fails

For ordinary debugging, send:

1. `C:\ProgramData\NvpwrControl\nvpwr-control.log`
2. a screenshot/copy of the GUI Technical details panel after the failure
3. if available, the `[NVPWR]` kernel DbgPrint lines covering the same attempt

The timestamps in the GUI log make it easier to correlate the user-mode request with kernel output.

## 8. Functional logic intentionally unchanged by this pass

This review/debug pass is intended to add observability and documentation. It does not intentionally change:

- profile wattage ranges
- 5 W target step
- `PPAB_FIXED = 25,000 mW`
- `base = target - PPAB_FIXED`
- exact NVIDIA 616.92 build/signature guard
- profile baseline guards
- staged MAX/UPPER/generator flow
- existing restore strategy

`build.ps1` was made more robust at locating MSBuild because the earlier script could report `MSBuild not found` on the user's Visual Studio 18 installation even when MSBuild existed.

## 9. One-command debug bundle

After rebuilding/running the program, this read-only collector packages the easiest evidence:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\collect-debug.ps1
```

It includes the GUI log, Nvpwr service status, current boot TESTSIGNING entry, Nvpwr.sys signature information, `NvpwrCtl status`, and `nvidia-smi -q -d POWER` when available. It does not apply or restore a power target.

---

## 1.6.1 additions — XMG / NvAPI audit

### XMG QUERY

`app/xmg_probe.cpp` opens `\\.\XMGPowerPatch` with read access and calls only QUERY IOCTL `0x00226000` using protocol v8. It parses semantic-layout evidence, capability flags, state classification and Type07 Entry13/Entry14 selector readbacks. It never calls the XMG apply IOCTL.

### NvAPI capability probe

`app/nvapi_probe.cpp` initializes NvAPI and performs only GET calls / SET-interface presence checks. No clock, voltage or power setting is written by this module.

### Driver restart

The GUI refuses to unload/restart the Nvpwr helper while a non-stock Nvpwr state is active unless OEM restore succeeds first. A separate `restart-nvidia-device.ps1` performs a supported Windows PnP display-device reset after Nvpwr restore.

## 1.7.0 coherent 250 W transaction

When the installed XMG v8 runtime passes the exact platform/capability gate, 250 W is no longer a read-only audit item. The GUI invokes the XMG Apply transaction and independently validates its compact result plus a fresh full resolver Query.

Transition rules are fail-closed:

- narrow Nvpwr state is normalized before coherent Apply;
- FULL_TARGET/legacy XMG state is rolled back coherently before any ordinary Nvpwr target;
- mixed/partial coherent state blocks new narrow writes;
- if a previously verified coherent state becomes unqueryable, normal writes are blocked until the XMG runtime is restored or Windows is rebooted.
