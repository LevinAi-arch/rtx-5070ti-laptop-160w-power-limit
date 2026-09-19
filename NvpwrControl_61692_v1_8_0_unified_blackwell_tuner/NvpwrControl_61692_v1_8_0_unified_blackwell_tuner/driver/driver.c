#include <ntddk.h>
#include <ntimage.h>
#include <aux_klib.h>
#include "..\\shared\\nvpwr_ioctl.h"

/*
    NVPWR 1.5.0 EXPERIMENTAL — REVIEW / DEBUG NOTES

    WHERE:
      This driver never patches nvlddmkm.sys on disk and does not patch NVIDIA
      executable code. It resolves the already-loaded 616.92 kernel image,
      locates NVIDIA's live power-policy objects, changes the policy data fields
      used by the native generator, and calls NVIDIA's existing native setters.

    WHAT:
      root+3D14  = base / cTGP input
      root+3D18  = dynamic amount (our controlled model uses 25 W)
      root+3D24  = F7 saturation ceiling / UPPER
      selector2  = Board MAX (source FE)
      selector3  = CURRENT, including source F7 produced by NVIDIA's generator

    WHY TWO PHASES:
      Phase A changes base+amount while MAX/UPPER remain at the saved OEM
      ceiling. This proves that the real NVIDIA generator produces the expected
      staged state before the ceiling itself is raised.
      Phase B raises selector2/MAX and root+3D24/UPPER.
      Phase C re-runs NVIDIA's native generator and verifies that MAX, CURRENT,
      F7 and UPPER converge on the requested target.

    FAIL-CLOSED:
      Exact PE identity and machine-code signatures are checked before any
      mutation. On a failed transition we attempt to restore the exact baseline
      captured before the first write in this driver session.

    LOGGING:
      Kernel diagnostics use DbgPrintEx and are prefixed with [NVPWR].
      User-mode GUI activity is also written to
      C:\\ProgramData\\NvpwrControl\\nvpwr-control.log.
*/

#define NVPWR_LOG_INFO(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[NVPWR][INFO] " __VA_ARGS__)
#define NVPWR_LOG_WARN(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, "[NVPWR][WARN] " __VA_ARGS__)
#define NVPWR_LOG_ERROR(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[NVPWR][ERROR] " __VA_ARGS__)

#define NVPWR_TAG 'RWPN'

#define NVPWR_EXPECTED_TIMESTAMP 0x6A9B4070u
#define NVPWR_EXPECTED_SIZE      0x06D3E000u

#define RVA_GPU_GLOBAL           0x013B2E18u
#define RVA_GPU_REGISTRY_FN      0x00107AC0u
#define RVA_F7_GENERATOR         0x004E3EF0u
#define RVA_F7_UPPER_LOAD        0x004E3FBFu
#define RVA_F7_RECORD            0x004E4088u
#define RVA_BOARD_TYPE0_SET      0x008D6960u
#define RVA_SET_AMOUNT           0x004E4610u
#define RVA_SET_ELIG             0x004E4680u

#define OFF_GLOBAL_GPU_TABLE     0x0208u
#define OFF_TABLE_ENTRY_PTR      0x48A48u
#define OFF_TABLE_ENTRY_GPUID    0x48A50u
#define OFF_TABLE_COUNT          0x48C48u
#define GPU_ENTRY_STRIDE         0x10u
#define GPU_COUNT_MAX            32u

#define OFF_MAJOR_POWER_ROOT     0x25B0u
#define OFF_ROOT_REGISTRY        0x1CC0u
#define OFF_ROOT_LOOKUP_FN       0x1CF8u
#define OFF_ROOT_INIT            0x3D10u
#define OFF_ROOT_ELIG            0x3D11u
#define OFF_ROOT_AMOUNT_ACTIVE   0x3D12u
#define OFF_ROOT_CTGP            0x3D14u
#define OFF_ROOT_AMOUNT          0x3D18u
#define OFF_ROOT_POLICY_KEY      0x3D1Cu
#define OFF_ROOT_LOWER           0x3D20u
#define OFF_ROOT_UPPER           0x3D24u
#define OFF_ROOT_AUX28           0x3D28u
#define OFF_ROOT_AUX2C           0x3D2Cu

#define OFF_BOARD_SET_FN         0x2D0u
#define OFF_SELECTOR2            0x104u
#define OFF_SELECTOR3            0x1F4u

#define SLOT_MODE                0x00u
#define SLOT_COUNT               0x01u
#define SLOT_EFFECTIVE           0x04u
#define SLOT_SECONDARY           0x08u
#define SLOT_SOURCE_ID0          0x0Cu
#define SLOT_SOURCE_VALUE0       0x10u
#define SLOT_SOURCE_STRIDE       0x08u
#define SLOT_SOURCE_MAX          8u

#define SOURCE_FE                0xFEu
#define SOURCE_F7                0xF7u
#define SELECTOR_MAX             2u
#define SELECTOR_CURRENT         3u

#define POWER_LOW_STOCK           115000u
#define POWER_LOW_MIN             120000u
#define POWER_LOW_MAX             140000u
#define POWER_5070_STOCK          140000u
#define POWER_5070_MIN            145000u
#define POWER_5070_MAX            180000u
#define POWER_HIGH_MIN            175000u
#define POWER_HIGH_MAX            225000u

/* RTX 40 Series Laptop Power Constants */
#define POWER_4050_STOCK          115000u
#define POWER_4050_MIN            115000u
#define POWER_4050_MAX            140000u
#define POWER_4060_STOCK          140000u
#define POWER_4060_MIN            120000u
#define POWER_4060_MAX            150000u
#define POWER_4070_STOCK          140000u
#define POWER_4070_MIN            120000u
#define POWER_4070_MAX            150000u
#define POWER_4080_STOCK          175000u
#define POWER_4080_MIN            150000u
#define POWER_4080_MAX            225000u
#define POWER_4090_STOCK          175000u
#define POWER_4090_MIN            150000u
#define POWER_4090_MAX            250000u

#define POWER_ABSOLUTE_MIN        100000u
#define POWER_ABSOLUTE_MAX        250000u
#define POWER_STEP                5000u
#define PPAB_FIXED               25000u
#define INVALID_POWER            0xFFFFFFFFu

static const UCHAR g_SigGpuRegistry[] = {
    0x48,0x8B,0x05,0x51,0xB3,0x2A,0x01,0x44,0x8B,0xD9,0x4C,0x8B,0x88,0x08,0x02,0x00,0x00
};
static const UCHAR g_SigUpperLoad[] = { 0x44,0x8B,0x8B,0x24,0x3D,0x00,0x00 };
static const UCHAR g_SigF7Record[] = { 0x66,0xC7,0x44,0x24,0x48,0xF7,0x03 };
static const UCHAR g_SigBoardSet[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18
};
static const UCHAR g_SigSetAmount[] = {
    0x48,0x83,0xEC,0x38,0x48,0x8B,0x81,0xB0,0x25,0x00,0x00,0x44,0x8B,0xC2,0x80,0xB8,0x10,0x3D,0x00,0x00,0x00
};
static const UCHAR g_SigSetElig[] = {
    0x48,0x83,0xEC,0x28,0x48,0x8B,0x81,0xB0,0x25,0x00,0x00,0x80,0xB8,0x10,0x3D,0x00,0x00,0x00
};

