; simdmt_x64.asm - per-thread SIMD state-retention probes (Win64, leaf funcs).
; Each keeps a per-thread signature in callee-saved vector regs across a long
; spin and counts how often it reads back wrong -> i.e. another thread's state
; leaked in across a context switch (the OS didn't preserve that part).
;   nasm -f win64 simdmt_x64.asm
bits 64
default rel
section .text

; u32 avx_probe(u32 sig, u64 iters)  -> mismatch count
; Keeps sig broadcast in ymm6/ymm7 (xmm6/7 are Win64 non-volatile; the UPPER
; 128 bits are what no NT/9x scheduler preserves). Lower 128 stay correct (OS
; FXSAVE), so a corrupted upper half shows as movmskps != 0xFF.
; u32 avx_usable(void) -> 1 if CPUID says OSXSAVE(ECX.27) AND AVX(ECX.28) set.
; A well-behaved app checks this before issuing AVX; in the driver's safe mode
; OSXSAVE stays 0, so this returns 0 and the app falls back to SSE.
global avx_usable
avx_usable:
    push    rbx
    mov     eax, 1
    cpuid
    mov     eax, ecx
    and     eax, (1 << 27) | (1 << 28)
    cmp     eax, (1 << 27) | (1 << 28)
    sete    al
    movzx   eax, al
    pop     rbx
    ret

global avx_probe
avx_probe:
    push    rbp
    mov     rbp, rsp
    sub     rsp, 32
    and     rsp, -32
    inc     ecx
    cvtsi2ss xmm0, ecx
    movss   [rsp], xmm0                 ; expected signature (float)
    vbroadcastss ymm6, dword [rsp]
    vbroadcastss ymm7, dword [rsp]
    xor     r9d, r9d                    ; mismatch count
.loop:
    vbroadcastss ymm0, dword [rsp]      ; expected x8 (recomputed from memory)
    vcmpeqps ymm1, ymm6, ymm0
    vmovmskps eax, ymm1
    cmp     eax, 0x0FF
    je      .ok6
    inc     r9d
    vbroadcastss ymm6, dword [rsp]      ; re-arm so we can count further events
.ok6:
    vcmpeqps ymm1, ymm7, ymm0
    vmovmskps eax, ymm1
    cmp     eax, 0x0FF
    je      .ok7
    inc     r9d
    vbroadcastss ymm7, dword [rsp]
.ok7:
    dec     rdx
    jnz     .loop
    mov     eax, r9d
    vzeroupper
    mov     rsp, rbp
    pop     rbp
    ret

; u32 sse_probe(u32 sig, u64 iters) -> mismatch count
; Same idea but 128-bit xmm6 only. On 2000/XP (OSFXSR set) the OS preserves all
; of xmm across switches, so this should stay 0; on NT4-/9x it would not.
global sse_probe
sse_probe:
    push    rbp
    mov     rbp, rsp
    sub     rsp, 32
    and     rsp, -16
    inc     ecx
    cvtsi2ss xmm0, ecx
    movss   [rsp], xmm0
    movss   xmm6, [rsp]
    shufps  xmm6, xmm6, 0
    xor     r9d, r9d
.sloop:
    movss   xmm0, [rsp]
    shufps  xmm0, xmm0, 0
    movaps  xmm1, xmm6
    cmpeqps xmm1, xmm0
    movmskps eax, xmm1
    cmp     eax, 0x0F
    je      .sok
    inc     r9d
    movss   xmm6, [rsp]
    shufps  xmm6, xmm6, 0
.sok:
    dec     rdx
    jnz     .sloop
    mov     eax, r9d
    mov     rsp, rbp
    pop     rbp
    ret
