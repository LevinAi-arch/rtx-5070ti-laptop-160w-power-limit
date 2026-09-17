#pragma once
#include <Windows.h>
#include <string>

struct XmgType07Selector {
    ULONG Mode = 0;
    ULONG Count = 0;
    ULONG EffectiveMilliwatts = 0;
    ULONG ReferenceMilliwatts = 0;
    ULONG Tag = 0;
    ULONG TaggedMilliwatts = 0;
};

struct XmgType07Entry {
    ULONG ArrayIndex = 0;
    ULONG EmbeddedIndex = 0;
    ULONG Id = 0;
    bool IdentityValid = false;
    XmgType07Selector Selectors[3]{};
};

struct XmgRuntimeSummary {
    bool Present = false;
    bool ProtocolOk = false;
    std::wstring Error;

    ULONG OverallStatus = 0;
    ULONG PlatformStatus = 0;
    bool PlatformGatePass = false;
    ULONG VendorId = 0;
    ULONG DeviceId = 0;
    ULONG SubsystemId = 0;

    ULONG NvpcfStatus = 0;
    ULONG NvidiaStatus = 0;
    bool GenerationStable = false;
    ULONG NvidiaResolverProfile = 0;
    ULONG NvpcfResolverProfile = 0;

    ULONG LayoutSource = 0;
    ULONG LayoutProfileId = 0;
    ULONG LayoutValidationState = 0;
    ULONG LayoutDiscoveryStatus = 0;
    ULONG Type07IdentityCandidates = 0;
    ULONG Type07ValidPairs = 0;
    ULONG Type07SelectorCandidates = 0;
    ULONG Type07ValidSelectors = 0;

    ULONG CapabilityFlags = 0;
    ULONG StateClassification = 0;
    ULONG Type07Status = 0;
    bool Type07PairValid = false;

    ULONG SafetyKernelWrites = 0;
    ULONG SafetyEcWrites = 0;
    ULONG SafetyVbiosWrites = 0;
    ULONG SafetyBcdChanges = 0;
    ULONG SafetyDebuggerSessions = 0;
    ULONG SafetyReboots = 0;
    ULONG SafetyMechControlApply = 0;

    XmgType07Entry Entry13{};
    XmgType07Entry Entry14{};
};

/*
    XMG protocol-v8 coherent transaction result.

    WHERE:
      This structure is populated only from the documented 568-byte Apply /
      RollbackToStock response returned by the supplied XMGPowerPatch runtime.

    WHAT:
      The XMG backend owns the actual 19-target transaction:
        7 Core/NVPCF targets + 6 Entry13 targets + 6 Entry14 targets.
      User mode cannot supply addresses, offsets or arbitrary target values.

    WHY:
      Keeping the semantic resolver and compare-and-swap writers inside the
      XMG kernel backend prevents NvpwrControl from guessing Type07 locations.
*/
struct XmgTransactionSummary {
    bool Present = false;
    bool ProtocolOk = false;
    bool IoctlOk = false;
    bool Success = false;
    bool PostQueryVerified = false;
    DWORD Win32Error = ERROR_SUCCESS;
    std::wstring Error;

    ULONG PatchStatus = 0;
    ULONG Operation = 0;
    ULONG BaseMilliwatts = 0;
    ULONG DynamicBoostMilliwatts = 0;
    ULONG TotalMilliwatts = 0;

    ULONG PreflightStatus = 0;
    ULONG NvpcfStatus = 0;
    ULONG NvidiaStatus = 0;

    ULONG WritesAttempted = 0;
    ULONG WritesCompleted = 0;
    ULONG WritesVerified = 0;
    ULONG KernelWriteCount = 0;
    ULONG SuccessMask = 0;

    ULONG RollbackAttempted = 0;
    ULONG RollbackCompleted = 0;
    ULONG RollbackVerified = 0;
    ULONG RollbackSuccessMask = 0;
    ULONG RollbackFailureMask = 0;
    bool PostKernelVerified = false;

    ULONG OriginalCore[7]{};
    ULONG ReadbackCore[8]{}; // seventh core value + source04 diagnostic

    ULONG EnvelopeState = 0;
    bool GenerationStable = false;
    ULONG WriterTargetCount = 0;
    ULONG WriterDistinctTargetCount = 0;
    bool ApplyAuthorized = false;
    ULONG BlockReason = 0;

    ULONG VendorId = 0;
    ULONG DeviceId = 0;
    ULONG SubsystemId = 0;
    ULONG CapabilityFlags = 0;
    ULONG Type07Status = 0;
    ULONG StateClassification = 0;
    ULONG ContractWriteCount = 0;

    ULONG OriginalType07[12]{};
    ULONG ReadbackType07[12]{};
};

bool XmgQueryRuntime(XmgRuntimeSummary& out);
std::wstring XmgFormatRuntime(const XmgRuntimeSummary& s);
bool XmgHasCoherent250Capability(const XmgRuntimeSummary& s);
bool XmgHasValidated250WContract(const XmgRuntimeSummary& s);
bool XmgIsFullStock(const XmgRuntimeSummary& s);
bool XmgIsFullTarget(const XmgRuntimeSummary& s);
bool XmgIsLegacyCoreTarget(const XmgRuntimeSummary& s);

bool XmgApplyCoherent250W(XmgTransactionSummary& tx, XmgRuntimeSummary& post);
bool XmgRollbackCoherent250W(XmgTransactionSummary& tx, XmgRuntimeSummary& post);
std::wstring XmgFormatTransaction(const XmgTransactionSummary& tx);

// Optional lifecycle for an already-installed XMG runtime.  NvpwrControl does
// not redistribute XMGPowerPatch.sys; it only uses the installed ProgramData copy.
bool XmgEnsureRuntimeAvailable(std::wstring& error);
void XmgReleaseRuntimeIfOwned();