static PDEVICE_OBJECT g_DeviceObject = NULL;
static UNICODE_STRING g_SymbolicLink;
static KMUTEX g_OperationMutex;
static volatile ULONG g_LastNvStatus = 0;
/* Tracks one controlled session. NVIDIA objects are not pinned; address equality
   is only a guard until explicit restore or reboot. */
static BOOLEAN g_MutationAttempted = FALSE;
static PVOID g_MutatedRoot = NULL;
static PVOID g_MutatedBoard = NULL;
static BOOLEAN g_SavedValid = FALSE;
static UCHAR g_SavedEligibility = 0;
static UCHAR g_SavedAmountActive = 0;
static ULONG g_SavedInput14 = 0;
static ULONG g_SavedAmount18 = 0;
static ULONG g_SavedUpper24 = 0;
static ULONG g_ActiveProfile = NvpwrProfileUnknown;

typedef PVOID (*PFN_POLICY_LOOKUP)(PVOID Registry, ULONG Key);
typedef ULONG (*PFN_BOARD_SET)(
    PVOID Major,
    PVOID Root,
    PVOID Board,
    ULONG Selector,
    ULONG Source,
    ULONG Value);
typedef ULONG (*PFN_SET_AMOUNT)(PVOID Major, ULONG Amount);
typedef ULONG (*PFN_SET_ELIG)(PVOID Major, ULONG Eligible);

typedef struct _NVPWR_CONTEXT {
    PUCHAR Base;
    ULONG ModuleSize;
    ULONG TimeDateStamp;
    ULONG SizeOfImage;
    PVOID DriverGlobal;
    PVOID GpuTable;
    ULONG RegistryCount;
    ULONG SelectedIndex;
    ULONG GpuId;
    PVOID Major;
    PVOID Root;
    PFN_POLICY_LOOKUP Lookup;
    PVOID Board;
    PFN_BOARD_SET BoardSet;
    PFN_SET_AMOUNT SetAmount;
    PFN_SET_ELIG SetEligibility;
} NVPWR_CONTEXT;

static BOOLEAN BytesEqual(const UCHAR* A, const UCHAR* B, SIZE_T N)
{
    SIZE_T i;
    for (i = 0; i < N; ++i) {
        if (A[i] != B[i]) return FALSE;
    }
    return TRUE;
}

static BOOLEAN IsKernelPointer(PVOID P)
{
    return P != NULL && (ULONG_PTR)P >= (ULONG_PTR)MmSystemRangeStart;
}

static BOOLEAN IsInsideImage(const NVPWR_CONTEXT* Ctx, PVOID P)
{
    ULONG_PTR p, b, e;
    if (!Ctx || !Ctx->Base || !P) return FALSE;
    p = (ULONG_PTR)P;
    b = (ULONG_PTR)Ctx->Base;
    e = b + Ctx->SizeOfImage;
    return p >= b && p < e;
}

static NTSTATUS FindNvlddmkm(PUCHAR* ImageBase, PULONG ImageSize)
{
    NTSTATUS status;
    ULONG bytes = 0;
    PAUX_MODULE_EXTENDED_INFO modules = NULL;
    ULONG count, i;
    ANSI_STRING wanted;

    if (!ImageBase || !ImageSize) return STATUS_INVALID_PARAMETER;
    *ImageBase = NULL;
    *ImageSize = 0;

    status = AuxKlibInitialize();
    if (!NT_SUCCESS(status)) return status;

    status = AuxKlibQueryModuleInformation(&bytes, sizeof(AUX_MODULE_EXTENDED_INFO), NULL);
    if (!NT_SUCCESS(status) || bytes == 0)
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;

    modules = (PAUX_MODULE_EXTENDED_INFO)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, NVPWR_TAG);
    if (!modules) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(modules, bytes);

    status = AuxKlibQueryModuleInformation(&bytes, sizeof(AUX_MODULE_EXTENDED_INFO), modules);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(modules, NVPWR_TAG);
        return status;
    }

    RtlInitAnsiString(&wanted, "nvlddmkm.sys");
    count = bytes / sizeof(AUX_MODULE_EXTENDED_INFO);
    for (i = 0; i < count; ++i) {
        ANSI_STRING current;
        const CHAR* baseName = (const CHAR*)&modules[i].FullPathName[modules[i].FileNameOffset];
        RtlInitAnsiString(&current, baseName);
        if (RtlEqualString(&current, &wanted, TRUE)) {
            *ImageBase = (PUCHAR)modules[i].BasicInfo.ImageBase;
            *ImageSize = modules[i].ImageSize;
            NVPWR_LOG_INFO("FindNvlddmkm: base=%p moduleSize=0x%lX\n", *ImageBase, *ImageSize);
            ExFreePoolWithTag(modules, NVPWR_TAG);
            return STATUS_SUCCESS;
        }
    }

    ExFreePoolWithTag(modules, NVPWR_TAG);
    NVPWR_LOG_ERROR("FindNvlddmkm: nvlddmkm.sys not found in loaded module list\n");
    return STATUS_NOT_FOUND;
}

