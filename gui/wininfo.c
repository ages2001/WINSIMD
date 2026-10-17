/*
 * wininfo.c - WINSINFO.EXE : SIMD feature viewer with a modern, iconified UI.
 * NT: reads the snapshot the driver published at
 *   HKLM\SYSTEM\CurrentControlSet\Services\winsimd\Status  (REG_BINARY)
 * Win9x: asks WINSIMD.VXD for the same ILF_CPU_INFO via DeviceIoControl.
 * and shows the CPU name and, per feature, whether it is supported and its
 * state (ENABLED by OS / by driver / absent / blocked), with coloured dots.
 * CRT-free Win32; uses only GDI available since NT 3.1.
 */
#include "ilf.h"

typedef unsigned int   UINT;
typedef unsigned long  DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef HANDLE HWND, HINSTANCE, HDC, HBRUSH, HICON, HCURSOR, HMENU, HGDIOBJ;
typedef HANDLE HFONT, HPEN, HKEY; typedef HKEY *PHKEY;
typedef const char *LPCSTR; typedef char *LPSTR;
typedef ILF_UPTR LONG_PTR, UINT_PTR; typedef long LONG;
typedef LONG_PTR LRESULT; typedef UINT_PTR WPARAM; typedef LONG_PTR LPARAM;
typedef DWORD COLORREF;

#if ILF_X64
#  define STDCALL
#  define IMP(n) __asm__(#n)
#else
#  define STDCALL __stdcall
#  define IMP(n) __asm__("_" #n)
#endif
typedef LRESULT (STDCALL *WNDPROC)(HWND,UINT,WPARAM,LPARAM);

typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra,cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCSTR lpszMenuName, lpszClassName; } WNDCLASSA;
typedef struct { ILF_U32 x,y; } POINT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam;
    DWORD time; POINT pt; } MSG;
typedef struct { LONG left,top,right,bottom; } RECT;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore,fIncUpdate;
    unsigned char rgbReserved[32]; } PAINTSTRUCT;

/* kernel32 */
HINSTANCE STDCALL GetModuleHandleA(LPCSTR)   IMP(GetModuleHandleA);
void STDCALL ExitProcess(UINT)               IMP(ExitProcess);
DWORD STDCALL GetVersion(void)               IMP(GetVersion);
HANDLE STDCALL CreateFileA(LPCSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE) IMP(CreateFileA);
BOOL STDCALL DeviceIoControl(HANDLE,DWORD,void*,DWORD,void*,DWORD,DWORD*,void*) IMP(DeviceIoControl);
BOOL STDCALL CloseHandle(HANDLE)             IMP(CloseHandle);
/* user32 */
HCURSOR STDCALL LoadCursorA(HINSTANCE,LPCSTR)              IMP(LoadCursorA);
HICON STDCALL LoadIconA(HINSTANCE,LPCSTR)                 IMP(LoadIconA);
ILF_U16 STDCALL RegisterClassA(const WNDCLASSA*)          IMP(RegisterClassA);
HWND STDCALL CreateWindowExA(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,void*) IMP(CreateWindowExA);
BOOL STDCALL ShowWindow(HWND,int)                         IMP(ShowWindow);
BOOL STDCALL UpdateWindow(HWND)                           IMP(UpdateWindow);
BOOL STDCALL GetMessageA(MSG*,HWND,UINT,UINT)             IMP(GetMessageA);
BOOL STDCALL TranslateMessage(const MSG*)                 IMP(TranslateMessage);
LRESULT STDCALL DispatchMessageA(const MSG*)              IMP(DispatchMessageA);
LRESULT STDCALL DefWindowProcA(HWND,UINT,WPARAM,LPARAM)   IMP(DefWindowProcA);
void STDCALL PostQuitMessage(int)                         IMP(PostQuitMessage);
HDC STDCALL BeginPaint(HWND,PAINTSTRUCT*)                 IMP(BeginPaint);
BOOL STDCALL EndPaint(HWND,const PAINTSTRUCT*)            IMP(EndPaint);
int STDCALL FillRect(HDC,const RECT*,HBRUSH)              IMP(FillRect);
BOOL STDCALL GetClientRect(HWND,RECT*)                    IMP(GetClientRect);
/* gdi32 */
BOOL STDCALL TextOutA(HDC,int,int,LPCSTR,int)             IMP(TextOutA);
int STDCALL SetBkMode(HDC,int)                            IMP(SetBkMode);
COLORREF STDCALL SetTextColor(HDC,COLORREF)               IMP(SetTextColor);
HGDIOBJ STDCALL GetStockObject(int)                       IMP(GetStockObject);
HGDIOBJ STDCALL SelectObject(HDC,HGDIOBJ)                 IMP(SelectObject);
BOOL STDCALL DeleteObject(HGDIOBJ)                        IMP(DeleteObject);
HBRUSH STDCALL CreateSolidBrush(COLORREF)                 IMP(CreateSolidBrush);
HPEN STDCALL CreatePen(int,int,COLORREF)                  IMP(CreatePen);
BOOL STDCALL Ellipse(HDC,int,int,int,int)                 IMP(Ellipse);
BOOL STDCALL Rectangle(HDC,int,int,int,int)               IMP(Rectangle);
int STDCALL GetDeviceCaps(HDC,int)                        IMP(GetDeviceCaps);
HFONT STDCALL CreateFontA(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR) IMP(CreateFontA);
/* advapi32 */
LONG STDCALL RegOpenKeyExA(HKEY,LPCSTR,DWORD,DWORD,PHKEY) IMP(RegOpenKeyExA);
LONG STDCALL RegQueryValueExA(HKEY,LPCSTR,DWORD*,DWORD*,void*,DWORD*) IMP(RegQueryValueExA);
LONG STDCALL RegCloseKey(HKEY)                            IMP(RegCloseKey);

