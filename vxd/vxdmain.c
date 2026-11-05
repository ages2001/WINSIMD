/*
 * vxdmain.c - Windows 9x side of WINSIMD (WINSIMD.VXD).
 * Runs the very same core.c detection + automatic-hybrid policy and guard.c
 * self-test as the NT driver, at Sys_Critical_Init on the (only) CPU, and
 * exposes the resulting ILF_CPU_INFO to Win32 (WINSINFO.EXE) through
 * DeviceIoControl on \\.\WINSIMD. Uses no VMM services at all.
 * Called from the register-ABI control procedure in vxdglue.asm.
 */
#include "ilf.h"
#include "lowlevel.h"

void         ilf_detect(ILF_CPU_INFO *ci);
ILF_NTSTATUS ilf_apply_policy(ILF_CPU_INFO *ci);
int          ilf_selftest(ILF_CPU_INFO *ci);

ILF_CPU_INFO g_ilf;          /* resident snapshot */
static int   g_done;
static int   g_selftest;

/* ---- thread-switch vector guard (see vxd/vxdsched.asm) ---- */
void           register_switch_cb(void);     /* asm: hook the scheduler        */
void           ymm_save8(void *buf);          /* asm: store XMM0-7 / YMM0-7      */
void           ymm_restore8(void *buf);       /* asm: load  XMM0-7 / YMM0-7      */
unsigned char  vxd_avx;                        /* asm reads this: 1=YMM, 0=XMM   */
unsigned char  vxd_upper;                      /* asm reads this: 1=upper-only   */

/* Per-thread saved vector state, keyed by VMM thread handle. Bounded, with
   round-robin eviction (an evicted thread just reloads zero upper next time -
   a benign correctness loss, never a fault), exactly like the x64 guard. */
/* buf: 8 vector regs * 32B = 0x100, then MXCSR (4B) at offset 0x100. */
typedef struct { ILF_U32 thread; ILF_U8 buf[0x100 + 16]; } VSlot;
#define NVSLOT 256
static VSlot   g_vs[NVSLOT];
static ILF_U32 g_vrr;                          /* round-robin cursor            */
static ILF_U32 g_prev;                         /* thread this CPU last ran      */
static int     g_guard_on;

static VSlot *vs_find(ILF_U32 thread)
{
    ILF_U32 i; VSlot *empty = 0;
    for (i = 0; i < NVSLOT; i++) {
        if (g_vs[i].thread == thread) return &g_vs[i];
        if (!g_vs[i].thread && !empty) empty = &g_vs[i];
    }
    if (!empty) { empty = &g_vs[g_vrr]; g_vrr = (g_vrr + 1) % NVSLOT; }
    empty->thread = thread;
    for (i = 0; i < sizeof(empty->buf); i++) empty->buf[i] = 0;
    return empty;
}

/* Called from the asm scheduler callback with the incoming thread handle.
   The vector registers still hold the OUTGOING thread's data (VMCPD does not
   touch XMM/YMM), so save them for g_prev, then load the incoming thread's. */
ILF_U32 g_sw_calls, g_sw_real;
/* The scheduler thread-switch callback hands us two thread-control-block
   pointers in the pushad frame - EAX and EDI - which each switch are the
   outgoing and incoming threads (EBX is the constant VM handle). We identify
   them by state rather than by register, since which one is which varies: at
   callback time the live vector registers belong to the thread we last
   switched to (g_prev), so the OUTGOING is g_prev and the INCOMING is whichever
   of {EAX, EDI} is not g_prev. The save (to g_prev) is therefore always
   correct; the restore picks the incoming.
   pushad frame order (top of stack -> ): edi,esi,ebp,esp,ebx,edx,ecx,eax */
void __cdecl vxd_switch(ILF_U32 *f)
{
    ILF_U32 a = f[7], d = f[0];       /* EAX, EDI : the two thread handles */
    ILF_U32 out = g_prev;
    ILF_U32 inc = (a != out) ? a : d; /* incoming = the handle that isn't live */
    g_sw_calls++;
    if (inc == out) return;
    g_sw_real++;
    if (out) ymm_save8(vs_find(out)->buf);   /* save the live (outgoing) regs */
    ymm_restore8(vs_find(inc)->buf);         /* load the incoming thread's    */
    g_prev = inc;
}

/* Win32 DeviceIoControl parameter block (VWIN32 -> VxD, ESI on entry) */
typedef struct {
    ILF_U32  Internal1, VMHandle, Internal2;
    ILF_U32  dwIoControlCode;
    void    *lpvInBuffer;   ILF_U32 cbInBuffer;
    void    *lpvOutBuffer;  ILF_U32 cbOutBuffer;
    ILF_U32 *lpcbBytesReturned;
    void    *lpoOverlapped;
    ILF_U32  hDevice, tagProcess;
} DIOCPARAMS;

#define DIOC_OPEN         0u
#define DIOC_CLOSEHANDLE  0xFFFFFFFFu
#define ERROR_NOT_SUPPORTED        50u
#define ERROR_INSUFFICIENT_BUFFER  122u

