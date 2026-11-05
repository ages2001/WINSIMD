/*
 * shortcuts.c - create/remove the WINSIMD tool and uninstall shortcuts, per OS.
 * #included by ilfinst.c (one CRT-free translation unit; it reuses that file's
 * STDCALL/IMP macros, the Win32 declarations above it, and outs()/dirof()).
 *
 * The rule the user asked for:
 *   - Program Manager shells (NT 3.1/3.5x, GetVersion major == 3): a "WINSIMD"
 *     program group via DDE to PROGMAN, holding the tool item (WINSINFO) AND an
 *     "Uninstall WINSIMD" item (these shells have no Add/Remove Programs, so the
 *     uninstall gets a shortcut here).
 *   - Explorer shells with Add/Remove Programs (Win9x, NT 4.0+, 2000, XP): a
 *     Start-Menu .lnk for the tool (WINSINFO) via IShellLink, and the uninstall
 *     is registered in Add/Remove Programs instead of getting a desktop shortcut.
 *
 * Everything lands in %SystemRoot%\WINSIMD\ (WINSINFO.EXE + this installer copied
 * as WINSIMD.EXE, so the uninstall item/entry can run "WINSIMD.EXE remove").
 *
 * Portability: the Start-Menu path is read from the registry "Shell Folders"
 * (present on every Win95+/NT4+), NOT from shell32!SHGetSpecialFolderPathA, which
 * is absent on bare Win95 / pre-IE4 NT4 and whose static import would stop this
 * EXE from launching there. DDE (user32) and OLE2 core (ole32) are present on
 * every target back to NT 3.1 / Win95, so importing them statically is safe. */

typedef void          *HWND;
typedef unsigned short WCHAR;
typedef long           HRESULT;
typedef unsigned long  ULONG;
typedef unsigned int   UINT;

/* ---- kernel32 extras ---- */
BOOL   STDCALL CreateDirectoryA(LPCSTR, void*)                          IMP(CreateDirectoryA);
DWORD  STDCALL GetWindowsDirectoryA(LPSTR, DWORD)                       IMP(GetWindowsDirectoryA);
HANDLE STDCALL CreateFileA(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE) IMP(CreateFileA);
BOOL   STDCALL CloseHandle(HANDLE)                                      IMP(CloseHandle);

#include "uninst_ico.h"   /* UNINST_ICO[] / UNINST_ICO_LEN : red-X uninstall icon */

#define GENERIC_WRITE_        0x40000000u
#define CREATE_ALWAYS_        2u
#define FILE_ATTR_NORMAL_     0x80u
#define INVALID_HANDLE_VALUE_ ((HANDLE)(ILF_UPTR)-1)

/* write len bytes to path (overwrite). Returns 1 on success. */
static int write_bytes(const char *path, const void *data, DWORD len)
{
    DWORD w = 0; HANDLE h;
    h = CreateFileA(path, GENERIC_WRITE_, 0, NULL, CREATE_ALWAYS_, FILE_ATTR_NORMAL_, NULL);
    if (h == INVALID_HANDLE_VALUE_) return 0;
    WriteFile(h, data, len, &w, NULL);
    CloseHandle(h);
    return w == len;
}

/* ---- advapi32 extras (RegCreateKeyExA/RegSetValueExA/RegDeleteKeyA/RegCloseKey
       are already declared in ilfinst.c above) ---- */
long  STDCALL RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, HKEY*)          IMP(RegOpenKeyExA);
long  STDCALL RegQueryValueExA(HKEY, LPCSTR, DWORD*, DWORD*, void*, DWORD*) IMP(RegQueryValueExA);

/* LoadLibrary/GetProcAddress: ole32 is bound at RUNTIME (see below). */
HANDLE STDCALL LoadLibraryA(LPCSTR)                                     IMP(LoadLibraryA);
void  *STDCALL GetProcAddress(HANDLE, LPCSTR)                           IMP(GetProcAddress);
BOOL   STDCALL FreeLibrary(HANDLE)                                      IMP(FreeLibrary);

