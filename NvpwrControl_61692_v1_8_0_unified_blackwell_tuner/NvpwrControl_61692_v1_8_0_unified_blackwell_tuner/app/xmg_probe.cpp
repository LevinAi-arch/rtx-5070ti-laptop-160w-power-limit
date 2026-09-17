#include "xmg_probe.h"
#include <array>
#include <sstream>
#include <iomanip>
#include <winsvc.h>
#include <vector>
#include <cwctype>

namespace {
constexpr wchar_t kXmgDevice[] = L"\\\\.\\XMGPowerPatch";
constexpr DWORD kXmgQueryIoctl = 0x00226000u;
constexpr DWORD kXmgApplyIoctl = 0x0022A004u;
constexpr ULONG kProtocol = 8u;
constexpr ULONG kQuerySize = 2056u;
constexpr ULONG kApplySize = 568u;

static bool g_xmgServiceCreatedByUs = false;
static bool g_xmgExistingServiceStartedByUs = false;

static bool XmgDeviceAvailable()
{
    HANDLE h = CreateFileW(kXmgDevice, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static std::wstring InstalledXmgDriverPath()
{
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p = std::wstring(base) + L"\\XMGPowerPatch\\Runtime\\Driver\\XMGPowerPatch.sys";
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return p;
}

static bool WaitServiceState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    SERVICE_STATUS_PROCESS sp{};
    DWORD got = 0;
    do {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof(sp), &got))
            return false;
        if (sp.dwCurrentState == wanted) return true;
        Sleep(100);
    } while (GetTickCount64() - start < timeoutMs);
    return false;
}

static bool ServicePathLooksLikeInstalledXmg(SC_HANDLE svc)
{
    DWORD need = 0;
    QueryServiceConfigW(svc, nullptr, 0, &need);
    if (!need) return false;
    std::vector<BYTE> buf(need);
    auto cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (!QueryServiceConfigW(svc, cfg, need, &need)) return false;
    if (cfg->dwServiceType != SERVICE_KERNEL_DRIVER || !cfg->lpBinaryPathName) return false;
    std::wstring path = cfg->lpBinaryPathName;
    for (auto& c : path) c = (wchar_t)towlower(c);
    return path.find(L"\\xmgpowerpatch\\runtime\\driver\\xmgpowerpatch.sys") != std::wstring::npos;
}

