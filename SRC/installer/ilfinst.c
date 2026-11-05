/*
 * ilfinst.c - tiny CRT-free console installer for WINSIMD.SYS (WINSIMD.EXE).
 *
 *   WINSIMD [install|remove|start|stop] [/auto|/system|/boot|/demand]
 *
 * install (default): copy WINSIMD.SYS (next to this exe) into
 *   %SystemRoot%\System32\drivers\ and register a kernel-driver service.
 *   Default start = auto (SERVICE_AUTO_START); /system, /boot, /demand override.
 *
 * Windows 9x (95/98/ME): the same EXE copies WINSIMD.VXD into
 *   %windir%\SYSTEM\ and registers it as a static VxD under
 *   HKLM\System\CurrentControlSet\Services\VxD\WINSIMD (StaticVxD, Start=0);
 *   it loads at the next boot.
 *
 * Uses only Win32 APIs present since NT 3.1 / Win95 (kernel32 + advapi32), no CRT.
 * Messages are plain ASCII on purpose (old OEM consoles mangle non-ASCII).
 *
 * Build: one i386 binary (NT 3.1 -> XP x86) and one amd64 binary (XP x64).
 */
#include "ilf.h"

typedef unsigned long DWORD;
typedef int           BOOL;
typedef void         *HANDLE;
typedef void         *SC_HANDLE;
typedef const char   *LPCSTR;
typedef char         *LPSTR;

/* per-arch import decoration: x86 stdcall + "_name", x64 plain */
#if ILF_X64
#  define STDCALL
#  define IMP(n) __asm__(#n)
#else
#  define STDCALL __stdcall
#  define IMP(n) __asm__("_" #n)
#endif

/* ---- kernel32 ---- */
DWORD STDCALL GetModuleFileNameA(HANDLE, LPSTR, DWORD)          IMP(GetModuleFileNameA);
DWORD STDCALL GetSystemDirectoryA(LPSTR, DWORD)                 IMP(GetSystemDirectoryA);
BOOL  STDCALL CopyFileA(LPCSTR, LPCSTR, BOOL)                   IMP(CopyFileA);
BOOL  STDCALL DeleteFileA(LPCSTR)                               IMP(DeleteFileA);
DWORD STDCALL GetLastError(void)                                IMP(GetLastError);
HANDLE STDCALL GetStdHandle(DWORD)                              IMP(GetStdHandle);
BOOL  STDCALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *) IMP(WriteFile);
LPSTR STDCALL GetCommandLineA(void)                             IMP(GetCommandLineA);
int   STDCALL lstrlenA(LPCSTR)                                  IMP(lstrlenA);
LPSTR STDCALL lstrcpyA(LPSTR, LPCSTR)                           IMP(lstrcpyA);
LPSTR STDCALL lstrcatA(LPSTR, LPCSTR)                           IMP(lstrcatA);
void  STDCALL ExitProcess(DWORD)                                IMP(ExitProcess);
DWORD STDCALL GetVersion(void)                                  IMP(GetVersion);
BOOL  STDCALL CloseHandle(HANDLE)                               IMP(CloseHandle);
void  STDCALL Sleep(DWORD)                                      IMP(Sleep);
HANDLE STDCALL GetModuleHandleA(LPCSTR)                         IMP(GetModuleHandleA);

/* ---- user32 / gdi32 (GUI install: confirm dialog + self-drawn progress bar;
   self-drawn so it works even on NT 3.1, which has no comctl32 progress) ---- */
typedef void *HWND; typedef void *HDC; typedef void *HBRUSH; typedef void *HFONT;
typedef void *HICON; typedef void *HCURSOR; typedef void *HINSTANCE; typedef void *HMENU;
typedef unsigned int UINT; typedef unsigned long WPARAM; typedef long LPARAM; typedef long LRESULT;
typedef LRESULT (STDCALL *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL r1,r2; unsigned char rgb[32]; } PAINTSTRUCT;
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbCls, cbWnd; HINSTANCE hInst;
                 HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR menu, cls; } WNDCLASSA;
typedef struct { HWND hwnd; UINT msg; WPARAM wp; LPARAM lp; DWORD time; long x, y; } MSG;

