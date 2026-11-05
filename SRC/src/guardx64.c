/* guardx64.c - WINSIMD 1.2 amd64 (Windows XP x64 / Server 2003 x64) YMM guard.
 *
 * Windows x64 saves XMM0-15 + MXCSR itself (fxsave/fxrstor in SwapContext) but
 * NOT the AVX YMM upper halves. Concurrent AVX threads therefore corrupt each
 * other's YMM upper state across a context switch. This guard preserves the
 * YMM0-7 upper halves per thread (upper-only), leaving the OS's XMM untouched.
 * YMM8-15 are intentionally not touched; see src/NOTES-xp-x64.md.
 *
 * Locating the kernel and the hook site is entirely dynamic: the running
 * ntoskrnl base is found from our DbgPrint import (ilf_iat_probe + module_base,
 * shared with smp.c) and the SwapContext site from a unique byte signature.
 * No fixed kernel address, RVA, build stamp or struct offset into KPRCB is
 * used; per-CPU identity is the KPRCB pointer itself, registered on first sight.
 *
 * PatchGuard: x64 Windows ships Kernel Patch Protection, which bugchecks (0x109)
 * on a modified SwapContext. PatchGuard is NOT armed when the kernel debugger is
 * enabled (boot.ini /debug; no debugger need be attached). This guard installs
 * ONLY when the exported KdDebuggerEnabled flag is set, and otherwise reports
 * CG_PATCHGUARD and changes nothing. It never fights PatchGuard.
 */
#include "ilf.h"
#include "ctxguard.h"

#if ILF_X64

/* shared helpers (smp.c) */
ILF_U8  *module_base(ILF_UPTR addr);
void    *get_export(ILF_U8 *base, const char *name);
ILF_UPTR ilf_iat_probe(void);                 /* VA of ntoskrnl!DbgPrint */
extern ILF_U32 __cdecl DbgPrint(const char *fmt, ...);

/* asm (guardx64.asm) */
#ifdef ILF_E9_DEBUG
void ilf_e9out(ILF_U8 c);
static void e9s(const char *s){ while(*s) ilf_e9out((ILF_U8)*s++); }
static void e9h(ILF_U64 v){ int i; for(i=60;i>=0;i-=4) ilf_e9out("0123456789abcdef"[(v>>i)&15]); }
#else
#define e9s(s) ((void)0)
#define e9h(v) ((void)0)
#endif

int  cgx64_has_avx(void);
void cgx64_save_upper(void *buf);
void cgx64_restore_upper(void *buf);
ILF_U8 cgx64_write14(ILF_U8 *site, ILF_U64 hook);
void cgx64_serialize(void);
ILF_U64 cgx64_hook_addr(void);
extern ILF_U64 cgx64_resume;                  /* trampoline address (asm reads) */
ILF_U64 cgx64_resume;

/* The 16 unique bytes at the KPRCB.ContextSwitches increment in SwapContext:
 *   inc dword[rbx+0xdf0] ; mov rax,[rbx-0x170] ; test rax,rax
 * RSI = incoming thread, RBX = KPRCB at this point. The scan refuses to install
 * unless this pattern occurs exactly once in the kernel's image. */
static const ILF_U8 PAT[16] = {0xff,0x83,0xf0,0x0d,0x00,0x00,
                               0x48,0x8b,0x83,0x90,0xfe,0xff,0xff,
                               0x48,0x85,0xc0};
#define PATLEN 16
#define STATE_OFF 0xb8u      /* KTHREAD.State; value 4 == Terminated */

#define NSLOTS 2048
typedef struct { ILF_U64 thread; ILF_U8 upper[8*16]; } CgSlot; /* 8 ymm-high */
static CgSlot slots[NSLOTS];
static ILF_U32 slot_rr;

#define NCPU 64
static ILF_U64 cpu_kprcb[NCPU];     /* KPRCB -> dense index, registered lazily */
static ILF_U64 cpu_thread[NCPU];    /* current owner thread per CPU index      */
static ILF_U32 ncpu_seen;
static volatile long tlock;

/* resolved kernel entry points */
typedef ILF_U8 (*mmvalid_fn)(void *);
typedef void * (*exalloc_fn)(ILF_U32, ILF_U64, ILF_U32);
typedef ILF_UPTR (*ipigen_worker)(ILF_UPTR);
typedef ILF_UPTR (*ipigen_fn)(ipigen_worker, ILF_UPTR);
static mmvalid_fn k_valid;

static void lk(void){ while(__sync_lock_test_and_set(&tlock,1)){} }
static void uk(void){ __sync_lock_release(&tlock); }

static int valid(void *p){ return k_valid ? k_valid(p) : 1; }

static ILF_U32 cpu_index(ILF_U64 kprcb)
{
    ILF_U32 i;
    for (i = 0; i < ncpu_seen; i++) if (cpu_kprcb[i] == kprcb) return i;
    if (ncpu_seen < NCPU) { cpu_kprcb[ncpu_seen] = kprcb; return ncpu_seen++; }
    return 0;   /* table full: fold onto 0 (benign) */
}