/* ---- user32: DDE, for the Program Manager (NT 3.x) ---- */
typedef UINT (STDCALL *DDECALLBACK)(UINT,UINT,void*,void*,void*,void*,ULONG,ULONG);
UINT  STDCALL DdeInitializeA(DWORD*, DDECALLBACK, DWORD, DWORD)         IMP(DdeInitializeA);
void *STDCALL DdeCreateStringHandleA(DWORD, LPCSTR, int)                IMP(DdeCreateStringHandleA);
void *STDCALL DdeConnect(DWORD, void*, void*, void*)                    IMP(DdeConnect);
void *STDCALL DdeClientTransaction(const void*, DWORD, void*, void*, UINT, UINT, DWORD, DWORD*) IMP(DdeClientTransaction);
BOOL  STDCALL DdeDisconnect(void*)                                      IMP(DdeDisconnect);
BOOL  STDCALL DdeFreeStringHandle(DWORD, void*)                         IMP(DdeFreeStringHandle);
UINT  STDCALL DdeUninitialize(DWORD)                                    IMP(DdeUninitialize);

/* ---- ole32: OLE2 core, for IShellLink (.lnk) on Explorer shells. It is bound
   at RUNTIME via LoadLibraryA because ole32.dll does NOT exist on NT 3.1 (whose
   OLE2 is compobj.dll/ole2.dll); a static import would stop the whole installer
   from loading there, and NT 3.1 uses the Program Manager/DDE path which needs
   no COM. These are only ever called on Explorer shells (nt_major>=4 / Win9x). */
typedef HRESULT (STDCALL *CoInitialize_t)(void*);
typedef void    (STDCALL *CoUninitialize_t)(void);
typedef HRESULT (STDCALL *CoCreateInstance_t)(const void*, void*, DWORD, const void*, void**);
static CoInitialize_t     p_CoInitialize;
static CoUninitialize_t   p_CoUninitialize;
static CoCreateInstance_t p_CoCreateInstance;

#define HKEY_CURRENT_USER ((HKEY)(ILF_UPTR)0x80000001u)
#define CP_ACP 0
#define DMLERR_NO_ERROR 0
#define APPCLASS_STANDARD 0
#define CF_TEXT 1
#define XTYP_EXECUTE  0x4050
#define CLSCTX_INPROC_SERVER 1
#define SHELL_FOLDERS_KEY "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders"
#define ADDREMOVE_KEY     "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WINSIMD"

/* GUIDs */
typedef struct { DWORD a; unsigned short b, c; unsigned char d[8]; } ILFGUID;
static const ILFGUID CLSID_ShellLink  = {0x00021401,0,0,{0xC0,0,0,0,0,0,0,0x46}};
static const ILFGUID IID_IShellLinkA  = {0x000214EE,0,0,{0xC0,0,0,0,0,0,0,0x46}};
static const ILFGUID IID_IPersistFile = {0x0000010B,0,0,{0xC0,0,0,0,0,0,0,0x46}};

/* IShellLinkA vtable (slots in interface order; we call SetPath/SetArguments/
   SetDescription + the IUnknown trio). */