static NTSTATUS ReadPeIdentity(PUCHAR ImageBase, PULONG TimeDateStamp, PULONG SizeOfImage)
{
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS64 nt;

    if (!ImageBase || !TimeDateStamp || !SizeOfImage) return STATUS_INVALID_PARAMETER;

    __try {
        dos = (PIMAGE_DOS_HEADER)ImageBase;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;
        nt = (PIMAGE_NT_HEADERS64)(ImageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return STATUS_INVALID_IMAGE_FORMAT;
        *TimeDateStamp = nt->FileHeader.TimeDateStamp;
        *SizeOfImage = nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ValidateBuild(NVPWR_CONTEXT* Ctx, PULONG Detail)
{
    NTSTATUS status;

    NVPWR_LOG_INFO("ValidateBuild: begin exact 616.92 guard\n");
    status = FindNvlddmkm(&Ctx->Base, &Ctx->ModuleSize);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("ValidateBuild: FindNvlddmkm failed NTSTATUS=0x%08X\n", status);
        return status;
    }

    status = ReadPeIdentity(Ctx->Base, &Ctx->TimeDateStamp, &Ctx->SizeOfImage);
    if (!NT_SUCCESS(status)) {
        if (Detail) *Detail = NvpwrDetailPeIdentity;
        return status;
    }

    NVPWR_LOG_INFO("ValidateBuild: timestamp=0x%08lX image=0x%08lX module=0x%08lX\n",
        Ctx->TimeDateStamp, Ctx->SizeOfImage, Ctx->ModuleSize);

    if (Ctx->TimeDateStamp != NVPWR_EXPECTED_TIMESTAMP ||
        Ctx->SizeOfImage != NVPWR_EXPECTED_SIZE ||
        Ctx->ModuleSize < Ctx->SizeOfImage) {
        NVPWR_LOG_ERROR("ValidateBuild: PE identity mismatch expected timestamp=0x%08lX image=0x%08lX\n",
            NVPWR_EXPECTED_TIMESTAMP, NVPWR_EXPECTED_SIZE);
        if (Detail) *Detail = NvpwrDetailPeIdentity;
        return STATUS_REVISION_MISMATCH;
    }

    __try {
        if (!BytesEqual(Ctx->Base + RVA_GPU_REGISTRY_FN, g_SigGpuRegistry, sizeof(g_SigGpuRegistry)) ||
            !BytesEqual(Ctx->Base + RVA_F7_UPPER_LOAD, g_SigUpperLoad, sizeof(g_SigUpperLoad)) ||
            !BytesEqual(Ctx->Base + RVA_F7_RECORD, g_SigF7Record, sizeof(g_SigF7Record)) ||
            !BytesEqual(Ctx->Base + RVA_BOARD_TYPE0_SET, g_SigBoardSet, sizeof(g_SigBoardSet)) ||
            !BytesEqual(Ctx->Base + RVA_SET_AMOUNT, g_SigSetAmount, sizeof(g_SigSetAmount)) ||
            !BytesEqual(Ctx->Base + RVA_SET_ELIG, g_SigSetElig, sizeof(g_SigSetElig))) {
            NVPWR_LOG_ERROR("ValidateBuild: one or more code signatures do not match analysed 616.92 image\n");
            if (Detail) *Detail = NvpwrDetailCodeSignature;
            return STATUS_REVISION_MISMATCH;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailCodeSignature;
        return GetExceptionCode();
    }

    NVPWR_LOG_INFO("ValidateBuild: exact PE + signature guard PASSED\n");
    return STATUS_SUCCESS;
}

static BOOLEAN FindSource(PUCHAR Slot, UCHAR Source, PULONG Value, PUCHAR Index)
{
    UCHAR count, i;
    if (!Slot || !Value) return FALSE;

    __try {
        count = Slot[SLOT_COUNT];
        if (count > SLOT_SOURCE_MAX) return FALSE;
        for (i = 0; i < count; ++i) {
            PUCHAR e = Slot + SLOT_SOURCE_ID0 + ((ULONG)i * SLOT_SOURCE_STRIDE);
            if (e[0] == Source) {
                *Value = *(UNALIGNED ULONG*)(e + 4);
                if (Index) *Index = i;
                return TRUE;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return FALSE;
}

static ULONG ComputeStrictActiveF7(PVOID Root)
{
    ULONG c, a, b, u, pre;
    UCHAR amountActive, eligible;

    __try {
        amountActive = *(volatile UCHAR*)((PUCHAR)Root + OFF_ROOT_AMOUNT_ACTIVE);
        eligible = *(volatile UCHAR*)((PUCHAR)Root + OFF_ROOT_ELIG);
        c = *(volatile ULONG*)((PUCHAR)Root + OFF_ROOT_CTGP);
        a = *(volatile ULONG*)((PUCHAR)Root + OFF_ROOT_AMOUNT);
        b = *(volatile ULONG*)((PUCHAR)Root + OFF_ROOT_LOWER);
        u = *(volatile ULONG*)((PUCHAR)Root + OFF_ROOT_UPPER);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return INVALID_POWER;
    }

    /* This is the exact 4E3EF0 finite-cTGP branch used by the planned proof:
       amountActive=1, eligibility=1, U>B, A<=U-B.
       It reduces to min(C, U-A) + A == min(C+A, U). */
    if (!amountActive || !eligible || c == INVALID_POWER || u <= b || a > (u - b))
        return INVALID_POWER;

    pre = u - a;
    if (c < pre) pre = c;
    if (pre > (0xFFFFFFFFu - a)) return INVALID_POWER;
    return pre + a;
}

static NTSTATUS ResolveContext(NVPWR_CONTEXT* Ctx, PULONG Detail)
{
    NTSTATUS status;
    ULONG i, count;
    PVOID globalState, table;

    if (!Ctx) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    if (Detail) *Detail = NvpwrDetailOk;

    NVPWR_LOG_INFO("ResolveContext: begin\n");
    status = ValidateBuild(Ctx, Detail);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("ResolveContext: build validation failed NTSTATUS=0x%08X detail=%lu\n",
            status, Detail ? *Detail : 0u);
        return status;
    }

    __try {
        globalState = *(PVOID*)(Ctx->Base + RVA_GPU_GLOBAL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGlobalPointer;
        return GetExceptionCode();
    }
    if (!IsKernelPointer(globalState)) {
        if (Detail) *Detail = NvpwrDetailGlobalPointer;
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->DriverGlobal = globalState;

    __try {
        table = *(PVOID*)((PUCHAR)globalState + OFF_GLOBAL_GPU_TABLE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return GetExceptionCode();
    }
    if (!IsKernelPointer(table)) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->GpuTable = table;

    __try {
        count = *(volatile ULONG*)((PUCHAR)table + OFF_TABLE_COUNT);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return GetExceptionCode();
    }
    Ctx->RegistryCount = count;
    NVPWR_LOG_INFO("ResolveContext: NVIDIA GPU registry count=%lu\n", count);
    if (count == 0 || count > GPU_COUNT_MAX) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return STATUS_DEVICE_NOT_READY;
    }

    for (i = 0; i < count; ++i) {
        PVOID major = NULL, root = NULL, board = NULL, lookupRaw = NULL, boardSetRaw = NULL;
        ULONG gpuId = 0;
        UCHAR init = 0, key = 0;
        PFN_POLICY_LOOKUP lookup;

        __try {
            major = *(PVOID*)((PUCHAR)table + OFF_TABLE_ENTRY_PTR + ((SIZE_T)i * GPU_ENTRY_STRIDE));
            gpuId = *(volatile ULONG*)((PUCHAR)table + OFF_TABLE_ENTRY_GPUID + ((SIZE_T)i * GPU_ENTRY_STRIDE));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!IsKernelPointer(major)) continue;

        __try {
            root = *(PVOID*)((PUCHAR)major + OFF_MAJOR_POWER_ROOT);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!IsKernelPointer(root)) continue;

        __try {
            init = *(volatile UCHAR*)((PUCHAR)root + OFF_ROOT_INIT);
            key = *(volatile UCHAR*)((PUCHAR)root + OFF_ROOT_POLICY_KEY);
            lookupRaw = *(PVOID*)((PUCHAR)root + OFF_ROOT_LOOKUP_FN);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (init != 1 || key >= 0x40 || !IsInsideImage(Ctx, lookupRaw)) continue;

        lookup = (PFN_POLICY_LOOKUP)lookupRaw;
        __try {
            board = lookup((PUCHAR)root + OFF_ROOT_REGISTRY, (ULONG)key);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            board = NULL;
        }
        if (!IsKernelPointer(board)) continue;

        __try {
            boardSetRaw = *(PVOID*)((PUCHAR)board + OFF_BOARD_SET_FN);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (boardSetRaw != (PVOID)(Ctx->Base + RVA_BOARD_TYPE0_SET)) continue;

        Ctx->SelectedIndex = i;
        Ctx->GpuId = gpuId;
        Ctx->Major = major;
        Ctx->Root = root;
        Ctx->Lookup = lookup;
        Ctx->Board = board;
        Ctx->BoardSet = (PFN_BOARD_SET)boardSetRaw;
        Ctx->SetAmount = (PFN_SET_AMOUNT)(Ctx->Base + RVA_SET_AMOUNT);
        Ctx->SetEligibility = (PFN_SET_ELIG)(Ctx->Base + RVA_SET_ELIG);
        NVPWR_LOG_INFO(
            "ResolveContext: selected index=%lu gpuId=0x%08lX major=%p root=%p board=%p boardSet=%p\n",
            Ctx->SelectedIndex, Ctx->GpuId, Ctx->Major, Ctx->Root, Ctx->Board, Ctx->BoardSet);
        return STATUS_SUCCESS;
    }

    NVPWR_LOG_ERROR("ResolveContext: no compatible NVIDIA power-policy object found\n");
    if (Detail) *Detail = NvpwrDetailGpuTable;
    return STATUS_NOT_FOUND;
}

static VOID FillStatusFromContext(const NVPWR_CONTEXT* Ctx, NVPWR_STATUS* Out)
{
    PUCHAR maxSlot, currentSlot;
    ULONG f7 = INVALID_POWER;

    Out->ModuleBase = (ULONGLONG)(ULONG_PTR)Ctx->Base;
    Out->TimeDateStamp = Ctx->TimeDateStamp;
    Out->SizeOfImage = Ctx->SizeOfImage;
    Out->RegistryCount = Ctx->RegistryCount;
    Out->SelectedIndex = Ctx->SelectedIndex;
    Out->GpuId = Ctx->GpuId;
    Out->DriverGlobal = (ULONGLONG)(ULONG_PTR)Ctx->DriverGlobal;
    Out->GpuTable = (ULONGLONG)(ULONG_PTR)Ctx->GpuTable;
    Out->MajorObject = (ULONGLONG)(ULONG_PTR)Ctx->Major;
    Out->PowerRoot = (ULONGLONG)(ULONG_PTR)Ctx->Root;
    Out->LookupFunction = (ULONGLONG)(ULONG_PTR)Ctx->Lookup;
    Out->BoardObject = (ULONGLONG)(ULONG_PTR)Ctx->Board;
    Out->BoardSetFunction = (ULONGLONG)(ULONG_PTR)Ctx->BoardSet;

    maxSlot = (PUCHAR)Ctx->Board + OFF_SELECTOR2;
    currentSlot = (PUCHAR)Ctx->Board + OFF_SELECTOR3;

    __try {
        Out->RootInitialized = *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_INIT);
        Out->Eligibility = *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_ELIG);
        Out->AmountActive = *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT_ACTIVE);
        Out->PolicyKey = *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_POLICY_KEY);
        Out->CtgpTarget = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_CTGP);
        Out->PpabAmount = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT);
        Out->LowerBoundary = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_LOWER);
        Out->UpperBoundary = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_UPPER);
        Out->Aux28 = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_AUX28);
        Out->Aux2C = *(volatile ULONG*)((PUCHAR)Ctx->Root + OFF_ROOT_AUX2C);

        Out->MaxMode = maxSlot[SLOT_MODE];
        Out->MaxCount = maxSlot[SLOT_COUNT];
        Out->MaxSource0 = maxSlot[SLOT_SOURCE_ID0];
        Out->MaxEffective = *(volatile ULONG*)(maxSlot + SLOT_EFFECTIVE);
        Out->MaxSecondary = *(volatile ULONG*)(maxSlot + SLOT_SECONDARY);
        Out->MaxSource0Value = *(volatile ULONG*)(maxSlot + SLOT_SOURCE_VALUE0);

        Out->CurrentMode = currentSlot[SLOT_MODE];
        Out->CurrentCount = currentSlot[SLOT_COUNT];
        Out->CurrentEffective = *(volatile ULONG*)(currentSlot + SLOT_EFFECTIVE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailBoardObject;
        Out->LastNtStatus = GetExceptionCode();
        return;
    }

    if (FindSource(currentSlot, SOURCE_F7, &f7, NULL))
        Out->CurrentF7Value = f7;
    else
        Out->CurrentF7Value = INVALID_POWER;

    Out->PredictedF7 = ComputeStrictActiveF7(Ctx->Root);
    Out->LastNvStatus = g_LastNvStatus;
    Out->ActiveProfile = g_ActiveProfile;
    if (g_SavedValid && Ctx->Root == g_MutatedRoot && Ctx->Board == g_MutatedBoard)
        Out->OemBaseline = g_SavedUpper24;
    if (g_ActiveProfile == NvpwrProfileRtx5050Laptop ||
        g_ActiveProfile == NvpwrProfileRtx5060Laptop || g_ActiveProfile == NvpwrProfileRtx5070Laptop) {
        Out->SupportedMin = POWER_LOW_MIN;
        Out->SupportedMax = POWER_LOW_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx5070TiLaptop) {
        Out->SupportedMin = POWER_5070_MIN;
        Out->SupportedMax = POWER_5070_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx5080Laptop || g_ActiveProfile == NvpwrProfileRtx5090Laptop) {
        Out->SupportedMin = POWER_HIGH_MIN;
        Out->SupportedMax = POWER_HIGH_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4090Laptop) {
        Out->SupportedMin = POWER_4090_MIN;
        Out->SupportedMax = POWER_4090_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4080Laptop) {
        Out->SupportedMin = POWER_4080_MIN;
        Out->SupportedMax = POWER_4080_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4070Laptop || g_ActiveProfile == NvpwrProfileRtx4060Laptop) {
        Out->SupportedMin = POWER_4060_MIN;
        Out->SupportedMax = POWER_4060_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4050Laptop) {
        Out->SupportedMin = POWER_4050_MIN;
        Out->SupportedMax = POWER_4050_MAX;
    }

    if (Out->MaxMode != 0 || Out->MaxCount != 1 || Out->MaxSource0 != SOURCE_FE) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailSelector2Layout;
        Out->LastNtStatus = STATUS_DATA_ERROR;
        return;
    }
    if (Out->CurrentF7Value == INVALID_POWER) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailSelector3F7;
        Out->LastNtStatus = STATUS_NOT_FOUND;
        return;
    }

    /* General applied state: base=(target-25 W), amount=25 W, and the
       generator/board/current paths all agree on the same target. */
    if (Out->UpperBoundary >= POWER_ABSOLUTE_MIN && Out->UpperBoundary <= POWER_ABSOLUTE_MAX &&
        (Out->UpperBoundary % POWER_STEP) == 0 &&
        Out->MaxEffective == Out->UpperBoundary &&
        Out->MaxSource0Value == Out->UpperBoundary &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 1 &&
        Out->AmountActive == 1 &&
        Out->PpabAmount == PPAB_FIXED &&
        Out->CtgpTarget == (Out->UpperBoundary - PPAB_FIXED) &&
        Out->CurrentF7Value == Out->UpperBoundary &&
        Out->CurrentEffective == Out->UpperBoundary &&
        Out->PredictedF7 == Out->UpperBoundary) {
        Out->State = NvpwrStateApplied;
        Out->AppliedTarget = Out->UpperBoundary;
        Out->OemBaseline = g_SavedValid ? g_SavedUpper24 : 0;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    /* Internal staging state: keep the saved OEM ceiling while changing
       generator inputs. This is dynamic so the same 616.92 code path can be
       evaluated on higher-power Blackwell Laptop GPUs without assuming 140 W. */
    if (g_SavedValid && Ctx->Root == g_MutatedRoot && Ctx->Board == g_MutatedBoard &&
        Out->UpperBoundary == g_SavedUpper24 &&
        Out->MaxEffective == g_SavedUpper24 &&
        Out->MaxSource0Value == g_SavedUpper24 &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 1 &&
        Out->AmountActive == 1 &&
        Out->PpabAmount == PPAB_FIXED &&
        Out->CtgpTarget >= (POWER_ABSOLUTE_MIN - PPAB_FIXED) &&
        Out->CtgpTarget <= (POWER_ABSOLUTE_MAX - PPAB_FIXED) &&
        Out->CurrentF7Value == g_SavedUpper24 &&
        Out->CurrentEffective == g_SavedUpper24 &&
        Out->PredictedF7 == g_SavedUpper24) {
        Out->State = NvpwrStateArmed;
        Out->AppliedTarget = Out->CtgpTarget + PPAB_FIXED;
        Out->OemBaseline = g_SavedUpper24;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    /* Generic coherent OEM baseline for this exact 616.92 KMD layout.
       No hard-coded 140 W assumption: all Board/F7 paths must agree and the
       generator's dynamic amount must be inactive. */
    if (Out->UpperBoundary >= 100000u && Out->UpperBoundary <= 200000u &&
        Out->MaxEffective == Out->UpperBoundary &&
        Out->MaxSource0Value == Out->UpperBoundary &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 0 &&
        Out->AmountActive == 0 &&
        Out->CtgpTarget == Out->UpperBoundary &&
        Out->PpabAmount == 0 &&
        Out->LowerBoundary < Out->UpperBoundary &&
        Out->CurrentF7Value == Out->UpperBoundary &&
        Out->CurrentEffective == Out->UpperBoundary) {
        Out->State = NvpwrStateStockBaseline;
        Out->AppliedTarget = Out->UpperBoundary;
        Out->OemBaseline = Out->UpperBoundary;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    if (g_SavedValid &&
        Out->UpperBoundary == g_SavedUpper24 &&
        Out->MaxEffective == g_SavedUpper24 &&
        Out->MaxSource0Value == g_SavedUpper24) {
        Out->State = NvpwrStatePreconditionNotReady;
        Out->Detail = NvpwrDetailTestState;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    Out->State = NvpwrStateMixed;
    Out->LastNtStatus = STATUS_SUCCESS;
}

static NTSTATUS QueryStatus(NVPWR_STATUS* Out)
{
    NVPWR_CONTEXT ctx;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;

    if (!Out) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = NVPWR_STATUS_VERSION;
    Out->CurrentF7Value = INVALID_POWER;
    Out->PredictedF7 = INVALID_POWER;

    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) {
        Out->Detail = detail;
        Out->LastNtStatus = status;
        if (status == STATUS_NOT_FOUND && ctx.Base == NULL)
            Out->State = NvpwrStateModuleNotFound;
        else if (status == STATUS_REVISION_MISMATCH)
            Out->State = NvpwrStateWrongBuild;
        else if (status == STATUS_NOT_FOUND)
            Out->State = NvpwrStateGpuNotFound;
        else
            Out->State = NvpwrStateContextInvalid;
        Out->ModuleBase = (ULONGLONG)(ULONG_PTR)ctx.Base;
        Out->TimeDateStamp = ctx.TimeDateStamp;
        Out->SizeOfImage = ctx.SizeOfImage;
        Out->RegistryCount = ctx.RegistryCount;
        return STATUS_SUCCESS; /* return diagnostics to user mode */
    }

    FillStatusFromContext(&ctx, Out);
    return STATUS_SUCCESS;
}

/*
    All Board writes go through NVIDIA's own type-0 Board setter.
    Logging both the request and NVIDIA's return code lets reviewers distinguish
    "we attempted a write" from "the vendor path accepted the write".
*/
static ULONG CallBoardSet(NVPWR_CONTEXT* Ctx, ULONG Selector, ULONG Source, ULONG Value)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->BoardSet) {
        NVPWR_LOG_ERROR("BoardSet: missing context/handler selector=%lu source=0x%02lX value=%lu\n",
            Selector, Source, Value);
        return nv;
    }
    NVPWR_LOG_INFO("BoardSet: selector=%lu source=0x%02lX value=%lu mW\n",
        Selector, Source, Value);
    __try {
        nv = Ctx->BoardSet(Ctx->Major, Ctx->Root, Ctx->Board, Selector, Source, Value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        nv = 0xFFFFFFFFu;
    }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv == 0)
        NVPWR_LOG_INFO("BoardSet: accepted nvStatus=0x%08lX\n", nv);
    else
        NVPWR_LOG_ERROR("BoardSet: rejected/failed nvStatus=0x%08lX\n", nv);
    return nv;
}