#ifdef ILF_E9_DEBUG
static void e9s(const char *s) { while (*s) ilf_e9out((ILF_U8)*s++); }
static void e9hex(ILF_U32 v)
{
    const char *h = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) ilf_e9out((ILF_U8)h[(v >> i) & 0xF]);
}
static void e9_report(ILF_CPU_INFO *ci, int st)
{
    e9s("\r\n[E9] winsimd.vxd load\r\n vendor="); e9s(ci->vendor);
    e9s(" feat=");  e9hex(ci->features);
    e9s(" cr4=");   e9hex(ci->cr4_before); e9s("->"); e9hex(ci->cr4_after);
    e9s("\r\n xcr0="); e9hex((ILF_U32)(ci->xcr0 >> 32)); e9hex((ILF_U32)ci->xcr0);
    e9s(" xsz=");   e9hex(ci->xsave_area_size);
    e9s("\r\n disp mmx="); e9hex((ILF_U32)ci->disp_mmx);
    e9s(" sse=");   e9hex((ILF_U32)ci->disp_sse);
    e9s(" avx=");   e9hex((ILF_U32)ci->disp_avx);
    e9s(" avx512=");e9hex((ILF_U32)ci->disp_avx512);
    e9s("\r\n brand="); e9s(ci->brand);
    e9s("\r\n selftest="); e9hex((ILF_U32)st);
    e9s((st & 0x80000000u) ? " FAIL\r\n" : " OK\r\n");
}
#endif

#ifdef ILF_E9_DEBUG
#  define TRACE(c) ilf_e9out((ILF_U8)(c))
#else
#  define TRACE(c) ((void)0)
#endif

/* Sys_Critical_Init: program the CPU once. */
void __cdecl vxd_init(void)
{
    if (g_done) return;
    g_done = 1;
    TRACE('d');
    ilf_detect(&g_ilf);
    TRACE('p');
    ilf_apply_policy(&g_ilf);
    g_ilf.nproc_applied = 1;          /* Win9x is uniprocessor */
    TRACE('s');
    g_selftest = ilf_selftest(&g_ilf);
    TRACE('r');
#ifdef ILF_E9_DEBUG
    e9_report(&g_ilf, g_selftest);
#endif
}

/* Device_Init: register the scheduler callback once the CPU is programmed and
   the scheduler is up, but only if there is vector state the OS won't keep. */
void __cdecl vxd_device_init(void)
{
    int os_keeps_xmm;
    if (g_guard_on) return;
    if (!(g_ilf.features & ILF_FEAT_SSE)) return;     /* nothing to guard   */
    g_guard_on = 1;
    TRACE('H');

    vxd_avx = (g_ilf.features & ILF_FEAT_AVX) &&
              (g_ilf.features & ILF_FEAT_XSAVE) &&
              (g_ilf.xcr0_after & ILF_XCR0_AVX) &&
              !g_ilf.safe_mode;

    /* If OSFXSR was already set before WINSIMD ran, the OS (its VMCPD) is
       SSE-aware and preserves XMM + MXCSR itself (Win98 SE / ME). Then we must
       NOT touch XMM - only keep the YMM upper halves (upper-only mode) - or we
       collide with its lazy fxsave/fxrstor. If the OS is not SSE-aware (Win95 /
       Win98 FE, cr4_before has no OSFXSR) we own the full register (full mode). */
    os_keeps_xmm = (g_ilf.cr4_before & ILF_CR4_OSFXSR) != 0;
    vxd_upper = os_keeps_xmm ? 1 : 0;

    /* When the OS already keeps XMM and there is no AVX, there is nothing left
       for the guard to preserve - leave XMM to the OS and do not hook. */
    if (os_keeps_xmm && !vxd_avx) {
        TRACE('N');                /* no-op: OS preserves XMM, no AVX to guard */
        return;
    }
#ifndef VXD_NO_GUARD
    register_switch_cb();          /* -DVXD_NO_GUARD builds an enable-only baseline */
    TRACE(vxd_upper ? 'U' : (vxd_avx ? 'Y' : 'X'));
#else
    TRACE('0');
#endif
}

/* W32_DEVICEIOCONTROL: returns a Win32 error code (0 = success). */
ILF_U32 __cdecl vxd_ioctl(DIOCPARAMS *p)
{
    switch (p->dwIoControlCode) {
    case DIOC_OPEN:
    case DIOC_CLOSEHANDLE:
        return 0;
    case ILF_IOCTL_GET_STATUS: {
        const ILF_U8 *s = (const ILF_U8 *)&g_ilf;
        ILF_U8 *d = (ILF_U8 *)p->lpvOutBuffer;
        ILF_U32 i, n = sizeof(g_ilf);
        if (!d || p->cbOutBuffer < n) return ERROR_INSUFFICIENT_BUFFER;
        for (i = 0; i < n; i++) d[i] = s[i];
        if (p->lpcbBytesReturned) *p->lpcbBytesReturned = n;
        return 0;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
}