typedef struct ISLVtbl {
    HRESULT (STDCALL *QueryInterface)(void*, const ILFGUID*, void**);
    ULONG   (STDCALL *AddRef)(void*);
    ULONG   (STDCALL *Release)(void*);
    HRESULT (STDCALL *GetPath)(void*, LPSTR, int, void*, DWORD);
    HRESULT (STDCALL *GetIDList)(void*, void*);
    HRESULT (STDCALL *SetIDList)(void*, void*);
    HRESULT (STDCALL *GetDescription)(void*, LPSTR, int);
    HRESULT (STDCALL *SetDescription)(void*, LPCSTR);
    HRESULT (STDCALL *GetWorkingDirectory)(void*, LPSTR, int);
    HRESULT (STDCALL *SetWorkingDirectory)(void*, LPCSTR);
    HRESULT (STDCALL *GetArguments)(void*, LPSTR, int);
    HRESULT (STDCALL *SetArguments)(void*, LPCSTR);
    HRESULT (STDCALL *GetHotkey)(void*, void*);
    HRESULT (STDCALL *SetHotkey)(void*, unsigned short);
    HRESULT (STDCALL *GetShowCmd)(void*, int*);
    HRESULT (STDCALL *SetShowCmd)(void*, int);
    HRESULT (STDCALL *GetIconLocation)(void*, LPSTR, int, int*);
    HRESULT (STDCALL *SetIconLocation)(void*, LPCSTR, int);
    HRESULT (STDCALL *SetRelativePath)(void*, LPCSTR, DWORD);
    HRESULT (STDCALL *Resolve)(void*, HWND, DWORD);
    HRESULT (STDCALL *SetPath)(void*, LPCSTR);
} ISLVtbl;
typedef struct { ISLVtbl *v; } ISL;

typedef struct IPFVtbl {
    HRESULT (STDCALL *QueryInterface)(void*, const ILFGUID*, void**);
    ULONG   (STDCALL *AddRef)(void*);
    ULONG   (STDCALL *Release)(void*);
    HRESULT (STDCALL *GetClassID)(void*, void*);
    HRESULT (STDCALL *IsDirty)(void*);
    HRESULT (STDCALL *Load)(void*, const WCHAR*, DWORD);
    HRESULT (STDCALL *Save)(void*, const WCHAR*, BOOL);
    HRESULT (STDCALL *SaveCompleted)(void*, const WCHAR*);
    HRESULT (STDCALL *GetCurFile)(void*, WCHAR**);
} IPFVtbl;
typedef struct { IPFVtbl *v; } IPF;

static int nt_major(void){ DWORD v=GetVersion(); return (v&0x80000000u)?0:(int)(v&0xFF); }

/* %SystemRoot%\WINSIMD into dir (no trailing slash). */
static void winsimd_dir(char *dir)
{
    GetWindowsDirectoryA(dir, 260);
    lstrcatA(dir, "\\WINSIMD");
}

/* Copy WINSINFO.EXE and this installer (as WINSIMD.EXE) into %SystemRoot%\WINSIMD
   so the shortcuts have a stable home. Fills dir/info/inst with full paths. */
static void stage_tools(char *dir, char *info, char *inst, char *ico)
{
    char src[300], me[300];
    winsimd_dir(dir);
    CreateDirectoryA(dir, 0);                         /* ok if it already exists */

    GetModuleFileNameA(NULL, me, sizeof(me));          /* ...\WINSIMD.EXE (installer) */
    lstrcpyA(src, me); dirof(src);
    lstrcatA(src, "WINSINFO.EXE");                      /* ...\WINSINFO.EXE beside it  */

    lstrcpyA(info, dir); lstrcatA(info, "\\WINSINFO.EXE");
    lstrcpyA(inst, dir); lstrcatA(inst, "\\WINSIMD.EXE");
    lstrcpyA(ico,  dir); lstrcatA(ico,  "\\UNINST.ICO");

    CopyFileA(src, info, 0);                            /* the tool */
    CopyFileA(me,  inst, 0);                            /* the installer (for uninstall) */
    write_bytes(ico, UNINST_ICO, (DWORD)UNINST_ICO_LEN);  /* red-X uninstall icon */
}

/* Common (all-users) Start Menu\Programs, else per-user Programs, from the
   registry. Returns 1 and fills out on success. */
static int programs_dir(char *out)
{
    HKEY k; DWORD type, len;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SHELL_FOLDERS_KEY, 0, KEY_ALL_ACCESS, &k) == 0) {
        type = 0; len = 260;
        if (RegQueryValueExA(k, "Common Programs", 0, &type, out, &len) == 0 && out[0]) {
            RegCloseKey(k); return 1;
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyExA(HKEY_CURRENT_USER, SHELL_FOLDERS_KEY, 0, KEY_ALL_ACCESS, &k) == 0) {
        type = 0; len = 260;
        if (RegQueryValueExA(k, "Programs", 0, &type, out, &len) == 0 && out[0]) {
            RegCloseKey(k); return 1;
        }
        RegCloseKey(k);
    }
    return 0;
}

