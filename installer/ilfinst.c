/*
 * ilfinst.c - tiny CRT-free console installer for intlfxsr.sys.
 *
 *   ilfinst [install|remove|start|stop] [/boot|/system|/demand]
 *
 * install (default): copy intlfxsr.sys (next to this exe) into
 *   %SystemRoot%\System32\drivers\ and register a kernel-driver service.
 *   Default start = system (SERVICE_SYSTEM_START); /boot = boot-start.
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

    GetModuleFileNameA(NULL, src, sizeof(src));   /* ...\ilfinst.exe */
    dirof(src);
    lstrcatA(src, SVC_FILE);                       /* ...\intlfxsr.sys */

    GetSystemDirectoryA(dst, sizeof(dst));         /* C:\WINNT\System32 */
    lstrcatA(dst, "\\drivers\\");
    lstrcatA(dst, SVC_FILE);

    outs("copy "); outs(src); outs(" -> "); outs(dst); outs("\n");
    if (!CopyFileA(src, dst, 0)) {
        outs("ERROR: CopyFile failed ("); outu(GetLastError()); outs(")\n");
        return 2;
    }

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
    set_avx_policy(g_policy);
    outs("installed service '" SVC_NAME "', start=");
    outu(startType);
    outs(startType == SERVICE_BOOT_START   ? " (boot)\n"   :
         startType == SERVICE_SYSTEM_START ? " (system)\n" : " (demand)\n");
    outs(g_policy ? "AVX policy: SAFE (AVX gated; apps use SSE) - AvxPolicy=1\n"
                  : "AVX policy: enabled (default) - AvxPolicy=0\n");
    outs("reboot to load at boot; or 'ilfinst start' to load now.\n");
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
    if (!scm) return 3;
    svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        ControlService(svc, SERVICE_CONTROL_STOP, status);   /* ignore errors */
        if (DeleteService(svc)) outs("service deleted.\n");
        else { outs("DeleteService failed ("); outu(GetLastError()); outs(")\n"); }
        CloseServiceHandle(svc);
    } else {
        outs("service not present.\n");
    }
    CloseServiceHandle(scm);
    GetSystemDirectoryA(dst, sizeof(dst));
    lstrcatA(dst, "\\drivers\\" SVC_FILE);
    if (DeleteFileA(dst)) outs("driver file removed.\n");
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
    DWORD startType = SERVICE_SYSTEM_START;
    int rc;

    outs("winsimd installer\n");
    report_os();

    if (contains(cmd, "/boot"))   startType = SERVICE_BOOT_START;
    if (contains(cmd, "/demand")) startType = SERVICE_DEMAND_START;
    if (contains(cmd, "/system")) startType = SERVICE_SYSTEM_START;
    if (contains(cmd, "/safe"))   g_policy = 1;   /* gate AVX */
    if (contains(cmd, "/avx"))    g_policy = 0;   /* force-enable AVX */

    if (is_win9x()) {
        if      (contains(cmd, "remove")) rc = do_remove_9x();
        else if (contains(cmd, "start") || contains(cmd, "stop")) {
            outs("start/stop: not supported on Windows 9x - reboot instead.\n"); rc = 1;
        }
        else                              rc = do_install_9x();
        ExitProcess((DWORD)rc);
    }

    if      (contains(cmd, "remove")) rc = do_remove();
    else if (contains(cmd, "start"))  rc = do_control(1);
    else if (contains(cmd, "stop"))   rc = do_control(0);
    else                              rc = do_install(startType);

    ExitProcess((DWORD)rc);
}
