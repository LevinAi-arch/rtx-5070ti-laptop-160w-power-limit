#include "nvapi_tuner.h"
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>

namespace {
using QueryInterfaceFn = void* (__cdecl*)(ULONG);
using NvInitFn = LONG (__cdecl*)();
using EnumGpuFn = LONG (__cdecl*)(void**, ULONG*);
using GpuBufferFn = LONG (__cdecl*)(void*, void*);

constexpr ULONG NVAPI_INITIALIZE = 0x0150E828;
constexpr ULONG NVAPI_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
constexpr ULONG NVAPI_UNLOAD = 0xD22BDD7E;
constexpr LONG NVAPI_OK = 0;

constexpr ULONG ID_PSTATES_GET = 0x6FF81213;
constexpr ULONG ID_PSTATES_SET = 0x0F4DAE6B;
constexpr ULONG PSTATES_VERSION = 0x00021CF8;
constexpr size_t PSTATES_SIZE = 7416;
constexpr ULONG PSTATES_SET_V1_VERSION = 0x00011C94;
constexpr size_t PSTATES_SET_V1_SIZE = 7316;
constexpr size_t PSTATES_HEADER = 20;
constexpr size_t PSTATES_COUNT = 16;
constexpr size_t PSTATE_SIZE = 456;
constexpr size_t PSTATE_CLOCKS = 8;
constexpr size_t CLOCK_SIZE = 44;
constexpr size_t CLOCKS_OFF = 8;
constexpr size_t VOLTAGES_OFF = 8 + PSTATE_CLOCKS * CLOCK_SIZE;
constexpr size_t VOLTAGE_SIZE = 24;
constexpr ULONG DOMAIN_GRAPHICS = 0;
constexpr ULONG DOMAIN_MEMORY = 4;
constexpr ULONG DOMAIN_CORE_VOLTAGE = 0;

constexpr ULONG ID_CLK_GET = 0xF58938F5;
constexpr ULONG ID_CLK_SET = 0xD14B69CF;
constexpr ULONG CLK_VERSION = 0x000261A4;
constexpr size_t CLK_BUFSIZE = 0x13000;
constexpr ULONG CLK_MASK = 0xFF;
constexpr ULONG CLK_MARKER = 0x0F;
constexpr size_t CLK_OFF_FREQ = 0x114;
constexpr size_t CLK_OFF_MSVDD = 0x11C;
constexpr ULONG ID_CLK_MEASURE = 0x527FC458;
constexpr ULONG CLK_MEASURE_VERSION = 0x0001000C;
constexpr ULONG XBAR_MEASURE_MASK = 0x2;

constexpr ULONG ID_PROP_INFO = 0xE826E4F0;
constexpr ULONG ID_PROP_GET = 0xCBFF71D0;
constexpr ULONG ID_PROP_SET = 0xEF3D20EA;
constexpr ULONG PROP_INFO_VERSION = 0x00015798;
constexpr ULONG PROP_CONTROL_VERSION = 0x0001075C;
constexpr size_t PROP_BUFSIZE = 0x20000;
constexpr ULONG PROP_MASK = 0xFF;
constexpr size_t PROP_CONTROL_FALLBACK_RATIO = 0x68; // base 0x64 + ratio +0x04
constexpr ULONG DEFAULT_RATIO_RAW = 0xE660;

constexpr ULONG ID_VF_INFO = 0x507B4B59;
constexpr ULONG ID_VF_CONTROL_GET = 0x23F1B133;
constexpr ULONG VF_INFO_VERSION = 0x0001182C;
constexpr ULONG VF_CONTROL_VERSION = 0x00012420;
constexpr size_t VF_INFO_SIZE = 6188;
constexpr size_t VF_CONTROL_SIZE = 9248;

constexpr ULONG ID_ADC_INFO = 0x68789E2A;
constexpr ULONG ID_ADC_STATUS = 0x43D9B26A;
constexpr ULONG ADC_INFO_VERSION = 0x000209F0;
constexpr ULONG ADC_STATUS_VERSION = 0x000109C8;
constexpr size_t ADC_INFO_SIZE = 2544;
constexpr size_t ADC_STATUS_SIZE = 2504;

struct NvSession {
    HMODULE Module = nullptr;
    QueryInterfaceFn QI = nullptr;
    void* Gpu = nullptr;
    NvInitFn Unload = nullptr;

    ~NvSession() {
        if (Unload) Unload();
        if (Module) FreeLibrary(Module);
    }