int    STDCALL MessageBoxA(HWND, LPCSTR, LPCSTR, UINT)          IMP(MessageBoxA);
short  STDCALL RegisterClassA(const WNDCLASSA *)                IMP(RegisterClassA);
HWND   STDCALL CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                               HWND, HMENU, HINSTANCE, void *)  IMP(CreateWindowExA);
LRESULT STDCALL DefWindowProcA(HWND, UINT, WPARAM, LPARAM)      IMP(DefWindowProcA);
BOOL   STDCALL ShowWindow(HWND, int)                            IMP(ShowWindow);
BOOL   STDCALL UpdateWindow(HWND)                               IMP(UpdateWindow);
BOOL   STDCALL DestroyWindow(HWND)                              IMP(DestroyWindow);
BOOL   STDCALL InvalidateRect(HWND, const RECT *, BOOL)         IMP(InvalidateRect);
BOOL   STDCALL GetClientRect(HWND, RECT *)                      IMP(GetClientRect);
HDC    STDCALL BeginPaint(HWND, PAINTSTRUCT *)                  IMP(BeginPaint);
BOOL   STDCALL EndPaint(HWND, const PAINTSTRUCT *)              IMP(EndPaint);
BOOL   STDCALL PeekMessageA(MSG *, HWND, UINT, UINT, UINT)      IMP(PeekMessageA);
LRESULT STDCALL DispatchMessageA(const MSG *)                   IMP(DispatchMessageA);
HCURSOR STDCALL LoadCursorA(HINSTANCE, LPCSTR)                  IMP(LoadCursorA);
int    STDCALL GetSystemMetrics(int)                            IMP(GetSystemMetrics);
void   STDCALL FillRect(HDC, const RECT *, HBRUSH)             IMP(FillRect);
void   STDCALL FrameRect(HDC, const RECT *, HBRUSH)           IMP(FrameRect);
HBRUSH STDCALL CreateSolidBrush(DWORD)                          IMP(CreateSolidBrush);
void  *STDCALL GetStockObject(int)                              IMP(GetStockObject);
BOOL   STDCALL DeleteObject(void *)                             IMP(DeleteObject);
int    STDCALL SetBkMode(HDC, int)                              IMP(SetBkMode);
DWORD  STDCALL SetTextColor(HDC, DWORD)                         IMP(SetTextColor);
BOOL   STDCALL TextOutA(HDC, int, int, LPCSTR, int)            IMP(TextOutA);
void  *STDCALL SelectObject(HDC, void *)                        IMP(SelectObject);

/* ---- advapi32 ---- */
SC_HANDLE STDCALL OpenSCManagerA(LPCSTR, LPCSTR, DWORD)         IMP(OpenSCManagerA);
SC_HANDLE STDCALL CreateServiceA(SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD, DWORD,
                                 DWORD, LPCSTR, LPCSTR, DWORD *, LPCSTR,
                                 LPCSTR, LPCSTR)                IMP(CreateServiceA);
SC_HANDLE STDCALL OpenServiceA(SC_HANDLE, LPCSTR, DWORD)        IMP(OpenServiceA);
BOOL  STDCALL StartServiceA(SC_HANDLE, DWORD, LPCSTR *)         IMP(StartServiceA);
BOOL  STDCALL ControlService(SC_HANDLE, DWORD, void *)         IMP(ControlService);
BOOL  STDCALL DeleteService(SC_HANDLE)                          IMP(DeleteService);
BOOL  STDCALL CloseServiceHandle(SC_HANDLE)                     IMP(CloseServiceHandle);
BOOL  STDCALL ChangeServiceConfigA(SC_HANDLE, DWORD, DWORD, DWORD, LPCSTR, LPCSTR,
                                   DWORD *, LPCSTR, LPCSTR, LPCSTR, LPCSTR)
                                                                IMP(ChangeServiceConfigA);
typedef void *HKEY;
long  STDCALL RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, void *,
                              HKEY *, DWORD *)                  IMP(RegCreateKeyExA);
long  STDCALL RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const void *, DWORD)
                                                                IMP(RegSetValueExA);
long  STDCALL RegDeleteKeyA(HKEY, LPCSTR)                       IMP(RegDeleteKeyA);
long  STDCALL RegCloseKey(HKEY)                                 IMP(RegCloseKey);
long  STDCALL RegFlushKey(HKEY)                                 IMP(RegFlushKey);

#define SC_MANAGER_ALL_ACCESS   0x000F003Fu
#define SERVICE_ALL_ACCESS      0x000F01FFu
#define SERVICE_KERNEL_DRIVER   0x00000001u
#define SERVICE_BOOT_START      0x00000000u
#define SERVICE_SYSTEM_START    0x00000001u
#define SERVICE_AUTO_START      0x00000002u
#define SERVICE_DEMAND_START    0x00000003u
#define SERVICE_ERROR_NORMAL    0x00000001u
#define SERVICE_CONTROL_STOP    0x00000001u
#define SERVICE_NO_CHANGE       0xFFFFFFFFu
#define STD_OUTPUT_HANDLE       ((DWORD)-11)
#define ERROR_SERVICE_EXISTS    1073u

