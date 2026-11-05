/*
 * ctxguard.c - generic SIMD register context guard (WINSIMD 1.2).
 *
 * Problem: on Windows NT 3.1/3.5x/4.0 and Windows 95/98 (first edition) the
 * scheduler saves only the x87/MMX state (FNSAVE/FRSTOR). Once WINSIMD has
 * enabled SSE (CR4.OSFXSR), the XMM registers and MXCSR leak between threads.
 * No Windows version before Windows 7 SP1 saves the AVX (YMM) or AVX-512
 * (opmask, ZMM) state, so those leak on every target.
 *
 * Mechanism: architectural, not tied to any kernel build.
 * Every one of these schedulers switches the FPU lazily: when a thread that
 * does not own the FPU registers runs, CR0.TS is set, and its first x87, MMX,
 * SSE, AVX or AVX-512 instruction raises #NM (vector 7). Windows' own #NM
 * handler then swaps the x87 state. WINSIMD hooks vector 7 in front of it and
 * swaps the register files the OS does not manage:
 *
 *   owner = the thread whose values are in this CPU's registers
 *   on #NM by thread T:  if owner != T: save registers -> owner's slot,
 *                        load T's slot (or the initial state) -> registers,
 *                        owner = T
 *   then continue into the OS handler (CR0.TS is left as it was).
 *
 * The state is swapped exactly when the OS swaps the x87 state, so the XMM/
 * YMM/ZMM ownership always follows the OS's own FPU ownership.
 *
 * Saved components are exactly those the OS does not preserve:
 *   XMM+MXCSR  if WINSIMD enabled SSE itself (disp_sse == DRIVER_ENABLED)
 *   YMM, opmask, ZMM  whenever WINSIMD enabled them (disp_avx/disp_avx512)
 * XSAVE/XRSTOR with exactly that component mask is used when OSXSAVE is on,
 * otherwise MOVUPS XMM0-7 + STMXCSR/LDMXCSR.
 *
 * Multiprocessor: a thread can leave a CPU with its values still in that
 * CPU's registers and continue on another CPU. When the #NM on the new CPU
 * finds the thread's values live on another CPU, it asks that CPU to store
 * them with an NMI sent through the local APIC (architectural, not OS
 * specific); the target's NMI hook saves its registers into the owner's slot
 * and releases ownership. Nothing is waited for while the table lock is
 * held, and a CPU that is itself waiting serves such requests while it
 * spins, so two CPUs cannot block each other. (Windows 9x is uniprocessor.)
 *
 * Thread identity is supplied by the platform hook (NT: KPRCB.CurrentThread;
 * 9x: VMM Get_Cur_Thread_Handle). Slots are released when the platform reports
 * thread exit; when no notification exists (NT 3.1/3.5x) the least recently
 * used slot that is not loaded in a CPU is reused.
 */
#include "ilf.h"
#include "lowlevel.h"
#include "ctxguard.h"

typedef struct {
    ILF_UPTR thread;   /* 0 = free */
    ILF_U32  stamp;    /* last use, for LRU reuse */
    ILF_U8  *area;     /* 64-byte aligned save area */
} CG_SLOT;

/* shared with ctxguard_x86.asm */
ILF_U32 cg_mask_lo, cg_mask_hi;   /* XSAVE component mask (EDX:EAX)   */
ILF_U32 cg_use_xsave;             /* 1 = XSAVE/XRSTOR, 0 = MOVUPS     */
ILF_U32 cg_armed;                 /* hook installed and active        */
ILF_UPTR cg_orig_nm;              /* original vector 7 handler        */

void __cdecl cg_save(void *area);     /* asm */
void __cdecl cg_restore(void *area);  /* asm */

