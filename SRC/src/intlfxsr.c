/*
 * intlfxsr.c - DriverEntry. Loads at boot/system start, programs the boot CPU,
 * and logs what it found/did via DbgPrint (visible on a kernel debugger or the
 * QEMU debug port). Import surface is deliberately tiny: only DbgPrint, which
 * every NT from 3.1 through XP x64 exports, so one binary loads everywhere.
 */
#include "ilf.h"
#include "lowlevel.h"

/* The only ntoskrnl import. cdecl varargs; present on all NT. */
extern ILF_U32 __cdecl DbgPrint(const char *fmt, ...);

/* Opaque NT objects; we only pass the pointers through. */
struct _DRIVER_OBJECT;
struct _UNICODE_STRING;

void         ilf_detect(ILF_CPU_INFO *ci);
ILF_NTSTATUS ilf_apply_policy(ILF_CPU_INFO *ci);
int          ilf_apply_all_cpus(ILF_CPU_INFO *ci);  /* smp.c */
int          ilf_selftest(ILF_CPU_INFO *ci);   /* guard.c */
void         ilf_write_status(void *RegistryPath, ILF_CPU_INFO *ci); /* regwrite.c */
ILF_U32      ilf_read_policy(void);                                  /* regwrite.c */
ILF_U32      ilf_read_guard_enabled(void);                           /* regwrite.c */
void         ilf_write_runtime(void);                                /* regwrite.c */
#if !ILF_X64
ILF_U32      ilf_guard_install(ILF_CPU_INFO *ci, ILF_U32 enabled);   /* guard_nt.c */
#else
void         ilf_guard_install_x64(const struct _ILF_CPU_INFO *ci, ILF_U32 enabled); /* guardx64.c */
#endif
#include "ctxguard.h"
#if ILF_X64
CG_STATUS g_cg;   /* x86 builds define it in ctxguard.c */
#endif

ILF_CPU_INFO g_ilf;   /* resident snapshot */
extern ILF_U32 g_smp_resolved, g_smp_oscount, g_ntos_found;  /* smp.c */
extern ILF_U32 g_cpu_cr4[8], g_cpu_n;

#ifdef ILF_E9_DEBUG
/* Headless verification path: emit a summary to QEMU's debug console (port
   0xE9). Needs no debugger and no OS services. Harmless no-op on real HW/other
   ports; only compiled into test builds (E9=1). */
static void e9s(const char *s) { while (*s) ilf_e9out((ILF_U8)*s++); }
static void e9hex(ILF_U32 v)
{
    const char *h = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) ilf_e9out((ILF_U8)h[(v >> i) & 0xF]);
}
static void e9_report(ILF_CPU_INFO *ci, int st)
{
    e9s("\r\n[E9] winsimd load\r\n vendor="); e9s(ci->vendor);
    e9s(" feat=");  e9hex(ci->features);
    e9s(" cr4=");   e9hex(ci->cr4_before); e9s("->"); e9hex(ci->cr4_after);
    e9s("\r\n xcr0="); e9hex((ILF_U32)(ci->xcr0 >> 32)); e9hex((ILF_U32)ci->xcr0);
    e9s(" xsz=");   e9hex(ci->xsave_area_size);
    e9s("\r\n disp mmx="); e9hex((ILF_U32)ci->disp_mmx);
    e9s(" sse=");   e9hex((ILF_U32)ci->disp_sse);
    e9s(" avx=");   e9hex((ILF_U32)ci->disp_avx);
    e9s(" avx512=");e9hex((ILF_U32)ci->disp_avx512);
    e9s("\r\n safe="); e9hex(ci->safe_mode);
    e9s(" nproc="); e9hex((ILF_U32)ci->nproc_applied);
    e9s(" ntos="); e9hex(g_ntos_found);
    e9s(" smp="); e9hex(g_smp_resolved);
    e9s(" oscpu="); e9hex(g_smp_oscount);
    { ILF_U32 i; for (i = 0; i < g_cpu_n; i++) {
        e9s("\r\n  cpu"); ilf_e9out((ILF_U8)('0'+i)); e9s(" cr4="); e9hex(g_cpu_cr4[i]); } }
    e9s(" selftest="); e9hex((ILF_U32)st);
    e9s((st & 0x80000000u) ? " FAIL\r\n" : " OK\r\n");
}
#endif

