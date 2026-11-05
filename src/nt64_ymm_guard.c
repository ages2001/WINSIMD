/* WINSIMD x64 vector context-switch guard (research adapter), NT 5.2 x64
   (Windows XP x64 / Server 2003 x64).

   x64 Windows saves XMM0-15 + MXCSR itself (fxsave/fxrstor in SwapContext), but
   not the AVX YMM upper halves, so concurrent AVX threads corrupt each other's
   YMM state across a context switch. This guard preserves ONLY the YMM upper
   halves per thread (upper-only mode), leaving the OS's XMM state untouched.

   It discovers the running kernel the same way the x86 guard does: scan for the
   unique bytes of a known SwapContext site and refuse unless they occur exactly
   once. The outgoing thread is tracked per-CPU (only the incoming thread, RSI,
   is read at the hook). See src/NOTES-xp-x64.md for the reverse-engineering.

   IMPORTANT: x64 Windows ships Kernel Patch Protection (PatchGuard), which
   bugchecks (0x109) on a patched SwapContext. The system must be booted with the
   kernel debugger enabled (boot.ini /debug) so PatchGuard does not arm; the
   installer sets that. This driver does not fight PatchGuard. */
typedef unsigned long long U64;
typedef unsigned int       U32;
typedef unsigned short     U16;
typedef unsigned char      U8;
typedef long               STATUS;

/* __declspec(dllimport) is REQUIRED: without it &DbgPrint would be the driver's
   own import thunk, so kernel_base() would walk down into the driver instead of
   ntoskrnl. With dllimport, &DbgPrint is the real kernel address (via __imp_). */
extern __declspec(dllimport) U32 __cdecl DbgPrint(const char *,...);
extern __declspec(dllimport) U8  MmIsAddressValid(void *);
extern __declspec(dllimport) void KeBugCheckEx(U32,U64,U64,U64,U64);
extern __declspec(dllimport) void *ExAllocatePoolWithTag(U32,U64,U32);

/* asm helpers */
extern void guard64_save_upper(void *buf);     /* vextractf128 ymm0-15 -> buf[16*16] */
extern void guard64_restore_upper(void *buf);
extern int  guard64_has_avx(void);
extern U64  guard64_hook;                       /* the asm hook entry (label)       */
extern U64  guard64_resume;                     /* set: trampoline address          */
extern U32  guard64_new_off;                    /* unused placeholder (RSI fixed)    */
extern void guard64_install(U8 *site,U8 *tramp,U64 hook); /* UP-safe 14-byte patch  */

/* set before patching */
U64 guard64_resume;
static U64 state_off = 0xb8;    /* KTHREAD.State (Terminated==4) */

/* SwapContext hook site: 16 unique bytes at the KPRCB.ContextSwitches increment
   (inc dword[rbx+0xdf0]) + mov rax,[rbx-0x170] + test rax,rax. RSI = incoming. */
static const U8 PAT[16] = {0xff,0x83,0xf0,0x0d,0x00,0x00,
                           0x48,0x8b,0x83,0x90,0xfe,0xff,0xff,
                           0x48,0x85,0xc0};
#define PATLEN 16

typedef struct Slot { U64 thread; U8 upper[16*16]; } Slot;  /* 16 ymm-high halves */
#define NSLOTS 2048
static Slot slots[NSLOTS];
static U32  slot_rr;                 /* round-robin eviction cursor */
static U64  cpu_current[64];
static volatile long table_lock;
U32 guard64_metrics[4];   /* active, peak, switches, cpumask */

static void dbg_e9(U8 c){__asm__ volatile("outb %0,$0xe9"::"a"(c));}
static void dbg_s(const char*s){while(*s)dbg_e9((U8)*s++);}
static void dbg_hex(U64 v){int i;for(i=60;i>=0;i-=4)dbg_e9("0123456789abcdef"[(v>>i)&15]);}

static void lock_t(void){for(;;){if(!__sync_lock_test_and_set(&table_lock,1))return;}}
static void unlock_t(void){__sync_lock_release(&table_lock);}