    bool Open(std::wstring& error) {
        Module = LoadLibraryW(L"nvapi64.dll");
        if (!Module) { error = L"nvapi64.dll could not be loaded"; return false; }
        QI = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(Module, "nvapi_QueryInterface"));
        if (!QI) { error = L"nvapi_QueryInterface is missing"; return false; }
        auto init = reinterpret_cast<NvInitFn>(QI(NVAPI_INITIALIZE));
        auto enumGpu = reinterpret_cast<EnumGpuFn>(QI(NVAPI_ENUM_PHYSICAL_GPUS));
        Unload = reinterpret_cast<NvInitFn>(QI(NVAPI_UNLOAD));
        if (!init || !enumGpu) { error = L"NvAPI initialize/enumeration interfaces are missing"; return false; }
        if (init() != NVAPI_OK) { error = L"NvAPI_Initialize failed"; return false; }
        void* gpus[64]{}; ULONG count = 0;
        if (enumGpu(gpus, &count) != NVAPI_OK || count == 0) { error = L"NvAPI_EnumPhysicalGPUs returned no GPU"; return false; }
        Gpu = gpus[0];
        return true;
    }

    GpuBufferFn Fn(ULONG id) const {
        return QI ? reinterpret_cast<GpuBufferFn>(QI(id)) : nullptr;
    }
};

static ULONG U32(const std::vector<unsigned char>& b, size_t off) {
    ULONG v = 0; if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4); return v;
}
static LONG I32(const std::vector<unsigned char>& b, size_t off) {
    LONG v = 0; if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4); return v;
}
static void PutU32(std::vector<unsigned char>& b, size_t off, ULONG v) {
    if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
}
static void PutI32(std::vector<unsigned char>& b, size_t off, LONG v) {
    if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
}

static LONG KHzToMHz(LONG khz) { return khz / 1000; }
static LONG MHzToKHz(LONG mhz) {
    const long long v = static_cast<long long>(mhz) * 1000ll;
    if (v > 0x7fffffffll) return 0x7fffffff;
    if (v < static_cast<long long>(INT_MIN)) return INT_MIN;
    return static_cast<LONG>(v);
}
static LONG UvToMv(LONG uv) { return uv / 1000; }
static LONG MvToUv(LONG mv) {
    const long long v = static_cast<long long>(mv) * 1000ll;
    if (v > 0x7fffffffll) return 0x7fffffff;
    if (v < static_cast<long long>(INT_MIN)) return INT_MIN;
    return static_cast<LONG>(v);
}

struct PstatesFields {
    bool Core = false; size_t CoreDelta = 0; LONG CoreCur = 0, CoreMin = 0, CoreMax = 0;
    bool Memory = false; size_t MemDelta = 0; LONG MemCur = 0, MemMin = 0, MemMax = 0;
    bool Nvvdd = false; size_t VoltDelta = 0; LONG VoltCur = 0, VoltMin = 0, VoltMax = 0;
};

static bool ReadPstates(NvSession& s, std::vector<unsigned char>& b, PstatesFields& f, std::wstring& error) {
    auto get = s.Fn(ID_PSTATES_GET);
    if (!get) { error = L"Pstates20 GET interface missing"; return false; }
    b.assign(PSTATES_SIZE, 0); PutU32(b, 0, PSTATES_VERSION);
    LONG rc = get(s.Gpu, b.data());
    if (rc != NVAPI_OK) { error = L"Pstates20 GET failed rc=" + std::to_wstring(rc); return false; }
    if (U32(b, 0) != PSTATES_VERSION) { error = L"Pstates20 version/layout mismatch"; return false; }
    const ULONG np = std::min<ULONG>(U32(b, 8), static_cast<ULONG>(PSTATES_COUNT));
    const ULONG nc = std::min<ULONG>(U32(b, 12), static_cast<ULONG>(PSTATE_CLOCKS));
    const ULONG nv = std::min<ULONG>(U32(b, 16), 4u);
    size_t p0 = SIZE_MAX;
    for (ULONG p = 0; p < np; ++p) {
        size_t po = PSTATES_HEADER + static_cast<size_t>(p) * PSTATE_SIZE;
        if (po + PSTATE_SIZE > b.size()) break;
        if (U32(b, po) == 0u) { p0 = po; break; }
    }
    if (p0 == SIZE_MAX) { error = L"Pstates20 P0 record was not found"; return false; }
    f = {};
    for (ULONG c = 0; c < nc; ++c) {
        const size_t co = p0 + CLOCKS_OFF + static_cast<size_t>(c) * CLOCK_SIZE;
        if (co + CLOCK_SIZE > b.size()) break;
        const ULONG domain = U32(b, co);
        const LONG cur = I32(b, co + 12), mn = I32(b, co + 16), mx = I32(b, co + 20);
        if (mn > mx) continue;
        if (domain == DOMAIN_GRAPHICS) {
            f.Core = true; f.CoreDelta = co + 12; f.CoreCur = cur; f.CoreMin = mn; f.CoreMax = mx;
        } else if (domain == DOMAIN_MEMORY) {
            f.Memory = true; f.MemDelta = co + 12; f.MemCur = cur; f.MemMin = mn; f.MemMax = mx;
        }
    }
    for (ULONG v = 0; v < nv; ++v) {
        const size_t vo = p0 + VOLTAGES_OFF + static_cast<size_t>(v) * VOLTAGE_SIZE;
        if (vo + VOLTAGE_SIZE > b.size()) break;
        const ULONG domain = U32(b, vo);
        const LONG cur = I32(b, vo + 12), mn = I32(b, vo + 16), mx = I32(b, vo + 20);
        if (domain == DOMAIN_CORE_VOLTAGE && mn <= mx && (mn != 0 || mx != 0)) {
            f.Nvvdd = true; f.VoltDelta = vo + 12; f.VoltCur = cur; f.VoltMin = mn; f.VoltMax = mx;
            break;
        }
    }
    return true;
}

