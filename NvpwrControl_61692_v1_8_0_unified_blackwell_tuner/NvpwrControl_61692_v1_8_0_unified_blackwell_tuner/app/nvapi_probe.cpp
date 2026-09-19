#include "nvapi_probe.h"
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace {
using QueryInterfaceFn = void* (__cdecl*)(ULONG);
using NvInitFn = LONG (__cdecl*)();
using EnumGpuFn = LONG (__cdecl*)(void**, ULONG*);
using GpuBufferFn = LONG (__cdecl*)(void*, void*);

constexpr ULONG NVAPI_INITIALIZE = 0x0150E828;
constexpr ULONG NVAPI_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
constexpr ULONG NVAPI_UNLOAD = 0xD22BDD7E;

struct Spec {
    const wchar_t* Name;
    ULONG Id;
    ULONG Version;
    ULONG Size;
    ULONG SeedOffset;
    ULONG SeedValue;
    bool Call;
};

void FillItem(NvapiProbeItem& dst, const Spec& s, QueryInterfaceFn qi, void* gpu)
{
    dst.Name = s.Name;
    dst.Id = s.Id;
    dst.Version = s.Version;
    dst.BufferSize = s.Size;
    void* p = qi(s.Id);
    dst.Present = p != nullptr;
    if (!p || !s.Call || !gpu) return;

    std::vector<unsigned char> b(s.Size, 0);
    if (b.size() >= 4) memcpy(b.data(), &s.Version, 4);
    if (s.SeedOffset != 0xffffffffu && s.SeedOffset + 4 <= b.size())
        memcpy(b.data() + s.SeedOffset, &s.SeedValue, 4);
    auto fn = reinterpret_cast<GpuBufferFn>(p);
    dst.Status = fn(gpu, b.data());
    dst.Called = true;
}

void AppendItem(std::wstringstream& ss, const NvapiProbeItem& i)
{
    ss << L"  " << i.Name << L" [0x" << std::hex << std::uppercase << i.Id << std::dec << L"]: "
       << (i.Present ? L"PRESENT" : L"MISSING");
    if (i.Called) ss << L", GET status=" << i.Status << L", ver=0x" << std::hex << i.Version << std::dec;
    ss << L"\r\n";
}
}