static CG_SLOT  *slots;
static ILF_U32   nslots, area_size, clock;
static ILF_U8   *init_area;
#define CG_MAX_CPUS 32
static volatile ILF_UPTR owner[CG_MAX_CPUS];   /* thread whose values are in CPU i */
static volatile ILF_UPTR wanted[CG_MAX_CPUS];  /* flush request: thread to store */
static volatile ILF_U32  inguard[CG_MAX_CPUS]; /* CPU i is inside cg_on_nm       */
static volatile ILF_U32  nmi_sent[CG_MAX_CPUS], nmi_seen[CG_MAX_CPUS];
static ILF_U32 ncpus = 1;
ILF_U8  cg_apic_index[256];                    /* initial APIC ID -> CPU index  */
ILF_U32 cg_apic_id[CG_MAX_CPUS];
volatile ILF_U32 *cg_apic;                     /* mapped local APIC (MP only)  */
ILF_U32 __cdecl cg_cpu_index(void);            /* asm: CPUID APIC ID -> index  */
ILF_U32 __cdecl cg_clts(void);                 /* asm: returns old CR0, clears TS */
void    __cdecl cg_set_cr0(ILF_U32 v);
static volatile ILF_U32 lock;
CG_STATUS        g_cg;                /* published status */

static void lock_acquire(void)
{
    ILF_U32 v;
    do { v = 1; __asm__ volatile("xchgl %0,%1" : "+r"(v), "+m"(lock) :: "memory"); } while (v);
}
static void lock_release(void) { __asm__ volatile("" ::: "memory"); lock = 0; }

static void zero(ILF_U8 *p, ILF_U32 n) { while (n--) *p++ = 0; }

/* Size and layout. ci must already be policy-applied on this CPU. */
ILF_U32 cg_plan(const ILF_CPU_INFO *ci, ILF_U32 *bytes_per_slot)
{
    ILF_U64 m = 0;
    ILF_U32 r[4];
    g_cg.version = CG_VERSION;
    g_cg.size = sizeof(g_cg);
    g_cg.magic = CG_MAGIC;
    if (ci->disp_sse == ILF_DISP_DRIVER_ENABLED) m |= ILF_XCR0_SSE;
    if (ci->disp_avx == ILF_DISP_DRIVER_ENABLED)
        m |= ci->xcr0 & (ILF_XCR0_AVX | ILF_XCR0_OPMASK | ILF_XCR0_ZMM_HI | ILF_XCR0_ZMM_HI16);
    if (!m) { *bytes_per_slot = 0; return 0; }
    if ((ci->cr4_after & ILF_CR4_OSXSAVE) && (ci->features & ILF_FEAT_XSAVE)) {
        cg_use_xsave = 1;
        /* Component 2 (AVX) is only the upper YMM halves; the XMM halves stay
           with whoever manages component 1 (the OS on 2000/XP/98 SE/ME). */
        ilf_cpuid(0x0D, 0, r);
        area_size = r[1];               /* EBX: size for the enabled XCR0 */
        if (area_size < 576) area_size = 576;
    } else {
        if (m & ~ILF_XCR0_SSE) { *bytes_per_slot = 0; return 0; } /* no XSAVE: AVX impossible */
        cg_use_xsave = 0;
        area_size = 144;                /* XMM0-7 + MXCSR */
    }
    area_size = (area_size + 63) & ~63u;
    cg_mask_lo = (ILF_U32)m;
    cg_mask_hi = (ILF_U32)(m >> 32);
    g_cg.components_lo = cg_mask_lo;
    g_cg.components_hi = cg_mask_hi;
    *bytes_per_slot = area_size + sizeof(CG_SLOT);
    return 1;
}

/* memory: nonpaged, any alignment; count slots fit. */
int cg_setup(void *memory, ILF_U32 bytes)
{
    ILF_U8 *p = (ILF_U8 *)(((ILF_UPTR)memory + 63) & ~(ILF_UPTR)63);
    ILF_U32 usable = bytes - (ILF_U32)(p - (ILF_U8 *)memory), i;
    if (!area_size || usable < 3 * (area_size + sizeof(CG_SLOT))) return 0;
    init_area = p; p += area_size; usable -= area_size;
    nslots = usable / (area_size + sizeof(CG_SLOT));
    if (nslots > 4096) nslots = 4096;
    slots = (CG_SLOT *)p; p += ((nslots * sizeof(CG_SLOT)) + 63) & ~63u;
    while ((ILF_UPTR)(p + nslots * area_size) > (ILF_UPTR)memory + bytes) nslots--;
    /* XRSTOR faults (#GP) unless the reserved part of each area's XSAVE header
       is zero, and XSAVE never writes it: clear every area once. */
    zero(p, nslots * area_size);
    for (i = 0; i < nslots; i++) { slots[i].thread = 0; slots[i].stamp = 0; slots[i].area = p + i * area_size; }
    /* Initial state: all registers zero, MXCSR = 1F80h (exceptions masked).
       For XRSTOR: XSTATE_BV = 0 puts every component in its init state; the
       legacy MXCSR field is still loaded, so it must hold 1F80h. */
    zero(init_area, area_size);
    if (cg_use_xsave) *(ILF_U32 *)(init_area + 24) = 0x1F80;
    else              *(ILF_U32 *)(init_area + 128) = 0x1F80;
    g_cg.slots = nslots;
    return 1;
}

