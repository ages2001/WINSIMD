#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
/* YMMSTRS.EXE [seconds] [nosound] - YMM (AVX) context-preservation stress test.
   Three threads keep distinct patterns in XMM0-XMM6 and verify them while
   spinning (no system calls inside the checked region), so any change can
   only come from a context switch or interrupt that does not preserve XMM.
   A fourth thread keeps waveOut busy (audio interrupts/DPCs) unless
   "nosound" is given. Appends the result to YMMSTRS.TXT next to the EXE.
   Requires an SSE2 CPU with OS/driver XMM support enabled. No CRT. */
#define NT 3
static volatile LONG stop;static LONG fails[NT],checks[NT];
static __declspec(align(32)) BYTE pats[NT][7][32];
static DWORD WINAPI worker(LPVOID p){int k=(int)(ULONG_PTR)p;BYTE*pt=&pats[k][0][0];
 while(!stop){int bad;
  __asm__ volatile(
   "vmovdqu 0(%1),%%ymm0\n vmovdqu 32(%1),%%ymm1\n vmovdqu 64(%1),%%ymm2\n vmovdqu 96(%1),%%ymm3\n"
   "vmovdqu 128(%1),%%ymm4\n vmovdqu 160(%1),%%ymm5\n vmovdqu 192(%1),%%ymm6\n"
   "xorl %0,%0\n movl $64,%%edx\n"
   "1: movl $30000,%%ecx\n 2: decl %%ecx\n jnz 2b\n"
#define CHK(r,o) "vextractf128 $1,%%ymm" #r ",%%xmm7\n vpcmpeqb " #o "+16(%1),%%xmm7,%%xmm7\n vpmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n" \
                 "vpcmpeqb " #o "(%1),%%xmm" #r ",%%xmm7\n vpmovmskb %%xmm7,%%eax\n cmpl $0xffff,%%eax\n jne 3f\n"
   CHK(0,0) CHK(1,32) CHK(2,64) CHK(3,96) CHK(4,128) CHK(5,160) CHK(6,192)
   "decl %%edx\n jnz 1b\n jmp 4f\n"
   "3: movl $1,%0\n 4: vzeroupper\n"
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
 for(k=0;k<NT;k++)for(i=0;i<7;i++)for(j=0;j<32;j++)pats[k][i][j]=(BYTE)(0x11*(k+1)+i*32+j*7+0x5a);
 for(k=0;k<NT;k++)th[k]=CreateThread(0,0,worker,(LPVOID)(ULONG_PTR)k,0,&id);
 th[NT]=nosound?0:CreateThread(0,0,sound,0,0,&id);
 Sleep(secs*1000);stop=1;WaitForMultipleObjects(NT,th,TRUE,10000);if(th[NT])WaitForSingleObject(th[NT],5000);
 wsprintfA(line,"YMMSTRS %lus sound=%s version %08lX: checks %ld/%ld/%ld failures %ld/%ld/%ld\r\n",secs,nosound?"off":"on",GetVersion(),checks[0],checks[1],checks[2],fails[0],fails[1],fails[2]);
 n=GetModuleFileNameA(0,path,MAX_PATH);while(n&&path[n-1]!='\\')n--;lstrcpyA(path+n,"YMMSTRS.TXT");
 f=CreateFileA(path,GENERIC_WRITE,0,0,OPEN_ALWAYS,FILE_FLAG_WRITE_THROUGH,0);if(f!=INVALID_HANDLE_VALUE){SetFilePointer(f,0,0,FILE_END);WriteFile(f,line,lstrlenA(line),&n,0);CloseHandle(f);}
 MessageBoxA(0,line,"YMMSTRS",MB_OK);ExitProcess(0);}
