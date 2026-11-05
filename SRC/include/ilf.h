/*
 * ilf.h  -  intlfxsr: shared definitions
 *
 * A boot/system-start kernel driver that detects the CPU's SIMD feature set
 * (MMX / SSE..SSE4 / AVX / AVX2 / AVX-512) and the OS's current CR0/CR4/XCR0
 * posture, then applies an "automatic hybrid" policy:
 *
 *   - If the OS already preserves a register file across context switches
 *     (e.g. XMM on Windows 2000/XP, which set CR4.OSFXSR), leave it alone.
 *   - If the OS does NOT (NT 3.1/3.5x/4.0, Win9x for XMM; and *every* target
 *     for YMM/ZMM, since AVX postdates them all), enable the feature bits
 *     ourselves so the instructions don't #UD, and treat the extended state
 *     as "driver-guarded": consumers must save/restore it around use inside a
 *     non-preemptible block (see ilf_simd_begin/ilf_simd_end).
 *
 * Targets one i386 binary (NT 3.1 -> XP x86) and one amd64 binary (XP x64).
 *
 * This header is freestanding: no CRT, no WDK required.
 */

#ifndef ILF_H
#define ILF_H

/* ------------------------------------------------------------------ */
/* Fixed-width + NT-ish base types (freestanding, no <windows.h>)      */
/* ------------------------------------------------------------------ */

typedef unsigned char       ILF_U8;
typedef unsigned short      ILF_U16;
typedef unsigned int        ILF_U32;      /* 32-bit on both LLP64 targets */
typedef int                 ILF_S32;

#if defined(_M_X64) || defined(__x86_64__)
typedef unsigned long long  ILF_UPTR;
typedef unsigned long long  ILF_U64;
#  define ILF_X64 1
#else
typedef unsigned long       ILF_UPTR;     /* 32-bit */
typedef unsigned long long  ILF_U64;
#  define ILF_X64 0
#endif

typedef long                ILF_NTSTATUS;

#ifndef NULL
#  define NULL ((void*)0)
#endif

#define ILF_STATUS_SUCCESS            ((ILF_NTSTATUS)0x00000000L)
#define ILF_STATUS_UNSUCCESSFUL       ((ILF_NTSTATUS)0xC0000001L)
#define ILF_STATUS_NOT_SUPPORTED      ((ILF_NTSTATUS)0xC00000BBL)

/* ------------------------------------------------------------------ */
/* CR0 / CR4 / XCR0 bits we care about                                 */
/* ------------------------------------------------------------------ */

#define ILF_CR0_MP   (1u << 1)   /* Monitor coprocessor                */
#define ILF_CR0_EM   (1u << 2)   /* Emulation (must be 0 to use x87/SSE)*/
#define ILF_CR0_TS   (1u << 3)   /* Task switched                      */
#define ILF_CR0_NE   (1u << 5)   /* Numeric error                      */

#define ILF_CR4_OSFXSR     (1u << 9)   /* FXSAVE/FXRSTOR + SSE enable   */
#define ILF_CR4_OSXMMEXCPT (1u << 10)  /* Unmasked SIMD FP exceptions   */
#define ILF_CR4_OSXSAVE    (1u << 18)  /* XSAVE + XGETBV/XSETBV enable  */

/* XCR0 (extended control register 0) feature bits */
#define ILF_XCR0_X87  (1ull << 0)
#define ILF_XCR0_SSE  (1ull << 1)   /* XMM */
#define ILF_XCR0_AVX  (1ull << 2)   /* YMM high halves */
#define ILF_XCR0_OPMASK   (1ull << 5)  /* AVX-512 k0-k7   */
#define ILF_XCR0_ZMM_HI   (1ull << 6)  /* AVX-512 ZMM0-15 high */
#define ILF_XCR0_ZMM_HI16 (1ull << 7)  /* AVX-512 ZMM16-31     */

/* ------------------------------------------------------------------ */
/* Feature flags (our normalized view of CPUID)                        */
/* ------------------------------------------------------------------ */