static bool SetPstatesBuffer(NvSession& s, std::vector<unsigned char>& b, std::wstring& error) {
    auto set = s.Fn(ID_PSTATES_SET);
    if (!set) { error = L"Pstates20 SET interface missing"; return false; }

    // NvAPI's SET path is intentionally built as a minimal V1 request rather
    // than replaying the entire GET/V2 status blob.  This mirrors the common
    // SetPstates20 contract: one P0 record, only the clock domains/voltage
    // delta that we intend to control, all reserved fields zero.
    PstatesFields f{};
    const ULONG np = std::min<ULONG>(U32(b, 8), static_cast<ULONG>(PSTATES_COUNT));
    const ULONG nc = std::min<ULONG>(U32(b, 12), static_cast<ULONG>(PSTATE_CLOCKS));
    const ULONG nv = std::min<ULONG>(U32(b, 16), 4u);
    size_t p0 = SIZE_MAX;
    for (ULONG p = 0; p < np; ++p) {
        size_t po = PSTATES_HEADER + static_cast<size_t>(p) * PSTATE_SIZE;
        if (po + PSTATE_SIZE <= b.size() && U32(b, po) == 0u) { p0 = po; break; }
    }
    if (p0 == SIZE_MAX) { error = L"Pstates20 SET: P0 record missing"; return false; }
    for (ULONG c = 0; c < nc; ++c) {
        size_t co = p0 + CLOCKS_OFF + static_cast<size_t>(c) * CLOCK_SIZE;
        if (co + CLOCK_SIZE > b.size()) break;
        ULONG d = U32(b, co);
        if (d == DOMAIN_GRAPHICS) { f.Core = true; f.CoreCur = I32(b, co + 12); }
        if (d == DOMAIN_MEMORY) { f.Memory = true; f.MemCur = I32(b, co + 12); }
    }
    for (ULONG v = 0; v < nv; ++v) {
        size_t vo = p0 + VOLTAGES_OFF + static_cast<size_t>(v) * VOLTAGE_SIZE;
        if (vo + VOLTAGE_SIZE > b.size()) break;
        if (U32(b, vo) == DOMAIN_CORE_VOLTAGE) { f.Nvvdd = true; f.VoltCur = I32(b, vo + 12); break; }
    }

    std::vector<unsigned char> setb(PSTATES_SET_V1_SIZE, 0);
    PutU32(setb, 0, PSTATES_SET_V1_VERSION);
    PutU32(setb, 8, 1); // one P-state: P0
    ULONG clockCount = 0;
    if (f.Core) {
        size_t co = PSTATES_HEADER + CLOCKS_OFF + static_cast<size_t>(clockCount++) * CLOCK_SIZE;
        PutU32(setb, co, DOMAIN_GRAPHICS); PutI32(setb, co + 12, f.CoreCur);
    }
    if (f.Memory) {
        size_t co = PSTATES_HEADER + CLOCKS_OFF + static_cast<size_t>(clockCount++) * CLOCK_SIZE;
        PutU32(setb, co, DOMAIN_MEMORY); PutI32(setb, co + 12, f.MemCur);
    }
    PutU32(setb, 12, clockCount);
    if (f.Nvvdd) {
        PutU32(setb, 16, 1);
        size_t vo = PSTATES_HEADER + VOLTAGES_OFF;
        PutU32(setb, vo, DOMAIN_CORE_VOLTAGE); PutI32(setb, vo + 12, f.VoltCur);
    }
    LONG rc = set(s.Gpu, setb.data());
    if (rc != NVAPI_OK) { error = L"Pstates20 SET failed rc=" + std::to_wstring(rc); return false; }
    return true;
}