static ULONG CallSetAmount(NVPWR_CONTEXT* Ctx, ULONG Amount)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->SetAmount) {
        NVPWR_LOG_ERROR("SetAmount: missing context/handler amount=%lu\n", Amount);
        return nv;
    }
    NVPWR_LOG_INFO("SetAmount: request amount=%lu mW\n", Amount);
    __try { nv = Ctx->SetAmount(Ctx->Major, Amount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { nv = 0xFFFFFFFFu; }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv != 0) NVPWR_LOG_ERROR("SetAmount: nvStatus=0x%08lX\n", nv);
    else NVPWR_LOG_INFO("SetAmount: accepted\n");
    return nv;
}

static ULONG CallSetEligibility(NVPWR_CONTEXT* Ctx, ULONG Eligible)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->SetEligibility) {
        NVPWR_LOG_ERROR("SetEligibility: missing context/handler eligible=%lu\n", Eligible);
        return nv;
    }
    NVPWR_LOG_INFO("SetEligibility: request eligible=%lu (also re-runs NVIDIA F7 generator)\n", Eligible);
    __try { nv = Ctx->SetEligibility(Ctx->Major, Eligible); }
    __except (EXCEPTION_EXECUTE_HANDLER) { nv = 0xFFFFFFFFu; }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv != 0) NVPWR_LOG_ERROR("SetEligibility: nvStatus=0x%08lX\n", nv);
    else NVPWR_LOG_INFO("SetEligibility: accepted\n");
    return nv;
}

