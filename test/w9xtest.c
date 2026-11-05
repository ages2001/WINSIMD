/* w9xtest.c - WINSIMD vector-guard verifier for Windows 9x (95/98/ME), CRT-free.
 *
 * Unlike VTEST (NT), this does NOT install anything: on 9x the guard is the
 * static VxD loaded at boot. It just runs the concurrency workload and reports.
 * Four threads each hold a unique signature in XMM0-7 (then YMM0-7 on AVX) plus
 * MXCSR and x87 across a long spin, counting any state that leaked across a
 * context switch. 9x is uniprocessor, so plain preemption (no affinity) drives
 * the switches. Results go to C:\W9XTEST.LOG and a PASS/FAIL message box.
 *
 *   - XMM / YMM / MXCSR mismatches == 0  => the VxD guard preserved them.
 *   - x87 mismatches == 0                => VMCPD preserved x87 (it always does).
 */
typedef unsigned int   U32;
typedef unsigned long  DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef const char    *LPCSTR;
typedef char          *LPSTR;
typedef U32            UPTR;
#define STDCALL __stdcall
#define IMP(n) __asm__("_" #n)
#define NULL ((void*)0)

/* kernel32 */
HANDLE STDCALL CreateFileA(LPCSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE) IMP(CreateFileA);
BOOL   STDCALL WriteFile(HANDLE,const void*,DWORD,DWORD*,void*)         IMP(WriteFile);
BOOL   STDCALL FlushFileBuffers(HANDLE)                                 IMP(FlushFileBuffers);
BOOL   STDCALL CloseHandle(HANDLE)                                      IMP(CloseHandle);
HANDLE STDCALL CreateThread(void*,UPTR,void*,void*,DWORD,DWORD*)        IMP(CreateThread);
DWORD  STDCALL WaitForMultipleObjects(DWORD,const HANDLE*,BOOL,DWORD)   IMP(WaitForMultipleObjects);
DWORD  STDCALL WaitForSingleObject(HANDLE,DWORD)                        IMP(WaitForSingleObject);
HANDLE STDCALL CreateEventA(void*,BOOL,BOOL,LPCSTR)                     IMP(CreateEventA);
BOOL   STDCALL SetEvent(HANDLE)                                         IMP(SetEvent);
DWORD  STDCALL GetTickCount(void)                                       IMP(GetTickCount);
void   STDCALL Sleep(DWORD)                                             IMP(Sleep);
BOOL   STDCALL GetExitCodeThread(HANDLE,DWORD*)                         IMP(GetExitCodeThread);
void   STDCALL ExitProcess(DWORD)                                       IMP(ExitProcess);
/* user32 */
int    STDCALL MessageBoxA(HANDLE,LPCSTR,LPCSTR,U32)                    IMP(MessageBoxA);

extern U32  __cdecl cpu_avx_usable(void);
extern void __cdecl xmm_probe(U32 seed,U32 iters,U32 *result,U32 mxcsr);
extern void __cdecl ymm_probe(U32 seed,U32 iters,U32 *result,U32 mxcsr);

#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define INFINITE      0xffffffffu
#define WAIT_TIMEOUT  258u

static HANDLE logh;
static HANDLE ready;
static U32 results[4][3];
static U32 g_phase;              /* 0 = XMM, 1 = YMM */
static U32 g_iters = 300000000u;

static void line(const char*s,DWORD v){char b[128],d[12];int n=0,k=0;DWORD w;while(*s)b[n++]=*s++;b[n++]=' ';do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)b[n++]=d[--k];b[n++]='\r';b[n++]='\n';if(logh)WriteFile(logh,b,(DWORD)n,&w,0);if(logh)FlushFileBuffers(logh);}

static DWORD STDCALL worker(void*ctx){
    U32 id=(U32)(UPTR)ctx;
    WaitForSingleObject(ready,INFINITE);
    if(g_phase==1){
        if(!cpu_avx_usable()){results[id][0]=results[id][1]=results[id][2]=0;return 2;}
        ymm_probe(0x12340000u+id,g_iters,results[id],0x1f80u|(id<<13));
    } else {
        xmm_probe(0x12340000u+id,g_iters,results[id],0x1f80u|(id<<13));
    }
    return 0;
}

static DWORD run_phase(U32 phase){
    HANDLE th[4];DWORD i,id,start,st,fail=0;
    g_phase=phase;
    for(i=0;i<4;i++)results[i][0]=results[i][1]=results[i][2]=0;
    ready=CreateEventA(0,1,0,0);
    for(i=0;i<4;i++)th[i]=CreateThread(0,0,worker,(void*)(UPTR)i,0,&id);
    start=GetTickCount();SetEvent(ready);
    while(WaitForMultipleObjects(4,th,1,100)==WAIT_TIMEOUT){
        if(GetTickCount()-start>120000){line("Watchdog",phase);break;}
    }
    line(phase?"YMM phase ms":"XMM phase ms",GetTickCount()-start);
    for(i=0;i<4;i++){
        line("Worker",i);
        line(phase?"YMM mismatches":"XMM mismatches",results[i][0]);
        line("MXCSR mismatches",results[i][1]);
        line("x87 mismatches",results[i][2]);
        if(GetExitCodeThread(th[i],&st)&&st==2)line("note worker skipped (no AVX)",i);
        fail+=results[i][0]+results[i][1]+results[i][2];
        CloseHandle(th[i]);
    }
    CloseHandle(ready);
    line(phase?"YMM total failures":"XMM total failures",fail);
    return fail;
}

void STDCALL w9xtest_start(void){
    U32 avx,fx=0,fy=0; char msg[160]; int n=0; const char*s;
    logh=CreateFileA("C:\\W9XTEST.LOG",GENERIC_WRITE,1,0,CREATE_ALWAYS,0,0);
    avx=cpu_avx_usable();
    line("W9XTEST start, AVX usable",avx);
    fx=run_phase(0);                 /* XMM + MXCSR */
    if(avx)fy=run_phase(1);          /* YMM + MXCSR */
    else line("YMM phase skipped (no AVX)",0);
    line("Done",0);
    if(logh)CloseHandle(logh);
    /* message box summary */
    s=(fx==0&&(!avx||fy==0))?"WINSIMD 9x guard: PASS":"WINSIMD 9x guard: FAIL";
    while(*s)msg[n++]=*s++; msg[n++]='\n';
    s="XMM fail="; while(*s)msg[n++]=*s++; { U32 v=fx;char d[12];int k=0;do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)msg[n++]=d[--k]; }
    s="  YMM fail="; while(*s)msg[n++]=*s++; { U32 v=fy;char d[12];int k=0;do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)msg[n++]=d[--k]; }
    msg[n]=0;
    MessageBoxA(0,msg,(fx==0&&(!avx||fy==0))?"PASS":"FAIL",0);
    ExitProcess(fx+fy);
}
