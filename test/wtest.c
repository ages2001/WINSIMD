/* WINSIMD native-behavior probe (CRT-free). Runs the same 4-worker XMM/YMM
   migration workload as VTEST but does NOT install the guard: it measures what
   the running OS preserves across context switches and CPU migration by itself.
   Writes C:\FP32\WRESULT.LOG. Built for NT 3.1+ (x86). */
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
void   STDCALL GetSystemInfo(void*)                                     IMP(GetSystemInfo);
void   STDCALL Sleep(DWORD)                                             IMP(Sleep);
void  *STDCALL GetProcAddress(HANDLE,LPCSTR)                            IMP(GetProcAddress);
HANDLE STDCALL GetModuleHandleA(LPCSTR)                                 IMP(GetModuleHandleA);
BOOL   STDCALL GetExitCodeThread(HANDLE,DWORD*)                         IMP(GetExitCodeThread);
void   STDCALL ExitProcess(DWORD)                                       IMP(ExitProcess);

typedef DWORD (STDCALL *affmask_t)(HANDLE,DWORD);
typedef long  (STDCALL *ntset_t)(HANDLE,DWORD,void*,DWORD);
typedef long  (STDCALL *ntquery_t)(HANDLE,DWORD,void*,DWORD,DWORD*);

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
static U32 g_phase;
static U32 g_iters = 200000000u;
static affmask_t aff;
static ntset_t ntset; static ntquery_t ntquery;

static void line(const char*s,DWORD v){char b[128],d[12];int n=0,k=0;DWORD w;while(*s)b[n++]=*s++;b[n++]=' ';do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)b[n++]=d[--k];b[n++]='\r';b[n++]='\n';WriteFile(logh,b,(DWORD)n,&w,0);FlushFileBuffers(logh);}

static DWORD pin(HANDLE th,DWORD mask){
    DWORD basic[7],len=0;
    if(aff)return aff(th,mask);
    if(!ntset||!ntquery||ntset(th,4,&mask,sizeof(mask))<0)return 0;
    if(ntquery(th,0,basic,sizeof(basic),&len)<0||basic[4]!=mask)return 0;
    return 1;
}

static DWORD STDCALL worker(void*ctx){
    U32 id=(U32)(UPTR)ctx;
    WaitForSingleObject(ready,INFINITE);
    if(g_phase==1){
        if(!cpu_avx_usable()){results[id][0]=0;results[id][1]=0;results[id][2]=0;return 2;}
        ymm_probe(0x12340000u+id,g_iters,results[id],0x1f80u|(id<<13));
    } else {
        xmm_probe(0x12340000u+id,g_iters,results[id],0x1f80u|(id<<13));
    }
    return 0;
}

static DWORD run_phase(DWORD nproc,U32 phase){
    HANDLE th[4];DWORD i,id,start,wait,step=0,fail=0,st;
    g_phase=phase;
    for(i=0;i<4;i++){results[i][0]=results[i][1]=results[i][2]=0;}
    ready=CreateEventA(0,1,0,0);
    for(i=0;i<4;i++)th[i]=CreateThread(0,0,worker,(void*)(UPTR)i,0,&id);
    start=GetTickCount();SetEvent(ready);
    while((wait=WaitForMultipleObjects(4,th,1,50))==WAIT_TIMEOUT){
        if(nproc>1&&(aff||ntset)){for(i=0;i<4;i++)if(WaitForSingleObject(th[i],0)==WAIT_TIMEOUT)pin(th[i],(DWORD)1<<((i+step)%nproc));step++;}
        if(GetTickCount()-start>90000){line("Watchdog",phase);break;}
    }
    line(phase?"YMM phase ms":"XMM phase ms",GetTickCount()-start);
    line("Affinity rounds",step);
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

void STDCALL wtest_start(void){
    DWORD si[16];DWORD nproc;U32 avx;
    logh=CreateFileA("C:\\FP32\\WRESULT.LOG",GENERIC_WRITE,1,0,CREATE_ALWAYS,0,0);
    line("WTEST native-behavior probe (no guard installed)",0);
    { int i;for(i=0;i<16;i++)si[i]=0; } GetSystemInfo(si); nproc=si[5];
    line("Logical processors",nproc);
    aff=(affmask_t)GetProcAddress(GetModuleHandleA("KERNEL32.DLL"),"SetThreadAffinityMask");
    ntset=(ntset_t)GetProcAddress(GetModuleHandleA("NTDLL.DLL"),"NtSetInformationThread");
    ntquery=(ntquery_t)GetProcAddress(GetModuleHandleA("NTDLL.DLL"),"NtQueryInformationThread");
    line("Affinity API",aff?1:(ntset&&ntquery)?2:0);
    avx=cpu_avx_usable();
    line("AVX usable",avx);
    if(nproc>1)g_iters=2000000000u;
    run_phase(nproc,0);                 /* XMM: does the OS preserve it? */
    if(avx)run_phase(nproc,1);          /* YMM: does the OS preserve it? */
    else line("YMM phase skipped (no AVX)",0);
    line("Done",0);
    CloseHandle(logh);
    ExitProcess(0);
}
