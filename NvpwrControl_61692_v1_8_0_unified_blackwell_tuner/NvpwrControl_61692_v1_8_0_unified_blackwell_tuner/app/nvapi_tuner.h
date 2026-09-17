#pragma once
#include <Windows.h>
#include <string>

struct NvapiTuneRange {
    bool Supported = false;
    LONG Current = 0;
    LONG Min = 0;
    LONG Max = 0;
};

struct NvapiTunerState {
    bool NvapiReady = false;
    std::wstring Error;

    NvapiTuneRange CoreMHz{};
    NvapiTuneRange MemoryMHz{};
    NvapiTuneRange NvvddMv{};

    bool XbarWritable = false;
    LONG XbarMHz = 0;
    LONG XbarPhysicalMHz = 0;
    LONG XbarMinMHz = -1000;
    LONG XbarMaxMHz = 1000;
    ULONG XbarEntryBase = 0;
    ULONG XbarEntryStride = 0;
    ULONG XbarDomainIndex = 1;

    bool MsvddWritable = false;
    LONG MsvddMv = 0;
    LONG MsvddMinMv = -100;
    LONG MsvddMaxMv = 100;

    bool RatioWritable = false;
    double GpcXbarRatio = 0.0;
    ULONG GpcXbarRatioRaw = 0;

    bool VfInfoAvailable = false;
    bool VfControlReadable = false;
    ULONG VfInfoVersion = 0;
    ULONG VfControlVersion = 0;

    bool AdcInfoAvailable = false;
    bool AdcStatusAvailable = false;
    ULONG AdcDeviceMask = 0;
};

struct NvapiTuneRequest {
    bool SetCore = false;
    LONG CoreMHz = 0;
    bool SetMemory = false;
    LONG MemoryMHz = 0;
    bool SetNvvdd = false;
    LONG NvvddMv = 0;

    bool SetXbar = false;
    LONG XbarMHz = 0;
    bool SetMsvdd = false;
    LONG MsvddMv = 0;

    bool SetRatio = false;
    double GpcXbarRatio = 0.9;
};

// Probe is read-only.  allowMsvdd should be true only for profiles where a
// separately controllable XBAR-domain MSVDD request is intended to be exposed.
bool ProbeNvapiTuner(NvapiTunerState& out, bool allowMsvdd);

// Applies only capabilities that were requested and re-validates every live
// structure before SET.  SET is followed by GET/readback; on failure this
// function attempts a session-baseline rollback.
bool ApplyNvapiTuning(const NvapiTuneRequest& request, bool allowMsvdd,
    NvapiTunerState& post, std::wstring& error);

// Restores the values captured before the first successful tuning mutation in
// this process.  If no mutation occurred it is a no-op followed by a probe.
bool ResetNvapiTuning(bool allowMsvdd, NvapiTunerState& post, std::wstring& error);

std::wstring FormatNvapiTuner(const NvapiTunerState& s);
