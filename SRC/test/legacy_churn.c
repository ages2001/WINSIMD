/* Exercise terminated-thread state retirement over 640 worker lifetimes.
   This tests reuse, not 640 simultaneous threads or an unbounded capacity. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct Result {DWORD xmm,mxcsr,x87;} Result;
static Result results[16];
static HANDLE logh;
extern void __cdecl legacy_xmm_probe(DWORD,DWORD,Result *,DWORD);
static void line(const char *s,DWORD v){char b[128],d[12];DWORD n=0,k=0,w;while(*s)b[n++]=*s++;b[n++]=' ';do{d[k++]='0'+v%10;v/=10;}while(v);while(k)b[n++]=d[--k];b[n++]='\r';b[n++]='\n';WriteFile(logh,b,n,&w,0);FlushFileBuffers(logh);}
static DWORD WINAPI worker(void *context){DWORD id=(DWORD)context;legacy_xmm_probe(0x12340000+id,4000000,&results[id],0x1f80|((id&3)<<13));return 0;}
void WINAPI entry(void){HANDLE threads[16];DWORD batch,i,id,wait,status,fail=0,created=0;
 logh=CreateFileA("C:\\FP32\\XMMCHURN.LOG",GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,0,0);
 for(batch=0;batch<40;batch++){
  for(i=0;i<16;i++){results[i].xmm=results[i].mxcsr=results[i].x87=0;threads[i]=CreateThread(0,0,worker,(void*)i,0,&id);if(!threads[i]){line("Create error",GetLastError());ExitProcess(2);}created++;}
  wait=WaitForMultipleObjects(16,threads,TRUE,30000);
  if(wait>=WAIT_OBJECT_0+16){line("Wait failure",wait);ExitProcess(3);}
  for(i=0;i<16;i++){fail+=results[i].xmm+results[i].mxcsr+results[i].x87;if(!GetExitCodeThread(threads[i],&status)||status)fail++;CloseHandle(threads[i]);}
  line("Completed batch",batch+1);
 }
 line("Worker lifetimes",created);line("Total failures",fail);CloseHandle(logh);ExitProcess(fail?1:0);
}
