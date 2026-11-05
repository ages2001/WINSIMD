/* Runtime correctness test: hold all eight XMM registers and MXCSR across real
   preemption; the controller migrates workers when multiple CPUs are available.
   No OS calls occur inside the assembly measurement interval. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct Result {DWORD xmm,mxcsr,x87;} Result;
static Result results[4];
static HANDLE ready;
static HANDLE logh;
static DWORD iterations=200000000;
static DWORD_PTR(WINAPI*native_affinity)(HANDLE,DWORD_PTR);
static LONG(WINAPI*nt_set_thread)(HANDLE,DWORD,void*,DWORD);
static LONG(WINAPI*nt_query_thread)(HANDLE,DWORD,void*,DWORD,DWORD*);
static DWORD_PTR WINAPI pin_thread(HANDLE thread,DWORD_PTR mask){
 DWORD basic[7],length=0;
 if(native_affinity)return native_affinity(thread,mask);
 if(!nt_set_thread||!nt_query_thread||nt_set_thread(thread,4,&mask,sizeof(mask))<0)return 0;
 if(nt_query_thread(thread,0,basic,sizeof(basic),&length)<0||basic[4]!=(DWORD)mask)return 0;
 return 1;
}
static void line(const char *s,DWORD value){char text[128],digits[12];DWORD n=0,k=0,w;while(*s)text[n++]=*s++;text[n++]=' ';do{digits[k++]='0'+value%10;value/=10;}while(value);while(k)text[n++]=digits[--k];text[n++]='\r';text[n++]='\n';WriteFile(logh,text,n,&w,0);FlushFileBuffers(logh);}
extern void __cdecl legacy_xmm_probe(DWORD seed,DWORD iterations,Result *result,DWORD mxcsr);
static DWORD WINAPI worker(void *context){DWORD id=(DWORD)context;WaitForSingleObject(ready,INFINITE);legacy_xmm_probe(0x12340000u+id,iterations,&results[id],0x1f80u|(id<<13));return 0;}
void WINAPI entry(void){HANDLE threads[4];SYSTEM_INFO system;DWORD i,id,start,wait,step=0,failures=0,status;DWORD_PTR(WINAPI*affinity)(HANDLE,DWORD_PTR);
 logh=CreateFileA("C:\\FP32\\XMMSAVE.LOG",GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,0,0);
 for(i=0;i<sizeof(system);i++)((BYTE*)&system)[i]=0;GetSystemInfo(&system);line("Logical processors",system.dwNumberOfProcessors);
 native_affinity=(void*)GetProcAddress(GetModuleHandleA("KERNEL32.DLL"),"SetThreadAffinityMask");
 nt_set_thread=(void*)GetProcAddress(GetModuleHandleA("NTDLL.DLL"),"NtSetInformationThread");
 nt_query_thread=(void*)GetProcAddress(GetModuleHandleA("NTDLL.DLL"),"NtQueryInformationThread");
 affinity=(native_affinity||(nt_set_thread&&nt_query_thread))?pin_thread:0;
 line("Affinity API",native_affinity?1:affinity?2:0);
 if(system.dwNumberOfProcessors>1)iterations=2000000000;
 ready=CreateEventA(0,TRUE,FALSE,0);if(!ready){line("Start event error",GetLastError());ExitProcess(2);}
 for(i=0;i<4;i++){threads[i]=CreateThread(0,0,worker,(void*)i,0,&id);if(!threads[i]){line("CreateThread error",GetLastError());ExitProcess(2);}}
 start=GetTickCount();SetEvent(ready);
 while((wait=WaitForMultipleObjects(4,threads,TRUE,50))==WAIT_TIMEOUT){
  if(system.dwNumberOfProcessors>1&&affinity){for(i=0;i<4;i++)if(WaitForSingleObject(threads[i],0)==WAIT_TIMEOUT&&!affinity(threads[i],(DWORD_PTR)1<<((i+step)%system.dwNumberOfProcessors))){if(!GetExitCodeThread(threads[i],&status)||status==STILL_ACTIVE)failures++;}step++;}
  if(GetTickCount()-start>60000){line("Watchdog timeout",60);ExitProcess(3);}
 }
 line("Elapsed milliseconds",GetTickCount()-start);line("Wait result",wait);line("Affinity rounds",step);line("Affinity errors",failures);
 /* Early NT may return the final signaled object's index even for wait-all. */
 if(wait>=WAIT_OBJECT_0+4)failures++;
 for(i=0;i<4;i++){line("Worker",i);line("XMM mismatches",results[i].xmm);line("MXCSR mismatches",results[i].mxcsr);line("x87 mismatches",results[i].x87);if(!GetExitCodeThread(threads[i],&status)||status)failures++;failures+=results[i].xmm+results[i].mxcsr+results[i].x87;CloseHandle(threads[i]);}
 line("Total failures",failures);CloseHandle(ready);CloseHandle(logh);ExitProcess(failures?1:0);
}