/* ---- Program Manager (NT 3.x) via DDE ---- */
static UINT STDCALL dde_cb(UINT t,UINT f,void*c,void*h1,void*h2,void*d,ULONG a,ULONG b)
{ (void)t;(void)f;(void)c;(void)h1;(void)h2;(void)d;(void)a;(void)b; return 0; }

static void progman_cmd(void *conv, const char *cmd)
{
    int n = 0; while (cmd[n]) n++;
    DdeClientTransaction((const void*)cmd, (DWORD)(n + 1), conv, 0,
                         CF_TEXT, XTYP_EXECUTE, 5000, 0);
}

static void progman_install(const char *info_exe, const char *inst_exe, const char *ico)
{
    DWORD id = 0; void *hsz, *conv; char buf[512];
    if (DdeInitializeA(&id, dde_cb, APPCLASS_STANDARD, 0) != DMLERR_NO_ERROR) {
        outs("note: DDE init failed; no Program Manager group.\n"); return;
    }
    hsz  = DdeCreateStringHandleA(id, "PROGMAN", CP_ACP);
    conv = DdeConnect(id, hsz, hsz, 0);
    if (conv) {
        progman_cmd(conv, "[CreateGroup(WINSIMD)]");
        progman_cmd(conv, "[ShowGroup(WINSIMD,1)]");
        lstrcpyA(buf, "[AddItem(");  lstrcatA(buf, info_exe);
        lstrcatA(buf, ",WINSIMD Info)]");          progman_cmd(conv, buf);
        /* Uninstall item with our red-X icon:
           [AddItem(CmdLine,Name,IconPath,IconIndex)] */
        lstrcpyA(buf, "[AddItem(");  lstrcatA(buf, inst_exe);
        lstrcatA(buf, " remove,Uninstall WINSIMD,");
        lstrcatA(buf, ico); lstrcatA(buf, ",0)]");      progman_cmd(conv, buf);
        DdeDisconnect(conv);
        outs("created Program Manager group 'WINSIMD' (tool + uninstall, red-X icon).\n");
    } else {
        outs("note: could not reach Program Manager (PROGMAN).\n");
    }
    DdeFreeStringHandle(id, hsz);
    DdeUninitialize(id);
}

static void progman_remove(void)
{
    DWORD id = 0; void *hsz, *conv;
    if (DdeInitializeA(&id, dde_cb, APPCLASS_STANDARD, 0) != DMLERR_NO_ERROR) return;
    hsz  = DdeCreateStringHandleA(id, "PROGMAN", CP_ACP);
    conv = DdeConnect(id, hsz, hsz, 0);
    if (conv) {
        progman_cmd(conv, "[DeleteGroup(WINSIMD)]");
        DdeDisconnect(conv);
        outs("removed Program Manager group 'WINSIMD'.\n");
    }
    DdeFreeStringHandle(id, hsz);
    DdeUninitialize(id);
}

/* ---- Explorer shells: Start-Menu .lnk via IShellLink + Add/Remove entry ---- */
static void make_lnk(const char *target, const char *args, const char *lnkpath)
{
    ISL *sl = 0; IPF *pf = 0; WCHAR w[300]; int i;
    if (p_CoCreateInstance(&CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER,
                           &IID_IShellLinkA, (void**)&sl) != 0 || !sl) return;
    sl->v->SetPath(sl, target);
    if (args && args[0]) sl->v->SetArguments(sl, args);
    sl->v->SetDescription(sl, "WINSIMD");
    if (sl->v->QueryInterface(sl, &IID_IPersistFile, (void**)&pf) == 0 && pf) {
        for (i = 0; lnkpath[i] && i < 299; i++) w[i] = (WCHAR)(unsigned char)lnkpath[i];
        w[i] = 0;
        pf->v->Save(pf, w, 1);
        pf->v->Release(pf);
    }
    sl->v->Release(sl);
}

