; lowlevel_x86.asm - intlfxsr ring-0 primitives, i386, __cdecl
; Assemble: nasm -f win32 lowlevel_x86.asm -o lowlevel_x86.obj
;
; __cdecl on i686-windows-msvc: args on stack, caller cleans, leading '_'.
; 32-bit return in EAX; 64-bit return in EDX:EAX.
; Callee-saved: EBX, ESI, EDI, EBP.

bits 32
section .text

; u32 ilf_has_cpuid(void)  -> 1 if CPUID present, else 0.
; 386-SAFE: uses only pushfd/popfd + integer ops. On a real 386 the ID bit
; (EFLAGS.21) cannot be toggled, so this returns 0 and we never run CPUID.
global _ilf_has_cpuid
_ilf_has_cpuid:
    pushfd                      ; [A] save original EFLAGS for restore
    pushfd
    pop     eax                 ; eax = original
    mov     ecx, eax
    xor     eax, 0x00200000     ; flip ID bit
    push    eax
    popfd                       ; try to load flipped
    pushfd
    pop     eax                 ; read back actual
    popfd                       ; [A] restore original EFLAGS
    xor     eax, ecx            ; changed bits
    and     eax, 0x00200000
    shr     eax, 21             ; -> 0 or 1
    ret

; void ilf_cpuid(u32 leaf, u32 subleaf, u32 out[4])
global _ilf_cpuid
_ilf_cpuid:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    edi
    mov     eax, [ebp+8]        ; leaf
    mov     ecx, [ebp+12]       ; subleaf
    cpuid
    mov     edi, [ebp+16]       ; out
    mov     [edi+0],  eax
    mov     [edi+4],  ebx
    mov     [edi+8],  ecx
    mov     [edi+12], edx
    pop     edi
    pop     ebx
    pop     ebp
    ret

; u32 ilf_read_cr0(void)
global _ilf_read_cr0
_ilf_read_cr0:
    mov     eax, cr0
    ret

; u32 ilf_read_cr4(void)
global _ilf_read_cr4
_ilf_read_cr4:
    mov     eax, cr4
    ret

; void ilf_write_cr4(u32 v)
global _ilf_write_cr4
_ilf_write_cr4:
    mov     eax, [esp+4]
    mov     cr4, eax
    ret

; u64 ilf_xgetbv(u32 idx)   [requires CR4.OSXSAVE]
global _ilf_xgetbv
_ilf_xgetbv:
    mov     ecx, [esp+4]
    xgetbv                      ; -> EDX:EAX  (already the cdecl u64 return)
    ret

; void ilf_xsetbv(u32 idx, u64 val)  [ring0 + CR4.OSXSAVE]
global _ilf_xsetbv
_ilf_xsetbv:
    mov     ecx, [esp+4]        ; idx
    mov     eax, [esp+8]        ; val low
    mov     edx, [esp+12]       ; val high
    xsetbv
    ret

; void ilf_fxsave(void *area)   area: 512 bytes, 16-aligned
global _ilf_fxsave
_ilf_fxsave:
    mov     eax, [esp+4]
    fxsave  [eax]
    ret

; void ilf_fxrstor(void *area)
global _ilf_fxrstor
_ilf_fxrstor:
    mov     eax, [esp+4]
    fxrstor [eax]
    ret

; void ilf_xsave(void *area, u64 mask)   area: 64-aligned
global _ilf_xsave
_ilf_xsave:
    mov     ecx, [esp+4]        ; area
    mov     eax, [esp+8]        ; mask low  (EDX:EAX)
    mov     edx, [esp+12]       ; mask high
    xsave   [ecx]
    ret

; void ilf_xrstor(void *area, u64 mask)
global _ilf_xrstor
_ilf_xrstor:
    mov     ecx, [esp+4]
    mov     eax, [esp+8]
    mov     edx, [esp+12]
    xrstor  [ecx]
    ret

; uptr ilf_save_irq(void)  -> saved EFLAGS, then CLI (disable maskable IRQs)
global _ilf_save_irq
_ilf_save_irq:
    pushfd
    pop     eax
    cli
    ret

; void ilf_restore_irq(uptr flags)
global _ilf_restore_irq
_ilf_restore_irq:
    mov     eax, [esp+4]
    push    eax
    popfd
    ret

; void ilf_emms(void)
global _ilf_emms
_ilf_emms:
    emms
    ret

; ---- SIMD self-test kernels (i386 forms). Return a known constant in EAX
; iff the ISA actually executes (proves the CR4/XCR0 enable worked). Caller
; runs these inside ilf_simd_begin/end so the clobbered state is restored.

global _ilf_test_sse1
_ilf_test_sse1:
    xorps    xmm0, xmm0
    cmpeqps  xmm0, xmm0          ; all lanes equal -> all ones
    movmskps eax, xmm0           ; -> 0x0000000F
    ret

global _ilf_test_sse2
_ilf_test_sse2:
    pcmpeqd  xmm0, xmm0          ; all ones
    movd     eax, xmm0           ; -> 0xFFFFFFFF
    ret

global _ilf_test_avx
_ilf_test_avx:
    vxorps    ymm0, ymm0, ymm0
    vcmpeqps  ymm0, ymm0, ymm0   ; 256-bit all ones (AVX1, FP)
    vmovmskps eax, ymm0          ; -> 0x000000FF
    vzeroupper
    ret

; void ilf_e9out(u8 c)  - write one byte to QEMU debug console port 0xE9
global _ilf_e9out
_ilf_e9out:
    mov     al, [esp+4]
    out     0xE9, al
    ret
