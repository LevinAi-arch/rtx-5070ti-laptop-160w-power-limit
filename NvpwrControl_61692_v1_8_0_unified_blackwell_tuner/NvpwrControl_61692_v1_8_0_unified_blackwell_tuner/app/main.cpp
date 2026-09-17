#include <Windows.h>
#include <CommCtrl.h>
#include <winsvc.h>
#include <dxgi.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <initializer_list>
#include <cstdlib>
#include "..\\shared\\nvpwr_ioctl.h"
#include "xmg_probe.h"
#include "nvapi_probe.h"
#include "nvapi_tuner.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Dxgi.lib")
#pragma comment(lib, "Shell32.lib")

static constexpr wchar_t kServiceName[] = L"Nvpwr";
static constexpr wchar_t kDriverFile[] = L"Nvpwr.sys";
static HANDLE g_device = INVALID_HANDLE_VALUE;
static HWND g_combo = nullptr;
static HWND g_status = nullptr;
static HWND g_state = nullptr;
static HWND g_apply = nullptr;
static HWND g_restore = nullptr;
static HWND g_refresh = nullptr;
static HWND g_restart = nullptr;
static HWND g_driverHelp = nullptr;
static HWND g_restartNvidia = nullptr;
static HWND g_tuningInfo = nullptr;
static ULONG g_profile = NvpwrProfileUnknown;
static ULONG g_oemBaselineWatts = 0;
static std::wstring g_gpuName = L"Unknown NVIDIA GPU";
static std::vector<ULONG> g_targets;
static ULONG g_populatedProfile = NvpwrProfileUnknown;
static ULONG g_populatedBaselineWatts = 0;
static bool g_populatedXmg250 = false;
static std::wstring g_logPath;
static XmgRuntimeSummary g_xmg{};
static XmgTransactionSummary g_lastXmgTx{};
static bool g_coherent250KnownApplied = false;
static NvapiProbeSummary g_nvapi{};
static NvapiTunerState g_tuner{};

// Unified OC controls. They live here (before ui_theme.h) so the DPI/layout
// layer can rescale them together with the power controls.
static HWND g_tuneTitle = nullptr;
static HWND g_tuneLabel[6]{};
static HWND g_tuneEdit[6]{};
static HWND g_tuneRange[6]{};
static HWND g_tuneApply = nullptr;
static HWND g_tuneReset = nullptr;

/*
    USER-MODE REVIEW / DEBUG LOGGING

    WHERE:
      The GUI is responsible for GPU-name/profile selection, service lifecycle,
      opening \\.\Nvpwr, sending IOCTLs and rendering driver readback.

    WHAT IS LOGGED:
      - detected GPU + selected profile
      - exact Nvpwr.sys path used for the service
      - service stop/start/open failures
      - every Apply / Restore request
      - compact driver status snapshots before/after transitions

    WHY:
      Kernel DbgPrint shows the internal driver phases, while this text file
      shows what the GUI asked for and what readback it received. Together they
      make it possible to correlate user-mode intent with kernel execution.

    LOG FILE:
      C:\ProgramData\NvpwrControl\nvpwr-control.log
*/

static std::wstring ResolveLogPath()
{
    if (!g_logPath.empty()) return g_logPath;

    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) {
        dir = std::wstring(base) + L"\\NvpwrControl";
        CreateDirectoryW(dir.c_str(), nullptr); /* already exists is fine */
    } else {
        wchar_t temp[MAX_PATH]{};
        DWORD t = GetTempPathW(MAX_PATH, temp);
        dir = (t > 0 && t < MAX_PATH) ? std::wstring(temp) : L"C:\\";
    }

    g_logPath = dir + L"\\nvpwr-control.log";
    return g_logPath;
}

