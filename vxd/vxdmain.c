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