static bool FindRepeatingDwordLayout(const std::vector<unsigned char>& b, ULONG marker, size_t& base, size_t& stride) {
    std::vector<size_t> hits;
    for (size_t off = 0x100; off + 4 <= b.size(); off += 4) if (U32(b, off) == marker) hits.push_back(off);
    // Prefer the audited Blackwell entry stride when it is visible in the
    // returned buffer. Resolve the *start* of a repeating run and require one
    // unique run; accepting the first incidental pair would not be fail-closed.
    std::vector<size_t> auditedRuns;
    for (size_t h : hits) {
        if (h >= 0x304 && U32(b, h - 0x304) == marker) continue; // not a run start
        size_t count = 1;
        for (size_t n = h + 0x304; n + 4 <= b.size(); n += 0x304) {
            if (U32(b, n) == marker) ++count; else break;
        }
        if (count >= 2) auditedRuns.push_back(h);
    }
    if (auditedRuns.size() == 1) { base = auditedRuns[0]; stride = 0x304; return true; }
    if (auditedRuns.size() > 1) return false;
    size_t bestCount = 0, bestBase = 0, bestStride = 0;
    for (size_t i = 0; i + 1 < hits.size(); ++i) {
        const size_t candidate = hits[i + 1] - hits[i];
        if (candidate < 0x40 || candidate > 0x1000) continue;
        size_t count = 0;
        for (size_t h : hits) if (h >= hits[i] && ((h - hits[i]) % candidate) == 0) ++count;
        if (count > bestCount) { bestCount = count; bestBase = hits[i]; bestStride = candidate; }
    }
    if (bestCount < 2) return false;
    base = bestBase; stride = bestStride; return true;
}

struct XbarFields { size_t Base = 0, Stride = 0; ULONG Index = 1; LONG FreqKHz = 0, MsvddUv = 0; };
static bool ReadXbar(NvSession& s, std::vector<unsigned char>& b, XbarFields& f, std::wstring& error) {
    auto get = s.Fn(ID_CLK_GET);
    auto set = s.Fn(ID_CLK_SET);
    if (!get || !set) { error = L"XBAR/MSVDD GET/SET interface missing"; return false; }
    b.assign(CLK_BUFSIZE, 0); PutU32(b, 0, CLK_VERSION); PutU32(b, 8, CLK_MASK);
    LONG rc = get(s.Gpu, b.data());
    if (rc != NVAPI_OK) { error = L"ClockDomains GET failed rc=" + std::to_wstring(rc); return false; }
    if (U32(b, 0) != CLK_VERSION) { error = L"ClockDomains version mismatch"; return false; }
    size_t base = 0, stride = 0;
    if (!FindRepeatingDwordLayout(b, CLK_MARKER, base, stride)) { error = L"ClockDomains dynamic entry layout was not uniquely established"; return false; }
    // Field offsets were cross-validated in the audited Blackwell layout. Refuse
    // another record size rather than guessing where voltage/clock fields moved.
    if (stride != 0x304) { error = L"ClockDomains entry stride is not the audited 0x304 layout"; return false; }
    ULONG idx = 1; // NvAPI clock-domain enum: XBAR=1.
    std::vector<ULONG> nonzero;
    for (ULONG i = 0; i < 32; ++i) {
        size_t e = base + static_cast<size_t>(i) * stride;
        if (e + CLK_OFF_MSVDD + 4 > b.size()) break;
        if (I32(b, e + CLK_OFF_FREQ) != 0 || I32(b, e + CLK_OFF_MSVDD) != 0) nonzero.push_back(i);
    }
    if (nonzero.size() == 1) idx = nonzero[0];
    const size_t entry = base + static_cast<size_t>(idx) * stride;
    if (entry + CLK_OFF_MSVDD + 4 > b.size()) { error = L"XBAR entry lies outside ClockDomains control buffer"; return false; }
    if (U32(b, entry) != CLK_MARKER) { error = L"XBAR domain entry marker is not present at the resolved index"; return false; }
    f.Base = base; f.Stride = stride; f.Index = idx;
    f.FreqKHz = I32(b, entry + CLK_OFF_FREQ);
    f.MsvddUv = I32(b, entry + CLK_OFF_MSVDD);
    return true;
}

static bool MeasureXbar(NvSession& s, LONG& mhz) {
    auto fn = s.Fn(ID_CLK_MEASURE); if (!fn) return false;
    std::vector<unsigned char> b(12, 0); PutU32(b, 0, CLK_MEASURE_VERSION); PutU32(b, 4, XBAR_MEASURE_MASK);
    if (fn(s.Gpu, b.data()) != NVAPI_OK) return false;
    mhz = static_cast<LONG>(U32(b, 8) / 1000u); return true;
}

static bool SetXbarBuffer(NvSession& s, std::vector<unsigned char>& b, std::wstring& error) {
    auto set = s.Fn(ID_CLK_SET); if (!set) { error = L"ClockDomains SET interface missing"; return false; }
    LONG rc = set(s.Gpu, b.data());
    if (rc != NVAPI_OK) { error = L"ClockDomains SET failed rc=" + std::to_wstring(rc); return false; }
    return true;
}

