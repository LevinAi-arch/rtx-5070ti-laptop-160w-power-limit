# RTX 5070 Ti Laptop GPU — 160W Power Limit Unlock

### Lenovo Legion Pro 5 Gen 10 / NVIDIA Blackwell Laptop GPU Research

> Experimental research into NVIDIA Laptop GPU power management and raising the GeForce RTX 5070 Ti Laptop GPU power limit beyond the factory 140W maximum.

---

## TL;DR

I developed and tested a method that allows the NVIDIA GeForce RTX 5070 Ti Laptop GPU in my Lenovo Legion Pro 5 Gen 10 to operate with a power limit above its factory 140W maximum.

### Factory configuration

```text
Maximum GPU Power Limit: 140W
```

### Modified target

```text
GPU Power Limit: 160W
```

This is **not** a conventional MSI Afterburner power-limit adjustment and does not rely on the normal desktop NVIDIA power-limit slider.

The research involved analyzing NVIDIA's laptop power-management stack, including NVAPI, NVIDIA RM, `nvlddmkm.sys`, internal power policies, and the path through which the final GPU power limit is applied.

---

# Tested Hardware

| Component | Configuration |
|---|---|
| Laptop | Lenovo Legion Pro 5 Gen 10 |
| Model | Lenovo Legion Pro 5 16IAX10H |
| GPU | NVIDIA GeForce RTX 5070 Ti Laptop GPU |
| Architecture | NVIDIA Blackwell |
| Factory GPU Power Limit | 140W |
| Factory cTGP | 115W |
| Dynamic Boost | up to +25W |
| Modified Target | 160W |
| OS | Windows 11 x64 |
| BIOS | Q6CN79WW |
| VBIOS | 98.05.4E.00.07 |
| NVIDIA Driver used during research | 616.92 |

---

# Why I Started This Research

The RTX 5070 Ti Laptop GPU in this Lenovo configuration has a factory maximum GPU power envelope of approximately:

```text
115W cTGP
+
25W Dynamic Boost
=
140W
```

Under sustained GPU-heavy workloads, the card can reach the factory power ceiling.

I wanted to understand where the 140W maximum was actually enforced and whether it was possible to modify that limit without physically modifying the laptop.

Several layers were investigated:

- NVIDIA VBIOS
- Lenovo BIOS / firmware
- Embedded Controller behavior
- NVIDIA NVAPI
- NVIDIA RM
- `nvlddmkm.sys`
- NVIDIA power-policy handling
- Dynamic Boost
- internal GPU power-management paths

The goal was not simply to find a registry tweak or hidden slider.

The goal was to understand the complete power-limit path.

---

# Stock Power Policy

On the stock configuration, the GPU power-policy information reported values approximately equivalent to:

```text
Default Power Limit : 70W
Maximum Power Limit : 140W
Current Power Limit : 140W
```

The GPU therefore exposes a 140W maximum to normal software.

Standard overclocking applications cannot simply move the power slider beyond this value.

---

# Why MSI Afterburner / Normal Power Limit Controls Are Not Enough

On this laptop, the conventional NVIDIA desktop power-limit interface does not expose a usable adjustable power-limit range above the OEM maximum.

This means that tools such as MSI Afterburner can modify frequency and memory offsets, but they cannot simply change:

```text
140W → 160W
```

through the normal power-limit slider.

This suggested that laptop GPU power management uses a different or additional policy layer compared with conventional desktop GPU implementations.

---

# Research Path

During the investigation, I traced parts of the power-management chain between user-mode NVIDIA interfaces and the final GPU power-policy implementation.

A simplified representation is:

```text
Application / Tool
        ↓
      NVAPI
        ↓
NVIDIA user-mode components
        ↓
   nvlddmkm.sys
        ↓
    NVIDIA RM
        ↓
Power Policy subsystem
        ↓
 GPU Power Controller
```

The research showed that the laptop power limit is not simply controlled by the normal desktop NVIDIA power-limit API.

---

# Desktop NVAPI Power Limit Test

A conventional desktop-style NVIDIA power-policy path was tested first.

A relevant NVAPI power-policy function was identified and traced into NVIDIA RM.

However, attempts to apply a modified power limit through this path were rejected on this laptop configuration.

Example result:

```text
NVAPI status:
INVALID_ARGUMENT
```

The GPU continued reporting:

```text
Current Power Limit : 140W
Maximum Power Limit : 140W
```

This was an important result because it confirmed that simply locating the desktop Power Limit API was not enough.

The laptop implementation follows a different path or performs additional validation.

---

# Internal Power Policy Research

Further reverse engineering identified internal NVIDIA power-policy processing related to the GPU power configuration.

The investigation included analysis of:

```text
nvlddmkm.sys
NVIDIA RM
Power Policy Objects
internal policy storage
policy selectors
policy validation
final GPU power application
```

One of the important findings was that the power-management chain contains internal policy values that are not exposed through the normal consumer power-limit controls.

