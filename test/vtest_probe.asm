bits 32
section .text
; u32 cpu_avx_usable(void) -> 1 if CPUID says OSXSAVE(ecx.27) & AVX(ecx.28).
global _cpu_avx_usable
_cpu_avx_usable:
    push ebx
    mov eax,1
    cpuid
    mov eax,ecx
    and eax,(1<<27)|(1<<28)
    cmp eax,(1<<27)|(1<<28)
    sete al
    movzx eax,al
    pop ebx
    ret

; void xmm_probe(u32 seed, u32 iters, u32 result[3], u32 mxcsr)
; Hold XMM0-7 + MXCSR + x87 across the spin; count mismatches in result[0..2].
global _xmm_probe
_xmm_probe:
    push ebp
    mov ebp,esp
    push ebx
    push esi
    push edi
    sub esp,320
    cld
    mov ebx,[ebp+16]
    mov eax,[ebp+8]
    mov [ebp-16],eax
    lea edi,[ebp-288]
    mov ecx,32
.fill:
    stosd
    add eax,01010101h
    loop .fill
    mov eax,[ebp+20]
    mov [ebp-20],eax
    ldmxcsr [ebp-20]
    fninit
    fild dword [ebp-16]
    call .load
    mov ecx,[ebp+12]
.loop:
    movd eax,xmm0
    cmp eax,[ebp-16]
    je .fast_ok
    inc dword [ebx]
    call .load
.fast_ok:
    dec ecx
    test ecx,03ffh
    jnz .continue
    push ecx
%assign n 0
%rep 8
    movups [ebp-160+n*16],xmm %+ n
%assign n n+1
%endrep
    lea esi,[ebp-288]
    lea edi,[ebp-160]
    mov ecx,32
    repe cmpsd
    je .xmm_ok
    inc dword [ebx]
    call .load
.xmm_ok:
    stmxcsr [ebp-28]
    mov eax,[ebp-28]
    cmp eax,[ebp-20]
    je .mxcsr_ok
    inc dword [ebx+4]
    ldmxcsr [ebp-20]
.mxcsr_ok:
    fist dword [ebp-24]
    mov eax,[ebp-24]
    cmp eax,[ebp-16]
    je .x87_ok
    inc dword [ebx+8]
    fninit
    fild dword [ebp-16]
.x87_ok:
    pop ecx
.continue:
    test ecx,ecx
    jnz .loop
    fninit
    mov dword [ebp-20],01f80h
    ldmxcsr [ebp-20]
    add esp,320
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
.load:
%assign n 0
%rep 8
    movups xmm %+ n,[ebp-288+n*16]
%assign n n+1
%endrep
    ret

; void ymm_probe(u32 seed, u32 iters, u32 result[3], u32 mxcsr)
; Same, but all 256 bits of YMM0-7. result[0]=YMM mismatches, [1]=MXCSR, [2]=x87.
global _ymm_probe
_ymm_probe:
    push ebp
    mov ebp,esp
    push ebx
    push esi
    push edi
    sub esp,576
    cld
    mov ebx,[ebp+16]
    mov eax,[ebp+8]
    mov [ebp-16],eax
    lea edi,[ebp-544]           ; reference 256 bytes = 64 dwords
    mov ecx,64
.yfill:
    stosd
    add eax,01010101h
    loop .yfill
    mov eax,[ebp+20]
    mov [ebp-20],eax
    ldmxcsr [ebp-20]
    fninit
    fild dword [ebp-16]
    call .yload
    mov ecx,[ebp+12]
.yloop:
    movd eax,xmm0
    cmp eax,[ebp-16]
    je .yfast_ok
    inc dword [ebx]
    call .yload
.yfast_ok:
    dec ecx
    test ecx,03ffh
    jnz .ycontinue
    push ecx
%assign n 0
%rep 8
    vmovups [ebp-288+n*32],ymm %+ n
%assign n n+1
%endrep
    lea esi,[ebp-544]
    lea edi,[ebp-288]
    mov ecx,64
    repe cmpsd
    je .yvec_ok
    inc dword [ebx]
    call .yload
.yvec_ok:
    stmxcsr [ebp-28]
    mov eax,[ebp-28]
    cmp eax,[ebp-20]
    je .ymxcsr_ok
    inc dword [ebx+4]
    ldmxcsr [ebp-20]
.ymxcsr_ok:
    fist dword [ebp-24]
    mov eax,[ebp-24]
    cmp eax,[ebp-16]
    je .yx87_ok
    inc dword [ebx+8]
    fninit
    fild dword [ebp-16]
.yx87_ok:
    pop ecx
.ycontinue:
    test ecx,ecx
    jnz .yloop
    vzeroupper
    fninit
    mov dword [ebp-20],01f80h
    ldmxcsr [ebp-20]
    add esp,576
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
.yload:
%assign n 0
%rep 8
    vmovups ymm %+ n,[ebp-544+n*32]
%assign n n+1
%endrep
    ret
