/* ctxguard.h - generic SIMD context guard (WINSIMD 1.2), see src/ctxguard.c */
#ifndef ILF_CTXGUARD_H
#define ILF_CTXGUARD_H
#include "ilf.h"

#define CG_VERSION 0x0102u
#define CG_MAGIC   0x44475357u          /* 'WSGD' */
#define CG_FLAG_ACTIVE 0x01u            /* hook installed, state is preserved */
#define CG_FLAG_XMM    0x02u            /* XMM0-7 + MXCSR preserved by WINSIMD  */
#define CG_FLAG_YMM    0x04u            /* YMM upper halves preserved           */
#define CG_FLAG_ZMM    0x08u            /* AVX-512 opmask + ZMM preserved       */

/* Reasons the guard is not armed (CG_STATUS.reason) */
#define CG_NOT_NEEDED    1u   /* the OS preserves every enabled register file  */
#define CG_MULTIPROC     2u   /* multiprocessor, but no local APIC / IDT access */
#define CG_NO_MEMORY     3u
#define CG_BAD_IDT       4u   /* vector 7 is not a plain interrupt/trap gate   */
#define CG_BAD_THREAD    5u   /* current-thread lookup failed validation       */
#define CG_DISABLED      6u   /* ContextGuard=0 in the registry                */
#define CG_NO_SITE       7u   /* amd64: SwapContext signature not found/unique */
#define CG_PATCHGUARD    8u   /* amd64: PatchGuard armed (kernel debugger off) */

#pragma pack(push, 1)
typedef struct _CG_STATUS {
    ILF_U32 size, magic, version, flags;
    ILF_U32 components_lo, components_hi;   /* XCR0-style mask that is saved */
    ILF_U32 cpus, slots, reason;
    ILF_U32 swaps, reused, released, lost;  /* counters */
    ILF_U32 migrations, flushes;            /* MP: cross-CPU state transfers */
} CG_STATUS;
#pragma pack(pop)

/* Win9x DeviceIoControl code on \\.\WINSIMD returning CG_STATUS */
#define ILF_IOCTL_GET_GUARD 2u

extern CG_STATUS g_cg;
extern ILF_U32 cg_armed;
ILF_U32 cg_plan(const ILF_CPU_INFO *ci, ILF_U32 *bytes_per_slot);
int     cg_setup(void *memory, ILF_U32 bytes);
void    cg_mark_armed(ILF_U32 cpus);
void    cg_set_cpus(ILF_U32 n);
extern ILF_U8  cg_apic_index[256];
extern ILF_U32 cg_apic_id[32];
extern volatile ILF_U32 *cg_apic;
void __cdecl cg_thread_exit(ILF_UPTR thread);
#endif