static CG_SLOT *find(ILF_UPTR t)
{
    ILF_U32 i;
    for (i = 0; i < nslots; i++) if (slots[i].thread == t) return &slots[i];
    return 0;
}
static int loaded_somewhere(ILF_UPTR t)
{
    ILF_U32 i;
    for (i = 0; i < ncpus; i++) if (owner[i] == t) return 1;
    return 0;
}
static CG_SLOT *take(ILF_UPTR t)
{
    ILF_U32 i, best = 0;
    CG_SLOT *s = 0;
    for (i = 0; i < nslots; i++) if (!slots[i].thread) { s = &slots[i]; break; }
    if (!s) {                                   /* reuse the least recently used */
        for (i = 0; i < nslots; i++) {
            ILF_U32 age = clock - slots[i].stamp;
            if (loaded_somewhere(slots[i].thread)) continue;
            if (!s || age > best) { best = age; s = &slots[i]; }
        }
        if (!s) return 0;
        g_cg.reused++;
    }
    s->thread = t;
    return s;
}

/* Store this CPU's registers for their owner and release ownership.
   Caller holds the lock; CR0.TS must be clear. */
static void store_owner(ILF_U32 me)
{
    CG_SLOT *s;
    if (!owner[me]) return;
    s = find(owner[me]);
    if (!s) s = take(owner[me]);
    if (s) { cg_save(s->area); s->stamp = clock; }
    else g_cg.lost++;
    owner[me] = 0;
}

/* Serve a flush request addressed to this CPU (TS already clear). */
static void serve_request(ILF_U32 me)
{
    ILF_UPTR t = wanted[me];
    if (!t) return;
    lock_acquire();
    if (owner[me] == t) { clock++; store_owner(me); g_cg.flushes++; }
    wanted[me] = 0;
    lock_release();
}

static void send_nmi(ILF_U32 cpu)
{
    ILF_U32 n;
    if (!cg_apic) return;
    for (n = 0; n < 1000000 && (cg_apic[0x300 / 4] & (1u << 12)); n++) {}  /* delivery idle */
    nmi_sent[cpu]++;
    cg_apic[0x310 / 4] = cg_apic_id[cpu] << 24;
    cg_apic[0x300 / 4] = (4u << 8) | (1u << 14);   /* NMI, assert, physical destination */
}

/* Called from the vector 7 hook: interrupts off, CR0.TS cleared. */
void __cdecl cg_on_nm(ILF_UPTR thread)
{
    CG_SLOT *s;
    ILF_U32 me = ncpus > 1 ? cg_cpu_index() : 0, j, spin, resend;
    if (!thread || me >= ncpus || owner[me] == thread) return;
    inguard[me] = 1;
    for (;;) {
        ILF_U32 remote = 0xFFFFFFFFu;
        lock_acquire();
        for (j = 0; j < ncpus; j++) if (j != me && owner[j] == thread) { remote = j; break; }
        if (remote == 0xFFFFFFFFu) break;      /* keep the lock */
        lock_release();
        /* The thread moved here from CPU 'remote' with its values still in
           that CPU's registers: ask it to store them, then retry. */
        wanted[remote] = thread;
        send_nmi(remote);
        g_cg.migrations++;
#ifdef ILF_E9_DEBUG
        ilf_e9out('M'); ilf_e9out((ILF_U8)('0' + me)); ilf_e9out((ILF_U8)('0' + remote));
#endif
        for (spin = 0, resend = 0; owner[remote] == thread; spin++) {
            if (wanted[me]) serve_request(me);  /* never block a CPU waiting on us */
            if (spin == 2000000) {
#ifdef ILF_E9_DEBUG
                ilf_e9out('R');
#endif
                if (++resend > 50) {             /* target unreachable: give up */
                    lock_acquire();
                    if (owner[remote] == thread) { owner[remote] = 0; g_cg.lost++; }
                    lock_release();
                    break;
                }
                spin = 0; send_nmi(remote);
            }
        }
    }
    clock++;
    g_cg.swaps++;
    store_owner(me);
    s = find(thread);
    if (s) { cg_restore(s->area); s->stamp = clock; }
    else   cg_restore(init_area);
    owner[me] = thread;
    if (wanted[me] && wanted[me] != thread) wanted[me] = 0;   /* already stored above */
#ifdef ILF_E9_DEBUG
    if ((g_cg.swaps & 255) == 0 || g_cg.migrations < 8) {     /* test builds: progress on port E9 */
        const char *h = "0123456789abcdef"; ILF_U32 v[4], k; int b;
        v[0] = g_cg.swaps; v[1] = g_cg.migrations; v[2] = g_cg.flushes; v[3] = g_cg.lost;
        ilf_e9out('{');
        for (k = 0; k < 4; k++) { for (b = 28; b >= 0; b -= 4) ilf_e9out((ILF_U8)h[(v[k] >> b) & 15]); ilf_e9out(k < 3 ? ',' : '}'); }
        ilf_e9out('\n');
    }
#endif
    lock_release();
    inguard[me] = 0;
}

