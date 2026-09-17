# NVIDIA Laptop GPU Power Limit Control (Unified RTX 40 & 50 Series Tuner)

> Low-level power management tuner and TDP unlocker for **NVIDIA GeForce RTX 40 Series** (Ada Lovelace) and **RTX 50 Series** (Blackwell) Laptop GPUs.

---

## Overview

Modern gaming and workstation laptops enforce strict Total Graphics Power (TGP) ceilings through internal NVIDIA driver policies and Dynamic Boost limits. Standard overclocking utilities like MSI Afterburner can adjust core and memory clock offsets, but they cannot adjust the TGP ceiling beyond the OEM factory limits on laptop GPUs.

**NvpwrControl** interfaces directly with the live NVIDIA kernel driver (`nvlddmkm.sys`) power-policy objects in system memory to raise the maximum power limit above factory OEM caps, unlocking sustained performance under heavy workloads.

---

## Supported Hardware & Power Target Ranges

### RTX 40 Series (Ada Lovelace) Laptop GPUs
| GPU | Baseline / Stock Range | Target Power Limit Range | Step |
|---|---|---|---|
| **RTX 4090 Laptop GPU** | 115W – 175W | **150W – 250W** | 5W |
| **RTX 4080 Laptop GPU** | 115W – 175W | **150W – 225W** | 5W |
| **RTX 4070 Laptop GPU** | 100W – 140W | **120W – 150W** | 5W |
| **RTX 4060 Laptop GPU** | 100W – 140W | **120W – 150W** | 5W |
| **RTX 4050 Laptop GPU** | 95W – 115W | **115W – 140W** | 5W |

### RTX 50 Series (Blackwell) Laptop GPUs
| GPU | Baseline / Stock Range | Target Power Limit Range | Step |
|---|---|---|---|
| **RTX 5090 Laptop GPU** | 150W – 175W | **175W – 225W** | 5W |
| **RTX 5080 Laptop GPU** | 150W – 175W | **175W – 225W** | 5W |
| **RTX 5070 Ti Laptop GPU** | 115W – 140W | **145W – 180W** | 5W |
| **RTX 5070 Laptop GPU** | 115W – 140W | **145W – 180W** | 5W |
| **RTX 5060 Laptop GPU** | 100W – 115W | **120W – 140W** | 5W |

---

## Driver Requirements

- Validated on **NVIDIA Driver 616.92** (`nvlddmkm.sys` PE timestamp `0x6A9B4070`, size `0x06D3E000`).
- The kernel driver validates exact driver binary structures before mutating any memory offsets, failing closed if signatures do not match.

---

## Quick Start Guide

Because `Nvpwr.sys` is a custom kernel driver built to interact with NVIDIA's driver in memory, Windows 64-bit requires **Test-Signing Mode** with **Secure Boot disabled**.

### Step 1: Disable Secure Boot in BIOS/UEFI
1. Restart your laptop and press **Del** (or **F2**) to enter BIOS.
2. Navigate to the **Security** or **Boot** settings.
3. Set **Secure Boot** to **Disabled**.
4. Press **F10** to save changes and restart your laptop.
   *(Note: Windows kernel ignores test-signing if Secure Boot is enabled in hardware).*

### Step 2: Enable Windows Test Mode & Trust Certificate
1. Open the downloaded release folder.
2. Right-click **`install-cert-and-enable-testmode.cmd`** and select **Run as administrator**.
   - This automatically installs `Nvpwr.cer` into Windows Trusted Root / Trusted Publishers.
   - It executes `bcdedit /set testsigning on`.
3. **Restart your PC**.
   *(After restart, "Test Mode" watermark will appear in the bottom-right corner of your desktop).*

### Step 3: Run the Tuner & Apply Desired Power
1. Right-click **`NvpwrControl.exe`** and select **Run as administrator**.
2. The application will detect your GPU model, display your current OEM baseline power, and load available target wattages.
3. Select your desired target from the dropdown (e.g. up to 250W on RTX 4090 Laptop) and click **Apply**.
4. Verify the power draw using **HWiNFO**, **GPU-Z**, or your favorite monitoring overlay under heavy GPU load.

---

## Switching Back for Games Requiring Secure Boot

Competitive multiplayer games with kernel-level anti-cheats (such as **Valorant / Riot Vanguard**, **EA Sports FC / EA Anti-Cheat**, or **Faceit CS2**) require Secure Boot to be enabled and Test Mode to be turned off.

To switch back to standard OEM mode:
1. Open Command Prompt or PowerShell as **Administrator**.
2. Run:
   ```cmd
   bcdedit /set testsigning off
   ```
3. Enter your BIOS on reboot and set **Secure Boot** back to **Enabled**.
4. Your laptop will boot normally and run at its standard factory OEM limits (e.g. 175W in Extreme Performance mode) with all anti-cheat games working.

---

## Built-In NVAPI Overclocking (User-Mode)

In addition to TGP unlocking, `NvpwrControl` includes direct user-mode NVAPI tuning:
- **Core Clock Offset**: Up to ±1000 MHz
- **Memory Clock Offset**: Up to ±3000 MHz
- **Telemetry Readout**: Real-time clock domains, P-states, and rail information.

---

## Building from Source

To compile the project from source:

### Prerequisites
- Visual Studio 2022 or Visual Studio 18 with **Desktop development with C++**
- Windows 10/11 SDK and Windows Driver Kit (WDK) 10.0.28000+

### Build Command
Run the included build script in an elevated PowerShell:
```powershell
powershell -ExecutionPolicy Bypass -File .\NvpwrControl_61692_v1_8_0_unified_blackwell_tuner\build.ps1
```
The compiled binaries (`NvpwrControl.exe`, `NvpwrCtl.exe`, `Nvpwr.sys`, and setup scripts) will be output to the `dist\` directory.

---

## Thermal & Electrical Safety Warning

> **WARNING**: Raising laptop GPU power limits increases electrical load and heat output across the GPU die, VRM power delivery, VRAM, and the laptop cooling subsystem.

- Always monitor temperatures (`GPU Temp`, `Hotspot`, `Memory Temp`, and `VRM`) using HWiNFO or GPU-Z.
- Ensure your laptop cooling vents are clean and your AC power adapter has sufficient wattage to sustain higher power draws.
- All modifications are performed at your own risk.

---

## License & Disclaimer

This project is independent research and is not affiliated with, sponsored by, or endorsed by NVIDIA Corporation. NVIDIA, GeForce, and RTX are trademarks of NVIDIA Corporation.
