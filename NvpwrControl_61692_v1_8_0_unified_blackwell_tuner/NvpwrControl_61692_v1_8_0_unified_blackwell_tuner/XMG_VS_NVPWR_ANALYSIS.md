# NvpwrControl vs XMGPowerPatch — source audit

Date: 2026-09-15

This note is based on the user-supplied `NvpwrControl_61692_v1_5_0_experimental(4).zip` and `XMGPowerPatch-Target(1).zip`.

## Executive result

The two projects solve related but different parts of the RTX 50 Laptop power stack.

Nvpwr 1.5 changes the live NVIDIA policy path used by the 616.92 `nvlddmkm.sys` image:

- cTGP/base
- dynamic amount (25 W model)
- UPPER/F7 saturation ceiling
- Board selector2/MAX
- NVIDIA native generator -> selector3/F7/CURRENT

XMGPowerPatch uses a broader 250 W semantic contract for a specific XMG platform and changes both NVIDIA/NVPCF core targets and Type07 entries. Its target is not simply `MAX=250 W`.

## XMG contract recovered from the supplied package

Hardware gate:

- NVIDIA vendor 10DE
- device 2C18 (RTX 5090 Laptop) or 2C19 (RTX 5080 Laptop)
- subsystem vendor 1D05
- exactly one supported GPU / layout / object

Fixed target:

- Base/Internal: 225000 mW
- Dynamic Boost: 25000 mW
- Total/External: 250000 mW

2C18 / RTX 5090 status in the package: live verified.

2C19 / RTX 5080 status in the package: the full Type07 implementation is present, but the package itself labels separate live validation as pending.

### 19-writer contract

Core — 7 targets:

1. StateMaximum: 175000 -> 250000
2. MainChannelSelector2Effective: 175000 -> 250000
3. MainChannelSelector2Tagged: 175000 -> 250000
4. NVPCF MaximumA: 175000 -> 250000
5. NVPCF MaximumB: 175000 -> 250000
6. NVPCF MaximumAlias: 175000 -> 250000
7. NVPCF Base: 150000 -> 225000

Type07 Entry13 — 6 targets:

- array index 13
- ID 0x1B
- selectors 1/2/3 effective + tagged
- 210000 -> 250000 mW

Type07 Entry14 — 6 targets:

- array index 14
- ID 0x1C
- selectors 1/2/3 effective + tagged
- 60000 -> 100000 mW

Total: 7 + 6 + 6 = 19 writer targets.

## Why XMG is more portable than Nvpwr 1.5

XMG authorizes writes only after semantic discovery. Important gates include:

- exact-one runtime layout
- exact-one target object
- stable object generation
- known state envelope
- complete Type07 evidence
- all 19 writer targets resolved
- compare-and-swap before mutation
- immediate readback after each mutation
- reverse rollback if a later mutation fails

Known state classifications are:

- FULL_STOCK
- FULL_TARGET
- LEGACY_CORE_TARGET_TYPE07_STOCK

Any other mixed/partial state writes zero fields.

Nvpwr 1.5 has strong exact-616.92 code-signature guards and readback/rollback, but its power model is narrower and does not currently establish the XMG Type07 contract.

## What can be reused safely

### Added in NvpwrControl 1.6.1 XMG Audit

- Existing Nvpwr 5070 Ti 145-180 W source path preserved.
- Existing Nvpwr 5080/5090 175-225 W experimental source path preserved.
- RTX 5050/5060/5070 Laptop recognition added for capability probing; their kernel power writer remains locked.
- XMG protocol-v8 QUERY integration is read-only.
- XMG Entry13/Entry14 values, capability flags, state classification and semantic-layout evidence are displayed.
- XMG 250 W capability is shown only when the runtime QUERY passes the semantic gate.
- NVIDIA private-NvAPI interfaces for Core/Memory/NVVDD, XBAR/MSVDD, GPC:XBAR and V/F are probed read-only.
- Nvpwr helper restart restores OEM state before unloading; restart is refused if restore fails.
- Separate supported Windows PnP NVIDIA-device restart script is included.
- User-mode + kernel logging and debug bundle collection retained.

## 275 W conclusion

### PROVEN

The supplied XMG package establishes a coherent 250 W contract for 2C18/RTX 5090 Laptop and implements the same contract for 2C19/RTX 5080 Laptop pending separate live validation.

### NOT PROVEN

Nothing in the supplied XMG package establishes a 275 W contract.

A 275 W implementation cannot be derived safely by changing `250000 -> 275000`. A coherent 275 W contract would need, at minimum, independently established values and invariants for:

- Core maximums
- NVPCF maximums
- NVPCF base
- Type07 Entry13
- Type07 Entry14
- associated selector/reference/tag invariants
- OEM/EC/VBIOS/platform behavior
- rollback states

For that reason 1.6.1 does not authorize a 275 W write. It explicitly reports 275 W as `LOCKED / CONTRACT NOT ESTABLISHED`.

## XBAR / MSVDD / clocks

Local 616.92 read-only captures already prove the following NvAPI interfaces exist on the reference RTX 5070 Ti Laptop:

- GetPstates20: 0x6FF81213
- SetPstates20 interface present: 0x0F4DAE6B (not called by the probe)
- ClockClkDomainsGetInfo: 0x57B5A5DF
- ClockClkDomainsGetControl: 0xF58938F5
- ClockClkDomainsSetControl interface present: 0xD14B69CF
- ClockClkPropTopRelsGetInfo: 0xE826E4F0
- ClockClkPropTopRelsGetControl: 0xCBFF71D0
- ClockClkPropTopRelsSetControl interface present: 0xEF3D20EA
- ClockClientClkVfPointsGetInfo: 0x507B4B59
- ClockClientClkVfPointsGetControl interface present: 0x23F1B133
- ClockClientClkVfPointsSetControl interface present: 0x0733E009
- ADC/Rail information: 0x68789E2A

The new probe calls only GET/read interfaces and checks SET interface presence. It intentionally does not issue a SET until the returned layout/ranges are parsed and validated.

## 1.7.0 update — coherent writer integration

The earlier 1.6.1 note that XMG integration was query-only is superseded for this branch.

1.7.0 keeps the same read-only semantic Query but additionally uses the supplied protocol-v8 Apply/Rollback IOCTL for the fixed 250 W transaction when the exact XMG capability gate passes. NvpwrControl does not supply writer addresses or arbitrary power values; XMGPowerPatch remains the semantic resolver and 19-target CAS/readback/rollback authority.

250 W therefore uses the recovered 7 Core/NVPCF + 6 Entry13 + 6 Entry14 transaction. 175–225 W targets still use the original Nvpwr two-phase backend. 275 W remains locked because the supplied source material contains no 275 W contract.