static void LogLine(const std::wstring& message)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t prefix[128]{};
    swprintf_s(prefix,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][PID %lu] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, GetCurrentProcessId());

    std::wstring line = std::wstring(prefix) + message + L"\r\n";
    OutputDebugStringW(line.c_str());

    const std::wstring path = ResolveLogPath();
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
        nullptr, 0, nullptr, nullptr);
    if (bytesNeeded > 0) {
        std::string utf8((size_t)bytesNeeded, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
            utf8.data(), bytesNeeded, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
    CloseHandle(h);
}

#include "ui_theme.h"


static std::wstring DetectNvidiaGpuName()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return L"Unknown NVIDIA GPU";

    std::wstring result = L"Unknown NVIDIA GPU";
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && d.VendorId == 0x10DE) {
            result = d.Description;
            adapter->Release();
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

/*
    UI profile selection is intentionally name-based and is NOT the security
    boundary. The kernel still validates the live OEM baseline and exact 616.92
    KMD layout before accepting a target. This keeps a spoofed/odd adapter name
    from being sufficient to authorize a high-power profile.
*/
static ULONG DetectProfile(const std::wstring& name)
{
    // Order matters: 5070 Ti must be matched before the generic 5070 profile.
    if (name.find(L"RTX 5070 Ti Laptop") != std::wstring::npos) return NvpwrProfileRtx5070TiLaptop;
    if (name.find(L"RTX 5050 Laptop") != std::wstring::npos) return NvpwrProfileRtx5050Laptop;
    if (name.find(L"RTX 5060 Laptop") != std::wstring::npos) return NvpwrProfileRtx5060Laptop;
    if (name.find(L"RTX 5070 Laptop") != std::wstring::npos) return NvpwrProfileRtx5070Laptop;
    if (name.find(L"RTX 5080 Laptop") != std::wstring::npos) return NvpwrProfileRtx5080Laptop;
    if (name.find(L"RTX 5090 Laptop") != std::wstring::npos) return NvpwrProfileRtx5090Laptop;

    // 40 Series Laptop GPUs (Ada Lovelace)
    if (name.find(L"RTX 4090 Laptop") != std::wstring::npos || name.find(L"RTX 4090 Mobile") != std::wstring::npos) return NvpwrProfileRtx4090Laptop;
    if (name.find(L"RTX 4080 Laptop") != std::wstring::npos || name.find(L"RTX 4080 Mobile") != std::wstring::npos) return NvpwrProfileRtx4080Laptop;
    if (name.find(L"RTX 4070 Laptop") != std::wstring::npos || name.find(L"RTX 4070 Mobile") != std::wstring::npos) return NvpwrProfileRtx4070Laptop;
    if (name.find(L"RTX 4060 Laptop") != std::wstring::npos || name.find(L"RTX 4060 Mobile") != std::wstring::npos) return NvpwrProfileRtx4060Laptop;
    if (name.find(L"RTX 4050 Laptop") != std::wstring::npos || name.find(L"RTX 4050 Mobile") != std::wstring::npos) return NvpwrProfileRtx4050Laptop;

    return NvpwrProfileUnknown;
}

static const wchar_t* ProfileName(ULONG p)
{
    switch (p) {
    case NvpwrProfileRtx5070TiLaptop: return L"RTX 5070 Ti Laptop";
    case NvpwrProfileRtx5080Laptop: return L"RTX 5080 Laptop";
    case NvpwrProfileRtx5090Laptop: return L"RTX 5090 Laptop";
    case NvpwrProfileRtx5050Laptop: return L"RTX 5050 Laptop";
    case NvpwrProfileRtx5060Laptop: return L"RTX 5060 Laptop";
    case NvpwrProfileRtx5070Laptop: return L"RTX 5070 Laptop";
    case NvpwrProfileRtx4090Laptop: return L"RTX 4090 Laptop";
    case NvpwrProfileRtx4080Laptop: return L"RTX 4080 Laptop";
    case NvpwrProfileRtx4070Laptop: return L"RTX 4070 Laptop";
    case NvpwrProfileRtx4060Laptop: return L"RTX 4060 Laptop";
    case NvpwrProfileRtx4050Laptop: return L"RTX 4050 Laptop";
    default: return L"Unsupported / unknown";
    }
}

static bool AllowSeparateMsvdd()
{
    return g_profile == NvpwrProfileRtx5070TiLaptop ||
        g_profile == NvpwrProfileRtx5080Laptop ||
        g_profile == NvpwrProfileRtx5090Laptop;
}

static bool Xmg250MatchesSelectedProfile()
{
    if (!XmgHasCoherent250Capability(g_xmg)) return false;
    if (g_profile == NvpwrProfileRtx5090Laptop) return g_xmg.DeviceId == 0x2C18u;
    if (g_profile == NvpwrProfileRtx5080Laptop) return g_xmg.DeviceId == 0x2C19u;
    return false;
}

static void PopulateTargets(const NVPWR_STATUS& s)
{
    if (!g_combo) return;
    UiComboMessage(g_combo, CB_RESETCONTENT, 0, 0);
    g_targets.clear();

    ULONG stockW = s.OemBaseline ? (s.OemBaseline / 1000u) :
        ((s.State == NvpwrStateStockBaseline && s.UpperBoundary != 0) ? (s.UpperBoundary / 1000u) : 0u);
    if (stockW) g_oemBaselineWatts = stockW;

    wchar_t b[96];
    if (g_oemBaselineWatts)
        swprintf_s(b, L"OEM stock — %lu W", g_oemBaselineWatts);
    else
        swprintf_s(b, L"OEM stock");
    UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
    g_targets.push_back(0); /* 0 means Restore stock */

    if (g_profile == NvpwrProfileRtx5050Laptop ||
        g_profile == NvpwrProfileRtx5060Laptop ||
        g_profile == NvpwrProfileRtx5070Laptop) {
        /*
            LOW-POWER BLACKWELL LAPTOP PROFILE
            WHERE: RTX 5050 / 5060 / 5070 Laptop.
            WHAT: OEM 115 W is represented by the first "OEM stock" entry;
                  only 120..140 W need an explicit power-policy transition.
            WHY: selecting the OEM value should never rewrite an already-correct
                 stock state.  The kernel still requires an exact 115 W OEM
                 ceiling before it accepts any of these experimental targets.
        */
        for (ULONG w = 120; w <= 140; w += 5) {
            swprintf_s(b, L"%lu W — EXPERIMENTAL", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    } else if (g_profile == NvpwrProfileRtx5070TiLaptop) {
        for (ULONG w = 145; w <= 180; w += 5) {
            if (w <= 160) swprintf_s(b, L"%lu W%s", w, (w == 145 || w == 150 || w == 160) ? L" — verified" : L"");
            else swprintf_s(b, L"%lu W — EXPERIMENTAL", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    } else if (g_profile == NvpwrProfileRtx5080Laptop || g_profile == NvpwrProfileRtx5090Laptop) {
        for (ULONG w = 175; w <= 225; w += 5) {
            swprintf_s(b, L"%lu W — Nvpwr experimental", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
        if (Xmg250MatchesSelectedProfile()) {
            swprintf_s(b, L"250 W — COHERENT 19-TARGET (XMG v8)");
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(250);
        }
    } else if (g_profile == NvpwrProfileRtx4090Laptop) {
        /*
            RTX 4090 LAPTOP PROFILE
            Stock factory limit: typically 150..175 W (e.g. 150 W + 25 W Dynamic Boost).
            Balanced/quiet OEM profiles may start at 130 W or 140 W.
            Target range: 150..250 W step 5.
        */
        for (ULONG w = 150; w <= 250; w += 5) {
            if (w <= 175) swprintf_s(b, L"%lu W%s", w, (w == 150 || w == 175) ? L" — OEM range" : L"");
            else swprintf_s(b, L"%lu W — UNLOCKED", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    } else if (g_profile == NvpwrProfileRtx4080Laptop) {
        for (ULONG w = 150; w <= 225; w += 5) {
            if (w <= 175) swprintf_s(b, L"%lu W%s", w, (w == 150 || w == 175) ? L" — OEM range" : L"");
            else swprintf_s(b, L"%lu W — UNLOCKED", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    } else if (g_profile == NvpwrProfileRtx4060Laptop || g_profile == NvpwrProfileRtx4070Laptop) {
        for (ULONG w = 120; w <= 150; w += 5) {
            if (w <= 140) swprintf_s(b, L"%lu W%s", w, (w == 140) ? L" — OEM max" : L"");
            else swprintf_s(b, L"%lu W — UNLOCKED", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    } else if (g_profile == NvpwrProfileRtx4050Laptop) {
        for (ULONG w = 115; w <= 140; w += 5) {
            swprintf_s(b, L"%lu W — UNLOCKED", w);
            UiComboMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)b);
            g_targets.push_back(w);
        }
    }
    UiComboMessage(g_combo, CB_SETCURSEL, 0, 0);
    LogLine(L"PopulateTargets: profile=" + std::to_wstring(g_profile) +
        L" OEM=" + std::to_wstring(g_oemBaselineWatts) +
        L" W entries=" + std::to_wstring((ULONG)g_targets.size()) +
        L" xmg250=" + std::to_wstring(Xmg250MatchesSelectedProfile() ? 1 : 0));
}

static std::wstring TuningCapabilityText()
{
    const bool xmg250 = XmgHasCoherent250Capability(g_xmg);
    std::wstringstream ss;
    if (g_uiRussian) {
        ss << L"OC: Core " << (g_tuner.CoreMHz.Supported ? L"WRITE" : L"N/A")
           << L" | Memory " << (g_tuner.MemoryMHz.Supported ? L"WRITE" : L"N/A")
           << L" | XBAR " << (g_tuner.XbarWritable ? L"WRITE" : L"N/A")
           << L" | MSVDD " << (g_tuner.MsvddWritable ? L"WRITE" : L"N/A")
           << L" | NVVDD " << (g_tuner.NvvddMv.Supported ? L"WRITE" : L"N/A")
           << L" | GPC:XBAR " << (g_tuner.RatioWritable ? L"WRITE" : L"N/A") << L"\r\n"
           << L"V/F info " << (g_tuner.VfInfoAvailable ? L"READ" : L"N/A")
           << L" | ADC/rail " << (g_tuner.AdcStatusAvailable ? L"READ" : L"N/A")
           << L" | XMG coherent 250 W " << (xmg250 ? L"PASS" : L"N/A")
           << L" | 275 W LOCKED";
    } else {
        ss << L"OC: Core " << (g_tuner.CoreMHz.Supported ? L"WRITE" : L"N/A")
           << L" | Memory " << (g_tuner.MemoryMHz.Supported ? L"WRITE" : L"N/A")
           << L" | XBAR " << (g_tuner.XbarWritable ? L"WRITE" : L"N/A")
           << L" | MSVDD " << (g_tuner.MsvddWritable ? L"WRITE" : L"N/A")
           << L" | NVVDD " << (g_tuner.NvvddMv.Supported ? L"WRITE" : L"N/A")
           << L" | GPC:XBAR " << (g_tuner.RatioWritable ? L"WRITE" : L"N/A") << L"\r\n"
           << L"V/F info " << (g_tuner.VfInfoAvailable ? L"READ" : L"N/A")
           << L" | ADC/rail " << (g_tuner.AdcStatusAvailable ? L"READ" : L"N/A")
           << L" | XMG coherent 250 W " << (xmg250 ? L"PASS" : L"N/A")
           << L" | 275 W LOCKED";
    }
    return ss.str();
}

static void SetEditLong(HWND h, LONG value)
{
    if (!h) return;
    const std::wstring t = std::to_wstring(value);
    SetWindowTextW(h, t.c_str());
}

static void SetEditDouble(HWND h, double value)
{
    if (!h) return;
    wchar_t b[64]{}; swprintf_s(b, L"%.4f", value); SetWindowTextW(h, b);
}

static bool ReadEditLong(HWND h, LONG& value)
{
    wchar_t b[64]{}; GetWindowTextW(h, b, 64); wchar_t* end = nullptr;
    long v = wcstol(b, &end, 10); if (end == b || *end != 0) return false; value = static_cast<LONG>(v); return true;
}

static bool ReadEditDouble(HWND h, double& value)
{
    wchar_t b[64]{}; GetWindowTextW(h, b, 64); wchar_t* end = nullptr;
    double v = wcstod(b, &end); if (end == b || *end != 0) return false; value = v; return true;
}

static void UpdateTuningControls()
{
    const NvapiTuneRange ranges[3] = { g_tuner.CoreMHz, g_tuner.MemoryMHz, g_tuner.NvvddMv };
    const int mapEdit[3] = {0, 1, 4};
    for (int i = 0; i < 3; ++i) {
        const int e = mapEdit[i];
        EnableWindow(g_tuneEdit[e], ranges[i].Supported ? TRUE : FALSE);
        if (ranges[i].Supported) {
            SetEditLong(g_tuneEdit[e], ranges[i].Current);
            std::wstring unit = (e == 4) ? L" mV" : L" MHz";
            std::wstring r = std::to_wstring(ranges[i].Min) + L" .. " + std::to_wstring(ranges[i].Max) + unit;
            UiSetText(g_tuneRange[e], r.c_str());
        } else {
            SetWindowTextW(g_tuneEdit[e], L""); UiSetText(g_tuneRange[e], L"N/A");
        }
    }
    EnableWindow(g_tuneEdit[2], g_tuner.XbarWritable ? TRUE : FALSE);
    if (g_tuner.XbarWritable) {
        SetEditLong(g_tuneEdit[2], g_tuner.XbarMHz);
        UiSetText(g_tuneRange[2], L"-1000 .. +1000 MHz (layout-gated)");
    } else { SetWindowTextW(g_tuneEdit[2], L""); UiSetText(g_tuneRange[2], L"N/A"); }

    EnableWindow(g_tuneEdit[3], g_tuner.MsvddWritable ? TRUE : FALSE);
    if (g_tuner.MsvddWritable) {
        SetEditLong(g_tuneEdit[3], g_tuner.MsvddMv);
        UiSetText(g_tuneRange[3], L"-100 .. +100 mV request");
    } else { SetWindowTextW(g_tuneEdit[3], L""); UiSetText(g_tuneRange[3], L"N/A / profile gated"); }

    EnableWindow(g_tuneEdit[5], g_tuner.RatioWritable ? TRUE : FALSE);
    if (g_tuner.RatioWritable) {
        SetEditDouble(g_tuneEdit[5], g_tuner.GpcXbarRatio);
        UiSetText(g_tuneRange[5], L"0.0 .. 2.0 (U16.16)");
    } else { SetWindowTextW(g_tuneEdit[5], L""); UiSetText(g_tuneRange[5], L"N/A"); }

    const bool anything = g_tuner.CoreMHz.Supported || g_tuner.MemoryMHz.Supported ||
        g_tuner.XbarWritable || g_tuner.MsvddWritable || g_tuner.NvvddMv.Supported || g_tuner.RatioWritable;
    EnableWindow(g_tuneApply, anything ? TRUE : FALSE);
    EnableWindow(g_tuneReset, anything ? TRUE : FALSE);
}

static void RefreshTunerOnly()
{
    ProbeNvapiTuner(g_tuner, AllowSeparateMsvdd());
    UpdateTuningControls();
}

static void RefreshExternalProbes()
{
    if (g_profile == NvpwrProfileRtx5080Laptop || g_profile == NvpwrProfileRtx5090Laptop) {
        std::wstring xerr;
        if (!XmgEnsureRuntimeAvailable(xerr) && !xerr.empty())
            LogLine(L"XMG runtime lifecycle: " + xerr);
    }

    XmgRuntimeSummary xr{};
    if (XmgQueryRuntime(xr)) {
        g_xmg = xr;
        if (XmgHasCoherent250Capability(g_xmg)) {
            if (XmgIsFullTarget(g_xmg) || XmgIsLegacyCoreTarget(g_xmg))
                g_coherent250KnownApplied = true;
            else if (XmgIsFullStock(g_xmg))
                g_coherent250KnownApplied = false;
            // INVALID/MIXED preserves the last known bit fail-closed.
        }
    } else {
        // Preserve g_coherent250KnownApplied fail-closed if a previously
        // verified coherent state becomes temporarily unqueryable.
        g_xmg = xr;
    }

    RunNvapiReadOnlyProbe(g_nvapi); // legacy/raw capability details for audit log.
    ProbeNvapiTuner(g_tuner, AllowSeparateMsvdd());
    UpdateTuningControls();
    if (g_tuningInfo) UiSetText(g_tuningInfo, TuningCapabilityText().c_str());

    LogLine(L"External probe: XMG present=" + std::to_wstring(g_xmg.Present ? 1 : 0) +
        L" protocol=" + std::to_wstring(g_xmg.ProtocolOk ? 1 : 0) +
        L" cap=0x" + std::to_wstring(g_xmg.CapabilityFlags) +
        L" state=" + std::to_wstring(g_xmg.StateClassification) +
        L" known250=" + std::to_wstring(g_coherent250KnownApplied ? 1 : 0));
}

static std::wstring Win32Error(DWORD code)
{
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out = n && msg ? msg : L"Unknown error";
    if (msg) LocalFree(msg);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
    return out;
}

static std::wstring AppDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

static std::wstring ParentDirectory(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    if (p == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, p);
}

static bool IsRegularFile(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring LocateDriverFile()
{
    /* Prefer a packaged copy next to the GUI, but also support running the
       freshly-built app\x64\Release\NvpwrControl.exe directly. */
    std::wstring dir = AppDirectory();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        const std::wstring candidates[] = {
            dir + L"\\" + kDriverFile,
            dir + L"\\dist\\" + kDriverFile,
            dir + L"\\driver\\x64\\Release\\" + kDriverFile,
            dir + L"\\driver\\x64\\Debug\\" + kDriverFile
        };
        for (const auto& c : candidates) {
            if (IsRegularFile(c)) return c;
        }
        std::wstring parent = ParentDirectory(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return L"";
}

static void CloseDeviceHandle()
{
    if (g_device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
    }
}

static bool WaitForServiceState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs, std::wstring& error)
{
    DWORD start = GetTickCount();
    for (;;) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            error = L"QueryServiceStatusEx failed: " + Win32Error(GetLastError());
            return false;
        }
        if (ssp.dwCurrentState == wanted) return true;
        if (ssp.dwCurrentState == SERVICE_STOPPED && wanted != SERVICE_STOPPED) {
            std::wstringstream ss;
            ss << L"Driver service stopped while starting. Win32 service exit="
               << ssp.dwWin32ExitCode << L", driver exit=" << ssp.dwServiceSpecificExitCode;
            error = ss.str();
            return false;
        }
        if (GetTickCount() - start >= timeoutMs) {
            std::wstringstream ss;
            ss << L"Timed out waiting for driver service state " << wanted
               << L" (current=" << ssp.dwCurrentState << L")";
            error = ss.str();
            return false;
        }
        Sleep(100);
    }
}

static bool EnsureDriverService(std::wstring& error, bool forceRestart)
{
    std::wstring sys = LocateDriverFile();
    if (sys.empty()) {
        error = L"Nvpwr.sys was not found.\r\n\r\n"
            L"Checked next to NvpwrControl.exe and the project dist/driver build folders.\r\n"
            L"Run build.ps1 again and make sure Nvpwr.sys was produced.";
        LogLine(L"EnsureDriverService: Nvpwr.sys not found");
        return false;
    }

    LogLine(L"EnsureDriverService: using driver path: " + sys +
        (forceRestart ? L" [force restart]" : L""));

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        error = L"OpenSCManager failed: " + Win32Error(GetLastError()) +
            L"\r\n\r\nRun NvpwrControl.exe as administrator.";
        return false;
    }

    const DWORD access = SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | DELETE;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, access);
    if (!svc) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_DOES_NOT_EXIST) {
            error = L"OpenService failed: " + Win32Error(e);
            CloseServiceHandle(scm);
            LogLine(L"EnsureDriverService: OpenService failed: " + Win32Error(e));
            return false;
        }
        LogLine(L"EnsureDriverService: service does not exist; creating demand-start kernel service");
        svc = CreateServiceW(scm, kServiceName, L"Nvpwr GPU Power Control",
            access, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            sys.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            error = L"CreateService failed: " + Win32Error(GetLastError());
            CloseServiceHandle(scm);
            return false;
        }
    } else {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE,
            sys.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            error = L"ChangeServiceConfig failed: " + Win32Error(GetLastError());
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
        error = L"QueryServiceStatusEx failed: " + Win32Error(GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    if (forceRestart && (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING)) {
        CloseDeviceHandle();
        SERVICE_STATUS tmp{};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &tmp)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_NOT_ACTIVE) {
                error = L"Could not restart stale Nvpwr driver (stop failed): " + Win32Error(e);
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return false;
            }
        }
        if (!WaitForServiceState(svc, SERVICE_STOPPED, 5000, error)) {
            LogLine(L"EnsureDriverService: stop timeout/failure: " + error);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
        ssp.dwCurrentState = SERVICE_STOPPED;
        LogLine(L"EnsureDriverService: stale service stopped; new image can be loaded");
    }

    if (ssp.dwCurrentState != SERVICE_RUNNING) {
        if (ssp.dwCurrentState == SERVICE_STOP_PENDING) {
            if (!WaitForServiceState(svc, SERVICE_STOPPED, 5000, error)) {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return false;
            }
        }

        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING) {
                std::wstringstream ss;
                ss << L"StartService failed (" << e << L"): " << Win32Error(e)
                   << L"\r\n\r\nThis build is test-signed and requires TESTSIGNING ON with Secure Boot disabled.";
                error = ss.str();
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return false;
            }
        }
        if (!WaitForServiceState(svc, SERVICE_RUNNING, 5000, error)) {
            LogLine(L"EnsureDriverService: start timeout/failure: " + error);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    LogLine(L"EnsureDriverService: kernel service RUNNING");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool OpenDevice(std::wstring& error)
{
    if (g_device != INVALID_HANDLE_VALUE) return true;
    for (int i = 0; i < 30; ++i) {
        g_device = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_device != INVALID_HANDLE_VALUE) {
            LogLine(L"OpenDevice: opened \\\\.\\Nvpwr successfully");
            return true;
        }
        Sleep(100);
    }
    error = L"Cannot open \\\\.\\Nvpwr: " + Win32Error(GetLastError());
    LogLine(L"OpenDevice: FAILED: " + error);
    return false;
}

static std::wstring LocateAuxFile(const wchar_t* name)
{
    std::wstring dir = AppDirectory();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        std::wstring c = dir + L"\\" + name;
        if (IsRegularFile(c)) return c;
        std::wstring parent = ParentDirectory(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return L"";
}

static bool BootstrapDriver(std::wstring& error)
{
    LogLine(L"BootstrapDriver: begin");
    /* Always restart the demand-start service once when this GUI starts.
       Updating ImagePath alone does not replace an already loaded kernel image. */
    CloseDeviceHandle();
    if (!EnsureDriverService(error, true)) {
        LogLine(L"BootstrapDriver: service bootstrap FAILED: " + error);
        return false;
    }
    if (!OpenDevice(error)) {
        error += L"\r\n\r\nNvpwr service reached RUNNING but \\\\.\\Nvpwr is absent. Reboot once, then run NvpwrControl.exe as administrator.";
        return false;
    }
    LogLine(L"BootstrapDriver: SUCCESS");
    return true;
}

static const wchar_t* StateName(ULONG s)
{
    switch (s) {
    case NvpwrStateArmed: return L"ARMED / staging at OEM ceiling";
    case NvpwrStateApplied: return L"APPLIED";
    case NvpwrStateMixed: return L"MIXED - click Restore stock";
    case NvpwrStateWrongBuild: return L"UNSUPPORTED NVIDIA DRIVER";
    case NvpwrStateModuleNotFound: return L"nvlddmkm.sys not found";
    case NvpwrStateGpuNotFound: return L"GPU context not found";
    case NvpwrStateContextInvalid: return L"CONTEXT INVALID";
    case NvpwrStatePreconditionNotReady: return L"PRECONDITION NOT READY";
    case NvpwrStateStockBaseline: return L"OEM STOCK";
    default: return L"UNKNOWN";
    }
}

static std::wstring W(ULONG mw)
{
    if (mw == 0xFFFFFFFFu) return L"N/A";
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1) << (mw / 1000.0) << L" W";
    return ss.str();
}

static void LogStatusSnapshot(const wchar_t* tag, const NVPWR_STATUS& s)
{
    std::wstringstream ss;
    ss << tag
       << L": state=" << StateName(s.State) << L"(" << s.State << L")"
       << L" detail=" << s.Detail
       << L" nt=0x" << std::hex << std::uppercase << (ULONG)s.LastNtStatus
       << L" nv=0x" << s.LastNvStatus << std::dec
       << L" profile=" << s.ActiveProfile
       << L" oem=" << s.OemBaseline
       << L" base=" << s.CtgpTarget
       << L" amount=" << s.PpabAmount
       << L" upper=" << s.UpperBoundary
       << L" max=" << s.MaxEffective
       << L" current=" << s.CurrentEffective
       << L" f7=" << s.CurrentF7Value
       << L" predicted=" << s.PredictedF7
       << L" applied=" << s.AppliedTarget;
    LogLine(ss.str());
}

static bool Query(NVPWR_STATUS& s, std::wstring& error)
{
    DWORD got = 0;
    ZeroMemory(&s, sizeof(s));
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_STATUS, nullptr, 0, &s, sizeof(s), &got, nullptr) || got < sizeof(s)) {
        error = L"Status query failed: " + Win32Error(GetLastError());
        LogLine(L"Query: FAILED: " + error);
        return false;
    }
    return true;
}

static void RenderStatus(const NVPWR_STATUS& s)
{
    ULONG baselineW = s.OemBaseline ? (s.OemBaseline / 1000u) :
        ((s.State == NvpwrStateStockBaseline && s.UpperBoundary) ? (s.UpperBoundary / 1000u) : 0u);
    const bool xmg250Now = Xmg250MatchesSelectedProfile();
    if (g_targets.empty() || g_populatedProfile != g_profile ||
        (baselineW && baselineW != g_populatedBaselineWatts) ||
        g_populatedXmg250 != xmg250Now) {
        PopulateTargets(s);
        g_populatedProfile = g_profile;
        g_populatedBaselineWatts = baselineW;
        g_populatedXmg250 = xmg250Now;
    }

    std::wstringstream top;
    if (XmgIsFullTarget(g_xmg) && Xmg250MatchesSelectedProfile()) {
        top << L"APPLIED — 250.0 W coherent 19-target";
    } else {
        top << StateName(s.State);
        if (s.State == NvpwrStateApplied) top << L"  —  " << W(s.AppliedTarget);
    }
    UiSetText(g_state, top.str().c_str());

    ULONG baselineDisplay = s.OemBaseline ? s.OemBaseline :
        ((s.State == NvpwrStateStockBaseline) ? s.UpperBoundary : 0xFFFFFFFFu);

    std::wstringstream ss;
    ss << L"GPU:                  " << g_gpuName << L"\r\n"
       << L"Profile:              " << ProfileName(g_profile) << L"\r\n"
       << L"OEM baseline:         " << W(baselineDisplay) << L"\r\n"
       << L"Supported KMD:        616.92\r\n"
       << L"Detected timestamp:   0x" << std::hex << std::uppercase << s.TimeDateStamp
       << L"    image: 0x" << s.SizeOfImage << std::dec << L"\r\n\r\n"
       << L"Base / F7 input:      " << W(s.CtgpTarget) << L"\r\n"
       << L"Dynamic amount:       " << W(s.PpabAmount) << L"\r\n"
       << L"Upper ceiling:        " << W(s.UpperBoundary) << L"\r\n"
       << L"MAX effective:        " << W(s.MaxEffective) << L"\r\n"
       << L"Current effective:    " << W(s.CurrentEffective) << L"\r\n"
       << L"Current F7:           " << W(s.CurrentF7Value) << L"\r\n"
       << L"Predicted F7:         " << W(s.PredictedF7) << L"\r\n\r\n"
       << L"NVIDIA status: 0x" << std::hex << std::uppercase << s.LastNvStatus
       << L"    NTSTATUS: 0x" << (ULONG)s.LastNtStatus << std::dec << L"\r\n"
       << L"Flags init/elig/amount: " << (int)s.RootInitialized << L" / "
       << (int)s.Eligibility << L" / " << (int)s.AmountActive;

    ss << XmgFormatRuntime(g_xmg);
    if (g_lastXmgTx.Operation != 0u || !g_lastXmgTx.Error.empty())
        ss << L"\r\n" << XmgFormatTransaction(g_lastXmgTx) << L"\r\n";
    ss << FormatNvapiProbe(g_nvapi);
    ss << FormatNvapiTuner(g_tuner);
    UiSetText(g_status, ss.str().c_str());
    UiSummary(s.CurrentEffective, baselineDisplay, s.State);

    const BOOL readable = (s.State != NvpwrStateWrongBuild &&
        s.State != NvpwrStateModuleNotFound &&
        s.State != NvpwrStateGpuNotFound &&
        s.State != NvpwrStateContextInvalid);
    BOOL profileReady = FALSE;
    if (g_profile == NvpwrProfileRtx5050Laptop || g_profile == NvpwrProfileRtx5060Laptop ||
        g_profile == NvpwrProfileRtx5070Laptop) {
        profileReady = (baselineW == 115);
    } else if (g_profile == NvpwrProfileRtx5070TiLaptop) {
        profileReady = (baselineW == 140);
    } else if (g_profile == NvpwrProfileRtx5080Laptop || g_profile == NvpwrProfileRtx5090Laptop) {
        profileReady = (baselineW >= 150 && baselineW <= 175);
    } else if (g_profile == NvpwrProfileRtx4090Laptop || g_profile == NvpwrProfileRtx4080Laptop) {
        profileReady = (baselineW >= 115 && baselineW <= 175);
    } else if (g_profile == NvpwrProfileRtx4070Laptop || g_profile == NvpwrProfileRtx4060Laptop) {
        profileReady = (baselineW >= 95 && baselineW <= 140);
    } else if (g_profile == NvpwrProfileRtx4050Laptop) {
        profileReady = (baselineW >= 75 && baselineW <= 140);
    }
    EnableWindow(g_apply, readable && profileReady);
    EnableWindow(g_restore, readable);
}

static void Refresh(HWND owner, bool showError)
{
    std::wstring error;
    if (g_device == INVALID_HANDLE_VALUE && !BootstrapDriver(error)) {
        UiSetText(g_state, L"Power helper unavailable / NvAPI tuner still available");
        EnableWindow(g_apply, FALSE);
        EnableWindow(g_restore, FALSE);
        RefreshExternalProbes();
        std::wstring details = error + FormatNvapiProbe(g_nvapi) + FormatNvapiTuner(g_tuner);
        UiSetText(g_status, details.c_str());
        return;
    }
    NVPWR_STATUS s{};
    if (!Query(s, error)) {
        UiSetText(g_state, L"Power status error / NvAPI tuner still available");
        RefreshExternalProbes();
        std::wstring details = error + FormatNvapiProbe(g_nvapi) + FormatNvapiTuner(g_tuner);
        UiSetText(g_status, details.c_str());
        if (showError) UiMessage(owner, error.c_str(), L"Nvpwr", MB_ICONERROR);
        return;
    }
    LogStatusSnapshot(L"Refresh", s);
    RefreshExternalProbes();
    RenderStatus(s);
}

static bool SendTarget(HWND owner, ULONG watts)
{
    LogLine(L"SendTarget: request " + std::to_wstring(watts) + L" W profile=" + std::to_wstring(g_profile));
    NVPWR_SET_POWER req{};
    req.Version = NVPWR_SET_VERSION;
    req.TargetMilliwatts = watts * 1000u;
    req.Profile = g_profile;
    req.Reserved = 0;
    DWORD got = 0;
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_SET_POWER, &req, sizeof(req), nullptr, 0, &got, nullptr)) {
        DWORD e = GetLastError();
        std::wstring msg = L"Set power failed (" + std::to_wstring(e) + L"): " + Win32Error(e) +
            L"\r\n\r\nNo further write is attempted. Use Restore stock or reboot if status is mixed.";
        LogLine(L"SendTarget: FAILED Win32=" + std::to_wstring(e) + L" " + Win32Error(e));
        UiMessage(owner, msg.c_str(), L"Nvpwr", MB_ICONERROR);
        Refresh(owner, false);
        return false;
    }
    LogLine(L"SendTarget: IOCTL accepted by driver; refreshing readback");
    Refresh(owner, true);
    return true;
}

static bool RestoreNvpwrOnlyRaw(HWND owner, bool showError)
{
    if (g_device == INVALID_HANDLE_VALUE) {
        if (showError) UiMessage(owner, L"Nvpwr driver is not open.", L"Nvpwr", MB_ICONERROR);
        return false;
    }

    NVPWR_STATUS before{};
    std::wstring qerr;
    if (Query(before, qerr) && before.State == NvpwrStateStockBaseline) {
        LogLine(L"RestoreNvpwrOnlyRaw: already OEM stock");
        return true;
    }

    DWORD got = 0;
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_RESTORE, nullptr, 0, nullptr, 0, &got, nullptr)) {
        DWORD e = GetLastError();
        if (showError) {
            std::wstring msg = L"Nvpwr restore failed (" + std::to_wstring(e) + L"): " + Win32Error(e) +
                L"\r\n\r\nIf this is an unrecognized external state, reboot Windows to reconstruct stock NVIDIA policy.";
            UiMessage(owner, msg.c_str(), L"Nvpwr", MB_ICONERROR);
        }
        LogLine(L"RestoreNvpwrOnlyRaw: FAILED Win32=" + std::to_wstring(e) + L" " + Win32Error(e));
        return false;
    }
    LogLine(L"RestoreNvpwrOnlyRaw: completed successfully");
    return true;
}

