bits 32
section .text
global _legacy_xmm_probe
_legacy_xmm_probe:
    push ebp
    mov ebp,esp
    push ebx
    push esi
    push edi
    sub esp,272
    cld
    mov ebx,[ebp+16]
    mov eax,[ebp+8]
    mov [ebp-16],eax
    lea edi,[ebp-284]
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
    movups [ebp-156+n*16],xmm %+ n
%assign n n+1
%endrep
    lea esi,[ebp-284]
    lea edi,[ebp-156]
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
    add esp,272
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
.load:
%assign n 0
%rep 8
    movups xmm %+ n,[ebp-284+n*16]
%assign n n+1
%endrep
    ret