/*
    Capture the exact live OEM state before the first mutation.
    WHY: rollback must restore what this machine actually had, not a guessed
    universal wattage. Root/Board identity is remembered too, so we refuse to
    restore into a different NVIDIA object after an unexpected rebuild.
*/
static VOID SaveBaseline(const NVPWR_CONTEXT* Ctx, const NVPWR_STATUS* S)
{
    NVPWR_LOG_INFO(
        "SaveBaseline: root=%p board=%p elig=%u amountActive=%u base=%lu amount=%lu upper=%lu\n",
        Ctx->Root, Ctx->Board, S->Eligibility, S->AmountActive,
        S->CtgpTarget, S->PpabAmount, S->UpperBoundary);
    g_MutationAttempted = TRUE;
    g_MutatedRoot = Ctx->Root;
    g_MutatedBoard = Ctx->Board;
    g_SavedEligibility = S->Eligibility;
    g_SavedAmountActive = S->AmountActive;
    g_SavedInput14 = S->CtgpTarget;
    g_SavedAmount18 = S->PpabAmount;
    g_SavedUpper24 = S->UpperBoundary;
    g_SavedValid = TRUE;
}

static NTSTATUS RestoreSavedRoot(NVPWR_CONTEXT* Ctx)
{
    ULONG nv;
    if (!g_SavedValid || Ctx->Root != g_MutatedRoot || Ctx->Board != g_MutatedBoard) {
        NVPWR_LOG_ERROR("RestoreSavedRoot: saved context invalid or NVIDIA object identity changed\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    NVPWR_LOG_WARN(
        "RestoreSavedRoot: restoring base=%lu amount=%lu upper=%lu elig=%u amountActive=%u\n",
        g_SavedInput14, g_SavedAmount18, g_SavedUpper24,
        g_SavedEligibility, g_SavedAmountActive);

    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_UPPER), (LONG)g_SavedUpper24);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_CTGP), (LONG)g_SavedInput14);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT), (LONG)g_SavedAmount18);
    *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT_ACTIVE) = g_SavedAmountActive;
    KeMemoryBarrier();

    /* Native eligibility setter also invokes the real 4E3EF0 generator. */
    nv = CallSetEligibility(Ctx, g_SavedEligibility);
    return (nv == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS RestoreStock(VOID);

static BOOLEAN IsSupportedTarget(ULONG Profile, ULONG Target, ULONG OemBaseline)
{
    if ((Target % POWER_STEP) != 0) return FALSE;

    if (Profile == NvpwrProfileRtx5050Laptop ||
        Profile == NvpwrProfileRtx5060Laptop || Profile == NvpwrProfileRtx5070Laptop) {
        /*
            LOW-POWER PROFILE SAFETY GATE
            WHERE: 5050/5060/5070 Laptop requests.
            WHAT: authorize 120..140 W only when the live coherent OEM ceiling
                  is exactly 115 W on this audited NVIDIA runtime layout.
            WHY: model-name matching is not permission to raise power.  An OEM
                 configured for a lower ceiling may have a different VRM/EC/
                 thermal design, so it fails closed instead of being treated as
                 an equivalent 115 W platform.
        */
        if (OemBaseline != POWER_LOW_STOCK) return FALSE;
        return Target >= POWER_LOW_MIN && Target <= POWER_LOW_MAX;
    }

    if (Profile == NvpwrProfileRtx5070TiLaptop) {
        if (OemBaseline != POWER_5070_STOCK) return FALSE;
        return Target >= POWER_5070_MIN && Target <= POWER_5070_MAX;
    }

    if (Profile == NvpwrProfileRtx5080Laptop || Profile == NvpwrProfileRtx5090Laptop) {
        /* High-power profiles are intentionally accepted only when the live OEM
           baseline is already in the 150-175 W class. This prevents selecting a
           5080/5090 profile on the verified 140 W 5070 Ti machine. */
        if (OemBaseline < 150000u || OemBaseline > 175000u) return FALSE;
        return Target >= POWER_HIGH_MIN && Target <= POWER_HIGH_MAX;
    }

    if (Profile == NvpwrProfileRtx4090Laptop) {
        /* RTX 4090 Laptop GPU: OEM baseline typically 115..175 W depending on OEM mode (e.g. Balanced 130 W / Extreme 175 W).
           Unlock target: 150..250 W. */
        if (OemBaseline < 115000u || OemBaseline > 175000u) return FALSE;
        return Target >= POWER_4090_MIN && Target <= POWER_4090_MAX;
    }

    if (Profile == NvpwrProfileRtx4080Laptop) {
        /* RTX 4080 Laptop GPU: OEM baseline typically 115..175 W. Unlock target: 150..225 W. */
        if (OemBaseline < 115000u || OemBaseline > 175000u) return FALSE;
        return Target >= POWER_4080_MIN && Target <= POWER_4080_MAX;
    }

    if (Profile == NvpwrProfileRtx4070Laptop || Profile == NvpwrProfileRtx4060Laptop) {
        /* RTX 4060 / 4070 Laptop GPU: OEM baseline typically 95..140 W. Target: 120..150 W. */
        if (OemBaseline < 95000u || OemBaseline > 140000u) return FALSE;
        return Target >= POWER_4060_MIN && Target <= POWER_4060_MAX;
    }

    if (Profile == NvpwrProfileRtx4050Laptop) {
        /* RTX 4050 Laptop GPU: OEM baseline typically 75..140 W. Target: 115..140 W. */
        if (OemBaseline < 75000u || OemBaseline > 140000u) return FALSE;
        return Target >= POWER_4050_MIN && Target <= POWER_4050_MAX;
    }

    return FALSE;
}

static NTSTATUS RestoreContextToBaseline(NVPWR_CONTEXT* Ctx)
{
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG nv;

    if (!Ctx || !g_SavedValid) return STATUS_INVALID_DEVICE_STATE;
    NVPWR_LOG_WARN("Rollback: begin restore to captured OEM baseline=%lu mW\n", g_SavedUpper24);
    status = RestoreSavedRoot(Ctx);
    if (!NT_SUCCESS(status)) return status;

    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, g_SavedUpper24);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    if (s.State == NvpwrStateStockBaseline) {
        NVPWR_LOG_INFO("Rollback: VERIFIED OEM baseline restored upper=%lu current=%lu\n",
            s.UpperBoundary, s.CurrentEffective);
        return STATUS_SUCCESS;
    }
    NVPWR_LOG_ERROR("Rollback: verification FAILED state=%lu upper=%lu max=%lu current=%lu f7=%lu\n",
        s.State, s.UpperBoundary, s.MaxEffective, s.CurrentEffective, s.CurrentF7Value);
    return STATUS_DATA_ERROR;
}