static Slot *lookup(U64 thread,int create){
 U32 i;Slot*empty=0;
 for(i=0;i<NSLOTS;i++){if(slots[i].thread==thread)return &slots[i];if(!slots[i].thread&&!empty)empty=&slots[i];}
 if(!create)return 0;
 /* On a full table, evict round-robin instead of bugchecking: the evicted
    thread merely loses its saved YMM upper (restored as zero next time), which
    is a benign correctness loss, never a crash. */
 if(!empty){empty=&slots[slot_rr];slot_rr=(slot_rr+1)%NSLOTS;}
 else if(++guard64_metrics[0]>guard64_metrics[1])guard64_metrics[1]=guard64_metrics[0];
 empty->thread=thread;for(i=0;i<16*16;i++)empty->upper[i]=0;
 return empty;
}

/* Called from the asm hook with (incoming_thread, cpu_number). */
void guard64_switch(U64 new_thread,U32 cpu){
 U64 old_thread;Slot*old,*next;
 cpu&=63u; old_thread=cpu_current[cpu];
 if(old_thread==new_thread)return;
 lock_t(); guard64_metrics[2]++; guard64_metrics[3]|=(cpu<32)?(1u<<cpu):0;
 if(old_thread && MmIsAddressValid((void*)(old_thread+state_off))){
   if(*(U8*)(old_thread+state_off)==4){old=lookup(old_thread,0);if(old){old->thread=0;guard64_metrics[0]--;}}
   else {old=lookup(old_thread,1);guard64_save_upper(old->upper);}
 }
 next=lookup(new_thread,1);guard64_restore_upper(next->upper);
 cpu_current[cpu]=new_thread;
 unlock_t();
}

static U8 *kernel_base(void){
 U64 p=((U64)&DbgPrint)&~0xfffull;U32 i;
 for(i=0;i<0x4000;i++,p-=0x1000){U8*b=(U8*)p;U32 e;
  if(!MmIsAddressValid(b))continue;
  if(b[0]!='M'||b[1]!='Z')continue;
  e=*(U32*)(b+0x3c);if(e<0x1000&&*(U32*)(b+e)==0x4550)return b;}
 return 0;
}
static U8 *scan_unique(U8*base,U64 size,U32*n){
 U64 i;U8*hit=0;U32 j;*n=0;
 for(i=0;i+PATLEN<=size;i++){
  if((i&0xfff)==0&&!MmIsAddressValid(base+i)){i+=0xfff;continue;}
  for(j=0;j<PATLEN;j++)if(base[i+j]!=PAT[j])break;
  if(j==PATLEN){if(++*n==1)hit=base+i;}
 }
 return (*n==1)?hit:0;
}

static U8 trampoline_store[64];

STATUS DriverEntry(void *drv,void *reg){
 U8 *base,*site;U32 n,i;U8 *tramp;U64 relsz;
 (void)drv;(void)reg;
 dbg_e9('G');
 base=kernel_base();
 if(!base){dbg_s(" x64 no-base\n");return (STATUS)0xc00000bb;}
 dbg_s("B base=");dbg_hex((U64)base);
 { U32 e=*(U32*)(base+0x3c);U64 size=*(U32*)(base+e+0x50);
   if(size<0x1000||size>0x4000000)return (STATUS)0xc00000bb;
   site=scan_unique(base,size,&n);
 }
 dbg_e9('S');dbg_hex(n);
 if(!site){dbg_s(" x64 no-site\n");return (STATUS)0xc00000bb;}
 if(!guard64_has_avx()){dbg_s(" x64 no-avx\n");return (STATUS)0xc00000bb;}
 /* executable trampoline: 16 displaced bytes + abs jmp back to site+16. */
 tramp=ExAllocatePoolWithTag(0,64,'GMYW');
 if(!tramp)tramp=trampoline_store;
 for(i=0;i<PATLEN;i++)tramp[i]=site[i];
 tramp[PATLEN]=0xff;tramp[PATLEN+1]=0x25;*(U32*)(tramp+PATLEN+2)=0;
 *(U64*)(tramp+PATLEN+6)=(U64)(site+PATLEN);
 guard64_resume=(U64)tramp;
 (void)relsz;
 guard64_install(site,tramp,(U64)&guard64_hook);
 dbg_s("YMMGUARD64 siteabs=");dbg_hex((U64)site);dbg_s(" rva=");dbg_hex((U64)(site-base));dbg_s(" tramp=");dbg_hex((U64)tramp);dbg_e9('\n');
 DbgPrint("WINSIMD x64 YMM guard installed (upper-only), 512 slots.\n");
 return 0;
}
