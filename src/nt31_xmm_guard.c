/* WINSIMD vector context-switch guard (research adapter).
   Preserves the SIMD register file (XMM / YMM / ZMM + opmask, width chosen from
   the CPU) per thread across context switches and CPU migration, for NT kernels
   whose scheduler does not itself save that state (NT 3.1/3.5x/4.0).

   GENERIC, not one kernel: the adapter discovers the running kernel at run time
   and only hooks a build it can positively identify AND byte-validate; on any
   unrecognised kernel it refuses cleanly (no patch, no crash). Each supported
   build is one validated row in kbuilds[]: the RVA of the per-thread
   context-switch-count increment (`inc dword [reg+disp32]`, the hook site), the
   pushad-frame offsets of the registers holding the outgoing/incoming thread at
   that point, and the KTHREAD.State offset used to reclaim a terminated thread's
   slot. The displaced instruction is re-executed from a generated trampoline, so
   its register/offset need not be hard-coded. Adding an OS = adding one row.

   Demand-load only, no unload. A bounded 512-entry table deliberately bugchecks
   on exhaustion rather than silently corrupting state. x86 only; x64 (eager FPU)
   and 9x (VxD) need their own adapters. */
typedef unsigned long U32;
typedef unsigned char U8;
typedef long STATUS;
extern __declspec(dllimport) U32 __cdecl DbgPrint(const char *,...);
extern __declspec(dllimport) U8 __stdcall MmIsAddressValid(void *);
extern __declspec(dllimport) void __stdcall KeBugCheckEx(U32,U32,U32,U32,U32);
extern void __cdecl guard_save(void *);
extern void __cdecl guard_restore(void *);
extern U32 __cdecl guard_install(void *target);
extern U32 __cdecl guard_has_sse2(void);
extern U32 __cdecl guard_detect_width(U32 *xcr0_out);
extern U32 __cdecl guard_cpu_number(void);
extern void guard_hook(void);

/* set for the asm hook / save-restore before the site is patched */
U32 guard_resume;        /* address of the generated trampoline */
U32 guard_width;         /* 0 = XMM, 1 = YMM, 2 = ZMM+opmask */
U32 guard_mode;          /* 0 = full register, 1 = upper-only (OS preserves the
                            low 128 bits + MXCSR itself; guard keeps only the
                            YMM upper halves it does not know about)           */
U32 guard_xcr0;          /* XCR0 to program when width >= 1 */
U32 guard_new_off;       /* pushad-frame byte offset of the incoming-thread reg */
static U32 state_off;    /* KTHREAD.State offset for the terminated check */

static U8 trampoline[32];

/* pushad pushes EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI; after it the frame holds
   [+0]EDI [+4]ESI [+8]EBP [+12]ESP [+16]EBX [+20]EDX [+24]ECX [+28]EAX. */
enum { OFF_EDI=0, OFF_ESI=4, OFF_EBP=8, OFF_EBX=16, OFF_EDX=20, OFF_ECX=24, OFF_EAX=28 };

/* A kernel "layout" is identified by the exact bytes of the per-thread
   context-switch-count increment inside the scheduler (`inc dword [reg+disp]`,
   where disp is KTHREAD.ContextSwitches and reg holds the INCOMING thread), plus
   as many following instruction bytes as are needed to make the displaced region
   at least 5 bytes long (so a 5-byte jmp fits). The bytes encode the KTHREAD
   offset and incoming-thread register, so they are unique to one KTHREAD layout.
   The guard SCANS the running kernel image for the bytes: exactly one match -> it
   is the hook site (works on any kernel sharing the layout and base, e.g. NT 3.1
   UP and MP, with no per-build RVA); zero or several -> refuse.

   The guard tracks the OUTGOING thread itself (per-CPU last-switched-to thread),
   so only the INCOMING-thread register has to be reliable at the hook site. This
   matters on kernels (e.g. NT 5.0) where the outgoing-thread register has already
   been reused by the time of the increment.

   A different NT KTHREAD layout -> add a validated row here. mode selects whether
   the OS already preserves the low 128 bits (upper-only) or not (full). */
typedef struct Layout {
    U8  pat[12];     /* displaced bytes: the increment, then enough to reach >=5 */
    U32 patlen;      /* how many of pat[] are valid (also the displacement size) */
    U32 new_off;     /* pushad offset of incoming-thread register */
    U32 state_off;   /* KTHREAD.State byte offset (==4 means Terminated)         */
    U32 mode;        /* 0 = full register save, 1 = upper-only (OS keeps low)    */
} Layout;

