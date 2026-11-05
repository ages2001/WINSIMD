/* lowlevel.h - prototypes for the nasm ring-0 primitives (__cdecl on x86) */
#ifndef ILF_LOWLEVEL_H
#define ILF_LOWLEVEL_H
#include "ilf.h"

ILF_U32  ilf_has_cpuid(void);                       /* 386-safe */
void     ilf_cpuid(ILF_U32 leaf, ILF_U32 sub, ILF_U32 out[4]);

ILF_U32  ilf_read_cr0(void);
ILF_U32  ilf_read_cr4(void);
void     ilf_write_cr4(ILF_U32 v);

ILF_U64  ilf_xgetbv(ILF_U32 idx);                   /* needs OSXSAVE */
void     ilf_xsetbv(ILF_U32 idx, ILF_U64 val);      /* ring0 + OSXSAVE */

void     ilf_fxsave(void *area);                    /* 512B / 16-aligned */
void     ilf_fxrstor(void *area);
void     ilf_xsave(void *area, ILF_U64 mask);       /* 64-aligned */
void     ilf_xrstor(void *area, ILF_U64 mask);

ILF_UPTR ilf_save_irq(void);                        /* pushf;cli -> flags */
void     ilf_restore_irq(ILF_UPTR flags);
void     ilf_emms(void);
void     ilf_e9out(ILF_U8 c);                       /* QEMU debugcon 0xE9 */

#endif