static NTSTATUS StageTargetAtStockCeiling(NVPWR_CONTEXT* Ctx, ULONG Target, ULONG Stock)
{
    NVPWR_STATUS s;
    ULONG nv;
    ULONG base;

    if (!Ctx || Target < POWER_ABSOLUTE_MIN || Target > POWER_ABSOLUTE_MAX || Stock == 0)
        return STATUS_INVALID_PARAMETER;

    base = Target - PPAB_FIXED;

    /*
        PHASE A — STAGE UNDER OEM CEILING
        WHAT: base=(target-25 W), amount=25 W, eligibility=1.
        WHY: MAX/UPPER stay at the saved OEM ceiling first so NVIDIA's own F7
             generator must prove the split is coherent before the ceiling moves.
    */
    NVPWR_LOG_INFO(
        "Phase A: target=%lu base=%lu amount=%lu savedOemCeiling=%lu\n",
        Target, base, PPAB_FIXED, Stock);

    /* Keep the saved OEM ceiling while changing generator inputs. */
    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, Stock);
    if (nv != 0) return STATUS_UNSUCCESSFUL;
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_UPPER), (LONG)Stock);
    KeMemoryBarrier();

    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_CTGP), (LONG)base);
    KeMemoryBarrier();

    nv = CallSetAmount(Ctx, PPAB_FIXED);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    nv = CallSetEligibility(Ctx, 1);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    NVPWR_LOG_INFO(
        "Phase A verify: state=%lu stagedTarget=%lu upper=%lu max=%lu current=%lu f7=%lu predicted=%lu\n",
        s.State, s.AppliedTarget, s.UpperBoundary, s.MaxEffective,
        s.CurrentEffective, s.CurrentF7Value, s.PredictedF7);
    if (s.State != NvpwrStateArmed || s.AppliedTarget != Target) {
        NVPWR_LOG_ERROR("Phase A: verification FAILED\n");
        return STATUS_DATA_ERROR;
    }

    NVPWR_LOG_INFO("Phase A: PASSED\n");
    return STATUS_SUCCESS;
}

