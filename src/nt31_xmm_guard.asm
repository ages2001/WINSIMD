bits 32
section .text
; Width-adaptive vector state for the NT 3.1 context-switch guard.
;   _guard_width : 0 = XMM0-7 (128B), 1 = YMM0-7 (256B), 2 = ZMM0-7 (512B)+k0-k7
;   _guard_xcr0  : XCR0 value to program when width >= 1 (7 = AVX, 0xe7 = AVX-512)
; Save buffer layout (per live thread slot):
;   [buf+0   .. +511] vector registers (low part used for narrower widths)
;   [buf+512 .. +575] opmask k0-k7 (ZMM width only)
;   [buf+576]         MXCSR
extern _guard_width
extern _guard_mode
extern _guard_xcr0

global _guard_cpu_number
_guard_cpu_number:
    mov eax,[fs:20h]
    mov eax,[eax+14h]
    bsf eax,eax
    ret
global _guard_has_sse2
_guard_has_sse2:
    push ebx
    pushfd
    pop eax
    mov ecx,eax
    xor eax,00200000h
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax,ecx
    test eax,00200000h
    jz .absent
    xor eax,eax
    cpuid
    cmp eax,1
    jb .absent
    mov eax,1
    cpuid
    and edx,07000000h
    cmp edx,07000000h
    sete al
    movzx eax,al
    pop ebx
    ret
.absent:
    xor eax,eax
    pop ebx
    ret

; u32 guard_detect_width(u32 *xcr0_out)
;  0 = SSE2 only, 1 = AVX usable (OSXSAVE-able + XSAVE + AVX), 2 = AVX-512F.
;  Writes the XCR0 value to program at *xcr0_out.
global _guard_detect_width
_guard_detect_width:
    push ebx
    push esi
    mov esi,[esp+12]            ; xcr0_out
    mov dword [esi],0
    mov eax,1
    cpuid
    ; need XSAVE (ecx bit26) and AVX (ecx bit28)
    mov ebx,ecx
    and ebx,(1<<26)|(1<<28)
    cmp ebx,(1<<26)|(1<<28)
    jne .sse
    mov dword [esi],7          ; x87|SSE|AVX
    ; AVX-512F? CPUID.7.0:EBX bit16
    xor eax,eax
    cpuid
    cmp eax,7
    jb .avx
    mov eax,7
    xor ecx,ecx
    cpuid
    test ebx,(1<<16)
    jz .avx
    mov dword [esi],0e7h       ; x87|SSE|AVX|opmask|ZMM_hi|ZMM16-31
    mov eax,2
    jmp .done
.avx:
    mov eax,1
    jmp .done
.sse:
    xor eax,eax
.done:
    pop esi
    pop ebx
    ret

global _guard_read_cr4
_guard_read_cr4:
    mov eax,cr4
    ret

global _guard_save
_guard_save:
    mov ecx,[esp+4]
    mov eax,[_guard_width]
    test eax,eax
    jnz .wide
%assign n 0
%rep 8
    movups [ecx+n*16],xmm %+ n
%assign n n+1
%endrep
    jmp .mxcsr
.wide:
    cmp eax,2
    je .zmm
    ; YMM width. Upper-only mode: the OS already preserves the low 128 bits and
    ; MXCSR, so keep only the YMM upper halves and do not touch MXCSR.
    mov eax,[_guard_mode]
    test eax,eax
    jnz .ymm_upper
%assign n 0
%rep 8
    vmovups [ecx+n*32],ymm %+ n
%assign n n+1
%endrep
    jmp .mxcsr
.ymm_upper:
%assign n 0
%rep 8
    vextractf128 [ecx+n*16],ymm %+ n,1
%assign n n+1
%endrep
    ret
.zmm:
%assign n 0
%rep 8
    vmovups [ecx+n*64],zmm %+ n