ULONG U32(const std::array<BYTE, kQuerySize>& b, size_t word)
{
    const size_t off = word * sizeof(ULONG);
    if (off + 4 > b.size()) return 0;
    ULONG v = 0;
    memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

ULONG U32At(const std::array<BYTE, kQuerySize>& b, size_t off)
{
    if (off + 4 > b.size()) return 0;
    ULONG v = 0;
    memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

XmgType07Entry ParseType07(const std::array<BYTE, kQuerySize>& b, size_t off)
{
    XmgType07Entry e{};
    e.ArrayIndex = U32At(b, off + 0);
    e.EmbeddedIndex = U32At(b, off + 4);
    e.Id = U32At(b, off + 8);
    e.IdentityValid = U32At(b, off + 12) != 0;
    for (size_t i = 0; i < 3; ++i) {
        const size_t s = off + 16 + i * 24;
        e.Selectors[i].Mode = U32At(b, s + 0);
        e.Selectors[i].Count = U32At(b, s + 4);
        e.Selectors[i].EffectiveMilliwatts = U32At(b, s + 8);
        e.Selectors[i].ReferenceMilliwatts = U32At(b, s + 12);
        e.Selectors[i].Tag = U32At(b, s + 16);
        e.Selectors[i].TaggedMilliwatts = U32At(b, s + 20);
    }
    return e;
}

const wchar_t* StateName(ULONG v)
{
    switch (v) {
    case 1: return L"FULL_STOCK";
    case 2: return L"FULL_TARGET";
    case 3: return L"LEGACY_CORE_TARGET_TYPE07_STOCK";
    default: return L"INVALID_MIXED_OR_PARTIAL";
    }
}

const wchar_t* ResolverName(ULONG v)
{
    switch (v) {
    case 1: return L"KNOWN_REFERENCE";
    case 3: return L"RUNTIME_DISCOVERY";
    default: return L"UNKNOWN";
    }
}

const wchar_t* LayoutSourceName(ULONG v)
{
    switch (v) {
    case 1: return L"KNOWN_REFERENCE";
    case 2: return L"RUNTIME_DISCOVERY";
    default: return L"UNKNOWN";
    }
}

const wchar_t* ValidationName(ULONG v)
{
    switch (v) {
    case 1: return L"VALIDATED_REFERENCE";
    case 2: return L"RUNTIME_VALIDATED_THIS_RUN";
    default: return L"UNKNOWN";
    }
}

void FormatEntry(std::wstringstream& ss, const wchar_t* name, const XmgType07Entry& e)
{
    ss << name << L": index=" << e.ArrayIndex
       << L" embedded=" << e.EmbeddedIndex
       << L" id=0x" << std::hex << std::uppercase << e.Id << std::dec
       << L" identity=" << (e.IdentityValid ? L"PASS" : L"FAIL") << L"\r\n";
    for (int i = 0; i < 3; ++i) {
        const auto& s = e.Selectors[i];
        ss << L"  selector" << (i + 1)
           << L" mode=" << s.Mode << L" count=" << s.Count
           << L" effective=" << s.EffectiveMilliwatts / 1000.0 << L" W"
           << L" reference=" << s.ReferenceMilliwatts / 1000.0 << L" W"
           << L" tag=0x" << std::hex << std::uppercase << s.Tag << std::dec
           << L" tagged=" << s.TaggedMilliwatts / 1000.0 << L" W\r\n";
    }
}
}

bool XmgQueryRuntime(XmgRuntimeSummary& out)
{
    out = {};
    HANDLE h = CreateFileW(kXmgDevice, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        out.Error = L"XMGPowerPatch device is not available (Win32 " + std::to_wstring(e) + L")";
        return false;
    }
    out.Present = true;

    ULONG input[2] = { 8u, kProtocol };
    std::array<BYTE, kQuerySize> bytes{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(h, kXmgQueryIoctl,
        input, sizeof(input), bytes.data(), (DWORD)bytes.size(), &returned, nullptr);
    const DWORD ioctlError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);

    if (!ok) {
        out.Error = L"XMG QUERY IOCTL failed (Win32 " + std::to_wstring(ioctlError) + L")";
        return false;
    }
    if (returned != kQuerySize || U32(bytes, 0) != kQuerySize || U32(bytes, 1) != kProtocol) {
        out.Error = L"XMG runtime protocol/size mismatch";
        return false;
    }
    out.ProtocolOk = true;

    out.OverallStatus = U32(bytes, 2);
    out.PlatformStatus = U32(bytes, 3);
    out.PlatformGatePass = U32(bytes, 4) != 0;
    out.VendorId = U32(bytes, 5);
    out.DeviceId = U32(bytes, 6);
    out.SubsystemId = U32(bytes, 7);
    out.NvpcfStatus = U32(bytes, 8);
    out.NvidiaStatus = U32(bytes, 29);
    out.GenerationStable = U32(bytes, 58) != 0;
    out.NvidiaResolverProfile = U32(bytes, 219);
    out.NvpcfResolverProfile = U32(bytes, 220);

    // Words 223..286 are the 64-word semantic-layout evidence block.
    out.LayoutSource = U32(bytes, 223 + 0);
    out.LayoutProfileId = U32(bytes, 223 + 1);
    out.LayoutValidationState = U32(bytes, 223 + 2);
    out.LayoutDiscoveryStatus = U32(bytes, 223 + 3);
    out.Type07IdentityCandidates = U32(bytes, 223 + 60);
    out.Type07ValidPairs = U32(bytes, 223 + 61);
    out.Type07SelectorCandidates = U32(bytes, 223 + 62);
    out.Type07ValidSelectors = U32(bytes, 223 + 63);

    out.CapabilityFlags = U32(bytes, 287);
    out.StateClassification = U32(bytes, 288);
    out.Type07Status = U32(bytes, 289);
    out.Type07PairValid = U32(bytes, 290) != 0;

    out.SafetyKernelWrites = U32(bytes, 49);
    out.SafetyEcWrites = U32(bytes, 50);
    out.SafetyVbiosWrites = U32(bytes, 51);
    out.SafetyBcdChanges = U32(bytes, 52);
    out.SafetyDebuggerSessions = U32(bytes, 53);
    out.SafetyReboots = U32(bytes, 54);
    out.SafetyMechControlApply = U32(bytes, 55);

    out.Entry13 = ParseType07(bytes, 1164);
    out.Entry14 = ParseType07(bytes, 1252);
    return true;
}

bool XmgHasCoherent250Capability(const XmgRuntimeSummary& s)
{
    if (!s.Present || !s.ProtocolOk || s.OverallStatus != 0 || !s.PlatformGatePass)
        return false;
    if (s.VendorId != 0x10DE || (s.DeviceId != 0x2C18 && s.DeviceId != 0x2C19))
        return false;
    // XMG's supplied v8 contract authorizes only subsystem vendor 1D05.
    // The upper 16-bit subsystem-device value is diagnostic and may vary.
    if ((s.SubsystemId & 0xFFFFu) != 0x1D05u)
        return false;
    if ((s.CapabilityFlags & 0x7u) != 0x7u || !s.Type07PairValid || !s.GenerationStable)
        return false;
    if (!s.Entry13.IdentityValid || s.Entry13.ArrayIndex != 13 || s.Entry13.EmbeddedIndex != 13 || s.Entry13.Id != 0x1B)
        return false;
    if (!s.Entry14.IdentityValid || s.Entry14.ArrayIndex != 14 || s.Entry14.EmbeddedIndex != 14 || s.Entry14.Id != 0x1C)
        return false;
    return true;
}

bool XmgHasValidated250WContract(const XmgRuntimeSummary& s)
{
    if (!XmgHasCoherent250Capability(s)) return false;
    return s.StateClassification >= 1u && s.StateClassification <= 3u;
}

std::wstring XmgFormatRuntime(const XmgRuntimeSummary& s)
{
    std::wstringstream ss;
    ss << L"\r\n=== XMGPowerPatch v8 semantic runtime ===\r\n";
    if (!s.Present) {
        ss << L"Runtime: not installed/running\r\n";
        if (!s.Error.empty()) ss << L"Reason: " << s.Error << L"\r\n";
        return ss.str();
    }
    if (!s.ProtocolOk) {
        ss << L"Runtime: present, protocol rejected\r\n";
        if (!s.Error.empty()) ss << L"Reason: " << s.Error << L"\r\n";
        return ss.str();
    }

    ss << L"Protocol:             v8 / 2056-byte query\r\n"
       << L"Overall status:       " << s.OverallStatus << L"\r\n"
       << L"Platform gate:        " << (s.PlatformGatePass ? L"PASS" : L"FAIL") << L"\r\n"
       << L"PCI identity:         VEN_" << std::hex << std::uppercase << std::setw(4) << std::setfill(L'0') << s.VendorId
       << L" DEV_" << std::setw(4) << s.DeviceId << L" SUBSYS_" << std::setw(8) << s.SubsystemId << std::dec << L"\r\n"
       << L"NVIDIA/NVPCF status:  " << s.NvidiaStatus << L" / " << s.NvpcfStatus << L"\r\n"
       << L"Generation stable:    " << (s.GenerationStable ? L"YES" : L"NO") << L"\r\n"
       << L"NVIDIA resolver:      " << ResolverName(s.NvidiaResolverProfile) << L"\r\n"
       << L"NVPCF resolver:       " << ResolverName(s.NvpcfResolverProfile) << L"\r\n"
       << L"Layout source:        " << LayoutSourceName(s.LayoutSource) << L"\r\n"
       << L"Layout validation:    " << ValidationName(s.LayoutValidationState) << L"\r\n"
       << L"Type07 evidence:      identities=" << s.Type07IdentityCandidates
       << L" pairs=" << s.Type07ValidPairs
       << L" selector candidates=" << s.Type07SelectorCandidates
       << L" valid selectors=" << s.Type07ValidSelectors << L"\r\n"
       << L"Capabilities:         core250=" << ((s.CapabilityFlags & 1) ? L"YES" : L"NO")
       << L" entry13=" << ((s.CapabilityFlags & 2) ? L"YES" : L"NO")
       << L" entry14=" << ((s.CapabilityFlags & 4) ? L"YES" : L"NO") << L"\r\n"
       << L"State:                " << StateName(s.StateClassification) << L"\r\n"
       << L"Type07 pair:          " << (s.Type07PairValid ? L"VALID" : L"INVALID") << L"\r\n"
       << L"250 W contract gate:  " << (XmgHasCoherent250Capability(s) ? L"PASS capability / coherent writer available" : L"FAIL/UNAVAILABLE") << L"\r\n";
    if (XmgHasCoherent250Capability(s)) {
        if (s.DeviceId == 0x2C18) ss << L"XMG validation note:  2C18 / RTX 5090 Laptop is marked live-verified by the supplied package.\r\n";
        if (s.DeviceId == 0x2C19) ss << L"XMG validation note:  2C19 / RTX 5080 Laptop implementation is present, separate live validation is marked pending.\r\n";
    }

    FormatEntry(ss, L"Entry13", s.Entry13);
    FormatEntry(ss, L"Entry14", s.Entry14);
    ss << L"Query safety counters: kernel=" << s.SafetyKernelWrites
       << L" EC=" << s.SafetyEcWrites << L" VBIOS=" << s.SafetyVbiosWrites
       << L" BCD=" << s.SafetyBcdChanges << L" debugger=" << s.SafetyDebuggerSessions
       << L" reboot=" << s.SafetyReboots << L" MechControl=" << s.SafetyMechControlApply << L"\r\n"
       << L"275 W contract:       NOT ESTABLISHED by the supplied XMG package; write remains disabled.\r\n";
    return ss.str();
}


bool XmgIsFullStock(const XmgRuntimeSummary& s) { return s.ProtocolOk && s.StateClassification == 1u; }
bool XmgIsFullTarget(const XmgRuntimeSummary& s) { return s.ProtocolOk && s.StateClassification == 2u; }
bool XmgIsLegacyCoreTarget(const XmgRuntimeSummary& s) { return s.ProtocolOk && s.StateClassification == 3u; }

namespace {
ULONG TxU32(const std::array<BYTE, kApplySize>& b, size_t word)
{
    const size_t off = word * sizeof(ULONG);
    if (off + 4 > b.size()) return 0;
    ULONG v = 0;
    memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

bool ValidateType07Readback(const XmgTransactionSummary& tx, bool target)
{
    const ULONG e13 = target ? 250000u : 210000u;
    const ULONG e14 = target ? 100000u : 60000u;
    for (int i = 0; i < 6; ++i) if (tx.ReadbackType07[i] != e13) return false;
    for (int i = 6; i < 12; ++i) if (tx.ReadbackType07[i] != e14) return false;
    return true;
}

bool ValidateCoreReadback(const XmgTransactionSummary& tx, bool target)
{
    if (target) {
        // StateMaximum, channel effective/tagged, NVPCF max A/B/alias = 250 W.
        for (int i = 0; i < 6; ++i) if (tx.ReadbackCore[i] != 250000u) return false;
        // NVPCF Base = 225 W.
        return tx.ReadbackCore[6] == 225000u;
    }
    // Rollback stock values are device-specific (2C18 vs 2C19 defaults), so
    // the authoritative stock decision comes from the post-transaction QUERY.
    return true;
}

bool RunXmgTransaction(ULONG operation, XmgTransactionSummary& tx, XmgRuntimeSummary& post)
{
    tx = {};
    post = {};

    HANDLE h = CreateFileW(kXmgDevice, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        tx.Win32Error = GetLastError();
        tx.Error = L"XMGPowerPatch coherent backend is not available (Win32 " +
            std::to_wstring(tx.Win32Error) + L")";
        return false;
    }
    tx.Present = true;

    ULONG input[4] = { 16u, kProtocol, operation, 0u };
    std::array<BYTE, kApplySize> bytes{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(h, kXmgApplyIoctl,
        input, sizeof(input), bytes.data(), (DWORD)bytes.size(), &returned, nullptr);
    tx.Win32Error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);

    if (!ok) {
        tx.Error = L"XMG coherent transaction IOCTL failed (Win32 " +
            std::to_wstring(tx.Win32Error) + L")";
        return false;
    }
    tx.IoctlOk = true;

    if (returned != kApplySize || TxU32(bytes, 0) != kApplySize || TxU32(bytes, 1) != kProtocol) {
        tx.Error = L"XMG coherent transaction protocol/size mismatch";
        return false;
    }
    tx.ProtocolOk = true;

    tx.PatchStatus = TxU32(bytes, 2);
    tx.BaseMilliwatts = TxU32(bytes, 3);
    tx.DynamicBoostMilliwatts = TxU32(bytes, 4);
    tx.TotalMilliwatts = TxU32(bytes, 5);
    tx.PreflightStatus = TxU32(bytes, 6);
    tx.NvpcfStatus = TxU32(bytes, 7);
    tx.NvidiaStatus = TxU32(bytes, 8);
    tx.WritesAttempted = TxU32(bytes, 9);
    tx.WritesCompleted = TxU32(bytes, 10);
    tx.WritesVerified = TxU32(bytes, 11);
    tx.RollbackAttempted = TxU32(bytes, 12);
    tx.RollbackCompleted = TxU32(bytes, 13);
    tx.RollbackVerified = TxU32(bytes, 14);
    tx.PostKernelVerified = TxU32(bytes, 15) != 0;

    for (int i = 0; i < 7; ++i) tx.OriginalCore[i] = TxU32(bytes, 16 + i);
    for (int i = 0; i < 8; ++i) tx.ReadbackCore[i] = TxU32(bytes, 23 + i);
    tx.KernelWriteCount = TxU32(bytes, 31);
    tx.SuccessMask = TxU32(bytes, 32);
    tx.RollbackSuccessMask = TxU32(bytes, 33);
    tx.RollbackFailureMask = TxU32(bytes, 34);

    tx.EnvelopeState = TxU32(bytes, 104);
    tx.GenerationStable = TxU32(bytes, 105) != 0;
    tx.WriterTargetCount = TxU32(bytes, 106);
    tx.WriterDistinctTargetCount = TxU32(bytes, 107);
    tx.ApplyAuthorized = TxU32(bytes, 108) != 0;
    tx.BlockReason = TxU32(bytes, 109);
    tx.Operation = TxU32(bytes, 110);
    tx.VendorId = TxU32(bytes, 111);
    tx.DeviceId = TxU32(bytes, 112);
    tx.SubsystemId = TxU32(bytes, 113);
    tx.CapabilityFlags = TxU32(bytes, 114);
    tx.Type07Status = TxU32(bytes, 115);
    tx.StateClassification = TxU32(bytes, 116);
    tx.ContractWriteCount = TxU32(bytes, 117);
    for (int i = 0; i < 12; ++i) tx.OriginalType07[i] = TxU32(bytes, 118 + i);
    for (int i = 0; i < 12; ++i) tx.ReadbackType07[i] = TxU32(bytes, 130 + i);

    const bool identity = tx.VendorId == 0x10DEu &&
        (tx.DeviceId == 0x2C18u || tx.DeviceId == 0x2C19u) &&
        ((tx.SubsystemId & 0xFFFFu) == 0x1D05u);
    const bool fixedContract = tx.BaseMilliwatts == 225000u &&
        tx.DynamicBoostMilliwatts == 25000u && tx.TotalMilliwatts == 250000u &&
        (tx.CapabilityFlags & 0x7u) == 0x7u && tx.ContractWriteCount == 19u;
    const bool resolverOk = tx.PreflightStatus == 0u && tx.NvpcfStatus == 0u &&
        tx.NvidiaStatus == 0u && tx.GenerationStable &&
        tx.WriterTargetCount == 19u && tx.WriterDistinctTargetCount == 19u;

    if (!identity || !fixedContract || !resolverOk || tx.Operation != operation) {
        tx.Error = L"XMG transaction returned a result outside the audited v8 250 W contract";
        return false;
    }

    // Always perform a fresh full resolver QUERY after the transaction.  This
    // is intentionally independent of the compact Apply response.
    if (!XmgQueryRuntime(post) || !XmgHasValidated250WContract(post)) {
        tx.Error = L"XMG post-transaction resolver query failed semantic validation";
        return false;
    }

    if (operation == 1u) {
        const bool statusOk = (tx.PatchStatus == 0u || tx.PatchStatus == 1u);
        const bool stateOk = tx.StateClassification == 2u && XmgIsFullTarget(post);
        const bool writesOk = (tx.PatchStatus == 1u) ||
            (tx.WritesCompleted == tx.WritesVerified &&
             ((tx.EnvelopeState == 1u && tx.WritesVerified == 19u) ||
              (tx.EnvelopeState == 3u && tx.WritesVerified == 12u)));
        tx.PostQueryVerified = stateOk;
        const bool authorizationOk = (tx.PatchStatus == 1u) || tx.ApplyAuthorized;
        tx.Success = statusOk && stateOk && writesOk && authorizationOk &&
            tx.PostKernelVerified && ValidateCoreReadback(tx, true) &&
            ValidateType07Readback(tx, true);
    } else if (operation == 2u) {
        const bool statusOk = (tx.PatchStatus == 2u);
        const bool stateOk = XmgIsFullStock(post);
        const bool writesOk = tx.WritesCompleted == tx.WritesVerified &&
            ((tx.EnvelopeState == 2u && tx.WritesVerified == 19u) ||
             (tx.EnvelopeState == 3u && tx.WritesVerified == 7u) ||
             (tx.EnvelopeState == 1u && tx.WritesVerified == 0u));
        tx.PostQueryVerified = stateOk;
        // Device-specific core defaults differ (2C18 vs 2C19), so the fresh
        // semantic resolver query is authoritative for core stock values.
        tx.Success = statusOk && stateOk && writesOk && tx.PostKernelVerified &&
            ValidateCoreReadback(tx, false) && ValidateType07Readback(tx, false);
    }

    if (!tx.Success && tx.Error.empty())
        tx.Error = L"XMG coherent transaction completed but final contract verification failed";
    return tx.Success;
}
}

bool XmgEnsureRuntimeAvailable(std::wstring& error)
{
    error.clear();
    if (XmgDeviceAvailable()) return true;

    const std::wstring driver = InstalledXmgDriverPath();
    if (driver.empty()) {
        error = L"Installed XMG runtime driver was not found under ProgramData\\XMGPowerPatch\\Runtime\\Driver.";
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        error = L"OpenSCManager failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, L"XMGPowerPatch",
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | DELETE);
    if (!svc) {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            error = L"OpenService(XMGPowerPatch) failed (" + std::to_wstring(GetLastError()) + L")";
            CloseServiceHandle(scm);
            return false;
        }
        svc = CreateServiceW(scm, L"XMGPowerPatch", L"XMGPowerPatch",
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | DELETE,
            SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            driver.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            error = L"CreateService(XMGPowerPatch) failed (" + std::to_wstring(GetLastError()) + L")";
            CloseServiceHandle(scm);
            return false;
        }
        g_xmgServiceCreatedByUs = true;
    } else if (!ServicePathLooksLikeInstalledXmg(svc)) {
        error = L"An existing XMGPowerPatch service points to an unexpected driver path; refusing to replace it.";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS sp{};
    DWORD got = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof(sp), &got)) {
        error = L"QueryServiceStatusEx(XMGPowerPatch) failed.";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    if (sp.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING) {
                error = L"StartService(XMGPowerPatch) failed (" + std::to_wstring(e) + L")";
                if (g_xmgServiceCreatedByUs) DeleteService(svc);
                g_xmgServiceCreatedByUs = false;
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return false;
            }
        } else if (!g_xmgServiceCreatedByUs) {
            g_xmgExistingServiceStartedByUs = true;
        }
        if (!WaitServiceState(svc, SERVICE_RUNNING, 5000)) {
            error = L"XMGPowerPatch service did not reach RUNNING state.";
            if (g_xmgServiceCreatedByUs) DeleteService(svc);
            g_xmgServiceCreatedByUs = false;
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    if (!XmgDeviceAvailable()) {
        error = L"XMGPowerPatch service is running but the device interface is unavailable.";
        return false;
    }
    return true;
}

void XmgReleaseRuntimeIfOwned()
{
    if (!g_xmgServiceCreatedByUs && !g_xmgExistingServiceStartedByUs) return;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"XMGPowerPatch", SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (svc) {
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        WaitServiceState(svc, SERVICE_STOPPED, 5000);
        if (g_xmgServiceCreatedByUs) DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    g_xmgServiceCreatedByUs = false;
    g_xmgExistingServiceStartedByUs = false;
}

bool XmgApplyCoherent250W(XmgTransactionSummary& tx, XmgRuntimeSummary& post)
{
    XmgRuntimeSummary pre{};
    if (!XmgQueryRuntime(pre) || !XmgHasValidated250WContract(pre)) {
        tx = {};
        tx.Error = L"XMG 250 W preflight semantic gate failed";
        return false;
    }
    if (!(XmgIsFullStock(pre) || XmgIsLegacyCoreTarget(pre) || XmgIsFullTarget(pre))) {
        tx = {};
        tx.Error = L"XMG state is mixed/partial; coherent Apply is blocked";
        return false;
    }
    return RunXmgTransaction(1u, tx, post);
}

bool XmgRollbackCoherent250W(XmgTransactionSummary& tx, XmgRuntimeSummary& post)
{
    XmgRuntimeSummary pre{};
    if (!XmgQueryRuntime(pre) || !XmgHasValidated250WContract(pre)) {
        tx = {};
        tx.Error = L"XMG rollback preflight semantic gate failed";
        return false;
    }
    if (!(XmgIsFullStock(pre) || XmgIsLegacyCoreTarget(pre) || XmgIsFullTarget(pre))) {
        tx = {};
        tx.Error = L"XMG state is mixed/partial; coherent Rollback is blocked";
        return false;
    }
    return RunXmgTransaction(2u, tx, post);
}

std::wstring XmgFormatTransaction(const XmgTransactionSummary& tx)
{
    std::wstringstream ss;
    ss << L"XMG coherent transaction: operation=" << tx.Operation
       << L" patch_status=" << tx.PatchStatus
       << L" fixed=" << tx.BaseMilliwatts / 1000.0 << L"+"
       << tx.DynamicBoostMilliwatts / 1000.0 << L"="
       << tx.TotalMilliwatts / 1000.0 << L" W"
       << L" writers=" << tx.WritesAttempted << L"/" << tx.WritesCompleted
       << L"/" << tx.WritesVerified
       << L" contract=" << tx.ContractWriteCount
       << L" post_kernel=" << (tx.PostKernelVerified ? L"PASS" : L"FAIL")
       << L" post_query=" << (tx.PostQueryVerified ? L"PASS" : L"FAIL")
       << L" result=" << (tx.Success ? L"SUCCESS" : L"FAIL");
    if (!tx.Error.empty()) ss << L" error=" << tx.Error;
    return ss.str();
}
