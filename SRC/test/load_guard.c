#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
static HANDLE logh;
static void result(const char *name,DWORD value){char b[140],digits[12];DWORD n=0,k=0,w;while(*name)b[n++]=*name++;b[n++]=' ';do{digits[k++]='0'+value%10;value/=10;}while(value);while(k)b[n++]=digits[--k];b[n++]='\r';b[n++]='\n';WriteFile(logh,b,n,&w,0);FlushFileBuffers(logh);}
void WINAPI entry(void){SC_HANDLE manager,service;SERVICE_STATUS status;char dest[MAX_PATH];DWORD n,i,error;
 logh=CreateFileA("C:\\FP32\\GUARD.LOG",GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,0,0);
 n=GetWindowsDirectoryA(dest,sizeof(dest));{const char*s="\\System32\\drivers\\XMMGUARD.SYS";while(*s&&n<MAX_PATH-1)dest[n++]=*s++;dest[n]=0;}
 manager=OpenSCManagerA(0,0,SC_MANAGER_ALL_ACCESS);if(!manager){result("SCM error",GetLastError());ExitProcess(2);}
 service=OpenServiceA(manager,"XMMGUARD",SERVICE_ALL_ACCESS);
 if(service){
  if(!QueryServiceStatus(service,&status)){result("Query error",GetLastError());ExitProcess(3);}
  if(status.dwCurrentState!=SERVICE_STOPPED){result("Existing service state",status.dwCurrentState);result("Reboot required before replacement",1);CloseServiceHandle(service);CloseServiceHandle(manager);CloseHandle(logh);ExitProcess(5);}
 }else if(GetLastError()!=ERROR_SERVICE_DOES_NOT_EXIST){result("Open service error",GetLastError());ExitProcess(3);}
 if(!CopyFileA("C:\\FP32\\XMMGUARD.SYS",dest,FALSE)){result("Copy error",GetLastError());ExitProcess(1);}
 if(!service)
 service=CreateServiceA(manager,"XMMGUARD","WINSIMD NT31 XMM research adapter",SERVICE_ALL_ACCESS,SERVICE_KERNEL_DRIVER,SERVICE_DEMAND_START,SERVICE_ERROR_NORMAL,"\\SystemRoot\\System32\\drivers\\XMMGUARD.SYS",0,0,0,0,0);
 if(!service){result("Service error",GetLastError());ExitProcess(3);}
 error=StartServiceA(service,0,0)?0:GetLastError();result("Start error",error);
 for(i=0;i<50;i++){if(!QueryServiceStatus(service,&status)){result("Query error",GetLastError());ExitProcess(3);}if(status.dwCurrentState!=SERVICE_START_PENDING)break;Sleep(100);}
 result("Service state",status.dwCurrentState);result("Win32 exit",status.dwWin32ExitCode);result("Service exit",status.dwServiceSpecificExitCode);
 CloseServiceHandle(service);CloseServiceHandle(manager);CloseHandle(logh);ExitProcess(status.dwCurrentState==SERVICE_RUNNING?0:4);
}
