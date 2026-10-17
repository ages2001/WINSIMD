/*
 * core.c - CPU feature detection + automatic-hybrid policy.
 * Freestanding (no CRT, no WDK). i386-baseline safe: the C here never uses
 * SIMD or CPUID directly; all of that goes through the nasm primitives, and
 * CPUID is gated behind ilf_has_cpuid() so a real 386 takes the no-op path.
 */
#include "ilf.h"
#include "lowlevel.h"

static void ilf_memzero(void *d, ILF_U32 n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = 0;
}

static int ilf_scpy(char *d, int p, const char *s)
{
    while (*s) d[p++] = *s++;
    d[p] = 0;
    return p;
}
static int ilf_sudec(char *d, int p, ILF_U32 v)
{
    char t[12]; int i = 0;
    if (!v) { d[p++] = '0'; d[p] = 0; return p; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) d[p++] = t[--i];
    d[p] = 0;
    return p;
}

/* Build a display name when the CPU has no CPUID brand string (old CPUs):
   "<vendor> family <f> model <m> stepping <s>". */
static void ilf_brand_fallback(ILF_CPU_INFO *ci)
{
    int p = 0;
    if (ci->vendor[0]) p = ilf_scpy(ci->brand, p, ci->vendor);
    else               p = ilf_scpy(ci->brand, p, "x86 CPU");
    p = ilf_scpy(ci->brand, p, " family ");
    p = ilf_sudec(ci->brand, p, ci->family);
    p = ilf_scpy(ci->brand, p, " model ");
    p = ilf_sudec(ci->brand, p, ci->model);
    p = ilf_scpy(ci->brand, p, " stepping ");
    p = ilf_sudec(ci->brand, p, ci->stepping);
    ci->has_brand = 0;
}

void ilf_detect(ILF_CPU_INFO *ci)
{
    ILF_U32 r[4];
    ILF_U32 feat = 0;

    ilf_memzero(ci, sizeof(*ci));
    ci->cr0 = ilf_read_cr0();
    ci->cr4 = ilf_read_cr4();

    /* No CPUID -> 386 / early 486. No SIMD exists; leave everything absent. */
    if (!ilf_has_cpuid()) {
        ilf_scpy(ci->brand, 0, "386-class CPU (no CPUID)");
        return;
    }

    ilf_cpuid(0, 0, r);
    ci->max_std_leaf = r[0];
    *(ILF_U32 *)&ci->vendor[0] = r[1];   /* EBX */
    *(ILF_U32 *)&ci->vendor[4] = r[3];   /* EDX */
    *(ILF_U32 *)&ci->vendor[8] = r[2];   /* ECX */
    ci->vendor[12] = 0;

    if (ci->max_std_leaf >= 1) {
        ILF_U32 a, c, d, bf, bm, ef, em;
        ilf_cpuid(1, 0, r);
        a = r[0]; c = r[2]; d = r[3];

        bf = (a >> 8) & 0xF;  bm = (a >> 4) & 0xF;
        ef = (a >> 20) & 0xFF; em = (a >> 16) & 0xF;
        ci->family   = bf + ((bf == 0xF) ? ef : 0);
        ci->model    = bm + ((bf == 0xF || bf == 0x6) ? (em << 4) : 0);
        ci->stepping = a & 0xF;

        if (d & (1u << 0))  feat |= ILF_FEAT_FPU;
        if (d & (1u << 23)) feat |= ILF_FEAT_MMX;
        if (d & (1u << 24)) feat |= ILF_FEAT_FXSR;
        if (d & (1u << 25)) feat |= ILF_FEAT_SSE;
        if (d & (1u << 26)) feat |= ILF_FEAT_SSE2;
        if (c & (1u << 0))  feat |= ILF_FEAT_SSE3;
        if (c & (1u << 9))  feat |= ILF_FEAT_SSSE3;
        if (c & (1u << 19)) feat |= ILF_FEAT_SSE41;
        if (c & (1u << 20)) feat |= ILF_FEAT_SSE42;
        if (c & (1u << 12)) feat |= ILF_FEAT_FMA;
        if (c & (1u << 26)) feat |= ILF_FEAT_XSAVE;  /* CPU supports XSAVE */
        if (c & (1u << 28)) feat |= ILF_FEAT_AVX;    /* CPU supports AVX  */
    }

    if (ci->max_std_leaf >= 7) {
        ilf_cpuid(7, 0, r);
        if (r[1] & (1u << 5))  feat |= ILF_FEAT_AVX2;
        if (r[1] & (1u << 16)) feat |= ILF_FEAT_AVX512F;
    }

    if (feat & ILF_FEAT_XSAVE) {
        ilf_cpuid(0x0D, 0, r);
        ci->xsave_area_size = r[2];      /* ECX: max size over all states   */
    }

    ilf_cpuid(0x80000000u, 0, r);
    ci->max_ext_leaf = r[0];
    if (ci->max_ext_leaf >= 0x80000001u) {
        ilf_cpuid(0x80000001u, 0, r);
        if (r[3] & (1u << 22)) feat |= ILF_FEAT_MMXEXT;
        if (r[3] & (1u << 31)) feat |= ILF_FEAT_3DNOW;
    }

    /* CPU display name: real brand string on new CPUs (0x80000002..4),
       otherwise a vendor+family/model fallback on old ones. */
    if (ci->max_ext_leaf >= 0x80000004u) {
        ILF_U32 *b = (ILF_U32 *)ci->brand;
        int s, k;
        ilf_cpuid(0x80000002u, 0, r); b[0]=r[0]; b[1]=r[1]; b[2]=r[2]; b[3]=r[3];
        ilf_cpuid(0x80000003u, 0, r); b[4]=r[0]; b[5]=r[1]; b[6]=r[2]; b[7]=r[3];
        ilf_cpuid(0x80000004u, 0, r); b[8]=r[0]; b[9]=r[1]; b[10]=r[2]; b[11]=r[3];
        ci->brand[48] = 0;
        ci->has_brand = 1;
        for (s = 0; ci->brand[s] == ' '; s++) ;      /* trim leading spaces */
        if (s) { for (k = 0; ci->brand[s]; k++, s++) ci->brand[k] = ci->brand[s];
                 ci->brand[k] = 0; }
    } else {
        ci->family   = ci->family ? ci->family : 0;  /* ensure set */
        ilf_brand_fallback(ci);
    }

    ci->features = feat;
}