static const char *disp_str(ILF_DISPOSITION d)
{
    switch (d) {
    case ILF_DISP_ABSENT:         return "absent";
    case ILF_DISP_OS_MANAGED:     return "os";
    case ILF_DISP_DRIVER_ENABLED: return "driver";
    default:                      return "blocked";
    }
}

#if ILF_X64
ILF_NTSTATUS DriverEntry(struct _DRIVER_OBJECT *drv, struct _UNICODE_STRING *reg)
#else
ILF_NTSTATUS __stdcall DriverEntry(struct _DRIVER_OBJECT *drv, struct _UNICODE_STRING *reg)
#endif
{
    int st;
    (void)drv;

    ilf_detect(&g_ilf);
#ifdef ILF_E9_DEBUG
    ilf_e9out('D');
#endif
    g_ilf.safe_mode = ilf_read_policy();   /* AvxPolicy: 1 = gate AVX (see core.c) */
    ilf_apply_all_cpus(&g_ilf);   /* programs every logical CPU (see smp.c) */
#ifdef ILF_E9_DEBUG
    ilf_e9out('A');
#endif

    DbgPrint("winsimd: vendor=%s family=%u model=%u stepping=%u feat=%08x\n",
             g_ilf.vendor, g_ilf.family, g_ilf.model, g_ilf.stepping,
             g_ilf.features);
    DbgPrint("winsimd: cr0=%08x cr4 %08x->%08x xcr0=%08x:%08x xsz=%u\n",
             g_ilf.cr0, g_ilf.cr4_before, g_ilf.cr4_after,
             (ILF_U32)(g_ilf.xcr0 >> 32), (ILF_U32)g_ilf.xcr0,
             g_ilf.xsave_area_size);
    DbgPrint("winsimd: mmx=%s sse=%s avx=%s avx512=%s\n",
             disp_str(g_ilf.disp_mmx), disp_str(g_ilf.disp_sse),
             disp_str(g_ilf.disp_avx), disp_str(g_ilf.disp_avx512));

#ifdef ILF_E9_DEBUG
    ilf_e9out('P');
#endif
    st = ilf_selftest(&g_ilf);
#ifdef ILF_E9_DEBUG
    ilf_e9out('S');
#endif
    DbgPrint("winsimd: selftest=%s (mask=%x)\n",
             (st & 0x80000000u) ? "FAIL" : "ok", st);

    ilf_write_status(reg, &g_ilf);   /* publish to registry for WINSINFO.EXE */

    /* 1.2: preserve the register files the OS does not save (ctxguard.c). */
#if !ILF_X64
    ilf_guard_install(&g_ilf, ilf_read_guard_enabled());
#else
    /* amd64: XMM is OS-managed; the YMM upper halves are not, so guard those
       on XP x64 / Server 2003 x64 (guardx64.c), when PatchGuard is un-armed. */
    ilf_guard_install_x64(&g_ilf, ilf_read_guard_enabled());
#endif
    ilf_write_runtime();
    DbgPrint("winsimd: context guard flags=%x components=%x reason=%u slots=%u\n",
             g_cg.flags, g_cg.components_lo, g_cg.reason, g_cg.slots);


#ifdef ILF_E9_DEBUG
    e9_report(&g_ilf, st);
    e9s(" guard flags="); e9hex(g_cg.flags); e9s(" comp="); e9hex(g_cg.components_lo);
    e9s(" reason="); e9hex(g_cg.reason); e9s(" slots="); e9hex(g_cg.slots); e9s("\r\n");
#endif
    return ILF_STATUS_SUCCESS;
}