struct PropFields { ULONG Raw = 0; size_t Offset = PROP_CONTROL_FALLBACK_RATIO; };
static bool ValidateAndReadProp(NvSession& s, std::vector<unsigned char>& ctrl, PropFields& f, std::wstring& error) {
    auto infoFn = s.Fn(ID_PROP_INFO), getFn = s.Fn(ID_PROP_GET), setFn = s.Fn(ID_PROP_SET);
    if (!infoFn || !getFn || !setFn) { error = L"GPC:XBAR propagation interfaces missing"; return false; }
    std::vector<unsigned char> info(PROP_BUFSIZE, 0); PutU32(info, 0, PROP_INFO_VERSION);
    if (infoFn(s.Gpu, info.data()) != NVAPI_OK || U32(info, 0) != PROP_INFO_VERSION) { error = L"Propagation GET_INFO validation failed"; return false; }
    unsigned relationCount = 0;
    for (size_t off = 0; off + 16 <= info.size(); off += 4) {
        if (U32(info, off) != 0u) continue;
        if (info[off + 4] != 0u || info[off + 5] != 1u || info[off + 6] != 1u) continue;
        if (U32(info, off + 8) != DEFAULT_RATIO_RAW) continue;
        ++relationCount;
    }
    if (relationCount != 1) { error = L"GPC->XBAR relationship was not resolved as exactly one semantic record"; return false; }
    ctrl.assign(PROP_BUFSIZE, 0); PutU32(ctrl, 0, PROP_CONTROL_VERSION); PutU32(ctrl, 4, PROP_MASK);
    LONG rc = getFn(s.Gpu, ctrl.data());
    if (rc != NVAPI_OK || U32(ctrl, 0) != PROP_CONTROL_VERSION) { error = L"Propagation GET_CONTROL validation failed rc=" + std::to_wstring(rc); return false; }
    size_t off = PROP_CONTROL_FALLBACK_RATIO;
    ULONG raw = U32(ctrl, off);
    if (raw > 2u * 65536u) {
        size_t found = SIZE_MAX; unsigned count = 0;
        for (size_t p = 0; p + 4 <= ctrl.size(); p += 4) if (U32(ctrl, p) == DEFAULT_RATIO_RAW) { found = p; ++count; }
        if (count != 1) { error = L"Propagation ratio control field was not uniquely resolved"; return false; }
        off = found; raw = U32(ctrl, off);
    }
    if (raw > 2u * 65536u) { error = L"Propagation ratio readback is outside 0.0..2.0"; return false; }
    f.Raw = raw; f.Offset = off; return true;
}

static bool SetPropBuffer(NvSession& s, std::vector<unsigned char>& b, std::wstring& error) {
    auto fn = s.Fn(ID_PROP_SET); if (!fn) { error = L"Propagation SET interface missing"; return false; }
    LONG rc = fn(s.Gpu, b.data());
    if (rc != NVAPI_OK) { error = L"Propagation SET failed rc=" + std::to_wstring(rc); return false; }
    return true;
}

static ULONG RatioToRaw(double ratio) {
    if (std::fabs(ratio - 0.9) < 1e-8) return DEFAULT_RATIO_RAW;
    return static_cast<ULONG>(std::llround(ratio * 65536.0));
}

struct Baseline {
    bool Captured = false;
    bool Pstates = false; LONG CoreKHz = 0, MemKHz = 0, NvvddUv = 0; bool NvvddPresent = false;
    bool Xbar = false; std::vector<unsigned char> XbarBuffer;
    bool Prop = false; std::vector<unsigned char> PropBuffer;
} g_baseline;

static void CaptureBaselineIfNeeded(NvSession& s, bool needPstates, bool needXbar, bool needProp) {
    // Capture each subsystem independently on its first mutation.  A user can
    // apply Core/Memory first and XBAR later in the same GUI session; a single
    // one-shot "Captured" flag would otherwise lose the later subsystem's
    // true pre-tuning state and make Reset incomplete.
    if (needPstates && !g_baseline.Pstates) {
        std::vector<unsigned char> b; PstatesFields f{}; std::wstring e;
        if (ReadPstates(s, b, f, e) && f.Core && f.Memory) {
            g_baseline.Pstates = true; g_baseline.CoreKHz = f.CoreCur; g_baseline.MemKHz = f.MemCur;
            g_baseline.NvvddPresent = f.Nvvdd; if (f.Nvvdd) g_baseline.NvvddUv = f.VoltCur;
        }
    }
    if (needXbar && !g_baseline.Xbar) {
        XbarFields f{}; std::wstring e; std::vector<unsigned char> b;
        if (ReadXbar(s, b, f, e)) { g_baseline.Xbar = true; g_baseline.XbarBuffer = b; }
    }
    if (needProp && !g_baseline.Prop) {
        PropFields f{}; std::wstring e; std::vector<unsigned char> b;
        if (ValidateAndReadProp(s, b, f, e)) { g_baseline.Prop = true; g_baseline.PropBuffer = b; }
    }
    g_baseline.Captured = g_baseline.Pstates || g_baseline.Xbar || g_baseline.Prop;
}