ILF_NTSTATUS ilf_apply_policy(ILF_CPU_INFO *ci)
{
    ILF_U32 feat = ci->features;
    ILF_U32 cr4  = ci->cr4;

    ci->cr4_before  = ci->cr4;
    ci->xcr0_before = 0;

    /* If x87/SSE are emulated (CR0.EM), we must not enable or use them. */
    if (ci->cr0 & ILF_CR0_EM) {
        ci->disp_mmx = ci->disp_sse = ILF_DISP_BLOCKED;
        ci->disp_avx = ci->disp_avx512 = ILF_DISP_BLOCKED;
        ci->cr4_after = cr4;
        return ILF_STATUS_SUCCESS;
    }

    /* MMX rides the x87 register file, which every NT/9x scheduler already
       saves (FNSAVE/FXSAVE). Safe with EMMS discipline; no CR4 change. */
    ci->disp_mmx = (feat & ILF_FEAT_MMX) ? ILF_DISP_OS_MANAGED : ILF_DISP_ABSENT;

    /* ---- SSE / XMM ---- */
    if ((feat & ILF_FEAT_SSE) && (feat & ILF_FEAT_FXSR)) {
        if (cr4 & ILF_CR4_OSFXSR) {
            ci->disp_sse = ILF_DISP_OS_MANAGED;         /* 2000/XP/98 */
        } else {
            cr4 |= ILF_CR4_OSFXSR | ILF_CR4_OSXMMEXCPT; /* NT3.1/3.5x/4, 95 */
            ci->disp_sse = ILF_DISP_DRIVER_ENABLED;
        }
    } else {
        ci->disp_sse = ILF_DISP_ABSENT;
    }

    /* ---- AVX / YMM (+ AVX-512 / ZMM) ---- */
    /* Safe mode (AvxPolicy=1): the CPU has AVX but no NT/9x scheduler preserves
       YMM/ZMM across context switches, so concurrent AVX threads would corrupt
       each other. Rather than enable it, we leave CR4.OSXSAVE/XCR0 untouched:
       apps then see AVX as unusable (CPUID OSXSAVE path) and fall back to SSE,
       which IS preserved. SSE above is still enabled. */
    if (ci->safe_mode && (feat & ILF_FEAT_AVX)) {
        ci->disp_avx    = ILF_DISP_BLOCKED;
        ci->disp_avx512 = (feat & ILF_FEAT_AVX512F) ? ILF_DISP_BLOCKED : ILF_DISP_ABSENT;
        if (cr4 != ci->cr4_before) { ilf_write_cr4(cr4); ci->cr4 = cr4; }
        ci->cr4_after = cr4;
        return ILF_STATUS_SUCCESS;
    }
    if ((feat & ILF_FEAT_AVX) && (feat & ILF_FEAT_XSAVE)) {
        ILF_U64 x, want;
        ILF_U32 r[4];

        if (!(cr4 & ILF_CR4_OSXSAVE))
            cr4 |= ILF_CR4_OSXSAVE;
        if (!(cr4 & ILF_CR4_OSFXSR))           /* XSAVE needs the SSE state too */
            cr4 |= ILF_CR4_OSFXSR | ILF_CR4_OSXMMEXCPT;

        ilf_write_cr4(cr4);                    /* commit so XGETBV/XSETBV work */
        ci->cr4 = cr4;

        x = ilf_xgetbv(0);
        ci->xcr0_before = x;
        want = x | ILF_XCR0_X87 | ILF_XCR0_SSE | ILF_XCR0_AVX;
        if (feat & ILF_FEAT_AVX512F)
            want |= ILF_XCR0_OPMASK | ILF_XCR0_ZMM_HI | ILF_XCR0_ZMM_HI16;
        if (want != x)
            ilf_xsetbv(0, want);

        ci->xcr0 = ilf_xgetbv(0);
        ci->xcr0_after = ci->xcr0;

        /* No NT (3.1..XP) nor 9x scheduler saves YMM/ZMM -> always guarded. */
        ci->disp_avx    = ILF_DISP_DRIVER_ENABLED;
        ci->disp_avx512 = (feat & ILF_FEAT_AVX512F) ? ILF_DISP_DRIVER_ENABLED
                                                     : ILF_DISP_ABSENT;

        ilf_cpuid(0x0D, 0, r);
        ci->xsave_area_size = r[1];            /* EBX: size for the live XCR0 */

        /* reflect SSE disposition if we had to enable OSFXSR for AVX's sake */
        if (ci->disp_sse == ILF_DISP_ABSENT && (feat & ILF_FEAT_SSE))
            ci->disp_sse = ILF_DISP_DRIVER_ENABLED;

        ci->cr4_after = cr4;
        return ILF_STATUS_SUCCESS;
    } else {
        ci->disp_avx = ci->disp_avx512 = ILF_DISP_ABSENT;
    }

    if (cr4 != ci->cr4_before) {
        ilf_write_cr4(cr4);
        ci->cr4 = cr4;
    }
    ci->cr4_after = cr4;
    return ILF_STATUS_SUCCESS;
}