This is the path that eventually led to a working experimental implementation.

---

# Working Result

The factory configuration is:

```text
Factory Maximum:
140W
```

The modified target is:

```text
Modified Target:
160W
```

Difference:

```text
+20W

approximately +14.3%
```

The modification is applied at the NVIDIA power-policy level rather than through a normal overclocking slider.

---

# Important Distinction

This project deals with the **GPU power limit**.

It should not be confused with ordinary GPU overclocking.

These are separate mechanisms.

For example:

```text
Power Limit modification:
140W → 160W

GPU Core overclock:
frequency offset

VRAM overclock:
memory frequency offset
```

A higher power limit does not automatically guarantee higher performance.

Performance scaling depends on:

- GPU voltage
- GPU frequency
- workload
- cooling
- thermal limits
- voltage/frequency curve
- VRM capability
- Dynamic Boost behavior
- CPU/GPU shared power budget

---

# GPU Tuning Used During Testing

The GPU has also been tested with additional clock tuning.

One conservative configuration used during testing was approximately:

```text
GPU Core:
+200 MHz

VRAM:
+400 MHz
```

More aggressive frequency configurations were investigated separately.

The power-limit modification itself should be evaluated independently from overclocking.

---

# How the Result Should Be Validated

Seeing a momentary 160W value is not enough to prove a useful performance improvement.

A proper comparison should use exactly the same workload at:

```text
140W
vs
160W
```

and compare:

- average GPU power
- peak GPU power
- average GPU clock
- GPU voltage
- GPU temperature
- VRAM temperature
- thermal throttling
- power throttling
- benchmark score
- sustained performance
- system stability

---

# Benchmark Results

Detailed controlled benchmark comparisons will be added as testing continues.

| Metric | 140W | 160W |
|---|---:|---:|
| Power Limit | 140W | 160W |
| Maximum Observed Power | TBD | TBD |
| Average GPU Power | TBD | TBD |
| Average GPU Clock | TBD | TBD |
| Peak GPU Clock | TBD | TBD |
| GPU Temperature | TBD | TBD |
| VRAM Temperature | TBD | TBD |
| Benchmark Score | TBD | TBD |

---

# Suggested Proof / Screenshots

The repository will include evidence such as:

- stock 140W telemetry
- modified power-limit telemetry
- HWiNFO monitoring
- GPU-Z information
- NVIDIA driver information
- benchmark results
- sustained load testing
- before / after comparisons

Suggested repository layout:

```text
rtx-5070ti-laptop-160w-power-limit/
│
├── README.md
│
├── screenshots/
│   ├── stock-140w.png
│   ├── modified-160w.png
│   ├── hwinfo-160w.png
│   └── benchmark-comparison.png
│
├── benchmarks/
│   ├── 140w-results.md
│   └── 160w-results.md
│
├── logs/
│   ├── 140w.csv
│   └── 160w.csv
│
└── docs/
    ├── research.md
    ├── power-policy-analysis.md
    └── compatibility.md
```

---

# Driver Dependency

The initial research was performed using:

```text
NVIDIA Driver:
616.92
```

The implementation interacts with driver behavior that may change between NVIDIA driver releases.

NVIDIA may modify:

- internal structures
- function layouts
- validation logic
- RM interfaces
- offsets
- power-policy handling

between driver versions.

Therefore:

> Compatibility with one NVIDIA driver version does not automatically imply compatibility with another.

A future version of the project may include automatic driver detection and compatibility validation.

---

# Current Compatibility

Currently researched and tested on:

```text
Laptop:
Lenovo Legion Pro 5 Gen 10
Lenovo Legion Pro 5 16IAX10H

GPU:
NVIDIA GeForce RTX 5070 Ti Laptop GPU

OS:
Windows 11 x64

BIOS:
Q6CN79WW

VBIOS:
98.05.4E.00.07

NVIDIA Driver:
616.92
```

---

# Not Yet Confirmed

The method has not yet been confirmed on every:

- Lenovo Legion model
- RTX 5060 Laptop GPU
- RTX 5070 Laptop GPU
- RTX 5080 Laptop GPU
- RTX 5090 Laptop GPU
- ASUS laptop
- MSI laptop
- Acer laptop
- HP laptop
- Alienware laptop
- other OEM laptop
- NVIDIA driver version

Different laptops may use different:

- VBIOS configurations
- VRM designs
- AC adapters
- cooling systems
- Embedded Controller policies
- BIOS policies
- Dynamic Boost implementations
- NVIDIA power-policy configurations

Do not assume that a configuration working on one laptop is safe or functional on another.

---

# Why 160W?

160W was selected as an experimental target above the factory 140W limit.

It should **not** be interpreted as a universally safe value for every RTX 5070 Ti Laptop GPU.

The purpose of the project is to investigate whether the factory software power ceiling can be modified and to measure the resulting behavior.

The electrical and thermal capability of the laptop remains the responsibility of the specific hardware platform.

---

# Thermal and Electrical Safety