static bool RestoreBaselineWithSession(NvSession& s, std::wstring& error) {
    bool ok = true; std::wstring all;
    if (g_baseline.Prop && !g_baseline.PropBuffer.empty()) {
        std::wstring e; auto b = g_baseline.PropBuffer;
        if (!SetPropBuffer(s, b, e)) { ok = false; all += L"Propagation restore: " + e + L"; "; }
    }
    if (g_baseline.Xbar && !g_baseline.XbarBuffer.empty()) {
        std::wstring e; auto b = g_baseline.XbarBuffer;
        if (!SetXbarBuffer(s, b, e)) { ok = false; all += L"XBAR/MSVDD restore: " + e + L"; "; }
    }
    if (g_baseline.Pstates) {
        std::vector<unsigned char> b; PstatesFields f{}; std::wstring e;
        if (ReadPstates(s, b, f, e)) {
            if (f.Core) PutI32(b, f.CoreDelta, g_baseline.CoreKHz);
            if (f.Memory) PutI32(b, f.MemDelta, g_baseline.MemKHz);
            if (f.Nvvdd && g_baseline.NvvddPresent) PutI32(b, f.VoltDelta, g_baseline.NvvddUv);
            if (!SetPstatesBuffer(s, b, e)) { ok = false; all += L"Pstates restore: " + e + L"; "; }
        } else { ok = false; all += L"Pstates restore GET: " + e + L"; "; }
    }
    if (!ok) error = all;
    else g_baseline = {};
    return ok;
}

static void ProbeExtraReadOnly(NvSession& s, NvapiTunerState& out) {
    std::vector<unsigned char> vfMask;
    if (auto infoFn = s.Fn(ID_VF_INFO)) {
        std::vector<unsigned char> b(VF_INFO_SIZE, 0); PutU32(b, 0, VF_INFO_VERSION);
        if (infoFn(s.Gpu, b.data()) == NVAPI_OK && U32(b, 0) == VF_INFO_VERSION) {
            out.VfInfoAvailable = true; out.VfInfoVersion = U32(b, 0);
            if (b.size() >= 36) vfMask.assign(b.begin() + 4, b.begin() + 36);
        }
    }
    if (!vfMask.empty()) {
        if (auto ctrlFn = s.Fn(ID_VF_CONTROL_GET)) {
            std::vector<unsigned char> b(VF_CONTROL_SIZE, 0); PutU32(b, 0, VF_CONTROL_VERSION);
            std::memcpy(b.data() + 4, vfMask.data(), std::min<size_t>(vfMask.size(), 32));
            if (ctrlFn(s.Gpu, b.data()) == NVAPI_OK && U32(b, 0) == VF_CONTROL_VERSION) {
                out.VfControlReadable = true; out.VfControlVersion = U32(b, 0);
            }
        }
    }
    ULONG adcMask = 0;
    if (auto infoFn = s.Fn(ID_ADC_INFO)) {
        std::vector<unsigned char> b(ADC_INFO_SIZE, 0); PutU32(b, 0, ADC_INFO_VERSION);
        if (infoFn(s.Gpu, b.data()) == NVAPI_OK && U32(b, 0) == ADC_INFO_VERSION) {
            out.AdcInfoAvailable = true; adcMask = U32(b, 4); out.AdcDeviceMask = adcMask;
        }
    }
    if (adcMask != 0) {
        if (auto statusFn = s.Fn(ID_ADC_STATUS)) {
            std::vector<unsigned char> b(ADC_STATUS_SIZE, 0); PutU32(b, 0, ADC_STATUS_VERSION); PutU32(b, 4, adcMask);
            if (statusFn(s.Gpu, b.data()) == NVAPI_OK && U32(b, 0) == ADC_STATUS_VERSION) out.AdcStatusAvailable = true;
        }
    }
}
static bool ProbeWithSession(NvSession& s, NvapiTunerState& out, bool allowMsvdd) {
    out.NvapiReady = true;
    std::wstring pe;
    std::vector<unsigned char> pb; PstatesFields pf{};
    if (ReadPstates(s, pb, pf, pe)) {
        if (pf.Core) { out.CoreMHz = {true, KHzToMHz(pf.CoreCur), KHzToMHz(pf.CoreMin), KHzToMHz(pf.CoreMax)}; }
        if (pf.Memory) { out.MemoryMHz = {true, KHzToMHz(pf.MemCur), KHzToMHz(pf.MemMin), KHzToMHz(pf.MemMax)}; }
        if (pf.Nvvdd) { out.NvvddMv = {true, UvToMv(pf.VoltCur), UvToMv(pf.VoltMin), UvToMv(pf.VoltMax)}; }
    } else out.Error += pe + L"; ";

    std::vector<unsigned char> xb; XbarFields xf{}; std::wstring xe;
    if (ReadXbar(s, xb, xf, xe)) {
        out.XbarWritable = true; out.XbarMHz = KHzToMHz(xf.FreqKHz); out.XbarEntryBase = static_cast<ULONG>(xf.Base);
        out.XbarEntryStride = static_cast<ULONG>(xf.Stride); out.XbarDomainIndex = xf.Index;
        out.MsvddWritable = allowMsvdd; out.MsvddMv = UvToMv(xf.MsvddUv);
        LONG physical = 0; if (MeasureXbar(s, physical)) out.XbarPhysicalMHz = physical;
    } else out.Error += xe + L"; ";

    std::vector<unsigned char> rb; PropFields rf{}; std::wstring re;
    if (ValidateAndReadProp(s, rb, rf, re)) {
        out.RatioWritable = true; out.GpcXbarRatioRaw = rf.Raw; out.GpcXbarRatio = rf.Raw / 65536.0;
    } else out.Error += re + L"; ";
    ProbeExtraReadOnly(s, out);
    return out.CoreMHz.Supported || out.MemoryMHz.Supported || out.XbarWritable || out.RatioWritable || out.VfInfoAvailable || out.AdcInfoAvailable;
}
}

