/*
 * smp.c - program every logical CPU, not just the boot CPU.
 *
 * CR4 (OSFXSR/OSXMMEXCPT/OSXSAVE) and XCR0 are per-logical-processor. If only
 * the boot CPU is enabled, an AVX/SSE instruction that runs on another CPU
 * faults (#UD). To cover all CPUs we migrate the current thread to each one in
 * turn (KeSetSystemAffinityThread) and run the policy there.
 *
 * Those SMP routines appear only in NT 4.0+; NT 3.1/3.5x do not export them
 * (and are effectively uniprocessor anyway). To keep ONE i386 binary that still
 * loads on NT 3.1, we do NOT import them statically -- we resolve them at run
 * time from ntoskrnl's export table, locating ntoskrnl via the IAT slot of the
 * one import we do have (DbgPrint). If they are absent we program the boot CPU
 * only. Win9x is uniprocessor, so the VxD never uses this path.
 */
#include "ilf.h"
#include "lowlevel.h"

ILF_NTSTATUS ilf_apply_policy(ILF_CPU_INFO *ci);
ILF_UPTR     ilf_iat_probe(void);      /* asm: VA of ntoskrnl!DbgPrint */
ILF_U32      ilf_read_cr4(void);

/* diagnostics for the E9 report: resolution status + OS-reported CPU count */
ILF_U32 g_smp_resolved;   /* 1 = SMP symbols found, 0 = fell back to boot CPU */
ILF_U32 g_smp_oscount;    /* KeNumberProcessors value (0 if unresolved)       */
ILF_U32 g_ntos_found;     /* 1 = located ntoskrnl image base                  */
ILF_U32 g_cpu_cr4[8];     /* per-CPU CR4 read back ON that CPU (proof each set)*/
ILF_U32 g_cpu_n;          /* how many entries of g_cpu_cr4 are valid          */

#if ILF_X64
#  define KSTDCALL
#else
#  define KSTDCALL __stdcall
#endif

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Scan down (toward lower addresses) from an address known to lie inside a
   loaded PE image to find its 'MZ' base. We stop at the first valid MZ+PE, so
   we never read below the image. */
ILF_U8 *module_base(ILF_UPTR addr)
{
    ILF_UPTR p = addr & ~(ILF_UPTR)0xFFF;
    int i;
    for (i = 0; i < 0x8000; i++, p -= 0x1000) {   /* <=128 MB back */
        ILF_U8 *b = (ILF_U8 *)p;
        ILF_U32 e;
        if (b[0] != 'M' || b[1] != 'Z') continue;
        e = *(ILF_U32 *)(b + 0x3c);
        if (e >= 0x1000) continue;
        if (b[e] == 'P' && b[e+1] == 'E' && b[e+2] == 0 && b[e+3] == 0)
            return b;
    }
    return NULL;
}

/* Minimal GetProcAddress over a module's export directory (by name). */
void *get_export(ILF_U8 *base, const char *name)
{
    ILF_U32 e   = *(ILF_U32 *)(base + 0x3c);
    ILF_U8 *nt  = base + e;
    ILF_U16 mag = *(ILF_U16 *)(nt + 0x18);
    ILF_U32 rva = *(ILF_U32 *)(nt + 0x18 + (mag == 0x20b ? 0x70 : 0x60)); /* dir[0] */
    ILF_U8 *ed;
    ILF_U32 nnames, i, *pnames, *pfuncs;
    ILF_U16 *pords;
    if (!rva) return NULL;
    ed     = base + rva;
    nnames = *(ILF_U32 *)(ed + 0x18);
    pfuncs = (ILF_U32 *)(base + *(ILF_U32 *)(ed + 0x1c));
    pnames = (ILF_U32 *)(base + *(ILF_U32 *)(ed + 0x20));
    pords  = (ILF_U16 *)(base + *(ILF_U32 *)(ed + 0x24));
    for (i = 0; i < nnames; i++)
        if (streq((const char *)(base + pnames[i]), name))
            return (void *)(base + pfuncs[pords[i]]);
    return NULL;
}

typedef void (KSTDCALL *revert_fn)(void);
typedef revert_fn revert_fn_t;
typedef void (KSTDCALL *set_aff_fn)(ILF_UPTR);
typedef long (KSTDCALL *ntsetinfo_fn)(void *, int, void *, ILF_U32);

/* Pin the current (system) thread to CPU i. NT 4.0+: KeSetSystemAffinityThread.
   NT 3.1/3.5x export neither it nor KeSetAffinityThread, but they do export
   NtSetInformationThread: ThreadAffinityMask (class 4) on the current thread
   (handle -2), called with previous mode KernelMode from DriverEntry. */
static set_aff_fn g_seta; static revert_fn_t g_rev; static ntsetinfo_fn g_nsi; static ILF_UPTR g_all;
typedef long (KSTDCALL *delay_fn)(int, int, ILF_U64 *);
static delay_fn g_delay;
/* Processor number of the running CPU: KPCR.Prcb->SetMember ([fs:20h]+14h),
   the same on every x86 NT from 3.1 to XP. (amd64 builds: not used.) */