static NTSTATUS SetPowerTarget(ULONG Profile, ULONG Target)
{
    NVPWR_CONTEXT ctx;
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;
    ULONG nv;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    NVPWR_LOG_INFO("SetPowerTarget: request profile=%lu target=%lu mW\n", Profile, Target);
    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);

    {
        ULONG baseline = g_SavedValid ? g_SavedUpper24 :
            ((s.State == NvpwrStateStockBaseline) ? s.UpperBoundary : 0);
        NVPWR_LOG_INFO("SetPowerTarget: liveState=%lu baseline=%lu mW activeProfile=%lu savedValid=%u\n",
            s.State, baseline, g_ActiveProfile, g_SavedValid);
        if (!IsSupportedTarget(Profile, Target, baseline)) {
            NVPWR_LOG_ERROR(
                "SetPowerTarget: target/profile rejected profile=%lu target=%lu baseline=%lu\n",
                Profile, Target, baseline);
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (s.State == NvpwrStateApplied && s.AppliedTarget == Target)
        return STATUS_SUCCESS;

    if (!g_SavedValid) {
        if (s.State != NvpwrStateStockBaseline)
            return STATUS_INVALID_DEVICE_STATE;
        SaveBaseline(&ctx, &s);
        g_ActiveProfile = Profile;
    } else {
        if (g_ActiveProfile != Profile) return STATUS_INVALID_DEVICE_STATE;
        if (ctx.Root != g_MutatedRoot || ctx.Board != g_MutatedBoard)
            return STATUS_INVALID_DEVICE_STATE;
        if (s.State != NvpwrStateStockBaseline &&
            s.State != NvpwrStateArmed &&
            s.State != NvpwrStateApplied)
            return STATUS_INVALID_DEVICE_STATE;
    }

    /* Phase A: prove the requested split under the saved OEM ceiling. */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE A (stage under OEM ceiling)\n");
    status = StageTargetAtStockCeiling(&ctx, Target, g_SavedUpper24);
    if (!NT_SUCCESS(status)) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE A failed NTSTATUS=0x%08X; rolling back\n", status);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return status;
    }

    /*
        PHASE B — RAISE THE TWO CEILINGS
        selector2/source FE is Board MAX; root+3D24 is the saturation ceiling
        consumed by F7. Raising only one is not enough for the proven path.
    */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE B target=%lu\n", Target);
    nv = CallBoardSet(&ctx, SELECTOR_MAX, SOURCE_FE, Target);
    if (nv != 0) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE B Board MAX failed nvStatus=0x%08lX; rolling back\n", nv);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_UNSUCCESSFUL;
    }

    NVPWR_LOG_INFO("Phase B: writing root+3D24 UPPER=%lu mW\n", Target);
    InterlockedExchange((volatile LONG*)((PUCHAR)ctx.Root + OFF_ROOT_UPPER), (LONG)Target);
    KeMemoryBarrier();

    /*
        PHASE C — REGENERATE + VERIFY
        Calling the native eligibility setter with 1 re-enters NVIDIA's real
        generator. selector3/CURRENT is not fabricated by Nvpwr here.
    */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE C (native generator + final readback)\n");
    nv = CallSetEligibility(&ctx, 1);
    if (nv != 0) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE C generator failed nvStatus=0x%08lX; rolling back\n", nv);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_UNSUCCESSFUL;
    }

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);
    NVPWR_LOG_INFO(
        "Final verify: state=%lu target=%lu base=%lu amount=%lu upper=%lu max=%lu current=%lu f7=%lu predicted=%lu\n",
        s.State, s.AppliedTarget, s.CtgpTarget, s.PpabAmount, s.UpperBoundary,
        s.MaxEffective, s.CurrentEffective, s.CurrentF7Value, s.PredictedF7);
    if (s.State != NvpwrStateApplied || s.AppliedTarget != Target) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: FINAL verification FAILED; rolling back\n");
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_DATA_ERROR;
    }

    NVPWR_LOG_INFO("SetPowerTarget: SUCCESS target=%lu mW\n", Target);
    return STATUS_SUCCESS;
}

static BOOLEAN IsRecoverableExternalModifiedState(const NVPWR_STATUS* S)
{
    if (!S) return FALSE;

    /* Recognize only the exact family produced by our earlier 616.92 proofs:
       known 145..160 W ceiling, FE MAX agreeing with current/F7, initialized
       generator, eligibility+amount active, and the verified 25 W amount.
       CtgpTarget is intentionally not constrained to target-25 because an
       NVIDIA/Lenovo policy refresh can update the base while leaving the
       raised ceiling live; that is the MIXED state v1.2 exposed. */
    if (S->UpperBoundary < 145000u || S->UpperBoundary > 160000u)
        return FALSE;
    if ((S->UpperBoundary % POWER_STEP) != 0)
        return FALSE;
    if (S->MaxMode != 0 || S->MaxCount != 1 || S->MaxSource0 != SOURCE_FE)
        return FALSE;
    if (S->MaxEffective != S->UpperBoundary ||
        S->MaxSource0Value != S->UpperBoundary ||
        S->CurrentEffective != S->UpperBoundary ||
        S->CurrentF7Value != S->UpperBoundary ||
        S->PredictedF7 != S->UpperBoundary)
        return FALSE;
    if (S->RootInitialized != 1 || S->Eligibility != 1 || S->AmountActive != 1)
        return FALSE;
    /* Accept the current 25 W model and the two earlier proof splits that were
       actually verified on this machine: 150=120+30 and 160=120+40. */
    if (S->PpabAmount != PPAB_FIXED &&
        !(S->UpperBoundary == 150000u && S->PpabAmount == 30000u) &&
        !(S->UpperBoundary == 160000u && S->PpabAmount == 40000u))
        return FALSE;
    return TRUE;
}