static const Layout layouts[] = {
    /* NT 3.1 UP/MP: inc dword[edx+0x184]; new=edx; scheduler saves no SIMD state
       at all, so the guard owns the whole register file. */
    { {0xff,0x82,0x84,0x01,0x00,0x00}, 6, OFF_EDX, 0x1ceu, 0 },
    /* NT 5.0 (Windows 2000) UP/MP: inc dword[esi+0x4c]; new=esi. 3 bytes, so the
       following inc dword[ebx+0x5c0] (KPRCB switch count) is displaced too. The
       NT 5.0 scheduler preserves XMM + MXCSR via lazy FP, so the guard only keeps
       the YMM upper halves (upper-only) to avoid disturbing the OS's XMM state. */
    { {0xff,0x46,0x4c,0xff,0x83,0xc0,0x05,0x00,0x00}, 9, OFF_ESI, 0x2du, 1 },
    /* NT 5.1 (Windows XP x86) UP/PAE: inc dword[esi+0x4c]; new=esi, same as NT 5.0
       but the following KPRCB switch count is inc dword[ebx+0x61c], so the 9
       displaced bytes differ in the KPRCB offset. Upper-only (XP preserves XMM +
       MXCSR via fxsave/fxrstor). KTHREAD.State is still 0x2d. */
    { {0xff,0x46,0x4c,0xff,0x83,0x1c,0x06,0x00,0x00}, 9, OFF_ESI, 0x2du, 1 },
};

U32 guard_resume_unused;
static volatile U32 table_lock;
typedef struct Slot { U32 thread; U32 vec[128]; U32 opmask[16]; U32 mxcsr; } Slot;
static Slot slots[512];
static U32 cpu_current[64];   /* per-CPU: the thread this CPU last switched to   */
U32 guard_metrics[5];
#define active guard_metrics[0]
#define peak guard_metrics[1]
#define switches guard_metrics[2]
#define retired guard_metrics[3]

static void debug_char(U8 c){__asm__ volatile("outb %0,$0xe9"::"a"(c));}
static void debug_string(const char*s){while(*s)debug_char((U8)*s++);}
static void debug_hex(U32 v){int i;for(i=28;i>=0;i-=4)debug_char("0123456789abcdef"[(v>>i)&15]);}
static void lock_table(void){U32 old;do{old=1;__asm__ volatile("xchgl %0,%1":"+r"(old),"+m"(table_lock)::"memory");}while(old);}
static void unlock_table(void){__asm__ volatile("":::"memory");table_lock=0;}
static Slot *lookup(U32 thread,int create){U32 i;Slot *empty=0;for(i=0;i<512;i++){if(slots[i].thread==thread)return &slots[i];if(!slots[i].thread&&!empty)empty=&slots[i];}if(!create)return 0;if(!empty)KeBugCheckEx(0xe0120001,active,thread,switches,retired);empty->thread=thread;for(i=0;i<128;i++)empty->vec[i]=0;for(i=0;i<16;i++)empty->opmask[i]=0;empty->mxcsr=0x1f80;if(++active>peak)peak=active;return empty;}

/* Called from the hook with only the INCOMING thread. The outgoing thread is the
   one this CPU last switched to (tracked here), so no outgoing-thread register is
   needed. Save the outgoing thread's live vector state, restore the incoming
   thread's, and record the incoming thread as this CPU's current. */
void __cdecl guard_switch_state(U32 new_thread){
 U32 cpu=guard_cpu_number()&63u; U32 old_thread=cpu_current[cpu]; Slot *old,*next;
 if(old_thread==new_thread) return;            /* same thread: nothing to do */
 lock_table(); switches++; guard_metrics[4]|=1u<<cpu;
 if(old_thread){
   if(*(U8*)(old_thread+state_off)==4){old=lookup(old_thread,0);if(old){old->thread=0;active--;retired++;}}
   else {old=lookup(old_thread,1);guard_save(old->vec);}
 }
 next=lookup(new_thread,1);guard_restore(next->vec);
 cpu_current[cpu]=new_thread;
 unlock_table();
}

