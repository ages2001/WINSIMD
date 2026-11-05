/*
 * simdmt.c - multi-threaded SIMD concurrency stress test (CRT-free, Win64/Win32).
 *
 * Spawns N threads that each hold a unique signature in vector registers across
 * a long spin and count how often another thread's state leaked in across a
 * context switch (see simdmt_x64.asm). This measures, empirically, whether
 * concurrent SIMD is safe on the running OS:
 *   - sse_probe mismatches == 0  => XMM is preserved across switches
 *   - avx_probe mismatches  > 0  => YMM upper halves are NOT preserved
 *
 *   simdmt          -> AVX probe (default)
 *   simdmt /sse     -> SSE (XMM) probe
 *   simdmt /n=8     -> thread count
 * Result is shown in a message box (title has PASS/FAIL) and written to
 * C:\SIMDMT.TXT.
 */
typedef unsigned int   U32;
typedef unsigned long long U64;
typedef unsigned long  DWORD;
typedef void          *HANDLE;
typedef const char    *LPCSTR;
typedef char          *LPSTR;
typedef int            BOOL;
int lstrlenA_(const char *s);

#if defined(_M_X64) || defined(__x86_64__)
#  define WApi
#  define IMP(n) __asm__(#n)
typedef U64 UPTR;
#else
#  define WApi __stdcall
#  define IMP(n) __asm__("_" #n)
typedef U32 UPTR;
#endif

/* kernel32 */
HANDLE WApi CreateThread(void*,UPTR,void*,void*,DWORD,DWORD*)   IMP(CreateThread);
DWORD  WApi WaitForSingleObject(HANDLE,DWORD)                   IMP(WaitForSingleObject);
void   WApi Sleep(DWORD)                                        IMP(Sleep);
DWORD  WApi GetTickCount(void)                                  IMP(GetTickCount);
LPSTR  WApi GetCommandLineA(void)                               IMP(GetCommandLineA);
HANDLE WApi CreateFileA(LPCSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE) IMP(CreateFileA);
BOOL   WApi WriteFile(HANDLE,const void*,DWORD,DWORD*,void*)    IMP(WriteFile);
BOOL   WApi CloseHandle(HANDLE)                                 IMP(CloseHandle);
void   WApi ExitProcess(U32)                                   IMP(ExitProcess);
/* user32 */
int    WApi MessageBoxA(HANDLE,LPCSTR,LPCSTR,U32)               IMP(MessageBoxA);
int         wsprintfA(LPSTR,LPCSTR,...)                         IMP(wsprintfA);

U32 avx_probe(U32 sig, U64 iters);   /* asm */
U32 sse_probe(U32 sig, U64 iters);   /* asm */
U32 avx_usable(void);                /* asm: CPUID OSXSAVE&AVX */

#define MAXT 16
static U64 g_iters = 60000000ull;
static int g_sse;
static int g_fellback;
static U32 g_mis[MAXT];
static U32 g_sig[MAXT];

static DWORD WApi worker(void *p)
{
    int i = (int)(UPTR)p;
    g_mis[i] = g_sse ? sse_probe(g_sig[i], g_iters) : avx_probe(g_sig[i], g_iters);
    return 0;
}

static int has(const char *s, const char *n)
{ for (; *s; s++){const char*a=s,*b=n; while(*b&&*a==*b){a++;b++;} if(!*b)return 1;} return 0; }

void WApi simdmt_start(void)
{
    char *cmd = GetCommandLineA();
    HANDLE th[MAXT]; int n = 6, i; U32 total = 0;
    char buf[512], f[160]; DWORD w;
    HANDLE fh;

    g_sse = has(cmd, "/sse") || has(cmd, "/SSE");
    if (has(cmd, "/n=")) { const char*p=cmd; while(*p && !(p[0]=='/'&&p[1]=='n'&&p[2]=='=')) p++; p+=3; n=0; while(*p>='0'&&*p<='9') n=n*10+(*p++-'0'); }
    if (n < 2) n = 2; if (n > MAXT) n = MAXT;

    /* A well-behaved AVX app checks CPUID first. In the driver's safe mode AVX
       is gated (OSXSAVE off), so we fall back to SSE instead of #UD-crashing. */
    if (!g_sse && !avx_usable()) g_fellback = 1, g_sse = 1;

    for (i = 0; i < n; i++) { g_sig[i] = (U32)(i + 1); g_mis[i] = 0; }
    for (i = 0; i < n; i++) th[i] = CreateThread(0,0,(void*)worker,(void*)(UPTR)i,0,0);
    for (i = 0; i < n; i++) WaitForSingleObject(th[i], 0xFFFFFFFFu);
    for (i = 0; i < n; i++) total += g_mis[i];

    wsprintfA(buf, "%s concurrency probe\r\n%sthreads=%d iters/thread=%u000000\r\n"
                   "total cross-thread corruption events = %u\r\n%s",
              g_sse ? "SSE (XMM)" : "AVX (YMM)",
              g_fellback ? "AVX gated by driver safe mode -> fell back to SSE\r\n" : "",
              n, (U32)(g_iters/1000000ull),
              total, total ? "=> state NOT preserved across context switches"
                           : "=> state preserved: concurrent SIMD is SAFE");
    wsprintfA(f, "%s: %s", g_fellback ? "AVX->SSE" : (g_sse ? "SSE" : "AVX"),
              total ? "FAIL (corruption)" : "PASS (safe)");

    fh = CreateFileA("C:\\SIMDMT.TXT", 0x40000000u, 0, 0, 2, 0x80, 0);
    if (fh != (HANDLE)(UPTR)-1) { WriteFile(fh, buf, (DWORD)0 + lstrlenA_(buf), &w, 0); CloseHandle(fh); }
    MessageBoxA(0, buf, f, 0);
    ExitProcess(total);
}

/* tiny strlen (avoid importing another symbol) */
int lstrlenA_(const char *s){int n=0;while(s[n])n++;return n;}
