/*
 * regwrite.c - publish detection/policy results to the driver's own service
 * key so a user-mode tool (WINSINFO.EXE) can display them. Uses only Zw and
 * Rtl exports present since NT 3.1, so one binary still loads everywhere.
 *
 * Writes into HKLM\SYSTEM\CurrentControlSet\Services\WINSIMD:
 *   Status   REG_BINARY  = the whole ILF_CPU_INFO snapshot
 *   Features REG_DWORD, Cr4After REG_DWORD, ...  (friendly duplicates)
 */
#include "ilf.h"

typedef unsigned short USHORT;
typedef unsigned long  ULONG;
typedef long           NTSTATUS;
typedef void          *PVOID;
typedef void          *HANDLE;
typedef HANDLE        *PHANDLE;
typedef unsigned short WCH;          /* wchar_t is 2 bytes on windows targets */

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; WCH *Buffer; } UNICODE_STRING;
typedef UNICODE_STRING *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG  Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG  Attributes;
    PVOID  SecurityDescriptor;
    PVOID  SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

#define OBJ_CASE_INSENSITIVE 0x00000040u
#define KEY_ALL_ACCESS       0x000F003Fu
#define REG_DWORD            4u
#define REG_BINARY           3u

#if ILF_X64
#  define NTAPI
#  define NTIMP(n) __asm__(#n)
#else
#  define NTAPI __stdcall
#  define NTIMP(n) __asm__("_" #n)
#endif

NTSTATUS NTAPI ZwCreateKey(PHANDLE, ULONG, OBJECT_ATTRIBUTES*, ULONG,
                           PUNICODE_STRING, ULONG, ULONG*)        NTIMP(ZwCreateKey);
NTSTATUS NTAPI ZwSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG,
                             PVOID, ULONG)                        NTIMP(ZwSetValueKey);
NTSTATUS NTAPI ZwClose(HANDLE)                                    NTIMP(ZwClose);
NTSTATUS NTAPI ZwOpenKey(PHANDLE, ULONG, OBJECT_ATTRIBUTES*)      NTIMP(ZwOpenKey);
NTSTATUS NTAPI ZwQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, ULONG*)
                                                                  NTIMP(ZwQueryValueKey);
void     NTAPI RtlInitUnicodeString(PUNICODE_STRING, const WCH*)  NTIMP(RtlInitUnicodeString);

/* Read HKLM\...\Services\winsimd\AvxPolicy (REG_DWORD) at boot. 1 = safe mode
   (don't enable AVX). Absent/0 = enable AVX (default). Uses ZwOpenKey +
   ZwQueryValueKey (KeyValuePartialInformation), both present since NT 3.1. */
ILF_U32 ilf_read_policy(void)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING vn, kp; HANDLE h = NULL;
    unsigned char buf[32]; ULONG rl = 0; ILF_U32 val = 0;

    RtlInitUnicodeString(&kp,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\winsimd");
    oa.Length = sizeof(oa); oa.RootDirectory = NULL; oa.ObjectName = &kp;
    oa.Attributes = OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    if (ZwOpenKey(&h, KEY_ALL_ACCESS, &oa) < 0 || !h) return 0;
    RtlInitUnicodeString(&vn, L"AvxPolicy");
    /* KeyValuePartialInformation = 2: [TitleIndex(4)][Type(4)][DataLength(4)][Data] */
    if (ZwQueryValueKey(h, &vn, 2, buf, sizeof(buf), &rl) >= 0 && rl >= 16)
        val = *(ILF_U32 *)(buf + 12);
    ZwClose(h);
    return val ? 1u : 0u;
}

static void setdw(HANDLE h, const WCH *name, ULONG v)
{
    UNICODE_STRING n; RtlInitUnicodeString(&n, name);
    ZwSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
}

void ilf_write_status(PUNICODE_STRING RegistryPath, ILF_CPU_INFO *ci)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING vn, kp;
    HANDLE h = NULL;
    ULONG disp = 0;

    (void)RegistryPath;  /* use a FIXED path so WINSINFO always finds it,
                            regardless of the service name we loaded under */
    RtlInitUnicodeString(&kp,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\winsimd");

    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &kp;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;

    if (ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp) < 0 || !h)
        return;

    RtlInitUnicodeString(&vn, L"Status");
    ZwSetValueKey(h, &vn, 0, REG_BINARY, ci, sizeof(*ci));

    setdw(h, L"Features",  ci->features);
    setdw(h, L"Cr4Before", ci->cr4_before);
    setdw(h, L"Cr4After",  ci->cr4_after);
    setdw(h, L"Xcr0Lo",    (ULONG)ci->xcr0);
    setdw(h, L"Xcr0Hi",    (ULONG)(ci->xcr0 >> 32));
    setdw(h, L"DispMmx",   (ULONG)ci->disp_mmx);
    setdw(h, L"DispSse",   (ULONG)ci->disp_sse);
    setdw(h, L"DispAvx",   (ULONG)ci->disp_avx);
    setdw(h, L"DispAvx512",(ULONG)ci->disp_avx512);
    setdw(h, L"XsaveSize", ci->xsave_area_size);
    setdw(h, L"SafeMode",  ci->safe_mode);

    ZwClose(h);
}