%assign n n+1
%endrep
    kmovq [ecx+512+0*8],k0
    kmovq [ecx+512+1*8],k1
    kmovq [ecx+512+2*8],k2
    kmovq [ecx+512+3*8],k3
    kmovq [ecx+512+4*8],k4
    kmovq [ecx+512+5*8],k5
    kmovq [ecx+512+6*8],k6
    kmovq [ecx+512+7*8],k7
.mxcsr:
    stmxcsr [ecx+576]
    ret

global _guard_restore
_guard_restore:
    mov ecx,[esp+4]
    mov eax,[_guard_width]
    test eax,eax
    jnz .wide
%assign n 0
%rep 8
    movups xmm %+ n,[ecx+n*16]
%assign n n+1
%endrep
    jmp .mxcsr
.wide:
    cmp eax,2
    je .zmm
    mov eax,[_guard_mode]
    test eax,eax
    jnz .ymm_upper
%assign n 0
%rep 8
    vmovups ymm %+ n,[ecx+n*32]
%assign n n+1
%endrep
    jmp .mxcsr
.ymm_upper:
    ; restore only the upper half; vinsertf128 keeps the low 128 bits the OS owns
%assign n 0
%rep 8
    vinsertf128 ymm %+ n,ymm %+ n,[ecx+n*16],1
%assign n n+1
%endrep
    ret
.zmm:
%assign n 0
%rep 8
    vmovups zmm %+ n,[ecx+n*64]
%assign n n+1
%endrep
    kmovq k0,[ecx+512+0*8]
    kmovq k1,[ecx+512+1*8]
    kmovq k2,[ecx+512+2*8]
    kmovq k3,[ecx+512+3*8]
    kmovq k4,[ecx+512+4*8]
    kmovq k5,[ecx+512+5*8]
    kmovq k6,[ecx+512+6*8]
    kmovq k7,[ecx+512+7*8]
.mxcsr:
    ldmxcsr [ecx+576]
    ret

extern _guard_switch_state
extern _guard_resume
extern _guard_new_off
global _guard_hook
_guard_hook:
    pushfd
    cli
    pushad
    mov eax,[_guard_new_off]
    mov edi,[esp+eax]            ; incoming thread (outgoing is tracked per-CPU)
    mov eax,cr4
    mov ecx,eax
    and ecx,600h
    cmp ecx,600h
    je .enabled
    or eax,600h
    mov cr4,eax
.enabled:
    ; width >= 1: enable OSXSAVE + program XCR0 once per CPU (first hook there).
    mov eax,[_guard_width]
    test eax,eax
    jz .no_xsave
    mov eax,cr4
    test eax,40000h
    jnz .no_xsave
    or eax,40000h
    mov cr4,eax
    xor ecx,ecx
    mov eax,[_guard_xcr0]
    xor edx,edx
    xsetbv
.no_xsave:
    mov eax,cr0
    push eax
    and eax,0fffffff3h
    mov cr0,eax
    push edi
    call _guard_switch_state
    add esp,4
    pop eax
    mov cr0,eax
    popad
    popfd
    jmp [_guard_resume]          ; trampoline: displaced instruction, then resume

global _guard_install
_guard_install:
    push ebx
    push esi
    push edi
    pushfd
    cli
    mov edi,[esp+20]
    mov esi,cr0
    push esi
    mov eax,esi
    and eax,0fffeffffh
    mov cr0,eax
    mov eax,[edi]
    mov edx,[edi+4]
    mov ebx,_guard_hook
    sub ebx,edi
    sub ebx,5
    mov ecx,ebx
    shr ecx,24
    mov esi,edx
    and esi,0ffff0000h
    or ecx,esi
    or ecx,00009000h
    shl ebx,8
    or ebx,0e9h
    lock cmpxchg8b [edi]
    sete bl
    movzx edi,bl
    pop esi
    mov cr0,esi
    xor eax,eax
    cpuid
    mov eax,edi
    popfd
    pop edi
    pop esi
    pop ebx
    ret