bool ProbeNvapiTuner(NvapiTunerState& out, bool allowMsvdd) {
    out = {};
    NvSession s; std::wstring e;
    if (!s.Open(e)) { out.Error = e; return false; }
    return ProbeWithSession(s, out, allowMsvdd);
}

bool ApplyNvapiTuning(const NvapiTuneRequest& request, bool allowMsvdd,
    NvapiTunerState& post, std::wstring& error) {
    error.clear(); post = {};
    NvSession s; if (!s.Open(error)) return false;

    const bool needP = request.SetCore || request.SetMemory || request.SetNvvdd;
    const bool needX = request.SetXbar || request.SetMsvdd;
    const bool needR = request.SetRatio;
    CaptureBaselineIfNeeded(s, needP, needX, needR);

    if (needP) {
        std::vector<unsigned char> b; PstatesFields f{};
        if (!ReadPstates(s, b, f, error)) goto fail;
        if (request.SetCore) {
            if (!f.Core) { error = L"Core offset is not exposed by Pstates20"; goto fail; }
            const LONG khz = MHzToKHz(request.CoreMHz);
            if (khz < f.CoreMin || khz > f.CoreMax) { error = L"Core offset is outside the driver-reported range"; goto fail; }
            PutI32(b, f.CoreDelta, khz);
        }
        if (request.SetMemory) {
            if (!f.Memory) { error = L"Memory offset is not exposed by Pstates20"; goto fail; }
            const LONG khz = MHzToKHz(request.MemoryMHz);
            if (khz < f.MemMin || khz > f.MemMax) { error = L"Memory offset is outside the driver-reported range"; goto fail; }
            PutI32(b, f.MemDelta, khz);
        }
        if (request.SetNvvdd) {
            if (!f.Nvvdd) { error = L"NVVDD offset/range is not exposed by Pstates20 on this GPU/driver"; goto fail; }
            const LONG uv = MvToUv(request.NvvddMv);
            if (uv < f.VoltMin || uv > f.VoltMax) { error = L"NVVDD offset is outside the driver-reported range"; goto fail; }
            PutI32(b, f.VoltDelta, uv);
        }
        if (!SetPstatesBuffer(s, b, error)) goto fail;
        std::vector<unsigned char> rb; PstatesFields rf{}; std::wstring rerr;
        if (!ReadPstates(s, rb, rf, rerr)) { error = L"Pstates readback failed: " + rerr; goto fail; }
        if (request.SetCore && (!rf.Core || rf.CoreCur != MHzToKHz(request.CoreMHz))) { error = L"Core offset readback mismatch"; goto fail; }
        if (request.SetMemory && (!rf.Memory || rf.MemCur != MHzToKHz(request.MemoryMHz))) { error = L"Memory offset readback mismatch"; goto fail; }
        if (request.SetNvvdd && (!rf.Nvvdd || rf.VoltCur != MvToUv(request.NvvddMv))) { error = L"NVVDD offset readback mismatch"; goto fail; }
    }

    if (needX) {
        std::vector<unsigned char> b; XbarFields f{};
        if (!ReadXbar(s, b, f, error)) goto fail;
        const size_t entry = f.Base + static_cast<size_t>(f.Index) * f.Stride;
        if (request.SetXbar) {
            if (request.XbarMHz < -1000 || request.XbarMHz > 1000) { error = L"XBAR offset outside absolute -1000..+1000 MHz bound"; goto fail; }
            PutI32(b, entry + CLK_OFF_FREQ, MHzToKHz(request.XbarMHz));
        }
        if (request.SetMsvdd) {
            if (!allowMsvdd) { error = L"Separate MSVDD control is not enabled for this GPU profile"; goto fail; }
            if (request.MsvddMv < -100 || request.MsvddMv > 100) { error = L"MSVDD offset outside absolute -100..+100 mV bound"; goto fail; }
            PutI32(b, entry + CLK_OFF_MSVDD, MvToUv(request.MsvddMv));
        }
        if (!SetXbarBuffer(s, b, error)) goto fail;
        std::vector<unsigned char> rb; XbarFields rf{}; std::wstring rerr;
        if (!ReadXbar(s, rb, rf, rerr)) { error = L"XBAR/MSVDD readback failed: " + rerr; goto fail; }
        if (request.SetXbar && rf.FreqKHz != MHzToKHz(request.XbarMHz)) { error = L"XBAR offset readback mismatch"; goto fail; }
        if (request.SetMsvdd && rf.MsvddUv != MvToUv(request.MsvddMv)) { error = L"MSVDD offset readback mismatch"; goto fail; }
    }

    if (needR) {
        if (request.GpcXbarRatio < 0.0 || request.GpcXbarRatio > 2.0) { error = L"GPC:XBAR ratio must be within 0.0..2.0"; goto fail; }
        std::vector<unsigned char> b; PropFields f{};
        if (!ValidateAndReadProp(s, b, f, error)) goto fail;
        const ULONG raw = RatioToRaw(request.GpcXbarRatio);
        PutU32(b, f.Offset, raw);
        if (!SetPropBuffer(s, b, error)) goto fail;
        std::vector<unsigned char> rb; PropFields rf{}; std::wstring rerr;
        if (!ValidateAndReadProp(s, rb, rf, rerr)) { error = L"GPC:XBAR ratio readback failed: " + rerr; goto fail; }
        if (rf.Raw != raw) { error = L"GPC:XBAR ratio readback mismatch"; goto fail; }
    }

    ProbeWithSession(s, post, allowMsvdd);
    return true;

fail:
    {
        std::wstring original = error, rollback;
        if (!RestoreBaselineWithSession(s, rollback) && !rollback.empty()) error = original + L" | rollback: " + rollback;
        else error = original;
        ProbeWithSession(s, post, allowMsvdd);
        return false;
    }
}