#define SVC_NAME "winsimd"
#define SVC_FILE "WINSIMD.SYS"
#define VXD_FILE "WINSIMD.VXD"
#define VXD_KEY  "System\\CurrentControlSet\\Services\\VxD\\WINSIMD"
#define HKEY_LOCAL_MACHINE ((HKEY)(ILF_UPTR)0x80000002u)
#define KEY_ALL_ACCESS     0x000F003Fu
#define REG_SZ             1u
#define REG_DWORD          4u
#define REG_BINARY         3u
#define NT_SVC_KEY "SYSTEM\\CurrentControlSet\\Services\\" SVC_NAME

static DWORD g_policy;   /* 0 = enable AVX (default), 1 = safe (gate AVX) */
static int   g_removed_present;   /* set by do_remove: 1 if anything was removed */

static int is_win9x(void) { return (GetVersion() & 0x80000000u) != 0; }

/* Write HKLM\...\Services\winsimd\AvxPolicy = policy (0 enable AVX, 1 safe). */
static void set_avx_policy(DWORD policy)
{
    HKEY k; DWORD disp;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, NT_SVC_KEY, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &k, &disp) == 0) {
        RegSetValueExA(k, "AvxPolicy", 0, REG_DWORD, (const void *)&policy, 4);
        RegCloseKey(k);
    }
}

/* ---- tiny string/console helpers (no CRT) ---- */
static void outs(const char *s)
{
    DWORD w;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, s, (DWORD)lstrlenA(s), &w, NULL);
}
static void outu(DWORD v)
{
    char b[12]; int i = 11; b[11] = 0;
    if (!v) { outs("0"); return; }
    while (v && i) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    outs(&b[i]);
}
static int contains(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
/* strip the file name from a full path (in place) -> keep trailing backslash */
static void dirof(char *path)
{
    int i, last = -1;
    for (i = 0; path[i]; i++) if (path[i] == '\\' || path[i] == '/') last = i;
    if (last >= 0) path[last + 1] = 0;
}

/* ------------------------------------------------------------------ */
/* GUI progress window (optional; only when launched without /silent). */
/* ------------------------------------------------------------------ */
#define WS_OVERLAPPED 0x00000000u
#define WS_CAPTION    0x00C00000u
#define WS_SYSMENU    0x00080000u
#define WS_VISIBLE    0x10000000u
#define SW_SHOW       5
#define SM_CXSCREEN   0
#define SM_CYSCREEN   1
#define TRANSPARENT   1
#define WM_PAINT      0x000F
#define WM_DESTROY    0x0002
#define MB_YESNO          0x00000004u
#define MB_ICONQUESTION   0x00000020u
#define MB_ICONINFORMATION 0x00000040u
#define MB_OK             0x00000000u
#define IDYES             6

static HWND g_pw; static int g_pct; static const char *g_msg = "";

static LRESULT STDCALL prog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; RECT rc, bar; HDC dc = BeginPaint(h, &ps);
        HBRUSH bg  = CreateSolidBrush(0x00C8C8C8u);   /* light gray panel  */
        HBRUSH frm = CreateSolidBrush(0x00808080u);   /* bar frame         */
        HBRUSH fil = CreateSolidBrush(0x00C05020u);   /* progress fill (blue-ish BGR) */
        HBRUSH emp = CreateSolidBrush(0x00FFFFFFu);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, 0x00000000u);
        TextOutA(dc, 16, 16, g_msg, (int)lstrlenA(g_msg));
        bar.left = 16; bar.right = rc.right - 16; bar.top = 48; bar.bottom = 70;
        FillRect(dc, &bar, emp);
        { RECT f = bar; FillRect(dc, &f, emp); FrameRect(dc, &bar, frm); }
        { RECT in; in.left = bar.left + 1; in.top = bar.top + 1; in.bottom = bar.bottom - 1;
          in.right = bar.left + 1 + ((bar.right - bar.left - 2) * g_pct) / 100;
          FillRect(dc, &in, fil); }
        { char pc[8]; int n = 0, v = g_pct; char t[4]; int k = 0;
          if (v == 0) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
          while (k) pc[n++] = t[--k]; pc[n++] = '%'; pc[n] = 0;
          TextOutA(dc, (rc.right - 24) / 2, 50, pc, n); }
        DeleteObject(bg); DeleteObject(frm); DeleteObject(fil); DeleteObject(emp);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) return 0;
    return DefWindowProcA(h, m, wp, lp);
}