bool RunNvapiReadOnlyProbe(NvapiProbeSummary& out)
{
    out = {};
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) {
        out.Error = L"nvapi64.dll could not be loaded";
        return false;
    }
    out.NvapiLoaded = true;
    auto qi = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!qi) {
        out.Error = L"nvapi_QueryInterface is missing";
        FreeLibrary(nvapi);
        return false;
    }
    auto init = reinterpret_cast<NvInitFn>(qi(NVAPI_INITIALIZE));
    auto enumGpu = reinterpret_cast<EnumGpuFn>(qi(NVAPI_ENUM_PHYSICAL_GPUS));
    if (!init || !enumGpu) {
        out.Error = L"NvAPI initialize/enumeration interfaces are missing";
        FreeLibrary(nvapi);
        return false;
    }
    if (init() != 0) {
        out.Error = L"NvAPI_Initialize failed";
        FreeLibrary(nvapi);
        return false;
    }
    out.Initialized = true;

    void* gpus[64]{};
    ULONG count = 0;
    if (enumGpu(gpus, &count) != 0 || count == 0) {
        out.Error = L"NvAPI_EnumPhysicalGPUs returned no GPU";
        FreeLibrary(nvapi);
        return false;
    }
    out.GpuCount = count;
    void* gpu = gpus[0];

    // Versions/buffer sizes below come from the read-only mVolt-compatible
    // capture on the reference 616.92 RTX 5070 Ti machine. Any NVAPI error is
    // treated as a capability-probe failure; no SET function is called here.
    const Spec pstGet { L"Pstates20 GET (core/memory/NVVDD)", 0x6FF81213, 0x00021CF8, 7416, 0xffffffffu, 0, true };
    const Spec pstSet { L"Pstates20 SET", 0x0F4DAE6B, 0, 0, 0xffffffffu, 0, false };
    const Spec domInfo{ L"Clock domains info", 0x57B5A5DF, 0x000486AC, 34476, 0xffffffffu, 0, true };
    const Spec domGet { L"Clock domains control GET (XBAR/MSVDD)", 0xF58938F5, 0x000261A4, 24996, 8, 2, true };
    const Spec domSet { L"Clock domains control SET", 0xD14B69CF, 0, 0, 0xffffffffu, 0, false };
    const Spec propInfo{ L"GPC:XBAR propagation info", 0xE826E4F0, 0x00015798, 24576, 0xffffffffu, 0, true };
    const Spec propGet{ L"GPC:XBAR propagation control GET", 0xCBFF71D0, 0x0001075C, 8192, 4, 0x3FFF, true };
    const Spec propSet{ L"GPC:XBAR propagation control SET", 0xEF3D20EA, 0, 0, 0xffffffffu, 0, false };
    const Spec vfInfo{ L"V/F points info", 0x507B4B59, 0x0001182C, 6188, 0xffffffffu, 0, true };
    const Spec vfGet{ L"V/F points control GET", 0x23F1B133, 0x00012420, 9248, 0xffffffffu, 0, false };
    const Spec vfSet{ L"V/F points control SET", 0x0733E009, 0, 0, 0xffffffffu, 0, false };
    const Spec adcInfo{ L"ADC/rail info", 0x68789E2A, 0x000209F0, 2544, 0xffffffffu, 0, true };
    const Spec adcStatus{ L"ADC/rail status", 0x43D9B26A, 0, 0, 0xffffffffu, 0, false };

    FillItem(out.Pstates20Get, pstGet, qi, gpu);
    FillItem(out.Pstates20Set, pstSet, qi, gpu);
    FillItem(out.ClockDomainsInfo, domInfo, qi, gpu);
    FillItem(out.ClockDomainsControlGet, domGet, qi, gpu);
    FillItem(out.ClockDomainsControlSet, domSet, qi, gpu);
    FillItem(out.PropInfo, propInfo, qi, gpu);
    FillItem(out.PropControlGet, propGet, qi, gpu);
    FillItem(out.PropControlSet, propSet, qi, gpu);
    FillItem(out.VfInfo, vfInfo, qi, gpu);
    FillItem(out.VfControlGet, vfGet, qi, gpu);
    FillItem(out.VfControlSet, vfSet, qi, gpu);
    FillItem(out.AdcInfo, adcInfo, qi, gpu);
    FillItem(out.AdcStatus, adcStatus, qi, gpu);

    auto unload = reinterpret_cast<NvInitFn>(qi(NVAPI_UNLOAD));
    if (unload) unload();
    FreeLibrary(nvapi);
    return true;
}

std::wstring FormatNvapiProbe(const NvapiProbeSummary& s)
{
    std::wstringstream ss;
    ss << L"\r\n=== NVIDIA private-NvAPI read-only capability probe ===\r\n";
    if (!s.NvapiLoaded || !s.Initialized) {
        ss << L"NvAPI: unavailable";
        if (!s.Error.empty()) ss << L" — " << s.Error;
        ss << L"\r\n";
        return ss.str();
    }
    ss << L"NvAPI: initialized, physical GPUs=" << s.GpuCount << L"\r\n";
    AppendItem(ss, s.Pstates20Get);
    AppendItem(ss, s.Pstates20Set);
    AppendItem(ss, s.ClockDomainsInfo);
    AppendItem(ss, s.ClockDomainsControlGet);
    AppendItem(ss, s.ClockDomainsControlSet);
    AppendItem(ss, s.PropInfo);
    AppendItem(ss, s.PropControlGet);
    AppendItem(ss, s.PropControlSet);
    AppendItem(ss, s.VfInfo);
    AppendItem(ss, s.VfControlGet);
    AppendItem(ss, s.VfControlSet);
    AppendItem(ss, s.AdcInfo);
    ss << L"SET interfaces are presence-checked only; this probe never calls them.\r\n";
    return ss.str();
}