static bool RestoreStockRaw(HWND owner, bool showError)
{
    LogLine(L"RestoreStockRaw: coherent restore request");

    // Never let the narrow Nvpwr restore write over an active XMG FULL_TARGET.
    // Entry13/Entry14 must be rolled back by the same coherent backend that
    // owns the 19-target contract.
    XmgRuntimeSummary xr{};
    const bool xmgQueryOk = XmgQueryRuntime(xr);
    if (!xmgQueryOk && g_coherent250KnownApplied) {
        if (showError) UiMessage(owner,
            L"The coherent 250 W state was previously verified, but XMGPowerPatch is no longer queryable.\r\n"
            L"Fail-closed: Nvpwr restore is blocked. Re-enable the XMG runtime or reboot Windows.",
            L"Nvpwr", MB_ICONERROR);
        return false;
    }
    if (xmgQueryOk && xr.ProtocolOk) {
        g_xmg = xr;
        if (g_coherent250KnownApplied && XmgHasCoherent250Capability(xr) &&
            !(XmgIsFullStock(xr) || XmgIsFullTarget(xr) || XmgIsLegacyCoreTarget(xr))) {
            if (showError) UiMessage(owner,
                L"A previously verified coherent state is now INVALID/MIXED.\r\n"
                L"Fail-closed: narrow Nvpwr restore is blocked; use the coherent backend or reboot.",
                L"Nvpwr", MB_ICONERROR);
            return false;
        }
        if (XmgHasValidated250WContract(xr)) {
            if (XmgIsFullTarget(xr) || XmgIsLegacyCoreTarget(xr)) {
                UiSetText(g_state, L"Rolling back coherent 250 W contract...");
                UpdateWindow(owner);
                XmgRuntimeSummary post{};
                XmgTransactionSummary tx{};
                const bool ok = XmgRollbackCoherent250W(tx, post);
                g_lastXmgTx = tx;
                LogLine(XmgFormatTransaction(tx));
                if (!ok) {
                    if (showError) {
                        std::wstring msg = L"Coherent XMG rollback failed.\r\n\r\n" + tx.Error +
                            L"\r\nNo Nvpwr write will be attempted over this state.";
                        UiMessage(owner, msg.c_str(), L"Nvpwr", MB_ICONERROR);
                    }
                    return false;
                }
                g_xmg = post;
                g_coherent250KnownApplied = false;
            } else if (!XmgIsFullStock(xr)) {
                if (showError) UiMessage(owner,
                    L"XMG resolver reports an INVALID/MIXED power state.\r\n"
                    L"Fail-closed: Nvpwr restore is blocked to avoid a partial Core/Entry13/Entry14 state.",
                    L"Nvpwr", MB_ICONERROR);
                LogLine(L"RestoreStockRaw: blocked by XMG mixed/partial state");
                return false;
            }
        }
    }

    return RestoreNvpwrOnlyRaw(owner, showError);
}

