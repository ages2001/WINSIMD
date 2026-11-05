#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
/* XMMSTRS.EXE [seconds] [nosound] - XMM context-preservation stress test.
   Three threads keep distinct patterns in XMM0-XMM6 and verify them while
   spinning (no system calls inside the checked region), so any change can
   only come from a context switch or interrupt that does not preserve XMM.
   A fourth thread keeps waveOut busy (audio interrupts/DPCs) unless
   "nosound" is given. Appends the result to XMMSTR6.TXT next to the EXE.
   Requires an SSE2 CPU with OS/driver XMM support enabled. No CRT. */
#define NT 6
static volatile LONG stop;static LONG fails[NT],checks[NT];
static __declspec(align(16)) BYTE pats[NT][7][16];
static DWORD WINAPI worker(LPVOID p){int k=(int)(ULONG_PTR)p;BYTE*pt=&pats[k][0][0];
 while(!stop){int bad;
  __asm__ volatile(
   "movdqu 0(%1),%%xmm0\n movdqu 16(%1),%%xmm1\n movdqu 32(%1),%%xmm2\n movdqu 48(%1),%%xmm3\n"
   "movdqu 64(%1),%%xmm4\n movdqu 80(%1),%%xmm5\n movdqu 96(%1),%%xmm6\n"
   "xorl %0,%0\n movl $64,%%edx\n"
   "1: movl $30000,%%ecx\n 2: decl %%ecx\n jnz 2b\n"
   "movdqa %%xmm0,%%xmm7\n pcmpeqb 0(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm1,%%xmm7\n pcmpeqb 16(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm2,%%xmm7\n pcmpeqb 32(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm3,%%xmm7\n pcmpeqb 48(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm4,%%xmm7\n pcmpeqb 64(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm5,%%xmm7\n pcmpeqb 80(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "movdqa %%xmm6,%%xmm7\n pcmpeqb 96(%1),%%xmm7\n pmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   "decl %%edx\n jnz 1b\n jmp 4f\n"
   "3: movl $1,%0\n 4:\n"
   :"=&r"(bad):"r"(pt):"eax","ecx","edx","memory","cc");
  InterlockedIncrement(&checks[k]);if(bad)InterlockedIncrement(&fails[k]);}
 return 0;}
static DWORD WINAPI sound(LPVOID p){HWAVEOUT w;WAVEFORMATEX f;static WAVEHDR h[4];static short buf[4][4096*2];int i,j;DWORD t=0;
 f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=44100;f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=44100*4;f.cbSize=0;
 if(waveOutOpen(&w,WAVE_MAPPER,&f,0,0,CALLBACK_NULL))return 1;
 for(i=0;i<4;i++){h[i].lpData=(LPSTR)buf[i];h[i].dwBufferLength=sizeof(buf[i]);h[i].dwFlags=0;waveOutPrepareHeader(w,&h[i],sizeof(WAVEHDR));
  for(j=0;j<4096*2;j++)buf[i][j]=(short)((t++*37)&0x0fff);waveOutWrite(w,&h[i],sizeof(WAVEHDR));}
 while(!stop){for(i=0;i<4;i++)if(h[i].dwFlags&WHDR_DONE){for(j=0;j<4096*2;j++)buf[i][j]=(short)((t++*37)&0x0fff);waveOutWrite(w,&h[i],sizeof(WAVEHDR));}Sleep(10);}
 waveOutReset(w);for(i=0;i<4;i++)waveOutUnprepareHeader(w,&h[i],sizeof(WAVEHDR));waveOutClose(w);return 0;}
void WINAPI entry(void){char*s=GetCommandLineA(),line[400],path[MAX_PATH];DWORD secs=120,i,n,k;HANDLE th[NT+1];int nosound=0,j;HANDLE f;DWORD id;
 if(*s=='"'){s++;while(*s&&*s!='"')s++;if(*s)s++;}else while(*s&&*s!=' ')s++;
 while(*s==' ')s++;if(*s>='0'&&*s<='9'){secs=0;while(*s>='0'&&*s<='9')secs=secs*10+(*s++-'0');}
 while(*s==' ')s++;if(*s=='n')nosound=1;
 for(k=0;k<NT;k++)for(i=0;i<7;i++)for(j=0;j<16;j++)pats[k][i][j]=(BYTE)(0x11*(k+1)+i*16+j*3+0x5a);
 for(k=0;k<NT;k++)th[k]=CreateThread(0,0,worker,(LPVOID)(ULONG_PTR)k,0,&id);
 th[NT]=nosound?0:CreateThread(0,0,sound,0,0,&id);
 Sleep(secs*1000);stop=1;WaitForMultipleObjects(NT,th,TRUE,10000);if(th[NT])WaitForSingleObject(th[NT],5000);
 {LONG c=0,fl=0;for(k=0;k<NT;k++){c+=checks[k];fl+=fails[k];}wsprintfA(line,"XMMSTR6 %lus 6 threads version %08lX: checks %ld failures %ld\r\n",secs,GetVersion(),c,fl);}
 n=GetModuleFileNameA(0,path,MAX_PATH);while(n&&path[n-1]!='\\')n--;lstrcpyA(path+n,"XMMSTR6.TXT");
 f=CreateFileA(path,GENERIC_WRITE,0,0,OPEN_ALWAYS,FILE_FLAG_WRITE_THROUGH,0);if(f!=INVALID_HANDLE_VALUE){SetFilePointer(f,0,0,FILE_END);WriteFile(f,line,lstrlenA(line),&n,0);CloseHandle(f);}
 MessageBoxA(0,line,"XMMSTR6",MB_OK);ExitProcess(0);}