static void gui_open(const char *title)
{
    WNDCLASSA wc; HINSTANCE hi = (HINSTANCE)GetModuleHandleA(NULL);
    int w = 380, hgt = 120, sx, sy;
    wc.style = 0; wc.lpfnWndProc = prog_proc; wc.cbCls = 0; wc.cbWnd = 0;
    wc.hInst = hi; wc.hIcon = NULL; wc.hCursor = LoadCursorA(NULL, (LPCSTR)(ILF_UPTR)32512);
    wc.hbrBackground = (HBRUSH)GetStockObject(0 /*WHITE_BRUSH*/);
    wc.menu = NULL; wc.cls = "WinSimdSetup";
    RegisterClassA(&wc);
    sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    sy = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    g_pw = CreateWindowExA(0, "WinSimdSetup", title,
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                           sx, sy, w, hgt, NULL, NULL, hi, NULL);
    ShowWindow(g_pw, SW_SHOW); UpdateWindow(g_pw);
}
static void gui_pump(void)
{
    MSG msg; int i;
    for (i = 0; i < 64; i++) if (PeekMessageA(&msg, NULL, 0, 0, 1 /*PM_REMOVE*/)) DispatchMessageA(&msg); else break;
}
static void prog(int pct, const char *msg)
{
    if (!g_pw) return;
    g_pct = pct; g_msg = msg;
    InvalidateRect(g_pw, NULL, 1); UpdateWindow(g_pw); gui_pump();
    Sleep(350);
}
static void gui_close(void) { if (g_pw) { DestroyWindow(g_pw); g_pw = NULL; } }

static SC_HANDLE open_scm(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { outs("ERROR: OpenSCManager failed ("); outu(GetLastError()); outs(") - admin rights?\n"); }
    return scm;
}


static int do_install(DWORD startType)
{
    char src[300], dst[300], tmp[300];
    SC_HANDLE scm, svc;

    GetModuleFileNameA(NULL, src, sizeof(src));   /* ...\WINSIMD.EXE */
    dirof(src);
    lstrcatA(src, SVC_FILE);                       /* ...\WINSIMD.SYS */

    GetSystemDirectoryA(dst, sizeof(dst));         /* C:\WINNT\System32 */
    lstrcatA(dst, "\\drivers\\");
    lstrcatA(dst, SVC_FILE);

    prog(10, "Copying WINSIMD.SYS to the drivers folder...");
    outs("copy "); outs(src); outs(" -> "); outs(dst); outs("\n");
    if (!CopyFileA(src, dst, 0)) {
        outs("ERROR: CopyFile failed ("); outu(GetLastError()); outs(")\n");
        return 2;
    }

    prog(45, "Registering the WINSIMD service...");
    scm = open_scm();
    if (!scm) return 3;

    svc = CreateServiceA(scm, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                         SERVICE_KERNEL_DRIVER, startType, SERVICE_ERROR_NORMAL,
                         dst, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            outs("service exists - updating config\n");
            svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
            if (svc)
                ChangeServiceConfigA(svc, SERVICE_KERNEL_DRIVER, startType,
                                     SERVICE_ERROR_NORMAL, dst, NULL, NULL,
                                     NULL, NULL, NULL, NULL);
        } else {
            outs("ERROR: CreateService failed ("); outu(e); outs(")\n");
            CloseServiceHandle(scm);
            return 4;
        }
    }
    (void)tmp;
    prog(80, "Applying the AVX policy...");
    set_avx_policy(g_policy);
    prog(100, "Installation complete.");
    outs("installed service '" SVC_NAME "', start=");
    outu(startType);
    outs(startType == SERVICE_BOOT_START   ? " (boot)\n"   :
         startType == SERVICE_SYSTEM_START ? " (system)\n" :
         startType == SERVICE_AUTO_START   ? " (auto)\n"   : " (demand)\n");
    outs(g_policy ? "AVX policy: SAFE (AVX gated; apps use SSE) - AvxPolicy=1\n"
                  : "AVX policy: enabled (default) - AvxPolicy=0\n");
    outs("Restart Windows to load the driver.\n");
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int do_control(int start)
{
    SC_HANDLE scm = open_scm(), svc;
    DWORD status[7];
    if (!scm) return 3;
    svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!svc) { outs("ERROR: OpenService ("); outu(GetLastError()); outs(")\n"); CloseServiceHandle(scm); return 4; }
    if (start) {
        if (StartServiceA(svc, 0, NULL)) outs("started.\n");
        else { outs("StartService failed ("); outu(GetLastError()); outs(") - check DbgPrint/KD log\n"); }
    } else {
        if (ControlService(svc, SERVICE_CONTROL_STOP, status)) outs("stopped.\n");
        else { outs("stop failed ("); outu(GetLastError()); outs(")\n"); }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int do_remove(void)
{
    char dst[300];
    SC_HANDLE scm = open_scm(), svc;
    DWORD status[7];
    int present = 0;
    if (!scm) return 3;
    prog(30, "Stopping and removing the WINSIMD service...");
    svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        present = 1;
        ControlService(svc, SERVICE_CONTROL_STOP, status);   /* ignore errors */
        if (DeleteService(svc)) outs("service deleted.\n");
        else { outs("DeleteService failed ("); outu(GetLastError()); outs(")\n"); }
        CloseServiceHandle(svc);
    } else {
        outs("service not present.\n");
    }
    CloseServiceHandle(scm);
    prog(75, "Deleting the driver file...");
    GetSystemDirectoryA(dst, sizeof(dst));
    lstrcatA(dst, "\\drivers\\" SVC_FILE);
    if (DeleteFileA(dst)) { outs("driver file removed.\n"); present = 1; }
    prog(100, present ? "WINSIMD removed." : "WINSIMD was not installed.");
    g_removed_present = present;
    return 0;
}

