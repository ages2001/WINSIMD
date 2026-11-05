/*
 * guard_nt.c - arms the generic context guard (ctxguard.c) in the NT driver.
 * Every service used is resolved from ntoskrnl's export table at run time,
 * so the one i386 binary still loads on NT 3.1 through XP.
 */
#include "ilf.h"
#include "lowlevel.h"
#include "ctxguard.h"

void *ilf_ntos_export(const char *name);           /* smp.c */
ILF_U32 __cdecl cg_idt_gate(ILF_U32 vector, ILF_U32 *handler);   /* asm */
void    __cdecl cg_idt_set(ILF_U32 vector, ILF_U32 handler);
ILF_U32 __cdecl cg_current_thread(void);
void    cg_nm_hook(void);
void    cg_nmi_hook(void);
ILF_U32 __cdecl cg_apic_id_now(void);
ILF_U32 __cdecl cg_rdmsr(ILF_U32 msr, ILF_U32 *hi);
void    __cdecl cg_idt_read8(ILF_U32 vector, ILF_U32 out[2]);
void    __cdecl cg_idt_write8(ILF_U32 vector, ILF_U32 lo, ILF_U32 hi);
int  ilf_cpu_pin(int i);                            /* smp.c */
void ilf_cpu_unpin(void);
extern ILF_UPTR cg_orig_nm;
#pragma pack(push, 1)
struct { ILF_U32 offset; ILF_U16 selector; } cg_orig_nmi_far;
#pragma pack(pop)
extern ILF_U32 g_smp_oscount;                       /* smp.c: KeNumberProcessors */

typedef void *(__stdcall *ExAllocatePool_t)(int, ILF_U32);
typedef unsigned short ILF_U16_;
typedef void *(__stdcall *KeGetCurrentThread_t)(void);
typedef void  (__stdcall *ThreadNotify_t)(void *, void *, unsigned char);
typedef long  (__stdcall *SetThreadNotify_t)(ThreadNotify_t);
typedef void *(__stdcall *MmMapIoSpace_t)(ILF_U32 lo, ILF_U32 hi, ILF_U32 bytes, ILF_U32 cache);

/* Check vector 7 on this CPU and remember its handler; all CPUs must agree. */
static int check_cpu(int i, ILF_U32 *handler)
{
    ILF_U32 h, type = cg_idt_gate(7, &h), id = cg_apic_id_now();
    if ((type & 0x9F) != 0x8E && (type & 0x9F) != 0x8F) return 0;
    if (*handler && *handler != h) return 0;
    *handler = h;
    cg_apic_index[id] = (ILF_U8)i;
    cg_apic_id[i] = id;
    return 1;
}
/* Point this CPU's vector 2 at the flush hook (32-bit interrupt gate, kernel
   code selector 08h, constant on x86 NT) and keep the old gate as a far
   jump target: a task gate (NT) or an interrupt gate. */
static void hook_nmi(void)
{
    ILF_U32 g[2], h = (ILF_U32)(ILF_UPTR)cg_nmi_hook;
    cg_idt_read8(2, g);
    if (!cg_orig_nmi_far.selector) {
        ILF_U32 type = (g[1] >> 8) & 0x1F;
        cg_orig_nmi_far.selector = (ILF_U16)(g[0] >> 16);
        cg_orig_nmi_far.offset = (type == 5) ? 0 : ((g[0] & 0xFFFF) | (g[1] & 0xFFFF0000u));
    }
    cg_idt_write8(2, (0x0008u << 16) | (h & 0xFFFF), (h & 0xFFFF0000u) | 0x8E00u);
}

static void __stdcall on_thread(void *process, void *thread_id, unsigned char create)
{
    (void)process; (void)thread_id;
    /* For a terminating thread the routine runs in that thread's context. */
    if (!create) cg_thread_exit(cg_current_thread());
}

/* enabled: registry ContextGuard (1 = default). Returns CG_STATUS.reason (0 = armed). */
ILF_U32 ilf_guard_install(ILF_CPU_INFO *ci, ILF_U32 enabled)
{
    ILF_U32 per, want, bytes, cpus, handler, type;
    void *mem;
    ExAllocatePool_t alloc = (ExAllocatePool_t)ilf_ntos_export("ExAllocatePool");
    KeGetCurrentThread_t cur = (KeGetCurrentThread_t)ilf_ntos_export("KeGetCurrentThread");
    SetThreadNotify_t notify = (SetThreadNotify_t)ilf_ntos_export("PsSetCreateThreadNotifyRoutine");
    signed char *pn = (signed char *)ilf_ntos_export("KeNumberProcessors");

    if (!cg_plan(ci, &per)) return g_cg.reason = CG_NOT_NEEDED;
    if (!enabled) return g_cg.reason = CG_DISABLED;
    cpus = pn ? (ILF_U32)(unsigned char)*pn : 1;
    if (cpus < 1) cpus = 1;
    if (cpus > 32) return g_cg.reason = CG_MULTIPROC;
    g_cg.cpus = cpus;
    if (!cur || (ILF_UPTR)cur() != (ILF_UPTR)cg_current_thread()) return g_cg.reason = CG_BAD_THREAD;
    want = (per <= 200) ? 1024 : 384;
    bytes = want * per + 2 * per + 128;
    if (bytes > 1048576u) bytes = 1048576u;
    mem = alloc ? alloc(0 /* NonPagedPool */, bytes) : 0;
    if (!mem || !cg_setup(mem, bytes)) return g_cg.reason = CG_NO_MEMORY;
    handler = 0;
    if (cpus == 1) {
        if (!check_cpu(0, &handler)) return g_cg.reason = CG_BAD_IDT;
    } else {
        /* Multiprocessor: local APIC for flush NMIs, every CPU's IDT. */
        MmMapIoSpace_t map = (MmMapIoSpace_t)ilf_ntos_export("MmMapIoSpace");
        ILF_U32 hi, lo = cg_rdmsr(0x1B, &hi), i;
        if (!map || !(lo & (1u << 11))) return g_cg.reason = CG_MULTIPROC;   /* APIC off */
        cg_apic = (volatile ILF_U32 *)map(lo & 0xFFFFF000u, hi & 0xF, 4096, 0 /* non-cached */);
        if (!cg_apic) return g_cg.reason = CG_MULTIPROC;
        for (i = 0; i < cpus; i++) {
            if (!ilf_cpu_pin((int)i) || !check_cpu((int)i, &handler)) { ilf_cpu_unpin(); return g_cg.reason = CG_BAD_IDT; }
        }
        ilf_cpu_unpin();
    }
    (void)type;
    cg_orig_nm = handler;
    cg_set_cpus(cpus);
    if (notify) notify(on_thread);
    if (cpus == 1) cg_idt_set(7, (ILF_U32)(ILF_UPTR)cg_nm_hook);
    else {
        ILF_U32 i;
        for (i = 0; i < cpus; i++) {
            ilf_cpu_pin((int)i);
            hook_nmi();
            cg_idt_set(7, (ILF_U32)(ILF_UPTR)cg_nm_hook);
        }
        ilf_cpu_unpin();
    }
    cg_mark_armed(cpus);
    g_cg.reason = 0;
    return 0;
}