/* Called from the vector 2 hook. Returns 1 if the NMI was a flush request. */
ILF_U32 __cdecl cg_on_nmi(void)
{
    ILF_U32 me, cr0;
    if (ncpus < 2) return 0;
    me = cg_cpu_index();
    if (me >= ncpus || nmi_seen[me] == nmi_sent[me]) return 0;   /* not ours */
    nmi_seen[me] = nmi_sent[me];
#ifdef ILF_E9_DEBUG
    ilf_e9out('N'); ilf_e9out((ILF_U8)('0' + me)); ilf_e9out(inguard[me] ? 'g' : '-'); ilf_e9out(wanted[me] ? (owner[me] == wanted[me] ? 'o' : 'x') : '0');
#endif
    if (inguard[me]) return 1;     /* cg_on_nm on this CPU handles it on exit */
    cr0 = cg_clts();
    serve_request(me);
    cg_set_cr0(cr0);
    return 1;
}

/* Thread exit (platform notification): forget its state. Runs at passive
   level with interrupts on; while the lock is held this CPU is marked
   in-guard so a flush NMI arriving meanwhile is deferred, not dead-locked. */
ILF_UPTR ilf_save_irq(void);
void     ilf_restore_irq(ILF_UPTR flags);
void __cdecl cg_thread_exit(ILF_UPTR thread)
{
    CG_SLOT *s;
    ILF_U32 i, me, cr0;
    ILF_UPTR fl;
    if (!cg_armed || !thread) return;
    fl = ilf_save_irq();
    me = ncpus > 1 ? cg_cpu_index() : 0;
    if (me >= ncpus) me = 0;
    inguard[me] = 1;
    lock_acquire();
    s = find(thread);
    if (s) { s->thread = 0; g_cg.released++; }
    for (i = 0; i < ncpus; i++) if (owner[i] == thread) owner[i] = 0;
    lock_release();
    inguard[me] = 0;
    if (wanted[me]) { cr0 = cg_clts(); serve_request(me); cg_set_cr0(cr0); }
    ilf_restore_irq(fl);
}

void cg_set_cpus(ILF_U32 n) { ncpus = n ? (n > CG_MAX_CPUS ? CG_MAX_CPUS : n) : 1; }

void cg_mark_armed(ILF_U32 cpus)
{
    g_cg.cpus = cpus;
    __asm__ volatile("" ::: "memory");
    cg_armed = 1;
    g_cg.flags = CG_FLAG_ACTIVE |
                 ((cg_mask_lo & (ILF_U32)ILF_XCR0_SSE) ? CG_FLAG_XMM : 0) |
                 ((cg_mask_lo & (ILF_U32)ILF_XCR0_AVX) ? CG_FLAG_YMM : 0) |
                 ((cg_mask_lo & (ILF_U32)(ILF_XCR0_OPMASK | ILF_XCR0_ZMM_HI | ILF_XCR0_ZMM_HI16)) ? CG_FLAG_ZMM : 0);
}