#define WS_OVERLAPPED       0x00000000
#define WS_CAPTION          0x00C00000
#define WS_SYSMENU          0x00080000
#define WS_MINIMIZEBOX      0x00020000
#define WS_VISIBLE          0x10000000
#define CW_USEDEFAULT       ((int)0x80000000)
#define SW_SHOW             5
#define WM_PAINT            0x000F
#define WM_DESTROY          0x0002
#define TRANSPARENT         1
#define BITSPIXEL           12
#define PLANES              14
#define FW_NORMAL           400
#define FW_BOLD             700
#define DEFAULT_CHARSET     1
#define ANSI_FIXED_FONT     11
#define PS_SOLID            0
#define HKEY_LOCAL_MACHINE  ((HKEY)(UINT_PTR)0x80000002u)
#define KEY_READ            0x20019u
#define IDI_APPLICATION     ((LPCSTR)(UINT_PTR)32512)
#define IDC_ARROW           ((LPCSTR)(UINT_PTR)32512)
#define RGB(r,g,b)          ((COLORREF)((unsigned char)(r)|((unsigned char)(g)<<8)|((unsigned char)(b)<<16)))

static ILF_CPU_INFO g_ci;
static int g_have;
static int g_9x;

static const char *HX="0123456789abcdef";
static void hx8(char *o,ILF_U32 v){int i;for(i=0;i<8;i++){o[7-i]=HX[v&0xF];v>>=4;}o[8]=0;}
static int scat(char *d,int p,const char *s){while(*s)d[p++]=*s++;d[p]=0;return p;}
static int slen(const char *s){int n=0;while(s[n])n++;return n;}

#define OPEN_EXISTING 3
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
static void load_status_9x(void){
    DWORD n=0;
    HANDLE h=CreateFileA("\\\\.\\WINSIMD",0,0,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE||!h) return;
    if(DeviceIoControl(h,ILF_IOCTL_GET_STATUS,NULL,0,&g_ci,sizeof(g_ci),&n,NULL)
       && n==sizeof(g_ci) && g_ci.brand[0]) g_have=1;
    CloseHandle(h);
}

