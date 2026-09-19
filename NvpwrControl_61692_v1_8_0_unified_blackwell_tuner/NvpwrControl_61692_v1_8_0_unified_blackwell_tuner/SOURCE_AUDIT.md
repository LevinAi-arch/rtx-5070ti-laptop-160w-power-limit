# Source audit — 1.8.0 unified

Static checks performed in the generation environment:

- all three `.vcxproj` files parse as XML.
- GUI compilation list contains `main.cpp`, `xmg_probe.cpp`, `nvapi_probe.cpp`, `nvapi_tuner.cpp`.
- `/utf-8` remains enabled for Debug and Release.
- `main.cpp` includes all corresponding headers before global objects.
- UTF-8/NUL and lexical brace/parenthesis/bracket balance checked for C/C++ sources; PowerShell sources decode as UTF-8.
- no prebuilt stale `NvpwrControl.exe`, `NvpwrCtl.exe` or `Nvpwr.sys` is shipped in the source ZIP.
- `driver/driver.c` and `shared/nvpwr_ioctl.h` are byte-identical to 1.7; the OC module is user-mode and does not silently replace the TGP kernel backend.
- 275 W remains locked; no guessed Entry13/Entry14 targets were added.
- XMG binary is not redistributed.

Runtime/physical validation still required:

- Windows WDK/MSVC compile.
- live Pstates20 SET/readback on each target laptop/driver.
- first live 616.92 XBAR/MSVDD layout validation (SET remains blocked if the `0x304` relation is not established).
- per-device stability testing for all clock/voltage offsets.
- 5080 XMG 250 W remains subject to the XMG package's own pending device validation; 5090/2C18 is the supplied live-verified reference.

- incremental OC baseline capture was checked so Pstates20, XBAR/MSVDD and GPC:XBAR can be first-mutated in separate Apply operations and still all restore correctly.
