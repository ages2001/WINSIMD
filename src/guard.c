/*
 * guard.c - the safe-usage contract for SIMD in kernel code.
 *
 * Regardless of whether the OS preserves a register file across context
 * switches, kernel code that *uses* SIMD must not leave another thread's live
 * SIMD state clobbered. The invariant we enforce: on the current CPU, disable
 * preemption (CLI), snapshot the exact state we will touch (FXSAVE / XSAVE),
 * do the work, restore it (FXRSTOR / XRSTOR), re-enable. This is correct on
 * every target because each CPU's register file is independent and no context
 * switch can occur with interrupts masked.
 *
 * ilf_simd_begin/ilf_simd_end are the reusable entry points other drivers call.
 */
#include "ilf.h"
#include "lowlevel.h"

typedef struct _ILF_GUARD {
    ILF_UPTR flags;
    void    *area;
    ILF_U64  mask;
    int      use_xsave;
} ILF_GUARD;

/* area: FXSAVE needs 512B/16-aligned; XSAVE needs xsave_area_size/64-aligned. */
void ilf_simd_begin(ILF_GUARD *g, void *area, ILF_U64 xcr0_mask, int use_xsave)
{
    g->area = area;
    g->mask = xcr0_mask;
    g->use_xsave = use_xsave;
    g->flags = ilf_save_irq();             /* pushf; cli */
    if (use_xsave) ilf_xsave(area, xcr0_mask);
    else           ilf_fxsave(area);
}

void ilf_simd_end(ILF_GUARD *g)
{
    if (g->use_xsave) ilf_xrstor(g->area, g->mask);
    else              ilf_fxrstor(g->area);
    ilf_restore_irq(g->flags);             /* restore flags (popf) */
}

/* asm self-test kernels (return a known constant iff the ISA really runs) */
extern ILF_U32 __cdecl ilf_test_sse1(void);   /* -> 0x0000000F */
extern ILF_U32 __cdecl ilf_test_sse2(void);   /* -> 0xFFFFFFFF */
extern ILF_U32 __cdecl ilf_test_avx(void);    /* -> 0x000000FF */

#define ILF_ST_SSE1 0x01
#define ILF_ST_SSE2 0x02
#define ILF_ST_AVX  0x04
#define ILF_ST_FAIL 0x80000000u

int ilf_selftest(ILF_CPU_INFO *ci)
{
    /* Align at run time: a loader may place the image at an address that is
       not 64-byte aligned (Win9x VMM loads VxD objects into its heap), and
       FXSAVE/XSAVE #GP on a misaligned area. */
    static ILF_U8 raw[4096 + 64];
    ILF_U8 *area = (ILF_U8 *)(((ILF_UPTR)raw + 63) & ~(ILF_UPTR)63);
    ILF_GUARD g;
    int mask = 0;

    if (ci->disp_sse == ILF_DISP_DRIVER_ENABLED ||
        ci->disp_sse == ILF_DISP_OS_MANAGED) {
        ilf_simd_begin(&g, area, 0, 0);            /* FXSAVE guard */
        if (ci->features & ILF_FEAT_SSE2) {
            if (ilf_test_sse2() == 0xFFFFFFFFu) mask |= ILF_ST_SSE2;
            else                                mask |= ILF_ST_FAIL;
        } else if (ci->features & ILF_FEAT_SSE) {
            if (ilf_test_sse1() == 0x0000000Fu) mask |= ILF_ST_SSE1;
            else                                mask |= ILF_ST_FAIL;
        }
        ilf_simd_end(&g);
    }

    if (ci->disp_avx == ILF_DISP_DRIVER_ENABLED) {
        ilf_simd_begin(&g, area, ci->xcr0, 1);     /* XSAVE guard */
        if (ilf_test_avx() == 0x000000FFu) mask |= ILF_ST_AVX;
        else                               mask |= ILF_ST_FAIL;
        ilf_simd_end(&g);
    }

    return mask;
}