static bool RestoreStock(HWND owner)
{
    UiSetText(g_state, L"Restoring OEM stock...");
    UpdateWindow(owner);
    bool ok = RestoreStockRaw(owner, true);
    Refresh(owner, ok);
    return ok;
}

static bool ApplyCoherent250(HWND owner)
{
    LogLine(L"ApplyCoherent250: requested");
    RefreshExternalProbes();

    if (!Xmg250MatchesSelectedProfile()) {
        UiMessage(owner,
            L"The XMG v8 coherent 250 W capability is not available for this exact GPU/platform/runtime.\r\n"
            L"No fallback 250 W MAX/UPPER/F7 write will be attempted.",
            L"Nvpwr", MB_ICONERROR);
        LogLine(L"ApplyCoherent250: capability/profile match failed");
        return false;
    }

    if (XmgIsFullTarget(g_xmg)) {
        g_coherent250KnownApplied = true;
        LogLine(L"ApplyCoherent250: already FULL_TARGET");
        Refresh(owner, false);
        return true;
    }

    // A legacy XMG core-target state is owned by the coherent backend. Bring
    // it back to FULL_STOCK before a new complete 19-target Apply.
    if (XmgIsLegacyCoreTarget(g_xmg)) {
        XmgTransactionSummary rtx{};
        XmgRuntimeSummary rpost{};
        if (!XmgRollbackCoherent250W(rtx, rpost)) {
            g_lastXmgTx = rtx;
            LogLine(XmgFormatTransaction(rtx));
            UiMessage(owner, L"Legacy coherent state could not be rolled back to FULL_STOCK.", L"Nvpwr", MB_ICONERROR);
            return false;
        }
        g_lastXmgTx = rtx;
        g_xmg = rpost;
        g_coherent250KnownApplied = false;
    }

    // If XMG sees an invalid envelope while our own Nvpwr helper reports a
    // known active/staged state, the invalidity can be caused by the narrow
    // Nvpwr target itself. Restore only Nvpwr first, then require XMG to resolve
    // a clean FULL_STOCK contract before any coherent writer is called.
    NVPWR_STATUS ns{};
    std::wstring err;
    const bool nvpwrReadable = Query(ns, err);
    if (!XmgIsFullStock(g_xmg)) {
        const bool knownNvpwrOwnedState = nvpwrReadable &&
            (ns.State == NvpwrStateApplied || ns.State == NvpwrStateArmed || ns.State == NvpwrStateMixed);
        if (!knownNvpwrOwnedState) {
            UiMessage(owner,
                L"XMG resolver reports an INVALID/MIXED state that cannot be attributed to the active Nvpwr helper.\r\n"
                L"Coherent Apply is blocked with zero new writes.",
                L"Nvpwr", MB_ICONERROR);
            return false;
        }
        UiSetText(g_state, L"Restoring Nvpwr OEM state before coherent transaction...");
        UpdateWindow(owner);
        if (!RestoreNvpwrOnlyRaw(owner, true)) return false;
        RefreshExternalProbes();
        if (!Xmg250MatchesSelectedProfile() || !XmgIsFullStock(g_xmg)) {
            UiMessage(owner,
                L"After Nvpwr normalization the XMG resolver did not establish FULL_STOCK.\r\n"
                L"Coherent Apply remains blocked.",
                L"Nvpwr", MB_ICONERROR);
            return false;
        }
    }

    UiSetText(g_state, L"Applying coherent 250 W / 19-target contract...");
    UpdateWindow(owner);

    XmgTransactionSummary tx{};
    XmgRuntimeSummary post{};
    const bool ok = XmgApplyCoherent250W(tx, post);
    g_lastXmgTx = tx;
    LogLine(XmgFormatTransaction(tx));
    if (!ok) {
        std::wstring msg = L"Coherent 250 W transaction failed.\r\n\r\n" + tx.Error +
            L"\r\n\r\nThe XMG backend performs CAS/readback and reverse rollback on partial failure.";
        UiMessage(owner, msg.c_str(), L"Nvpwr", MB_ICONERROR);
        Refresh(owner, false);
        return false;
    }
    g_xmg = post;
    g_coherent250KnownApplied = true;
    Refresh(owner, false);
    return true;
}