static void load_status(void){
    HKEY k; DWORD sz=sizeof(g_ci), ty=0; g_have=0;
    g_9x=(GetVersion()&0x80000000u)!=0;
    if(g_9x){ load_status_9x(); return; }
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SYSTEM\\CurrentControlSet\\Services\\winsimd",0,KEY_READ,&k)!=0) return;
    if(RegQueryValueExA(k,"Status",0,&ty,&g_ci,&sz)==0 && sz>=4) g_have=1;
    RegCloseKey(k);
}

struct FROW{const char*name;ILF_U32 bit;int grp;};
static const struct FROW rows[]={
 {"MMX",ILF_FEAT_MMX,0},{"SSE",ILF_FEAT_SSE,1},{"SSE2",ILF_FEAT_SSE2,1},
 {"SSE3",ILF_FEAT_SSE3,1},{"SSSE3",ILF_FEAT_SSSE3,1},{"SSE4.1",ILF_FEAT_SSE41,1},
 {"SSE4.2",ILF_FEAT_SSE42,1},{"AVX",ILF_FEAT_AVX,2},{"FMA",ILF_FEAT_FMA,2},
 {"AVX2",ILF_FEAT_AVX2,2},{"AVX-512F",ILF_FEAT_AVX512F,3},
};
static ILF_DISPOSITION gd(int g){switch(g){case 0:return g_ci.disp_mmx;case 1:return g_ci.disp_sse;case 2:return g_ci.disp_avx;default:return g_ci.disp_avx512;}}

/* palette: full-colour design, plus a VGA-16 fallback (pure colours, no
   dithering) used automatically on <= 256-colour displays (Win95/NT 3.1 VGA) */
enum { P_HDR, P_WIN, P_CARD, P_TXT, P_SUB, P_GREEN, P_BLUE, P_GRAY, P_RED,
       P_ROW, P_HSUB, P_CHIP, P_PIN, P_EDGE, P_CHIPTXT, P_N };
static const COLORREF pal_full[P_N] = {
    RGB(28,46,82), RGB(244,246,250), RGB(255,255,255), RGB(32,36,44), RGB(120,128,140),
    RGB(28,160,72), RGB(36,110,210), RGB(176,182,190), RGB(206,54,54),
    RGB(236,240,247), RGB(188,205,235), RGB(210,220,236), RGB(200,210,228),
    RGB(150,170,200), RGB(60,90,140) };
static const COLORREF pal_vga[P_N] = {
    RGB(0,0,128), RGB(192,192,192), RGB(255,255,255), RGB(0,0,0), RGB(128,128,128),
    RGB(0,128,0), RGB(0,0,255), RGB(128,128,128), RGB(255,0,0),
    RGB(255,255,255), RGB(192,192,192), RGB(192,192,192), RGB(192,192,192),
    RGB(128,128,128), RGB(0,0,128) };
static const COLORREF *g_pal = pal_full;
#define C_HDR    g_pal[P_HDR]
#define C_WIN    g_pal[P_WIN]
#define C_CARD   g_pal[P_CARD]
#define C_TXT    g_pal[P_TXT]
#define C_SUB    g_pal[P_SUB]
#define C_GREEN  g_pal[P_GREEN]
#define C_BLUE   g_pal[P_BLUE]
#define C_GRAY   g_pal[P_GRAY]
#define C_RED    g_pal[P_RED]

