# NvpwrControl 1.8.0 — Unified Blackwell Laptop Tuner

One UI/source tree combining the power-control work, coherent XMG-style high-power contract, Blackwell clock/rail tuning, telemetry, recovery and the DPI/Unicode UI work.

## GPU profiles / TGP

| GPU | Power menu | Backend |
|---|---:|---|
| RTX 5050 Laptop | OEM 115 W + 120–140 W | Nvpwr two-phase runtime policy; experimental; exact coherent 115 W OEM gate |
| RTX 5060 Laptop | OEM 115 W + 120–140 W | same |
| RTX 5070 Laptop | OEM 115 W + 120–140 W | same |
| RTX 5070 Ti Laptop | 145–180 W | Nvpwr; 145/150/160 W physically validated on the reference 616.92 machine; other values experimental |
| RTX 5080 Laptop | 175–225 W | Nvpwr experimental |
| RTX 5090 Laptop | 175–225 W | Nvpwr experimental |
| RTX 5080/5090 compatible XMG platform | 250 W | coherent XMG v8 19-target backend (Core/NVPCF + Entry13 + Entry14) |

275 W is deliberately **not** exposed: no coherent 275 W Core/NVPCF/Entry13/Entry14 contract was established in the supplied material.

The 250 W backend does not fall back to MAX/UPPER/F7. It requires the XMG v8 semantic runtime and the exact profile/capability state. The XMG driver binary is not redistributed in this project.

## Advanced Blackwell tuning

The GUI contains a single Advanced Tuning panel.

- **Core offset** — Pstates20, driver-reported min/max, SET + GET readback.
- **Memory offset** — Pstates20, driver-reported min/max. `+5000 MHz` is available only if the installed GPU/driver reports it. On the saved reference RTX 5070 Ti / 616.92 capture the actual range is `-1000..+3000 MHz`.
- **NVVDD offset** — enabled only when Pstates20 exposes an editable voltage-delta range. It is not fabricated when the GPU/driver reports no such control.
- **XBAR offset** — ClockDomains private NvAPI. Full buffer read, dynamic layout discovery, audited `0x304` entry stride requirement, SET, exact readback, rollback.
- **XBAR-domain MSVDD request offset** — same ClockDomains transaction; UI gated to RTX 5070 Ti / 5080 / 5090 profiles and only after the live XBAR layout validates. Absolute software guard `-100..+100 mV`.
- **GPC→XBAR propagation ratio** — validates the semantic GPC(0)→XBAR(1), bidirectional relationship before SET; range `0.0..2.0`, U16.16 readback.
- **V/F information** — read-only capability/control-table telemetry. This build does not expose a generic V/F writer because the current 616.92 status layout differs from the smaller public reference layout and a blanket point write would not be fail-closed.
- **ADC / rail information** — read-only info/status capability telemetry. Physical MSVDD ADC is not claimed.
- **Physical XBAR clock** — read with CLK_MEASURE_FREQ when available.

Every writable OC stage does `GET -> validate -> SET -> GET/readback`. Each writable subsystem captures its own session baseline before its first mutation. A failed later stage attempts to restore every captured subsystem baseline.

## UI / recovery

- Unicode Win32 UI, `/utf-8`, Segoe UI.
- RU / EN interface.
- Per-Monitor DPI Awareness V2 and `WM_DPICHANGED` relayout.
- **Restart Nvpwr driver**: refuses to unload a non-stock helper unless OEM restore succeeds first.
- **Restart NVIDIA device**: first restores Nvpwr OEM power plus the captured NvAPI tuning baseline, then launches the supported Windows PnP disable/enable helper. The screen can go black during the reset.
- User log: `C:\ProgramData\NvpwrControl\nvpwr-control.log`.
- Kernel log prefix: `[NVPWR]`.
- `collect-debug.ps1` packages the app log, service state, BCD state, signatures, Nvpwr status, NVIDIA power/clock data and display-device state.

## Driver signing

This project does not implement DSE/Secure-Boot bypasses. `Nvpwr.sys` remains a test-signed development driver. The supported development flow is Secure Boot disabled manually in UEFI + Windows TESTSIGNING enabled + reboot, or a legitimate production signing path.

The user-mode NvAPI tuning module is independent of whether `Nvpwr.sys` loads.

## Build

Run elevated PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
cd C:\NvpwrControl_61692_v1_8_0_unified_blackwell_tuner
.\build.ps1
```

Output:

```text
dist\NvpwrControl.exe
dist\NvpwrCtl.exe
dist\Nvpwr.sys
dist\restart-nvidia-device.ps1
dist\collect-debug.ps1
```

The source is statically audited in this environment, but Windows/WDK compilation must be performed on Windows.