static bool ApplyDesired(HWND owner, ULONG watts)
{
    LogLine(L"ApplyDesired: selected " + std::to_wstring(watts) + L" W");
    UiSetText(g_state, watts == 0u ? L"Restoring OEM stock..." : L"Preparing power transition...");
    UpdateWindow(owner);

    if (watts == 0u) {
        bool ok = RestoreStockRaw(owner, true);
        Refresh(owner, ok);
        return ok;
    }

    if (watts == 250u &&
        (g_profile == NvpwrProfileRtx5080Laptop || g_profile == NvpwrProfileRtx5090Laptop)) {
        return ApplyCoherent250(owner);
    }

    // A normal Nvpwr target must never be layered over XMG FULL_TARGET or the
    // legacy core-only state. Roll the coherent contract back first.
    RefreshExternalProbes();
    if (g_coherent250KnownApplied && !g_xmg.ProtocolOk) {
        UiMessage(owner,
            L"A coherent 250 W state is known to be active, but the XMG runtime is unavailable.\r\n"
            L"Normal Nvpwr writes are blocked until coherent rollback or reboot.",
            L"Nvpwr", MB_ICONERROR);
        return false;
    }
    if (g_coherent250KnownApplied && XmgHasCoherent250Capability(g_xmg) &&
        !(XmgIsFullStock(g_xmg) || XmgIsFullTarget(g_xmg) || XmgIsLegacyCoreTarget(g_xmg))) {
        UiMessage(owner,
            L"The coherent power state is INVALID/MIXED. Normal Nvpwr writes are blocked.",
            L"Nvpwr", MB_ICONERROR);
        return false;
    }
    if (XmgHasValidated250WContract(g_xmg)) {
        if (XmgIsFullTarget(g_xmg) || XmgIsLegacyCoreTarget(g_xmg)) {
            if (!RestoreStockRaw(owner, true)) {
                Refresh(owner, false);
                return false;
            }
        } else if (!XmgIsFullStock(g_xmg)) {
            UiMessage(owner,
                L"XMG resolver reports an INVALID/MIXED state. Normal Nvpwr writes are blocked.",
                L"Nvpwr", MB_ICONERROR);
            return false;
        }
    }

    NVPWR_STATUS s{};
    std::wstring error;
    if (!Query(s, error)) {
        LogLine(L"ApplyDesired: preflight status query failed: " + error);
        UiMessage(owner, error.c_str(), L"Nvpwr", MB_ICONERROR);
        Refresh(owner, false);
        return false;
    }

    LogStatusSnapshot(L"ApplyDesired preflight", s);
    const ULONG target = watts * 1000u;
    if (s.State == NvpwrStateApplied && s.AppliedTarget == target) {
        Refresh(owner, false);
        return true;
    }

    /* A newly loaded Nvpwr.sys has no saved baseline from older proof helpers.
       Normalize any recognized non-stock state first, then apply from the saved OEM baseline. */
    if (s.State != NvpwrStateStockBaseline) {
        LogLine(L"ApplyDesired: live state is not OEM baseline; normalizing first");
        UiSetText(g_state, L"Normalizing to OEM stock first...");
        UpdateWindow(owner);
        if (!RestoreStockRaw(owner, true)) {
            Refresh(owner, false);
            return false;
        }
        ZeroMemory(&s, sizeof(s));
        error.clear();
        if (!Query(s, error) || s.State != NvpwrStateStockBaseline) {
            std::wstring msg = L"OEM stock state could not be verified after restore.";
            if (!error.empty()) msg += L"\r\n" + error;
            UiMessage(owner, msg.c_str(), L"Nvpwr", MB_ICONERROR);
            Refresh(owner, false);
            return false;
        }
    }

    std::wstring action = L"Applying " + std::to_wstring(watts) + L" W...";
    UiSetText(g_state, action.c_str());
    UpdateWindow(owner);
    return SendTarget(owner, watts);
}