/* ---- Windows 9x: static VxD ---- */
static void vxd_paths(char *src, char *dst)
{
    GetModuleFileNameA(NULL, src, 300);
    dirof(src);
    lstrcatA(src, VXD_FILE);
    GetSystemDirectoryA(dst, 300);                 /* C:\WINDOWS\SYSTEM */
    lstrcatA(dst, "\\" VXD_FILE);
}

static int do_install_9x(void)
{
    char src[300], dst[300];
    HKEY k; DWORD disp;
    static const unsigned char start0 = 0;
    long e;

    vxd_paths(src, dst);
    outs("copy "); outs(src); outs(" -> "); outs(dst); outs("\n");
    if (!CopyFileA(src, dst, 0)) {
        outs("ERROR: CopyFile failed ("); outu(GetLastError()); outs(")\n");
        return 2;
    }
    e = RegCreateKeyExA(HKEY_LOCAL_MACHINE, VXD_KEY, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, &disp);
    if (e) { outs("ERROR: RegCreateKeyEx failed ("); outu((DWORD)e); outs(")\n"); return 3; }
    RegSetValueExA(k, "StaticVxD", 0, REG_SZ, "winsimd.vxd", 12);
    RegSetValueExA(k, "Start", 0, REG_BINARY, &start0, 1);
    RegFlushKey(k);                                /* commit to SYSTEM.DAT now */
    RegCloseKey(k);
    outs("installed static VxD (HKLM\\" VXD_KEY ").\n");
    outs("reboot to load it.\n");
    return 0;
}

static int do_remove_9x(void)
{
    char src[300], dst[300];
    vxd_paths(src, dst);
    if (RegDeleteKeyA(HKEY_LOCAL_MACHINE, VXD_KEY) == 0) outs("registry entry removed.\n");
    else outs("registry entry not present.\n");
    if (DeleteFileA(dst)) outs("VxD file removed.\n");
    outs("reboot to unload it.\n");
    return 0;
}

/* Report the detected OS and which binary/files this installer will use. */
static void report_os(void)
{
    DWORD v = GetVersion();
    DWORD maj = v & 0xFF, min = (v >> 8) & 0xFF;
    outs("detected OS: ");
    if (v & 0x80000000u) {
        /* Win9x: 4.00 = 95, 4.10 = 98, 4.90 = ME */
        outs("Windows 9x (");  outu(maj); outs("."); outu(min); outs(")\n");
        outs("  -> installing " VXD_FILE " (static VxD)\n");
    } else {
        outs("Windows NT (");  outu(maj); outs("."); outu(min); outs(")\n");
#if ILF_X64
        outs("  -> installing amd64 " SVC_FILE " (kernel driver, XP x64)\n");
#else
        outs("  -> installing i386 " SVC_FILE " (kernel driver, NT 3.1 - XP x86)\n");
#endif
    }
}