Increasing a laptop GPU power limit increases load on more than just the GPU chip.

Potentially affected components include:

- GPU silicon
- GPU VRM
- VRAM
- motherboard power delivery
- AC adapter
- cooling system
- heatpipes / vapor chamber
- fans
- battery / charging power path

At minimum, monitor:

```text
GPU Temperature
GPU Hotspot Temperature
VRAM Temperature
GPU Power
GPU Voltage
GPU Clock
VRM behavior
Thermal Throttling
Power Throttling
System Stability
```

A GPU accepting a higher power target does **not** mean the laptop is automatically capable of sustaining that power indefinitely.

---

# Warning

This is experimental research.

Increasing GPU power limits can cause:

- crashes
- driver resets
- instability
- excessive temperatures
- thermal shutdowns
- VRM overheating
- AC adapter overload
- reduced hardware lifespan
- data loss
- hardware damage
- warranty issues

Do not blindly apply power limits from another laptop.

Every laptop power-delivery and cooling design is different.

Use this information at your own risk.

---

# Secure Boot / Driver Security

The research also involves low-level interaction with NVIDIA driver behavior.

Some experimental builds or research environments may require Windows configurations that differ from a normal production installation.

This repository does **not** recommend permanently disabling operating-system security features simply to obtain additional GPU performance.

Any future public tool should clearly identify its requirements and refuse to operate on unsupported configurations.

---

# Project Status

- [x] Factory 140W configuration analyzed
- [x] NVIDIA laptop power-policy behavior investigated
- [x] Conventional NVAPI power-limit path tested
- [x] NVIDIA RM path investigated
- [x] Internal power-policy path investigated
- [x] 160W target implementation tested
- [ ] Publish complete 140W vs 160W benchmark comparison
- [ ] Publish telemetry logs
- [ ] Publish screenshots
- [ ] Test additional NVIDIA drivers
- [ ] Test additional Lenovo configurations
- [ ] Test additional RTX 50 Laptop GPUs
- [ ] Automatic NVIDIA driver detection
- [ ] Automatic compatibility validation
- [ ] GUI
- [ ] Public tool release

---

# Future Development

The long-term goal is to turn the research prototype into a safer and cleaner utility.

Possible future features include:

1. Detect the installed NVIDIA driver automatically.
2. Detect the installed GPU.
3. Detect laptop and VBIOS information.
4. Verify that the expected NVIDIA power-policy path exists.
5. Refuse to operate on unsupported driver versions.
6. Read the current GPU power configuration.
7. Validate requested power targets.
8. Apply configurable power targets.
9. Restore the factory configuration.
10. Log GPU telemetry.
11. Provide clear error reporting.
12. Provide a simple GUI.
13. Detect changes after NVIDIA driver updates.
14. Support multiple validated driver versions.

Safety and compatibility validation are more important than blindly supporting every configuration.

---

# Test Reports Wanted

Reports from other RTX 50-series laptop owners are welcome.

If you test related behavior, please include:

```text
Laptop manufacturer:
Laptop model:

GPU:

BIOS:
VBIOS:
NVIDIA Driver:

Factory Power Limit:
Modified Power Limit:

Maximum Observed GPU Power:
Average GPU Power:

Average GPU Clock:
Peak GPU Clock:

GPU Temperature:
GPU Hotspot:
VRAM Temperature:

Benchmark:
Stock Score:
Modified Score:

Stable:
Yes / No

Additional Notes:
```

This will help determine whether the same NVIDIA power-policy behavior exists across different laptop implementations.

---

# Contributing

Technical discussion, independent verification, driver research, benchmark results and compatibility reports are welcome.

Useful contributions include:

- testing on additional laptops
- testing different NVIDIA drivers
- telemetry logs
- benchmark comparisons
- NVIDIA power-policy research
- compatibility reports
- bug reports

Please include detailed hardware and software information when opening an Issue.

---

# Responsible Disclosure / Redistribution

Please do not upload proprietary NVIDIA or Lenovo binaries to this repository unless redistribution is explicitly permitted.

Examples include:

```text
nvlddmkm.sys
NVIDIA proprietary DLLs
Lenovo firmware binaries
modified proprietary firmware images
```

Research notes, hashes, offsets, logs, screenshots and independently written source code can be documented separately where appropriate.

---

# Disclaimer

This repository documents independent experimental research.

This project is not affiliated with, sponsored by, approved by, or endorsed by:

- NVIDIA Corporation
- Lenovo

NVIDIA, GeForce and related names are trademarks of NVIDIA Corporation.

Lenovo and Legion are trademarks of Lenovo.

All information is provided for educational and research purposes.

Modifying hardware power-management behavior may cause instability or permanent hardware damage.

You are responsible for understanding the risks before reproducing any experiment described here.

Use at your own risk.

---

# Updates

Additional technical documentation, benchmark results, screenshots and compatibility information will be published as testing continues.

If you have compatible hardware and want to contribute results, open an Issue in this repository.