static void SetDefaultFont(HWND h)
{
    UiFont(h);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        LogLine(L"WM_CREATE: GUI initialization started");
        auto label=[&](const wchar_t* text)->HWND {
            HWND h=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,nullptr,nullptr);
            UiFont(h);UiSetText(h,text);return h;
        };
        g_title=label(L"NVPWR CONTROL");UiFont(g_title,g_uiHeadingFont);
        g_subtitle=label(L"GPU power settings");
        g_language=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,0,0,0,0,hwnd,(HMENU)1010,nullptr,nullptr);
        SendMessageW(g_language,CB_ADDSTRING,0,(LPARAM)L"EN — English");
        SendMessageW(g_language,CB_ADDSTRING,0,(LPARAM)L"RU — Русский");
        SendMessageW(g_language,CB_SETCURSEL,g_uiRussian?1:0,0);UiFont(g_language);
        g_state=label(L"Starting driver...");UiFont(g_state,g_uiStateFont);
        const wchar_t* titles[]={L"Active limit",L"OEM baseline",L"Selected target"};
        const wchar_t* hints[]={L"Last successful status read",L"Existing application profile",L"Not applied by selection"};
        for(int i=0;i<3;++i) {
            g_summaryLabel[i]=label(titles[i]);g_summaryValue[i]=label(L"N/A");g_summaryHint[i]=label(hints[i]);
            UiFont(g_summaryValue[i],g_uiHeadingFont);
        }
        g_label=label(L"Target power");
        g_hint=label(L"Choose a target, then click Apply. Selecting a value alone does not apply it.");
        g_combo=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,hwnd,(HMENU)1001,nullptr,nullptr);
        UiComboMessage(g_combo,CB_ADDSTRING,0,(LPARAM)L"Detecting GPU / OEM baseline...");
        UiComboMessage(g_combo,CB_SETCURSEL,0,0);UiFont(g_combo);
        g_apply=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,0,0,0,0,hwnd,(HMENU)1002,nullptr,nullptr);
        g_restore=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1003,nullptr,nullptr);
        g_refresh=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1004,nullptr,nullptr);
        g_restart=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1005,nullptr,nullptr);
        g_driverHelp=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1006,nullptr,nullptr);
        for(HWND control:{g_apply,g_restore,g_refresh,g_restart,g_driverHelp})UiFont(control);
        UiSetText(g_apply,L"Apply");UiSetText(g_restore,L"Restore OEM");UiSetText(g_refresh,L"Refresh");
        UiSetText(g_restart,L"Restart Nvpwr driver");UiSetText(g_driverHelp,L"Driver mode help");

        g_tuneTitle=label(L"ADVANCED BLACKWELL TUNING");
        const wchar_t* tuneNames[]={L"Core offset",L"Memory offset",L"XBAR offset",L"MSVDD offset",L"NVVDD offset",L"GPC:XBAR ratio"};
        for(int i=0;i<6;++i) {
            g_tuneLabel[i]=label(tuneNames[i]);
            g_tuneEdit[i]=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,hwnd,(HMENU)(1110+i),nullptr,nullptr);
            UiFont(g_tuneEdit[i]);
            g_tuneRange[i]=label(L"N/A");
        }
        g_tuneApply=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1100,nullptr,nullptr);
        g_tuneReset=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1101,nullptr,nullptr);
        g_restartNvidia=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)1007,nullptr,nullptr);
        UiFont(g_tuneApply);UiFont(g_tuneReset);UiFont(g_restartNvidia);
        UiSetText(g_tuneApply,L"Apply tuning");UiSetText(g_tuneReset,L"Reset tuning");UiSetText(g_restartNvidia,L"Restart NVIDIA device");
        EnableWindow(g_tuneApply,FALSE);EnableWindow(g_tuneReset,FALSE);

        g_details=label(L"Technical details");
        g_status=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,0,0,0,0,hwnd,nullptr,nullptr,nullptr);UiFont(g_status);
        g_note=label(L"RTX 4090: 150-250 W. 4080: 150-225 W. 4050/4060/4070: 115-150 W. RTX 5050/5060/5070: 115-140 W. 5070 Ti: 145-180 W. 5080/5090: 175-225 W (250 W via XMG v8).");
        g_tuningInfo=label(L"Running capability probes...");
        g_footer=label(L"1.8.0 Unified Ada & Blackwell Tuner  /  RTX 40 & 50 Series TDP Unlock");
        UiLayout(hwnd);

        g_gpuName = DetectNvidiaGpuName();
        g_profile = DetectProfile(g_gpuName);
        LogLine(L"GPU detection: name='" + g_gpuName + L"' profile=" + std::to_wstring(g_profile));
        UiSetText(g_subtitle,g_gpuName.c_str());

        std::wstring err;
        if (!BootstrapDriver(err)) {
            LogLine(L"Startup: driver bootstrap FAILED: " + err);
            UiSetText(g_state, L"Driver could not be started");
            UiSetText(g_status, err.c_str());
            EnableWindow(g_apply, FALSE);
            EnableWindow(g_restore, FALSE);
            // Clock/voltage tuning is user-mode NvAPI and can still be probed
            // even when the optional Nvpwr power helper cannot load.
            RefreshExternalProbes();
        } else {
            LogLine(L"Startup: driver bootstrap succeeded; requesting initial status");
            Refresh(hwnd, false);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001:
            if(HIWORD(wParam)==CBN_SELCHANGE)UiSelection();
            return 0;
        case 1010:
            if(HIWORD(wParam)==CBN_SELCHANGE) {
                UiLanguageChanged(hwnd);
                if (g_tuningInfo) UiSetText(g_tuningInfo, TuningCapabilityText().c_str());
            }
            return 0;
        case 1002: {
            int sel = (int)UiComboMessage(g_combo, CB_GETCURSEL, 0, 0);
            if (sel < 0 || static_cast<size_t>(sel) >= g_targets.size()) return 0;
            ULONG watts = g_targets[(size_t)sel];
            LogLine(L"UI Apply clicked: selected=" + std::to_wstring(watts) + L" W");
            if (watts != 0) {
                const bool unverified =
                    (g_profile == NvpwrProfileRtx5050Laptop) ||
                    (g_profile == NvpwrProfileRtx5060Laptop) ||
                    (g_profile == NvpwrProfileRtx5070Laptop) ||
                    (g_profile == NvpwrProfileRtx5070TiLaptop && watts > 160) ||
                    (g_profile == NvpwrProfileRtx5080Laptop) ||
                    (g_profile == NvpwrProfileRtx5090Laptop) ||
                    (g_profile == NvpwrProfileRtx4090Laptop && watts > 175) ||
                    (g_profile == NvpwrProfileRtx4080Laptop && watts > 175) ||
                    (g_profile == NvpwrProfileRtx4070Laptop && watts > 140) ||
                    (g_profile == NvpwrProfileRtx4060Laptop && watts > 140) ||
                    (g_profile == NvpwrProfileRtx4050Laptop && watts > 115);
                std::wstring warn = L"Apply " + std::to_wstring(watts) + L" W?\r\n\r\n";
                if (watts == 250u && Xmg250MatchesSelectedProfile()) {
                    warn += L"COHERENT XMG v8 transaction: 7 Core/NVPCF + 6 Entry13 + 6 Entry14 targets. Each writer is guarded by CAS/readback and the backend performs reverse rollback on partial failure.\r\n\r\n";
                } else if (unverified) {
                    warn += L"EXPERIMENTAL / NOT LOAD-VALIDATED on this GPU profile. This can exceed the laptop OEM electrical and thermal design.\r\n\r\n";
                }
                warn += L"All power modifications can exceed the laptop OEM electrical/thermal design. Use only with adequate cooling and power delivery.";
                if (UiMessage(hwnd, warn.c_str(), L"Confirm experimental power target", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                    LogLine(L"UI Apply: user cancelled confirmation");
                    return 0;
                }
            }
            ApplyDesired(hwnd, watts);
            return 0;
        }
        case 1003:
            LogLine(L"UI Restore OEM clicked");
            RestoreStock(hwnd);
            return 0;
        case 1004:
            LogLine(L"UI Refresh clicked");
            UiSetText(g_state, L"Refreshing...");
            UpdateWindow(hwnd);
            Refresh(hwnd, true);
            return 0;
        case 1005: {
            LogLine(L"Manual Nvpwr helper restart requested");
            NVPWR_STATUS before{};
            std::wstring qerr;
            if (g_device != INVALID_HANDLE_VALUE && Query(before, qerr) && before.State != NvpwrStateStockBaseline) {
                LogLine(L"Restart gate: active/non-stock state detected; restoring OEM before unload");
                if (!RestoreStockRaw(hwnd, false)) {
                    UiMessage(hwnd, L"OEM restore failed. Nvpwr helper restart was refused. Reboot Windows to reconstruct OEM NVIDIA runtime state.", L"Nvpwr safety gate", MB_ICONERROR);
                    return 0;
                }
            }
            CloseDeviceHandle();
            std::wstring err;
            if (!EnsureDriverService(err, true) || !OpenDevice(err)) {
                LogLine(L"Driver restart failed: " + err);
                UiMessage(hwnd, err.c_str(), L"Nvpwr", MB_ICONERROR);
            } else {
                LogLine(L"Driver restart successful");
                Refresh(hwnd, false);
            }
            return 0;
        }
        case 1100: {
            NvapiTuneRequest req{};
            LONG v = 0; double ratio = 0.0;
            if (g_tuner.CoreMHz.Supported) { if (!ReadEditLong(g_tuneEdit[0], v)) { UiMessage(hwnd,L"Invalid Core offset.",L"Tuning",MB_ICONERROR); return 0; } req.SetCore=true; req.CoreMHz=v; }
            if (g_tuner.MemoryMHz.Supported) { if (!ReadEditLong(g_tuneEdit[1], v)) { UiMessage(hwnd,L"Invalid Memory offset.",L"Tuning",MB_ICONERROR); return 0; } req.SetMemory=true; req.MemoryMHz=v; }
            if (g_tuner.XbarWritable) { if (!ReadEditLong(g_tuneEdit[2], v)) { UiMessage(hwnd,L"Invalid XBAR offset.",L"Tuning",MB_ICONERROR); return 0; } req.SetXbar=true; req.XbarMHz=v; }
            if (g_tuner.MsvddWritable) { if (!ReadEditLong(g_tuneEdit[3], v)) { UiMessage(hwnd,L"Invalid MSVDD offset.",L"Tuning",MB_ICONERROR); return 0; } req.SetMsvdd=true; req.MsvddMv=v; }
            if (g_tuner.NvvddMv.Supported) { if (!ReadEditLong(g_tuneEdit[4], v)) { UiMessage(hwnd,L"Invalid NVVDD offset.",L"Tuning",MB_ICONERROR); return 0; } req.SetNvvdd=true; req.NvvddMv=v; }
            if (g_tuner.RatioWritable) { if (!ReadEditDouble(g_tuneEdit[5], ratio)) { UiMessage(hwnd,L"Invalid GPC:XBAR ratio.",L"Tuning",MB_ICONERROR); return 0; } req.SetRatio=true; req.GpcXbarRatio=ratio; }

            {
                std::wstringstream ls; ls << L"OC request: core=" << req.CoreMHz << L" mem=" << req.MemoryMHz
                    << L" xbar=" << req.XbarMHz << L" msvdd=" << req.MsvddMv << L" nvvdd=" << req.NvvddMv
                    << L" ratio=" << std::fixed << std::setprecision(4) << req.GpcXbarRatio; LogLine(ls.str());
            }
            if (UiMessage(hwnd,
                L"Apply clock/voltage tuning?\r\n\r\nEvery live structure is revalidated before SET and read back after SET. A failed stage triggers session-baseline rollback. GPU instability can still crash applications, the display driver, or Windows.",
                L"Confirm tuning", MB_OKCANCEL|MB_ICONWARNING) != IDOK) return 0;
            std::wstring terr; NvapiTunerState post{};
            if (!ApplyNvapiTuning(req, AllowSeparateMsvdd(), post, terr)) {
                g_tuner=post; UpdateTuningControls(); LogLine(L"OC Apply failed: "+terr);
                UiMessage(hwnd,(L"Tuning failed:\r\n"+terr).c_str(),L"Nvpwr tuning",MB_ICONERROR);
            } else {
                g_tuner=post; UpdateTuningControls(); LogLine(L"OC Apply successful");
                UiSetText(g_state,L"Tuning applied / readback verified");
            }
            Refresh(hwnd,false);
            return 0;
        }
        case 1101: {
            std::wstring terr; NvapiTunerState post{};
            if (!ResetNvapiTuning(AllowSeparateMsvdd(),post,terr)) {
                g_tuner=post; UpdateTuningControls(); LogLine(L"OC Reset failed: "+terr);
                UiMessage(hwnd,(L"Tuning reset failed:\r\n"+terr).c_str(),L"Nvpwr tuning",MB_ICONERROR);
            } else {
                g_tuner=post; UpdateTuningControls(); LogLine(L"OC Reset successful");
                UiSetText(g_state,L"Tuning restored to session baseline");
            }
            Refresh(hwnd,false);
            return 0;
        }
        case 1007: {
            if (UiMessage(hwnd,
                L"Restart the NVIDIA display device?\r\n\r\nThe app will first restore the Nvpwr OEM power state and the captured NvAPI tuning baseline. The display can go black while Windows disables/enables the GPU.",
                L"Restart NVIDIA device", MB_OKCANCEL|MB_ICONWARNING) != IDOK) return 0;
            if (g_device != INVALID_HANDLE_VALUE && !RestoreStockRaw(hwnd,true)) return 0;
            {
                NvapiTunerState post{}; std::wstring terr;
                if (!ResetNvapiTuning(AllowSeparateMsvdd(),post,terr)) {
                    UiMessage(hwnd,(L"NvAPI tuning restore failed; NVIDIA restart refused:\r\n"+terr).c_str(),L"Safety gate",MB_ICONERROR);
                    return 0;
                }
                g_tuner=post; UpdateTuningControls();
            }
            const std::wstring script=LocateAuxFile(L"restart-nvidia-device.ps1");
            if (script.empty()) { UiMessage(hwnd,L"restart-nvidia-device.ps1 was not found next to the project/app.",L"Nvpwr",MB_ICONERROR); return 0; }
            std::wstring params=L"-NoProfile -ExecutionPolicy Bypass -File \""+script+L"\" -Yes";
            HINSTANCE r=ShellExecuteW(hwnd,L"open",L"powershell.exe",params.c_str(),nullptr,SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) UiMessage(hwnd,L"Could not start the supported Windows PnP restart script.",L"Nvpwr",MB_ICONERROR);
            else LogLine(L"NVIDIA PnP restart script launched: "+script);
            return 0;
        }
        case 1006: {
            const wchar_t* help = g_uiRussian ?
                L"Nvpwr.sys — test-signed kernel driver. Программа не обходит Secure Boot/DSE. Для тестовой сборки: Secure Boot выключается вручную в UEFI, затем штатный Windows TESTSIGNING ON и перезагрузка. Кнопка Restart перезапускает только Nvpwr helper и перед выгрузкой требует OEM restore. Для полного сброса NVIDIA runtime используйте перезагрузку Windows или отдельный PnP restart script." :
                L"Nvpwr.sys is a test-signed kernel driver. The app does not bypass Secure Boot/DSE. For test builds: disable Secure Boot manually in UEFI, enable Windows TESTSIGNING using the supported BCD setting, then reboot. Restart only restarts the Nvpwr helper and requires OEM restore before unload. For a full NVIDIA runtime reset use a Windows reboot or the separate PnP restart script.";
            UiMessage(hwnd, help, L"Driver mode", MB_ICONINFORMATION);
            return 0;
        }
        }
        break;
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam) ? HIWORD(wParam) : LOWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        UiSetDpi(newDpi);
        if (suggested) {
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UiLayout(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        LogLine(L"DPI: WM_DPICHANGED -> " + std::to_wstring(newDpi));
        return 0;
    }
    case WM_SIZE:
        UiLayout(hwnd);InvalidateRect(hwnd,nullptr,TRUE);return 0;
    case WM_GETMINMAXINFO: {
        auto limits=reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x=UiScale(980);limits->ptMinTrackSize.y=UiScale(960);return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);
        RECT rect{};GetClientRect(hwnd,&rect);FillRect(dc,&rect,g_uiBackground);
        int width=MulDiv(rect.right,96,g_uiDpi),cardWidth=(width-80)/3;
        for(int i=0;i<3;++i) {
            int left=30+i*(cardWidth+10);
            RECT cardRect{UiScale(left),UiScale(158),UiScale(left+cardWidth),UiScale(282)};
            FillRect(dc,&cardRect,g_uiCard);
        }
        EndPaint(hwnd,&ps);return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc=reinterpret_cast<HDC>(wParam);HWND control=reinterpret_cast<HWND>(lParam);
        bool panel=control==g_status || msg==WM_CTLCOLORLISTBOX;
        for(int i=0;i<6;++i) if(control==g_tuneEdit[i] || control==g_tuneRange[i]) panel=true;
        COLORREF foreground=RGB(225,232,242);
        for(int i=0;i<3;++i)if(control==g_summaryLabel[i] || control==g_summaryValue[i] || control==g_summaryHint[i]) {
            panel=true;if(control!=g_summaryValue[i])foreground=RGB(172,187,207);
        }
        if(control==g_state)foreground=g_uiStateColor;
        if(control==g_hint || control==g_subtitle || control==g_note || control==g_tuningInfo || control==g_footer)foreground=RGB(170,185,205);
        SetTextColor(dc,foreground);SetBkColor(dc,panel?kUiCard:kUiBackground);
        return reinterpret_cast<LRESULT>(panel?g_uiCard:g_uiBackground);
    }
    case WM_DESTROY:
        LogLine(L"WM_DESTROY: closing GUI/device handle and owned XMG runtime session");
        XmgReleaseRuntimeIfOwned();
        CloseDeviceHandle();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnablePerMonitorDpiV2()
{
    /*
        WHERE: process startup, before any HWND is created.
        WHAT: request Per-Monitor DPI Awareness V2 dynamically from user32.
        WHY: the old SetProcessDPIAware() mode scales correctly only for the
             startup monitor.  Per-Monitor V2 lets WM_DPICHANGED resize the
             window/fonts when it is moved between 100/125/150/175/200% displays.
             Dynamic lookup keeps the project buildable against older SDK setups.
    */
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
        auto setContext = reinterpret_cast<SetDpiContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) {
            HANDLE kPerMonitorAwareV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
            if (setContext(kPerMonitorAwareV2)) {
                LogLine(L"DPI: Per-Monitor V2 enabled");
                return;
            }
        }
    }
    SetProcessDPIAware();
    LogLine(L"DPI: Per-Monitor V2 unavailable; using legacy system-DPI awareness");
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    ResolveLogPath();
    LogLine(L"============================================================");
    LogLine(L"NvpwrControl 1.8.0 Unified Blackwell Tuner starting");
    LogLine(L"Log file: " + g_logPath);
    EnablePerMonitorDpiV2();
    UiInit();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = g_uiBackground;
    wc.lpszClassName = L"NvpwrControlWindow";
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Nvpwr Control 1.8.0 — Unified Ada & Blackwell Laptop Tuner",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, UiScale(1050), UiScale(1040), nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 2;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogLine(L"NvpwrControl exiting code=" + std::to_wstring((int)msg.wParam));
    UiDestroy();
    return (int)msg.wParam;
}