void STDCALL ilf_start(void)
{
    char *cmd = GetCommandLineA();
    /* Default to auto-start. On some multiprocessor HALs (seen on NT 3.51 MPS),
       a system-start driver loads before every application processor is fully
       online, so the per-CPU affinity programming can stall and the service is
       reported as failed. Auto-start runs after all CPUs are up and still well
       before user programs need SSE/AVX. /system and /boot remain available. */
    DWORD startType = SERVICE_AUTO_START;
    int rc;

    outs("winsimd installer\n");
    report_os();

    {
    int silent  = contains(cmd, "/silent");
    int remove  = contains(cmd, "remove");    /* matches "remove" and "/remove" */
    int control = contains(cmd, "start") || contains(cmd, "stop");
    /* GUI mode = an install or remove that is neither /silent nor a control verb.
       It shows a Yes/No confirmation and a progress bar. Control verbs and
       Windows 9x stay console-only, as before. */
    int gui = !silent && !control;

    if (contains(cmd, "/boot"))   startType = SERVICE_BOOT_START;
    if (contains(cmd, "/demand")) startType = SERVICE_DEMAND_START;
    if (contains(cmd, "/system")) startType = SERVICE_SYSTEM_START;
    if (contains(cmd, "/auto"))   startType = SERVICE_AUTO_START;
    if (contains(cmd, "/safe"))   g_policy = 1;   /* gate AVX */
    if (contains(cmd, "/avx"))    g_policy = 0;   /* force-enable AVX */

    if (is_win9x()) {
        if (remove) {
            if (gui && MessageBoxA(NULL, "Remove WINSIMD (the SIMD enabler VxD) from this computer?",
                                   "WINSIMD Setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
                ExitProcess(0);
            if (gui) gui_open("WINSIMD Setup");
            prog(40, "Removing the WINSIMD VxD...");
            rc = do_remove_9x();
            prog(100, "WINSIMD removed.");
            if (gui) { gui_close(); MessageBoxA(NULL, "WINSIMD has been removed.\nRestart Windows to finish.",
                                                "WINSIMD Setup", MB_OK | MB_ICONINFORMATION); }
            else outs("completed.\n");
        } else if (control) {
            outs("start/stop: not supported on Windows 9x - reboot instead.\n"); rc = 1;
        } else {
            if (gui && MessageBoxA(NULL, "Install WINSIMD (the SIMD enabler VxD) on this computer?",
                                   "WINSIMD Setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
                ExitProcess(0);
            if (gui) gui_open("WINSIMD Setup");
            prog(20, "Installing the WINSIMD VxD...");
            rc = do_install_9x();
            prog(100, "Installation complete.");
            if (gui) { gui_close(); MessageBoxA(NULL, "WINSIMD has been installed.\nRestart Windows to load it.",
                                                "WINSIMD Setup", MB_OK | MB_ICONINFORMATION); }
            else outs("completed.\n");
        }
        ExitProcess((DWORD)rc);
    }

    if (remove) {
        if (gui && MessageBoxA(NULL, "Remove WINSIMD (the SIMD enabler driver) from this computer?",
                               "WINSIMD Setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
            ExitProcess(0);
        if (gui) gui_open("WINSIMD Setup");
        rc = do_remove();
        if (gui) {
            gui_close();
            MessageBoxA(NULL, g_removed_present ? "WINSIMD has been removed.\nRestart Windows to finish."
                                                : "WINSIMD was not installed on this computer.",
                        "WINSIMD Setup", MB_OK | MB_ICONINFORMATION);
        } else outs("completed.\n");
    } else if (control) {
        rc = contains(cmd, "start") ? do_control(1) : do_control(0);
    } else {
        if (gui && MessageBoxA(NULL,
                "Install WINSIMD (the SIMD enabler driver) on this computer?\n\n"
                "It enables MMX/SSE/AVX on this CPU and preserves those\n"
                "registers across threads where the OS does not.",
                "WINSIMD Setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
            ExitProcess(0);
        if (gui) gui_open("WINSIMD Setup");
        rc = do_install(startType);
        if (gui) {
            gui_close();
            if (rc == 0)
                MessageBoxA(NULL, "WINSIMD has been installed.\nRestart Windows to load the driver.",
                            "WINSIMD Setup", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxA(NULL, "WINSIMD installation failed.\nTry running from an elevated Command Prompt.",
                            "WINSIMD Setup", MB_OK | MB_ICONINFORMATION);
        } else outs("completed.\n");
    }

    ExitProcess((DWORD)rc);
    }
}
