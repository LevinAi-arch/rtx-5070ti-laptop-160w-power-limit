# Coherent TGP contract — implementation notes

## Old Nvpwr backend

The original backend changes NVIDIA's live `base / amount / UPPER`, Board selector2/MAX and regenerates F7/CURRENT. It remains used for 5050/5060/5070/5070 Ti and for the 5080/5090 175–225 W experimental menu.

## New 250 W backend

Source evidence: supplied `XMGPowerPatch-Target(1).zip`, protocol v8.

Query IOCTL:
- `0x00226000`
- input: `{ size=8, version=8 }`
- output: 2056 bytes

Apply/Rollback IOCTL:
- `0x0022A004`
- input: `{ size=16, version=8, operation, reserved=0 }`
- operation 1 = Apply
- operation 2 = RollbackToStock
- output: 568 bytes

User mode cannot supply the target values. The backend's fixed target is 225 W base + 25 W Dynamic Boost = 250 W total.

## Verification performed by NvpwrControl

Before exposing 250 W:
- PCI/platform/capability gate
- stable generation
- Entry13/Entry14 identity
- correct GPU-profile ↔ device ID match

After Apply:
- result protocol/size
- fixed 225/25/250 values
- 19 writer targets / 19 distinct targets
- operation code
- Core/NVPCF readback (six 250 W maxima + 225 W base)
- all six Entry13 readbacks = 250 W
- all six Entry14 readbacks = 100 W
- post-kernel verified flag
- fresh full Query = `FULL_TARGET`

Rollback requires a fresh Query = `FULL_STOCK` before Nvpwr may write again.

## 275 W

No values are inferred for a 275 W contract. Entry13/Entry14/core targets are not extrapolated from the 250 W package.
