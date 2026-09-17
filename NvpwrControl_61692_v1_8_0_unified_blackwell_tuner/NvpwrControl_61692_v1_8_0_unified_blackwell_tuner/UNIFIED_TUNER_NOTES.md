# Unified tuner implementation notes

## Power backends

There are intentionally two power backends rather than one universal write path.

1. `Nvpwr.sys` — the audited NVIDIA live policy route used for 5050/5060/5070/5070 Ti and the 175–225 W experimental 5080/5090 menu. It stages base + 25 W amount under the existing ceiling, raises selector2/MAX + UPPER, reruns the NVIDIA generator and verifies F7/CURRENT.
2. XMG v8 coherent backend — only the 250 W menu item on an exact compatible 5080/5090 XMG runtime. It owns the 19-target Core/NVPCF + Entry13 + Entry14 transaction and rollback.

The app never emulates XMG 250 W by writing only MAX/UPPER/F7.

## Pstates20 raw layout used for live range discovery

The current 616.92 capture returns V2 size 7416 (`0x21CF8`). The parser walks P0 and reads the delta/range fields for domain 0 (graphics), domain 4 (memory), and core-voltage domain 0 if present. SET uses a minimal V1 request rather than replaying the GET status blob.

Reference capture:

- Core: `-1000..+1000 MHz`.
- Memory: `-1000..+3000 MHz`.
- Base voltage entries: none, therefore NVVDD offset is disabled on that exact capture.

## XBAR/MSVDD

Required live conditions before a SET:

- NvAPI GET and SET IDs present.
- control version `0x000261A4`.
- repeated ClockDomains layout is discovered live.
- entry stride must equal the audited Blackwell `0x304` structure.
- XBAR index resolves to the unique active entry or the NvAPI enum default `1`.
- field locations are XBAR frequency `entry+0x114` and MSVDD request `entry+0x11C`.

A complete original buffer is captured for rollback. The SET is followed by a fresh GET and exact field comparison.

## GPC:XBAR

The writer first validates GET_INFO version `0x00015798` and a semantic relationship record:

- mapped type 0
- source GPC = 0
- destination XBAR = 1
- bidirectional = 1
- audited default ratio raw `0xE660`

GET/SET control version is `0x0001075C`. The ratio is U16.16. `0.9` is encoded as exact hardware default `0xE660` to avoid a one-LSB mismatch.

## V/F / ADC

V/F is deliberately information/readback-only in this unified build. The reference 616.92 V/F status structure is substantially larger than the small public reference layout; no generic writer is enabled merely because the SET ID exists.

ADC/rail INFO/STATUS are also read-only. The UI does not call an MSVDD request a physical ADC measurement.