bool ResetNvapiTuning(bool allowMsvdd, NvapiTunerState& post, std::wstring& error) {
    error.clear(); post = {};
    NvSession s; if (!s.Open(error)) return false;
    if (g_baseline.Captured && !RestoreBaselineWithSession(s, error)) { ProbeWithSession(s, post, allowMsvdd); return false; }
    ProbeWithSession(s, post, allowMsvdd);
    return true;
}

std::wstring FormatNvapiTuner(const NvapiTunerState& s) {
    std::wstringstream ss;
    ss << L"\r\n=== Unified Blackwell OC / telemetry ===\r\n";
    if (!s.NvapiReady) { ss << L"NvAPI tuner: unavailable"; if (!s.Error.empty()) ss << L" — " << s.Error; ss << L"\r\n"; return ss.str(); }
    auto range = [&](const wchar_t* name, const NvapiTuneRange& r, const wchar_t* unit) {
        ss << name << L": ";
        if (!r.Supported) ss << L"N/A\r\n";
        else ss << r.Current << unit << L"  range " << r.Min << L".." << r.Max << unit << L"\r\n";
    };
    range(L"Core offset", s.CoreMHz, L" MHz");
    range(L"Memory offset", s.MemoryMHz, L" MHz");
    range(L"NVVDD offset", s.NvvddMv, L" mV");
    ss << L"XBAR offset: " << (s.XbarWritable ? std::to_wstring(s.XbarMHz) + L" MHz" : L"N/A")
       << L" | physical: " << (s.XbarPhysicalMHz ? std::to_wstring(s.XbarPhysicalMHz) + L" MHz" : L"N/A") << L"\r\n";
    ss << L"MSVDD request offset: " << (s.MsvddWritable ? std::to_wstring(s.MsvddMv) + L" mV" : L"N/A") << L"\r\n";
    ss << L"GPC:XBAR ratio: ";
    if (s.RatioWritable) ss << std::fixed << std::setprecision(4) << s.GpcXbarRatio << L" (raw 0x" << std::hex << std::uppercase << s.GpcXbarRatioRaw << std::dec << L")\r\n";
    else ss << L"N/A\r\n";
    ss << L"V/F information: " << (s.VfInfoAvailable ? L"AVAILABLE" : L"N/A")
       << L" | control readback: " << (s.VfControlReadable ? L"AVAILABLE" : L"N/A") << L"\r\n";
    ss << L"ADC/rail information: " << (s.AdcInfoAvailable ? L"AVAILABLE" : L"N/A")
       << L" | status: " << (s.AdcStatusAvailable ? L"AVAILABLE" : L"N/A") << L"\r\n";
    if (!s.Error.empty()) ss << L"Probe notes: " << s.Error << L"\r\n";
    return ss.str();
}