static CgSlot *lookup(ILF_U64 thread, int create)
{
    ILF_U32 i; CgSlot *empty = 0;
    for (i = 0; i < NSLOTS; i++) {
        if (slots[i].thread == thread) return &slots[i];
        if (!slots[i].thread && !empty) empty = &slots[i];
    }
    if (!create) return 0;
    if (!empty) { empty = &slots[slot_rr]; slot_rr = (slot_rr + 1) % NSLOTS;
                  g_cg.lost++; }
    empty->thread = thread;
    for (i = 0; i < 8*16; i++) empty->upper[i] = 0;
    return empty;
}

/* Called from the asm hook: incoming thread and this CPU's KPRCB. Runs inside
   the scheduler (high IRQL, dispatcher state in flux), so it makes NO kernel
   calls and dereferences NO thread pointer: thread values are used only as slot
   keys (compared, never read). The outgoing thread is always live here (threads
   are descheduled, not freed, through SwapContext); real exits reclaim their
   slot through cg_thread_notify below. */
void cgx64_switch(ILF_U64 new_thread, ILF_U64 kprcb)
{
    ILF_U32 ci; ILF_U64 old_thread; CgSlot *old, *next;
    lk();
    ci = cpu_index(kprcb);
    old_thread = cpu_thread[ci];
    if (old_thread == new_thread) { uk(); return; }
    g_cg.swaps++;
    if (old_thread) { old = lookup(old_thread, 1); cgx64_save_upper(old->upper); }
    next = lookup(new_thread, 1);
    cgx64_restore_upper(next->upper);
    cpu_thread[ci] = new_thread;
    uk();
#ifdef ILF_E9_DEBUG
    { static volatile long once; if(__sync_val_compare_and_swap(&once,0,1)==0) e9s("[Hx]"); }
#endif
}

/* Thread-exit reclamation: PsSetCreateThreadNotifyRoutine callback. On exit
   (create == 0) it runs in the exiting thread's own context, so the KTHREAD to
   drop is KeGetCurrentThread(). Freeing the slot keeps the table from filling
   with dead threads; nothing in the hot path ever reads a thread's memory. */
typedef ILF_U64 (*getthread_fn)(void);
static getthread_fn k_curthread;
static void cg_thread_notify(void *pid, void *tid, ILF_U8 create)
{
    ILF_U64 t; CgSlot *s;
    (void)pid; (void)tid;
    if (create || !k_curthread) return;
    t = k_curthread();
    lk();
    s = lookup(t, 0);
    if (s) { s->thread = 0; g_cg.released++; }
    uk();
}

static ILF_U8 *scan_unique(ILF_U8 *base, ILF_U64 size, ILF_U32 *n)
{
    ILF_U64 i; ILF_U8 *hit = 0; ILF_U32 j; *n = 0;
    for (i = 0; i + PATLEN <= size; i++) {
        if ((i & 0xfff) == 0 && !valid(base + i)) { i += 0xfff; continue; }
        for (j = 0; j < PATLEN; j++) if (base[i+j] != PAT[j]) break;
        if (j == PATLEN) { if (++*n == 1) hit = base + i; }
    }
    return (*n == 1) ? hit : 0;
}

/* broadcast worker: the owner CPU patches, every CPU serializes. */
static volatile long patch_claim;
static ILF_U8 *patch_site; static ILF_U64 patch_hook;
static ILF_UPTR patch_worker(ILF_UPTR ctx)
{
    (void)ctx;
    if (__sync_val_compare_and_swap(&patch_claim, 0, 1) == 0)
        cgx64_write14(patch_site, patch_hook);   /* clears WP, CLI, serializes */
    else
        cgx64_serialize();
    return 0;
}

static ILF_U8 trampoline_static[64];