static U8 *kernel_base(void){U32 p=((U32)&DbgPrint)&~4095u,i;for(i=0;i<1024;i++,p-=4096){U8*b=(U8*)p;U32 e;if(!MmIsAddressValid(b))continue;if(b[0]!='M'||b[1]!='Z')continue;e=*(U32*)(b+0x3c);if(e<0x1000&&*(U32*)(b+e)==0x4550)return b;}return 0;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void *exported(U8 *base,const char *name){U32 pe=*(U32*)(base+0x3c),rva=*(U32*)(base+pe+0x78);U8*e=base+rva;U32 n=*(U32*)(e+24),i,*names=(U32*)(base+*(U32*)(e+32)),*functions=(U32*)(base+*(U32*)(e+28));unsigned short *ordinals=(void*)(base+*(U32*)(e+36));for(i=0;i<n;i++)if(eq((char*)(base+names[i]),name))return base+functions[ordinals[i]];return 0;}

/* Scan [base, base+SizeOfImage) for a byte pattern, page-validated. Returns the
   match address if it occurs EXACTLY once, else 0 (count returned via *n). */
static U8 *scan_unique(U8 *base,U32 size,const U8 *pat,U32 patlen,U32 *n){
 U32 i,j;U8 *hit=0;*n=0;
 for(i=0;i+patlen<=size;i++){
  if((i&0xfff)==0&&!MmIsAddressValid(base+i)){i+=0xfff;continue;}
  for(j=0;j<patlen;j++)if(base[i+j]!=pat[j])break;
  if(j==patlen){if(++*n==1)hit=base+i;}
 }
 return (*n==1)?hit:0;
}

STATUS __stdcall DriverEntry(void *driver,void *registry){
 U8 *base=kernel_base(),*cpus,*target=0;U32 pe,i,size,n,rel;const Layout *ly=0;
 (void)driver;(void)registry;
 debug_char('G');
 if(!base){debug_string("no-base\n");return (STATUS)0xc00000bbu;}
 debug_char('B');
 pe=*(U32*)(base+0x3c);size=*(U32*)(base+pe+0x50);           /* SizeOfImage */
 if(size<0x1000||size>0x2000000)return (STATUS)0xc00000bbu;
 /* Identify the kernel by locating a known scheduler increment uniquely. */
 for(i=0;i<sizeof(layouts)/sizeof(layouts[0]);i++){
  target=scan_unique(base,size,layouts[i].pat,layouts[i].patlen,&n);
  if(target){ly=&layouts[i];break;}
 }
 debug_char('S');debug_hex(n);
 if(!ly||!target){debug_string(" no-site\n");return (STATUS)0xc00000bbu;}  /* unknown/ambiguous: refuse */
 cpus=exported(base,"KeNumberProcessors");if(!cpus||*cpus<1||*cpus>32){debug_string(" no-cpus\n");return (STATUS)0xc00000bbu;}
 if(!guard_has_sse2())return (STATUS)0xc00000bbu;
 guard_width=guard_detect_width(&guard_xcr0);
 guard_mode=ly->mode;guard_new_off=ly->new_off;state_off=ly->state_off;
 /* Upper-only kernels already preserve the low 128 bits + MXCSR; if the CPU has
    no AVX there is nothing above that for the guard to keep, so do not patch. */
 if(guard_mode==1&&guard_width==0){debug_string(" nop-noavx\n");return (STATUS)0xc00000bbu;}
 /* Trampoline: the displaced instruction, then jmp back past it. */
 for(i=0;i<ly->patlen;i++)trampoline[i]=target[i];
 trampoline[ly->patlen]=0xe9;rel=(U32)(target+ly->patlen)-((U32)&trampoline[ly->patlen]+5);
 trampoline[ly->patlen+1]=(U8)rel;trampoline[ly->patlen+2]=(U8)(rel>>8);
 trampoline[ly->patlen+3]=(U8)(rel>>16);trampoline[ly->patlen+4]=(U8)(rel>>24);
 guard_resume=(U32)trampoline;
 if(!guard_install(target))return (STATUS)0xc0000001u;
 debug_string("XMMGUARD width=");debug_hex(guard_width);debug_string(" mode=");debug_hex(guard_mode);debug_string(" site=");debug_hex((U32)(target-base));debug_string(" cpus=");debug_hex(*cpus);debug_char('\n');
 DbgPrint("WINSIMD vector guard installed: CPUs=%u, width=%u, mode=%u, 512 slots.\n",*cpus,guard_width,guard_mode);
 return 0;
}
