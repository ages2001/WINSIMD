; vxdsched.asm - WINSIMD.VXD thread-switch YMM/XMM guard for Windows 9x.
;
; Windows 95's VMCPD virtualizes only x87/MMX (lazily, via CR0.TS); it does NOT
; save or restore XMM/YMM across a thread switch. So, exactly as on NT 3.1, the
; full vector register file leaks between concurrent SSE/AVX threads. This hooks
; the VMM scheduler via Call_When_Thread_Switched and does FULL per-thread
; save/restore of XMM0-7 (SSE) or YMM0-7 (AVX).
;
; Because VMCPD never touches XMM/YMM, at the switch callback the vector
; registers still hold the OUTGOING thread's data; we save them for the thread
; we last switched to (tracked in C) and restore the incoming thread's. The
; incoming thread handle is read with Get_Cur_Thread_Handle so we do not depend
; on the callback's (undocumented-here) register contract. CR0.TS is cleared
; around the vector ops because VMCPD may have set it for its lazy x87 handling.
;
;   nasm -f win32 vxd/vxdsched.asm
bits 32

Call_When_Thread_Switched equ 00010104h   ; VMM service id (int 20h + dd)
Get_Cur_Thread_Handle     equ 00010108h    ; -> EBX = current thread handle

extern _vxd_switch              ; void __cdecl vxd_switch(u32 incoming_thread)
extern _vxd_avx                 ; u8: 1 = AVX (YMM) present, 0 = XMM only
extern _vxd_upper               ; u8: 1 = upper-only (OS keeps XMM/MXCSR; guard
                                ;      keeps only YMM upper halves), 0 = full

section .text

; void register_switch_cb(void) - register the scheduler callback. Call once,
; at Device_Init (scheduler is up by then).
global _register_switch_cb
_register_switch_cb:
    push    esi
    mov     esi, [_switch_cb_ptr]       ; relocated runtime address of callback
    int     20h
    dd      Call_When_Thread_Switched
    pop     esi
    ret

; The scheduler thread-switch callback. Must preserve every register and flags.
global _ymm_switch_cb
_ymm_switch_cb:
    pushfd
    pushad
    cld
%ifdef E9TRACE
    cmp     byte [_cb_fired], 0         ; emit '!' once, the first time we fire
    jne     .nomark
    mov     byte [_cb_fired], 1
    mov     al, '!'
    out     0E9h, al
.nomark:
%endif
    ; Pass a pointer to the pushad frame so vxd_switch can read whichever GPR
    ; carries the incoming thread handle. pushad order (top->): edi,esi,ebp,
    ; esp,ebx,edx,ecx,eax. We must NOT call a VMM service (int 20h) here - that
    ; derails the scheduler callback and never returns.
    mov     esi, esp                    ; esi -> pushad frame
    mov     eax, cr0
    push    eax                         ; save CR0 (TS state)
    and     eax, 0FFFFFFF7h             ; clear TS so SSE/AVX do not #NM
    mov     cr0, eax
    push    esi
    call    _vxd_switch                 ; save outgoing, restore incoming
    add     esp, 4
    pop     eax
    mov     cr0, eax                    ; restore TS
    popad
    popfd
    ret

; void ymm_save8(void *buf) - save the per-thread vector state the OS does NOT.
;   full mode (_vxd_upper=0): XMM0-7 (SSE) or YMM0-7 (AVX) + MXCSR @buf+100h.
;     Win95 RTM / Win98 FE VMCPD is not SSE-aware and keeps neither.
;   upper-only (_vxd_upper=1, implies AVX): only the YMM0-7 upper 128 bits
;     (16B each). The OS (Win98 SE / ME, SSE-aware VMCPD) already keeps XMM +
;     MXCSR, so touching them would collide with its lazy fxsave/fxrstor.
;   x87 is always left to VMCPD.
global _ymm_save8
_ymm_save8:
    mov     ecx, [esp+4]
    cmp     byte [_vxd_upper], 0
    jne     .upper
    stmxcsr [ecx+100h]
    cmp     byte [_vxd_avx], 0
    je      .sse
    vmovups [ecx+000h], ymm0
    vmovups [ecx+020h], ymm1
    vmovups [ecx+040h], ymm2
    vmovups [ecx+060h], ymm3
    vmovups [ecx+080h], ymm4
    vmovups [ecx+0A0h], ymm5
    vmovups [ecx+0C0h], ymm6
    vmovups [ecx+0E0h], ymm7
    ret
.sse:
    movups  [ecx+000h], xmm0
    movups  [ecx+010h], xmm1
    movups  [ecx+020h], xmm2
    movups  [ecx+030h], xmm3
    movups  [ecx+040h], xmm4
    movups  [ecx+050h], xmm5
    movups  [ecx+060h], xmm6
    movups  [ecx+070h], xmm7
    ret
.upper:
    vextractf128 [ecx+000h], ymm0, 1
    vextractf128 [ecx+010h], ymm1, 1
    vextractf128 [ecx+020h], ymm2, 1
    vextractf128 [ecx+030h], ymm3, 1
    vextractf128 [ecx+040h], ymm4, 1
    vextractf128 [ecx+050h], ymm5, 1
    vextractf128 [ecx+060h], ymm6, 1
    vextractf128 [ecx+070h], ymm7, 1
    ret

; void ymm_restore8(void *buf) - mirror of ymm_save8.
global _ymm_restore8
_ymm_restore8:
    mov     ecx, [esp+4]
    cmp     byte [_vxd_upper], 0
    jne     .upper
    cmp     byte [_vxd_avx], 0
    je      .sse
    vmovups ymm0, [ecx+000h]
    vmovups ymm1, [ecx+020h]
    vmovups ymm2, [ecx+040h]
    vmovups ymm3, [ecx+060h]
    vmovups ymm4, [ecx+080h]
    vmovups ymm5, [ecx+0A0h]
    vmovups ymm6, [ecx+0C0h]
    vmovups ymm7, [ecx+0E0h]
    ldmxcsr [ecx+100h]
    ret
.sse:
    movups  xmm0, [ecx+000h]
    movups  xmm1, [ecx+010h]
    movups  xmm2, [ecx+020h]
    movups  xmm3, [ecx+030h]
    movups  xmm4, [ecx+040h]
    movups  xmm5, [ecx+050h]
    movups  xmm6, [ecx+060h]
    movups  xmm7, [ecx+070h]
    ldmxcsr [ecx+100h]
    ret
.upper:
    vinsertf128 ymm0, ymm0, [ecx+000h], 1
    vinsertf128 ymm1, ymm1, [ecx+010h], 1
    vinsertf128 ymm2, ymm2, [ecx+020h], 1
    vinsertf128 ymm3, ymm3, [ecx+030h], 1
    vinsertf128 ymm4, ymm4, [ecx+040h], 1
    vinsertf128 ymm5, ymm5, [ecx+050h], 1
    vinsertf128 ymm6, ymm6, [ecx+060h], 1
    vinsertf128 ymm7, ymm7, [ecx+070h], 1
    ret

section .data
align 4
global _switch_cb_ptr
_switch_cb_ptr: dd _ymm_switch_cb        ; pe2le relocates this to runtime addr
%ifdef E9TRACE
global _cb_fired
_cb_fired:      db 0
%endif