static NTSTATUS ForceKnownStockBaseline(NVPWR_CONTEXT* Ctx)
{
    NVPWR_STATUS s;
    ULONG nv;

    if (!Ctx) return STATUS_INVALID_PARAMETER;

    /* Reconstruct the exact stock baseline observed and repeatedly verified on
       this 616.92 machine. Order mirrors the successful saved-state rollback:
       restore generator inputs/ceiling, regenerate through NVIDIA's native
       eligibility setter, then restore the board MAX source FE. */
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_UPPER), (LONG)POWER_5070_STOCK);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_CTGP), (LONG)POWER_5070_STOCK);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT), 0);
    *(volatile UCHAR*)((PUCHAR)Ctx->Root + OFF_ROOT_AMOUNT_ACTIVE) = 0;
    KeMemoryBarrier();

    nv = CallSetEligibility(Ctx, 0);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, POWER_5070_STOCK);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    if (s.State != NvpwrStateStockBaseline)
        return STATUS_DATA_ERROR;

    g_MutationAttempted = FALSE;
    g_MutatedRoot = NULL;
    g_MutatedBoard = NULL;
    g_SavedValid = FALSE;
    g_SavedEligibility = 0;
    g_SavedAmountActive = 0;
    g_SavedInput14 = 0;
    g_SavedAmount18 = 0;
    g_SavedUpper24 = 0;
    g_ActiveProfile = NvpwrProfileUnknown;
    return STATUS_SUCCESS;
}

static NTSTATUS RestoreStock(VOID)
{
    NVPWR_CONTEXT ctx;
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;
    ULONG nv;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    NVPWR_LOG_WARN("RestoreStock: request received\n");
    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);

    if (s.State == NvpwrStateStockBaseline) {
        NVPWR_LOG_INFO("RestoreStock: already at coherent OEM baseline=%lu mW\n", s.UpperBoundary);
        return STATUS_SUCCESS;
    }

    /* Normal same-session restore: use the state captured before our write. */
    if (g_MutationAttempted && g_SavedValid) {
        NVPWR_LOG_WARN("RestoreStock: using same-session captured baseline=%lu mW\n", g_SavedUpper24);
        if (ctx.Root != g_MutatedRoot || ctx.Board != g_MutatedBoard) {
            NVPWR_LOG_ERROR("RestoreStock: NVIDIA object identity changed; refusing blind restore\n");
            return STATUS_INVALID_DEVICE_STATE;
        }

        status = RestoreSavedRoot(&ctx);
        if (!NT_SUCCESS(status)) return status;

        nv = CallBoardSet(&ctx, SELECTOR_MAX, SOURCE_FE, g_SavedUpper24);
        if (nv != 0) return STATUS_UNSUCCESSFUL;

        RtlZeroMemory(&s, sizeof(s));
        s.Version = NVPWR_STATUS_VERSION;
        FillStatusFromContext(&ctx, &s);
        if (s.State != NvpwrStateStockBaseline)
            return STATUS_DATA_ERROR;

        g_MutationAttempted = FALSE;
        g_MutatedRoot = NULL;
        g_MutatedBoard = NULL;
        g_SavedValid = FALSE;
        g_SavedEligibility = 0;
        g_SavedAmountActive = 0;
        g_SavedInput14 = 0;
        g_SavedAmount18 = 0;
        g_SavedUpper24 = 0;
        g_ActiveProfile = NvpwrProfileUnknown;
        NVPWR_LOG_INFO("RestoreStock: SUCCESS captured OEM baseline restored\n");
        return STATUS_SUCCESS;
    }

    /* Driver was loaded after an older proof helper had already changed the
       616.92 runtime policy. Recover only if the live state matches that very
       narrow verified family; arbitrary MIXED states are still rejected. */
    if (IsRecoverableExternalModifiedState(&s)) {
        NVPWR_LOG_WARN("RestoreStock: recognized legacy 5070 Ti proof state; using narrow known-stock recovery\n");
        return ForceKnownStockBaseline(&ctx);
    }

    NVPWR_LOG_ERROR("RestoreStock: live state is not a known safe restore family; refusing mutation\n");
    return STATUS_INVALID_DEVICE_STATE;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;

    switch (code) {
    case IOCTL_NVPWR_STATUS:
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(NVPWR_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = QueryStatus((NVPWR_STATUS*)Irp->AssociatedIrp.SystemBuffer);
        information = sizeof(NVPWR_STATUS);
        break;

    case IOCTL_NVPWR_SET_POWER:
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(NVPWR_SET_POWER)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            NVPWR_SET_POWER* request = (NVPWR_SET_POWER*)Irp->AssociatedIrp.SystemBuffer;
            if (request->Version != NVPWR_SET_VERSION) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            NVPWR_LOG_INFO("IOCTL_SET_POWER: profile=%lu target=%lu mW\n",
                request->Profile, request->TargetMilliwatts);
            KeWaitForSingleObject(&g_OperationMutex, Executive, KernelMode, FALSE, NULL);
            status = SetPowerTarget(request->Profile, request->TargetMilliwatts);
            KeReleaseMutex(&g_OperationMutex, FALSE);
            if (NT_SUCCESS(status))
                NVPWR_LOG_INFO("IOCTL_SET_POWER: completed NTSTATUS=0x%08X\n", status);
            else
                NVPWR_LOG_ERROR("IOCTL_SET_POWER: failed NTSTATUS=0x%08X\n", status);
        }
        break;

    case IOCTL_NVPWR_RESTORE:
        NVPWR_LOG_WARN("IOCTL_RESTORE: requested\n");
        KeWaitForSingleObject(&g_OperationMutex, Executive, KernelMode, FALSE, NULL);
        status = RestoreStock();
        KeReleaseMutex(&g_OperationMutex, FALSE);
        if (NT_SUCCESS(status))
            NVPWR_LOG_INFO("IOCTL_RESTORE: completed NTSTATUS=0x%08X\n", status);
        else
            NVPWR_LOG_ERROR("IOCTL_RESTORE: failed NTSTATUS=0x%08X\n", status);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    NVPWR_LOG_WARN("DriverUnload: unloading. No implicit NVIDIA restore is performed.\n");

    /* No implicit calls into NVIDIA on unload. Explicit restore is separate.
       If restore has not succeeded, reboot to reconstruct stock state. */

    IoDeleteSymbolicLink(&g_SymbolicLink);
    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    NVPWR_LOG_INFO("DriverEntry: Nvpwr 1.5.0 experimental review/debug build loading\n");
    KeInitializeMutex(&g_OperationMutex, 0);
    RtlInitUnicodeString(&deviceName, L"\\Device\\Nvpwr");
    RtlInitUnicodeString(&g_SymbolicLink, L"\\DosDevices\\Nvpwr");

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject);
    if (!NT_SUCCESS(status)) return status;

    status = IoCreateSymbolicLink(&g_SymbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = DispatchCreateClose;

    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload = DriverUnload;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    NVPWR_LOG_INFO("DriverEntry: device \\Device\\Nvpwr and \\DosDevices\\Nvpwr ready\n");
    return STATUS_SUCCESS;
}