/* Install the guard. ci must be policy-applied. enabled == ContextGuard reg. */
void ilf_guard_install_x64(const ILF_CPU_INFO *ci, ILF_U32 enabled)
{
    ILF_U8 *ntos, *site, *tramp; ILF_U32 n, i; ILF_U64 size; ILF_U64 e;
    exalloc_fn alloc; ipigen_fn ipi; ILF_U8 *kdbg; ILF_U8 *kdnp;

    g_cg.size = sizeof(g_cg); g_cg.magic = CG_MAGIC; g_cg.version = CG_VERSION;
    g_cg.flags = 0; g_cg.cpus = 0; g_cg.slots = 0;
    g_cg.components_lo = (ILF_U32)ILF_XCR0_AVX; g_cg.components_hi = 0;

    e9s("\r\nx64guard: ");
    if (!enabled)                        { g_cg.reason = CG_DISABLED; e9s("disabled\r\n"); return; }
    /* Only needed when WINSIMD itself enabled AVX; if the OS gated it (AvxPolicy)
       or AVX is unavailable there is no unsaved YMM to protect. */
    if (ci->disp_avx != ILF_DISP_DRIVER_ENABLED || !cgx64_has_avx()) {
        g_cg.reason = CG_NOT_NEEDED; return;
    }

    ntos = module_base(ilf_iat_probe());
    if (!ntos) { g_cg.reason = CG_NO_SITE; e9s("no-base\r\n"); return; }
    e9s("ntos="); e9h((ILF_U64)(ILF_UPTR)ntos);

    /* PatchGuard (Kernel Patch Protection) on XP x64 / Server 2003 SP1 hashes
       ntoskrnl's code, so ANY patch to SwapContext is detected and bugchecks
       (0x109, CRITICAL_STRUCTURE_CORRUPTION). PatchGuard v2 here does NOT honour
       the KdDebuggerNotPresent flag alone; it stands down only when a kernel
       debugger is genuinely attached (KdDebuggerEnabled == 1 and
       KdDebuggerNotPresent == 0, i.e. /debug AND a debugger connected).
       To be safe we install the SwapContext hook ONLY in that configuration;
       otherwise we touch nothing and report CG_PATCHGUARD. This guarantees
       WINSIMD never bugchecks XP x64 in a normal boot. (On XP x64 the OS saves
       XMM/MXCSR itself, so SSE apps - including the Flash Player ports - run
       correctly with no guard; only concurrent AVX needs YMM preservation.) */
    kdbg = (ILF_U8 *)get_export(ntos, "KdDebuggerEnabled");
    kdnp = (ILF_U8 *)get_export(ntos, "KdDebuggerNotPresent");
    e9s(" kd="); e9h(kdbg ? (ILF_U64)*kdbg : 0xff);
    e9s(" kdnp="); e9h(kdnp ? (ILF_U64)*kdnp : 0xff);
    if (!kdbg || !*kdbg || !kdnp || *kdnp) {
        g_cg.reason = CG_PATCHGUARD; e9s(" patchguard-active\r\n"); return;
    }

    k_valid = (mmvalid_fn)get_export(ntos, "MmIsAddressValid");

    e    = *(ILF_U32 *)(ntos + 0x3c);
    size = *(ILF_U32 *)(ntos + e + 0x50);          /* OptionalHeader.SizeOfImage */
    if (size < 0x1000 || size > 0x4000000) { g_cg.reason = CG_NO_SITE; return; }
    site = scan_unique(ntos, size, &n);
    e9s(" n="); e9h(n); e9s(" site="); e9h((ILF_U64)(ILF_UPTR)site);
    if (!site) { g_cg.reason = CG_NO_SITE; e9s(" no-site\r\n"); return; }

    /* executable trampoline: the 16 displaced bytes + an abs jmp to site+16 */
    alloc = (exalloc_fn)get_export(ntos, "ExAllocatePoolWithTag");
    tramp = alloc ? (ILF_U8 *)alloc(0 /*NonPagedPool*/, 64, 'GMYW') : 0;
    if (!tramp) tramp = trampoline_static;
    for (i = 0; i < PATLEN; i++) tramp[i] = site[i];
    tramp[PATLEN] = 0xff; tramp[PATLEN+1] = 0x25; *(ILF_U32 *)(tramp+PATLEN+2) = 0;
    *(ILF_U64 *)(tramp+PATLEN+6) = (ILF_U64)(ILF_UPTR)(site + PATLEN);
    cgx64_resume = (ILF_U64)(ILF_UPTR)tramp;

    /* patch across all CPUs atomically (others halted at IPI_LEVEL). */
    patch_site = site; patch_hook = cgx64_hook_addr(); patch_claim = 0;
    e9s(" tramp="); e9h((ILF_U64)(ILF_UPTR)tramp); e9s(" patching");
    ipi = (ipigen_fn)get_export(ntos, "KeIpiGenericCall");
    if (ipi) ipi(patch_worker, 0);
    else     cgx64_write14(site, patch_hook);     /* UP fallback */
    e9s(" armed\r\n");

    /* slot reclamation on thread exit (NT 4.0+; 2003 x64 exports both) */
    k_curthread = (getthread_fn)get_export(ntos, "KeGetCurrentThread");
    {
        typedef long (*psnotify_fn)(void *);
        psnotify_fn psn = (psnotify_fn)get_export(ntos, "PsSetCreateThreadNotifyRoutine");
        if (psn && k_curthread) psn((void *)cg_thread_notify);
    }

    g_cg.flags  = CG_FLAG_ACTIVE | CG_FLAG_YMM;
    g_cg.reason = 0;
    g_cg.cpus   = ncpu_seen ? ncpu_seen : 1;
    g_cg.slots  = NSLOTS;

    DbgPrint("winsimd x64: YMM guard armed (upper-only YMM0-7), site rva=%x\n",
             (ILF_U32)(ILF_UPTR)(site - ntos));
}

#endif /* ILF_X64 */
