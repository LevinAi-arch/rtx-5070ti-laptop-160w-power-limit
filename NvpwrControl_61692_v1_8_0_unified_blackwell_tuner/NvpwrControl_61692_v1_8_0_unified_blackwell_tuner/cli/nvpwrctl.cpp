#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include "..\\shared\\nvpwr_ioctl.h"

static const char* StateName(ULONG s)
{
    switch (s) {
    case NvpwrStateArmed: return "ARMED";
    case NvpwrStateApplied: return "APPLIED";
    case NvpwrStateMixed: return "MIXED";
    case NvpwrStateWrongBuild: return "WRONG_BUILD";
    case NvpwrStateModuleNotFound: return "MODULE_NOT_FOUND";
    case NvpwrStateGpuNotFound: return "GPU_NOT_FOUND";
    case NvpwrStateContextInvalid: return "CONTEXT_INVALID";
    case NvpwrStatePreconditionNotReady: return "PRECONDITION_NOT_READY";
    case NvpwrStateStockBaseline: return "STOCK_BASELINE";
    default: return "UNKNOWN";
    }
}

static ULONG ParseProfile(const wchar_t* s)
{
    if (!s) return NvpwrProfileUnknown;
    if (_wcsicmp(s, L"5050") == 0) return NvpwrProfileRtx5050Laptop;
    if (_wcsicmp(s, L"5060") == 0) return NvpwrProfileRtx5060Laptop;
    if (_wcsicmp(s, L"5070") == 0) return NvpwrProfileRtx5070Laptop;
    if (_wcsicmp(s, L"5070ti") == 0) return NvpwrProfileRtx5070TiLaptop;
    if (_wcsicmp(s, L"5080") == 0) return NvpwrProfileRtx5080Laptop;
    if (_wcsicmp(s, L"5090") == 0) return NvpwrProfileRtx5090Laptop;
    if (_wcsicmp(s, L"4090") == 0) return NvpwrProfileRtx4090Laptop;
    if (_wcsicmp(s, L"4080") == 0) return NvpwrProfileRtx4080Laptop;
    if (_wcsicmp(s, L"4070") == 0) return NvpwrProfileRtx4070Laptop;
    if (_wcsicmp(s, L"4060") == 0) return NvpwrProfileRtx4060Laptop;
    if (_wcsicmp(s, L"4050") == 0) return NvpwrProfileRtx4050Laptop;
    return NvpwrProfileUnknown;
}

static void PrintPower(const char* label, ULONG v)
{
    if (v == 0xFFFFFFFFu) std::printf("%-20s : FFFFFFFF\n", label);
    else std::printf("%-20s : %lu (%.3f W)\n", label, v, v / 1000.0);
}

static bool ShowStatus(HANDLE h)
{
    NVPWR_STATUS s{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_NVPWR_STATUS, nullptr, 0, &s, sizeof(s), &got, nullptr) || got < sizeof(s)) {
        std::printf("STATUS failed: Win32=%lu bytes=%lu\n", GetLastError(), got);
        return false;
    }

    std::printf("State                : %s (%lu)\n", StateName(s.State), s.State);
    std::printf("Detail               : %lu\n", s.Detail);
    std::printf("Last NTSTATUS        : 0x%08lX\n", (unsigned long)s.LastNtStatus);
    std::printf("Last NVIDIA status   : 0x%08lX\n", s.LastNvStatus);
    std::printf("TimeDateStamp        : 0x%08lX\n", s.TimeDateStamp);
    std::printf("SizeOfImage          : 0x%08lX\n", s.SizeOfImage);
    std::printf("Flags init/elig/amt  : %u / %u / %u\n", s.RootInitialized, s.Eligibility, s.AmountActive);
    std::printf("Active profile       : %lu\n", s.ActiveProfile);
    PrintPower("OEM baseline", s.OemBaseline);
    PrintPower("F7 input (+3D14)", s.CtgpTarget);
    PrintPower("amount (+3D18)", s.PpabAmount);
    PrintPower("UPPER (+3D24)", s.UpperBoundary);
    PrintPower("predicted F7", s.PredictedF7);
    PrintPower("MAX effective", s.MaxEffective);
    PrintPower("Current effective", s.CurrentEffective);
    PrintPower("Current F7", s.CurrentF7Value);
    PrintPower("Applied target", s.AppliedTarget);
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 4) {
        std::printf("Usage: NvpwrCtl status | set <4050|4060|4070|4080|4090|5050|5060|5070|5070ti|5080|5090> <watts> | restore\n");
        return 2;
    }

    HANDLE h = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("Cannot open \\\\.\\Nvpwr: Win32=%lu\n", GetLastError());
        return 3;
    }

    int rc = 0;
    if (_wcsicmp(argv[1], L"status") == 0 && argc == 2) {
        rc = ShowStatus(h) ? 0 : 4;
    } else if (_wcsicmp(argv[1], L"restore") == 0 && argc == 2) {
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_NVPWR_RESTORE, nullptr, 0, nullptr, 0, &got, nullptr)) {
            std::printf("Restore failed: Win32=%lu\n", GetLastError());
            rc = 5;
        }
        ShowStatus(h);
    } else if (_wcsicmp(argv[1], L"set") == 0 && argc == 4) {
        ULONG profile = ParseProfile(argv[2]);
        wchar_t* end = nullptr;
        unsigned long watts = wcstoul(argv[3], &end, 10);
        bool valid = end && *end == L'\0' && profile != NvpwrProfileUnknown;
        if (profile == NvpwrProfileRtx5050Laptop || profile == NvpwrProfileRtx5060Laptop || profile == NvpwrProfileRtx5070Laptop)
            valid = valid && watts >= 120 && watts <= 140 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx5070TiLaptop)
            valid = valid && watts >= 145 && watts <= 180 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx5080Laptop || profile == NvpwrProfileRtx5090Laptop)
            valid = valid && watts >= 175 && watts <= 225 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx4090Laptop)
            valid = valid && watts >= 150 && watts <= 250 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx4080Laptop)
            valid = valid && watts >= 150 && watts <= 225 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx4060Laptop || profile == NvpwrProfileRtx4070Laptop)
            valid = valid && watts >= 120 && watts <= 150 && (watts % 5) == 0;
        else if (profile == NvpwrProfileRtx4050Laptop)
            valid = valid && watts >= 115 && watts <= 140 && (watts % 5) == 0;
        if (!valid) {
            std::printf("4090: 150..250 W; 4080: 150..225 W; 4060/4070: 120..150 W; 4050: 115..140 W;\n"
                        "5050/5060/5070: 120..140 W; 5070ti: 145..180 W; 5080/5090: 175..225 W. Step 5 W.\n");
            CloseHandle(h);
            return 2;
        }
        NVPWR_SET_POWER req{};
        req.Version = NVPWR_SET_VERSION;
        req.TargetMilliwatts = (ULONG)watts * 1000u;
        req.Profile = profile;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_NVPWR_SET_POWER, &req, sizeof(req), nullptr, 0, &got, nullptr)) {
            std::printf("Set failed: Win32=%lu\n", GetLastError());
            rc = 6;
        }
        ShowStatus(h);
    } else {
        std::printf("Usage: NvpwrCtl status | set <4050|4060|4070|4080|4090|5050|5060|5070|5070ti|5080|5090> <watts> | restore\n");
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}