#define ILF_FEAT_FPU      (1u << 0)
#define ILF_FEAT_MMX      (1u << 1)
#define ILF_FEAT_FXSR     (1u << 2)   /* FXSAVE/FXRSTOR supported       */
#define ILF_FEAT_SSE      (1u << 3)
#define ILF_FEAT_SSE2     (1u << 4)
#define ILF_FEAT_SSE3     (1u << 5)
#define ILF_FEAT_SSSE3    (1u << 6)
#define ILF_FEAT_SSE41    (1u << 7)
#define ILF_FEAT_SSE42    (1u << 8)
#define ILF_FEAT_XSAVE    (1u << 9)   /* XSAVE/XRSTOR + XGETBV supported */
#define ILF_FEAT_AVX      (1u << 10)
#define ILF_FEAT_FMA      (1u << 11)
#define ILF_FEAT_AVX2     (1u << 12)
#define ILF_FEAT_AVX512F  (1u << 13)
#define ILF_FEAT_MMXEXT   (1u << 14)  /* AMD extended MMX               */
#define ILF_FEAT_3DNOW    (1u << 15)

/* ------------------------------------------------------------------ */
/* Per-feature policy decision (how we made each usable)               */
/* ------------------------------------------------------------------ */

typedef enum _ILF_DISPOSITION {
    ILF_DISP_ABSENT = 0,     /* CPU does not support it                */
    ILF_DISP_OS_MANAGED,     /* OS already enabled & preserves state   */
    ILF_DISP_DRIVER_ENABLED, /* we flipped CR4/XCR0; OS does NOT       */
                             /* preserve -> consumers must guard       */
    ILF_DISP_BLOCKED         /* present but unusable (e.g. CR0.EM set) */
} ILF_DISPOSITION;

/* ------------------------------------------------------------------ */
/* Snapshot the driver fills during DriverEntry                        */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct _ILF_CPU_INFO {
    char    vendor[16];      /* 12 chars + NUL, padded                 */
    ILF_U32 max_std_leaf;
    ILF_U32 max_ext_leaf;
    ILF_U32 family;          /* display family                        */
    ILF_U32 model;           /* display model                         */
    ILF_U32 stepping;
    ILF_U32 features;        /* ILF_FEAT_* bitmask                     */

    /* OS posture read from control regs on the boot CPU */
    ILF_U32 cr0;
    ILF_U32 cr4;
    ILF_U64 xcr0;            /* valid only if OSXSAVE was set          */
    ILF_U32 xsave_area_size; /* CPUID.(0D,0).ECX if XSAVE, else 0      */

    /* What we decided / did, per register file */
    ILF_DISPOSITION disp_mmx;
    ILF_DISPOSITION disp_sse;   /* XMM path                            */
    ILF_DISPOSITION disp_avx;   /* YMM path                            */
    ILF_DISPOSITION disp_avx512;/* ZMM path                            */

    ILF_U32 cr4_before;
    ILF_U32 cr4_after;
    ILF_U64 xcr0_before;
    ILF_U64 xcr0_after;
    ILF_U32 nproc_applied;   /* # of logical CPUs we programmed        */
    char    brand[52];       /* CPU display name (brand string, or a   */
                             /* vendor+family/model fallback on old CPUs)*/
    ILF_U32 has_brand;       /* 1 if a real CPUID brand string was read */
    ILF_U32 safe_mode;       /* AvxPolicy: 1 = AVX intentionally NOT enabled   */
                             /* (gated so concurrent AVX can't silently corrupt;*/
                             /* apps fall back to SSE). 0 = AVX enabled (default)*/
} ILF_CPU_INFO;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* IOCTL (optional query device, NT4+ build only)                      */
/* ------------------------------------------------------------------ */
/* METHOD_BUFFERED=0, FILE_DEVICE_UNKNOWN=0x22, custom function 0x800  */
#define ILF_IOCTL_QUERY  \
    ( (0x22u << 16) | (0u << 14) | (0x800u << 2) | 0u )

/* Win9x: WINSIMD.VXD answers DeviceIoControl(\\.\WINSIMD, this code) with
   the whole ILF_CPU_INFO snapshot (VxD control code W32_DEVICEIOCONTROL). */
#define ILF_IOCTL_GET_STATUS  1u

#endif /* ILF_H */