static void dot(HDC hdc,int x,int y,COLORREF c){
    HBRUSH b=CreateSolidBrush(c); HPEN pn=CreatePen(PS_SOLID,1,c);
    HGDIOBJ ob=SelectObject(hdc,b), op=SelectObject(hdc,pn);
    Ellipse(hdc,x,y,x+11,y+11);
    SelectObject(hdc,ob); SelectObject(hdc,op); DeleteObject(b); DeleteObject(pn);
}
static void draw_chip(HDC hdc,int x,int y){   /* little CPU-chip icon, ~34px */
    int i; HBRUSH body=CreateSolidBrush(g_pal[P_CHIP]);
    HPEN pin=CreatePen(PS_SOLID,2,g_pal[P_PIN]);
    HPEN edge=CreatePen(PS_SOLID,1,g_pal[P_EDGE]);
    HGDIOBJ op=SelectObject(hdc,pin);
    for(i=0;i<5;i++){ /* pins */
        int px=x+4+i*6;
        SelectObject(hdc,pin);
        Rectangle(hdc,px,y-3,px+2,y+1); Rectangle(hdc,px,y+30,px+2,y+34);
        Rectangle(hdc,x-3,y+4+i*6,x+1,y+6+i*6); Rectangle(hdc,x+30,y+4+i*6,x+34,y+6+i*6);
    }
    SelectObject(hdc,body); SelectObject(hdc,edge);
    Rectangle(hdc,x,y,x+31,y+31);
    SelectObject(hdc,GetStockObject(0)); SelectObject(hdc,op);
    DeleteObject(body); DeleteObject(pin); DeleteObject(edge);
    SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,g_pal[P_CHIPTXT]);
    TextOutA(hdc,x+6,y+10,"SIMD",4);
}

