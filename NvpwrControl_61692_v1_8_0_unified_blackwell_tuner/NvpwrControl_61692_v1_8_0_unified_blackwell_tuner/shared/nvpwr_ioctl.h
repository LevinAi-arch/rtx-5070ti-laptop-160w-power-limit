#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define NVPWR_DEVICE_WIN32 L"\\\\.\\Nvpwr"
#define NVPWR_STATUS_VERSION 7u
#define NVPWR_SET_VERSION 2u

#define IOCTL_NVPWR_STATUS    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVPWR_SET_POWER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NVPWR_RESTORE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

typedef enum _NVPWR_GPU_PROFILE {
    NvpwrProfileUnknown = 0,
    NvpwrProfileRtx5070TiLaptop = 1,
    NvpwrProfileRtx5080Laptop = 2,
    NvpwrProfileRtx5090Laptop = 3,
    NvpwrProfileRtx5050Laptop = 4,
    NvpwrProfileRtx5060Laptop = 5,
    NvpwrProfileRtx5070Laptop = 6,
    NvpwrProfileRtx4090Laptop = 7,
    NvpwrProfileRtx4080Laptop = 8,
    NvpwrProfileRtx4070Laptop = 9,
    NvpwrProfileRtx4060Laptop = 10,
    NvpwrProfileRtx4050Laptop = 11
} NVPWR_GPU_PROFILE;

typedef enum _NVPWR_STATE {
    NvpwrStateUnknown = 0,
    NvpwrStateArmed = 1,
    NvpwrStateApplied = 2,
    NvpwrStateMixed = 3,
    NvpwrStateWrongBuild = 4,
    NvpwrStateModuleNotFound = 5,
    NvpwrStateGpuNotFound = 6,
    NvpwrStateContextInvalid = 7,
    NvpwrStatePreconditionNotReady = 8,
    NvpwrStateStockBaseline = 9
} NVPWR_STATE;

typedef enum _NVPWR_DETAIL {
    NvpwrDetailOk = 0,
    NvpwrDetailPeIdentity = 1,
    NvpwrDetailCodeSignature = 2,
    NvpwrDetailGlobalPointer = 3,
    NvpwrDetailGpuTable = 4,
    NvpwrDetailMajorObject = 5,
    NvpwrDetailPowerRoot = 6,
    NvpwrDetailLookupFunction = 7,
    NvpwrDetailBoardObject = 8,
    NvpwrDetailBoardHandler = 9,
    NvpwrDetailSelector2Layout = 10,
    NvpwrDetailSelector3F7 = 11,
    NvpwrDetailTestState = 12,
    NvpwrDetailBoardSet = 13,
    NvpwrDetailVerify = 14,
    NvpwrDetailTargetRange = 15
} NVPWR_DETAIL;

#pragma pack(push, 8)
typedef struct _NVPWR_SET_POWER {
    ULONG Version;
    ULONG TargetMilliwatts;
    ULONG Profile;
    ULONG Reserved;
} NVPWR_SET_POWER;

typedef struct _NVPWR_STATUS {
    ULONG Version;
    ULONG State;
    LONG LastNtStatus;
    ULONG Detail;

    ULONGLONG ModuleBase;
    ULONG TimeDateStamp;
    ULONG SizeOfImage;

    ULONG RegistryCount;
    ULONG SelectedIndex;
    ULONG GpuId;
    ULONG Reserved0;

    ULONGLONG DriverGlobal;
    ULONGLONG GpuTable;
    ULONGLONG MajorObject;
    ULONGLONG PowerRoot;
    ULONGLONG LookupFunction;
    ULONGLONG BoardObject;
    ULONGLONG BoardSetFunction;

    UCHAR RootInitialized;
    UCHAR Eligibility;
    UCHAR AmountActive;
    UCHAR PolicyKey;
    ULONG CtgpTarget;
    ULONG PpabAmount;
    ULONG LowerBoundary;
    ULONG UpperBoundary;
    ULONG Aux28;
    ULONG Aux2C;

    UCHAR MaxMode;
    UCHAR MaxCount;
    UCHAR MaxSource0;
    UCHAR CurrentMode;
    UCHAR CurrentCount;
    UCHAR Reserved1[3];
    ULONG MaxEffective;
    ULONG MaxSecondary;
    ULONG MaxSource0Value;
    ULONG CurrentEffective;
    ULONG CurrentF7Value;
    ULONG PredictedF7;

    ULONG AppliedTarget;
    ULONG LastNvStatus;

    ULONG OemBaseline;
    ULONG ActiveProfile;
    ULONG SupportedMin;
    ULONG SupportedMax;
} NVPWR_STATUS;
#pragma pack(pop)