static int cpu_now(void)
{
#if ILF_X64
    return -1;
#else
    ILF_U32 m;
    __asm__ volatile("movl %%fs:0x20,%0\n\tmovl 0x14(%0),%0" : "=r"(m));
    { int i = 0; if (!m) return -1; while (!(m & 1)) { m >>= 1; i++; } return i; }
#endif
}
int ilf_cpu_pin(int i)
{
    ILF_UPTR m = (ILF_UPTR)1 << i;
    int k;
    if (g_seta) g_seta(m);
    else if (g_nsi) { if (g_nsi((void *)(ILF_UPTR)-2, 4, &m, sizeof(m)) < 0) return 0; }
    else return i == 0;
    /* A new affinity takes effect at the next dispatch; wait until the thread
       is really running there (1 ms sleeps, at most ~2 s). */
    for (k = 0; k < 2000; k++) {
        int now = cpu_now();
        if (now < 0 || now == i) return 1;
        if (g_delay) { ILF_U64 t = (ILF_U64)(-10000LL); g_delay(0, 0, &t); }
    }
    return 0;
}
void ilf_cpu_unpin(void)
{
    if (g_seta && g_rev) { g_rev(); return; }
    if (g_nsi && g_all) g_nsi((void *)(ILF_UPTR)-2, 4, &g_all, sizeof(g_all));
}

/* Program the policy on every logical CPU. Returns the count programmed. */
int ilf_apply_all_cpus(ILF_CPU_INFO *ci)
{
    ILF_U8 *ntos = module_base(ilf_iat_probe());

    g_ntos_found = ntos ? 1 : 0;
    g_smp_resolved = 0; g_smp_oscount = 0;
    if (ntos) {
        signed char *pn   = (signed char *)get_export(ntos, "KeNumberProcessors");
        set_aff_fn   seta = (set_aff_fn)   get_export(ntos, "KeSetSystemAffinityThread");
        revert_fn    rev  = (revert_fn)    get_export(ntos, "KeRevertToUserAffinityThread");
        /* Only target processors that are actually ONLINE. On some multiprocessor
           HALs (seen on NT 3.51 MPS), setting affinity to an application processor
           that is not yet online stalls, which made a system-start load look like
           a failed service. KeActiveProcessors is a KAFFINITY bitmask of online
           CPUs; if it is absent (older/odd kernels) or looks bogus (CPU0 not set),
           we ignore it and fall back to the plain KeNumberProcessors range. */
        ILF_UPTR *pactive = (ILF_UPTR *)get_export(ntos, "KeActiveProcessors");
        ILF_UPTR  active  = pactive ? *pactive : 0;
        if (!(active & 1)) active = 0;   /* 0 => no mask */
        if (pn) g_smp_oscount = (ILF_U32)(unsigned char)*pn;
        g_seta = (seta && rev) ? seta : 0; g_rev = rev;
        g_nsi = (ntsetinfo_fn)get_export(ntos, "NtSetInformationThread");
        g_delay = (delay_fn)get_export(ntos, "KeDelayExecutionThread");
#ifdef ILF_E9_DEBUG
        ilf_e9out(pn ? (ILF_U8)('0' + *pn) : '?'); ilf_e9out(g_seta ? 'K' : '-'); ilf_e9out(g_nsi ? 'N' : '-');
#endif
        if (pn && (g_seta || g_nsi)) {
            int n = *pn, i, done = 0;
            g_smp_resolved = 1;
            if (n < 1)  n = 1;
            if (n > 32) n = 32;
            g_all = active ? active : (((ILF_UPTR)1 << n) - 1);
            for (i = 0; i < n; i++) {
                if (active && !((active >> i) & 1)) continue;  /* skip offline CPU */
                if (!ilf_cpu_pin(i)) {
#ifdef ILF_E9_DEBUG
                    ilf_e9out('!'); ilf_e9out((ILF_U8)('0' + i));
#endif
                    continue;
                }
#ifdef ILF_E9_DEBUG
                ilf_e9out('c'); ilf_e9out((ILF_U8)('0' + cpu_now()));
#endif
                ilf_apply_policy(ci);        /* ...and enable it there */
                done++;
            }
            /* verification pass: re-visit each CPU and read its CR4 back, so
               the log proves every CPU (not just the boot one) was programmed */
            for (i = 0; i < n && i < 8; i++) {
                if (active && !((active >> i) & 1)) continue;
                if (!ilf_cpu_pin(i)) continue;
                g_cpu_cr4[i] = ilf_read_cr4();
            }
            g_cpu_n = (n < 8) ? (ILF_U32)n : 8;
            ilf_cpu_unpin();
            ci->nproc_applied = done;
            return done;
        }
    }
    ilf_apply_policy(ci);                    /* NT 3.1/3.5x or resolve failed */
    ci->nproc_applied = 1;
    return 1;
}

/* Export of ntoskrnl by name (NULL if absent), for optional services. */
void *ilf_ntos_export(const char *name)
{
    ILF_U8 *ntos = module_base(ilf_iat_probe());
    return ntos ? get_export(ntos, name) : NULL;
}
