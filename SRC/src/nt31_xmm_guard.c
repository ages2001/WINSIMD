/* NT 3.1 UP/MP research adapter. NOT the completed WINSIMD 1.2 release.
   Exact kernel validation, demand-load only, no unload. A bounded state table
   deliberately bugchecks on exhaustion instead of silently corrupting state.
   Other kernel builds must use their own validated switch adapters. */
typedef unsigned long U32;
typedef unsigned char U8;
typedef long STATUS;
extern __declspec(dllimport) U32 __cdecl DbgPrint(const char *,...);
extern __declspec(dllimport) U8 __stdcall MmIsAddressValid(void *);
extern __declspec(dllimport) void __stdcall KeBugCheckEx(U32,U32,U32,U32,U32);
extern void __cdecl guard_save(void *);
extern void __cdecl guard_restore(void *);
extern U32 __cdecl guard_install(void *target);
extern U32 __cdecl guard_read_cr4(void);
extern U32 __cdecl guard_has_sse2(void);
extern U32 __cdecl guard_cpu_number(void);
extern void guard_hook(void);
U32 guard_resume;
static volatile U32 table_lock;
typedef struct Slot {U32 thread;U32 xmm[32];U32 mxcsr;} Slot;
static Slot slots[512];
U32 guard_metrics[5];
#define active guard_metrics[0]
#define peak guard_metrics[1]
#define switches guard_metrics[2]
#define retired guard_metrics[3]
static void debug_char(U8 c){__asm__ volatile("outb %0,$0xe9"::"a"(c));}
static void debug_address(void){const char*s="XMMGUARD metrics=";int i;U32 address=(U32)guard_metrics;while(*s)debug_char(*s++);for(i=28;i>=0;i-=4)debug_char("0123456789abcdef"[(address>>i)&15]);debug_char('\n');}
static void lock_table(void){U32 old;do{old=1;__asm__ volatile("xchgl %0,%1":"+r"(old),"+m"(table_lock)::"memory");}while(old);}
static void unlock_table(void){__asm__ volatile("":::"memory");table_lock=0;}
static Slot *lookup(U32 thread,int create){U32 i;Slot *empty=0;for(i=0;i<512;i++){if(slots[i].thread==thread)return &slots[i];if(!slots[i].thread&&!empty)empty=&slots[i];}if(!create)return 0;if(!empty)KeBugCheckEx(0xe0120001,active,thread,switches,retired);empty->thread=thread;for(i=0;i<32;i++)empty->xmm[i]=0;empty->mxcsr=0x1f80;if(++active>peak)peak=active;return empty;}
void __cdecl guard_switch_state(U32 old_thread,U32 new_thread){Slot *old,*next;lock_table();switches++;
 guard_metrics[4]|=1u<<guard_cpu_number();
 /* KTHREAD.State==Terminated is verified for this exact kernel only. */
 if(*(U8*)(old_thread+0x1ce)==4){old=lookup(old_thread,0);if(old){old->thread=0;active--;retired++;}}
 else {old=lookup(old_thread,1);guard_save(old->xmm);}
 next=lookup(new_thread,1);guard_restore(next->xmm);unlock_table();
}
static U8 *kernel_base(void){U32 p=((U32)&DbgPrint)&~4095u,i;for(i=0;i<512;i++,p-=4096){U8*b=(U8*)p;U32 e;if(!MmIsAddressValid(b))continue;if(b[0]!='M'||b[1]!='Z')continue;e=*(U32*)(b+0x3c);if(e<0x1000&&*(U32*)(b+e)==0x4550)return b;}return 0;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void *exported(U8 *base,const char *name){U32 pe=*(U32*)(base+0x3c),rva=*(U32*)(base+pe+0x78);U8*e=base+rva;U32 n=*(U32*)(e+24),i,*names=(U32*)(base+*(U32*)(e+32)),*functions=(U32*)(base+*(U32*)(e+28));unsigned short *ordinals=(void*)(base+*(U32*)(e+36));for(i=0;i<n;i++)if(eq((char*)(base+names[i]),name))return base+functions[ordinals[i]];return 0;}
#ifdef GUARD_QUERY
extern long __cdecl guard_query_init(void *);
extern void __cdecl guard_query_abort(void);
extern void __cdecl guard_query_activate(U32,U32);
#endif
static STATUS start_guard(void *driver,void *registry){U8 *base=kernel_base(),*cpus,*target;U32 pe,i;static const U8 expected[6]={0xff,0x82,0x84,0x01,0x00,0x00};(void)driver;(void)registry;
 if(!base)return (STATUS)0xc00000bbu;pe=*(U32*)(base+0x3c);
 if((U32)base!=0x80100000u)return (STATUS)0xc00000bbu;
 if(*(U32*)(base+pe+8)==0x2c921d20u)target=base+0x5e420;
 else if(*(U32*)(base+pe+8)==0x2c51c08au)target=base+0x604b0;
 else return (STATUS)0xc00000bbu;
 cpus=exported(base,"KeNumberProcessors");if(!cpus||*cpus<1||*cpus>8)return (STATUS)0xc00000bbu;
 if(!guard_has_sse2())return (STATUS)0xc00000bbu;
 for(i=0;i<6;i++)if(target[i]!=expected[i])return (STATUS)0xc00000bbu;
 if(((U32)target&7)||target[6]!=0xff||target[7]!=0x05)return (STATUS)0xc00000bbu;
 guard_resume=(U32)target+6;
 if(!guard_install(target))return (STATUS)0xc0000001u;
#ifdef GUARD_QUERY
 guard_query_activate(*cpus,*(U32*)(base+pe+8));
#endif
 debug_address();
 DbgPrint("WINSIMD NT31 XMM research adapter installed: CPUs=%u, 512 live-thread slots.\n",*cpus);
 return 0;
}
STATUS __stdcall DriverEntry(void *driver,void *registry){STATUS status;
#ifdef GUARD_QUERY
 status=guard_query_init(driver);if(status<0)return status;
#endif
 status=start_guard(driver,registry);
#ifdef GUARD_QUERY
 if(status<0)guard_query_abort();
#endif
 return status;
}