static void paint(HDC hdc,HWND hwnd){
    RECT rc;
    g_pal = (GetDeviceCaps(hdc,BITSPIXEL)*GetDeviceCaps(hdc,PLANES) <= 8) ? pal_vga : pal_full;
    GetClientRect(hwnd,&rc);
    int W=rc.right, i, y;
    HFONT fTitle=CreateFontA(26,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,"Arial");
    HFONT fSub  =CreateFontA(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,"MS Sans Serif");
    HFONT fName =CreateFontA(18,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,"Arial");
    HFONT fBody =CreateFontA(15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,"MS Sans Serif");
    HFONT fBold =CreateFontA(15,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,"MS Sans Serif");
    HBRUSH bg=CreateSolidBrush(C_WIN), hdr=CreateSolidBrush(C_HDR), card=CreateSolidBrush(C_CARD);
    RECT r;

    /* background */
    r=rc; FillRect(hdc,&r,bg);
    /* header band */
    r.left=0; r.top=0; r.right=W; r.bottom=62; FillRect(hdc,&r,hdr);
    SelectObject(hdc,fSub);              /* chip label uses the small font */
    draw_chip(hdc,16,15);
    SetBkMode(hdc,TRANSPARENT);
    SelectObject(hdc,fTitle); SetTextColor(hdc,RGB(255,255,255));
    TextOutA(hdc,62,10,"WINSIMD",7);
    SelectObject(hdc,fSub); SetTextColor(hdc,g_pal[P_HSUB]);
    {
        const char *sub = g_have && g_ci.safe_mode
            ? (g_9x ? "SIMD status  -  WINSIMD.VXD  -  SAFE MODE (AVX gated)"
                    : "SIMD status  -  WINSIMD.SYS  -  SAFE MODE (AVX gated)")
            : (g_9x ? "SIMD feature status  -  WINSIMD.VXD (Windows 9x)"
                    : "SIMD feature status  -  WINSIMD.SYS (Windows NT)");
        TextOutA(hdc,64,40,sub,slen(sub));
    }

    if(!g_have){
        SelectObject(hdc,fBody); SetTextColor(hdc,C_RED);
        const char*m="Driver not loaded - install WINSIMD.EXE and reboot.";
        TextOutA(hdc,16,80,m,slen(m));
        goto done;
    }

    /* CPU name card */
    r.left=12; r.top=74; r.right=W-12; r.bottom=104; FillRect(hdc,&r,card);
    SelectObject(hdc,fName); SetTextColor(hdc,C_TXT);
    TextOutA(hdc,20,78,g_ci.brand,slen(g_ci.brand));

    /* cr4/xcr0 line */
    {
        char l[96]; int p; char a[9],b2[9],xc[9];
        hx8(a,g_ci.cr4_before); hx8(b2,g_ci.cr4_after); hx8(xc,(ILF_U32)g_ci.xcr0);
        p=scat(l,0,"CR4 "); p=scat(l,p,a); p=scat(l,p," -> "); p=scat(l,p,b2);
        p=scat(l,p,"    XCR0 "); p=scat(l,p,xc);
        SelectObject(hdc,GetStockObject(ANSI_FIXED_FONT)); SetTextColor(hdc,C_SUB);
        TextOutA(hdc,20,110,l,p);
    }

    /* table header */
    y=138;
    SelectObject(hdc,fBold); SetTextColor(hdc,C_SUB);
    TextOutA(hdc,40,y,"Feature",7);
    TextOutA(hdc,160,y,"Supported",9);
    TextOutA(hdc,270,y,"State",5);
    y+=22;

    for(i=0;i<(int)(sizeof(rows)/sizeof(rows[0]));i++){
        int sup=(g_ci.features&rows[i].bit)!=0;
        ILF_DISPOSITION d=gd(rows[i].grp);
        COLORREF sc; const char*st;
        if(!sup){ sc=C_GRAY; st="not present"; }
        else if(g_ci.safe_mode && rows[i].grp>=2){ sc=C_SUB; st="off (safe mode)"; }
        else switch(d){
            case ILF_DISP_DRIVER_ENABLED: sc=C_GREEN; st="ENABLED (driver)"; break;
            case ILF_DISP_OS_MANAGED:     sc=C_BLUE;  st="ENABLED (OS)";     break;
            case ILF_DISP_BLOCKED:        sc=C_RED;   st="blocked";          break;
            default:                      sc=C_GRAY;  st="available";        break;
        }
        /* alternating row tint */
        if(i&1){ r.left=12; r.top=y-3; r.right=W-12; r.bottom=y+17;
                 HBRUSH t=CreateSolidBrush(g_pal[P_ROW]); FillRect(hdc,&r,t); DeleteObject(t); }
        SelectObject(hdc,fBody); SetTextColor(hdc,C_TXT);
        TextOutA(hdc,40,y,rows[i].name,slen(rows[i].name));
        dot(hdc,162,y+3, sup?C_GREEN:C_GRAY);
        SetTextColor(hdc, sup?C_TXT:C_SUB);
        TextOutA(hdc,180,y, sup?"yes":"no", sup?3:2);
        dot(hdc,272,y+3,sc);
        SetTextColor(hdc, sc);
        TextOutA(hdc,290,y,st,slen(st));
        y+=20;
    }
done:
    SelectObject(hdc,GetStockObject(ANSI_FIXED_FONT));
    DeleteObject(fTitle);DeleteObject(fSub);DeleteObject(fName);DeleteObject(fBody);DeleteObject(fBold);
    DeleteObject(bg);DeleteObject(hdr);DeleteObject(card);
}

static LRESULT STDCALL WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    PAINTSTRUCT ps;
    if(m==WM_PAINT){ HDC dc=BeginPaint(h,&ps); paint(dc,h); EndPaint(h,&ps); return 0; }
    if(m==WM_DESTROY){ PostQuitMessage(0); return 0; }
    return DefWindowProcA(h,m,w,l);
}

void STDCALL wininfo_start(void){
    WNDCLASSA wc; MSG msg; HWND hwnd;
    HINSTANCE hi=GetModuleHandleA(NULL);
    load_status();
    wc.style=0; wc.lpfnWndProc=WndProc; wc.cbClsExtra=0; wc.cbWndExtra=0;
    wc.hInstance=hi; wc.hIcon=LoadIconA(NULL,IDI_APPLICATION);
    wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(UINT_PTR)6;
    wc.lpszMenuName=NULL; wc.lpszClassName="WinSimdInfo";
    RegisterClassA(&wc);
    hwnd=CreateWindowExA(0,"WinSimdInfo","WINSIMD - SIMD Info",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,440,404,NULL,NULL,hi,NULL);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    while(GetMessageA(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessageA(&msg); }
    ExitProcess(0);
}