/* Bind ole32 at runtime. Returns the module handle (FreeLibrary later) or 0. */
static HANDLE load_ole32(void)
{
    HANDLE h = LoadLibraryA("ole32.dll");
    if (!h) return 0;
    p_CoInitialize     = (CoInitialize_t)    GetProcAddress(h, "CoInitialize");
    p_CoUninitialize   = (CoUninitialize_t)  GetProcAddress(h, "CoUninitialize");
    p_CoCreateInstance = (CoCreateInstance_t)GetProcAddress(h, "CoCreateInstance");
    if (!p_CoInitialize || !p_CoUninitialize || !p_CoCreateInstance) {
        FreeLibrary(h); return 0;
    }
    return h;
}

static void explorer_install(const char *info_exe)
{
    char progs[300], lnk[320]; HANDLE ole;
    if (!programs_dir(progs)) { outs("note: no Start-Menu folder found.\n"); return; }
    ole = load_ole32();
    if (!ole) { outs("note: ole32 unavailable; no Start-Menu shortcut.\n"); return; }
    p_CoInitialize(0);
    lstrcpyA(lnk, progs); lstrcatA(lnk, "\\WINSIMD Info.lnk");
    make_lnk(info_exe, 0, lnk);
    p_CoUninitialize();
    FreeLibrary(ole);
    outs("created Start-Menu shortcut 'WINSIMD Info'.\n");
}

static void addremove_install(const char *inst_exe)
{
    HKEY k; DWORD disp; char cmd[320];
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, ADDREMOVE_KEY, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &k, &disp) != 0) return;
    RegSetValueExA(k, "DisplayName", 0, REG_SZ, "WINSIMD SIMD driver",
                   lstrlenA("WINSIMD SIMD driver") + 1);
    lstrcpyA(cmd, inst_exe); lstrcatA(cmd, " remove");
    RegSetValueExA(k, "UninstallString", 0, REG_SZ, cmd, lstrlenA(cmd) + 1);
    RegSetValueExA(k, "DisplayIcon", 0, REG_SZ, inst_exe, lstrlenA(inst_exe) + 1);
    RegSetValueExA(k, "Publisher", 0, REG_SZ, "WINSIMD", lstrlenA("WINSIMD") + 1);
    RegCloseKey(k);
    outs("registered in Add/Remove Programs.\n");
}

static void explorer_remove(void)
{
    char progs[300], lnk[320];
    if (programs_dir(progs)) {
        lstrcpyA(lnk, progs); lstrcatA(lnk, "\\WINSIMD Info.lnk");
        DeleteFileA(lnk);
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, ADDREMOVE_KEY);
    outs("removed Start-Menu shortcut and Add/Remove entry.\n");
}

/* Public: called after a successful driver/VxD install, and on remove. */
static void install_shortcuts(void)
{
    char dir[300], info[320], inst[320], ico[320];
    stage_tools(dir, info, inst, ico);
    if (nt_major() == 3) progman_install(info, inst, ico);     /* Program Manager  */
    else { explorer_install(info); addremove_install(inst); }  /* Explorer+Add/Rem */
}

static void remove_shortcuts(void)
{
    char dir[300], info[320], inst[320];
    winsimd_dir(dir);
    lstrcpyA(info, dir); lstrcatA(info, "\\WINSINFO.EXE");
    lstrcpyA(inst, dir); lstrcatA(inst, "\\WINSIMD.EXE");
    if (nt_major() == 3) progman_remove();
    else explorer_remove();
    DeleteFileA(info);
    lstrcpyA(info, dir); lstrcatA(info, "\\UNINST.ICO"); DeleteFileA(info);
    /* leave inst (WINSIMD.EXE) in place: it is the running uninstaller. */
    (void)inst;
}
