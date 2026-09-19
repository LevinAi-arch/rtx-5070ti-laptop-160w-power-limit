#pragma once
#include <Windows.h>
#include <string>

struct NvapiProbeItem {
    const wchar_t* Name = L"";
    ULONG Id = 0;
    bool Present = false;
    bool Called = false;
    LONG Status = 0x7fffffff;
    ULONG Version = 0;
    ULONG BufferSize = 0;
};

struct NvapiProbeSummary {
    bool NvapiLoaded = false;
    bool Initialized = false;
    ULONG GpuCount = 0;
    std::wstring Error;

    NvapiProbeItem Pstates20Get{};
    NvapiProbeItem Pstates20Set{};
    NvapiProbeItem ClockDomainsInfo{};
    NvapiProbeItem ClockDomainsControlGet{};
    NvapiProbeItem ClockDomainsControlSet{};
    NvapiProbeItem PropInfo{};
    NvapiProbeItem PropControlGet{};
    NvapiProbeItem PropControlSet{};
    NvapiProbeItem VfInfo{};
    NvapiProbeItem VfControlGet{};
    NvapiProbeItem VfControlSet{};
    NvapiProbeItem AdcInfo{};
    NvapiProbeItem AdcStatus{};
};

bool RunNvapiReadOnlyProbe(NvapiProbeSummary& out);
std::wstring FormatNvapiProbe(const NvapiProbeSummary& s);
