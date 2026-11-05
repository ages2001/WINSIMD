; lowlevel_x64.asm - intlfxsr ring-0 primitives, amd64, MS x64 ABI
; Assemble: nasm -f win64 lowlevel_x64.asm -o lowlevel_x64.obj
;
; MS x64: integer args in RCX, RDX, R8, R9; return in RAX. No leading '_'.
; Callee-saved: RBX, RBP, RDI, RSI, R12-R15, XMM6-15.
; These are leaf functions (no stack frame, no shadow-space use).

bits 64
default rel
section .text

; u32 ilf_has_cpuid(void) -> always 1 on x86-64 (CPUID is architectural)
global ilf_has_cpuid
ilf_has_cpuid:
    mov     eax, 1
    ret

; void ilf_cpuid(u32 leaf, u32 subleaf, u32 out[4])
global ilf_cpuid
ilf_cpuid:
    mov     r9, rbx             ; preserve RBX (nonvolatile, clobbered by cpuid)
    mov     eax, ecx            ; leaf
    mov     ecx, edx            ; subleaf
    cpuid
    mov     [r8+0],  eax
    mov     [r8+4],  ebx
    mov     [r8+8],  ecx
    mov     [r8+12], edx
    mov     rbx, r9
    ret

; u32 ilf_read_cr0(void)
global ilf_read_cr0
ilf_read_cr0:
    mov     rax, cr0
    ret

; u32 ilf_read_cr4(void)
global ilf_read_cr4
ilf_read_cr4:
    mov     rax, cr4
    ret

; void ilf_write_cr4(u32 v)   (zero-extended in RCX by caller)
global ilf_write_cr4
ilf_write_cr4:
    mov     cr4, rcx
    ret

; u64 ilf_xgetbv(u32 idx)   [requires CR4.OSXSAVE]
global ilf_xgetbv
ilf_xgetbv:
    ; ECX already = idx
    xgetbv                      ; -> EDX:EAX
    shl     rdx, 32
    mov     eax, eax            ; zero-extend low dword
    or      rax, rdx
    ret

; void ilf_xsetbv(u32 idx, u64 val)  [ring0 + CR4.OSXSAVE]
global ilf_xsetbv
ilf_xsetbv:
    ; RCX = idx (ECX used), RDX = val
    mov     r8, rdx
    mov     eax, r8d            ; val low -> EAX
    shr     r8, 32
    mov     edx, r8d            ; val high -> EDX
    xsetbv
    ret

; void ilf_fxsave(void *area)   area: 512 bytes, 16-aligned
global ilf_fxsave
ilf_fxsave:
    fxsave  [rcx]
    ret

; void ilf_fxrstor(void *area)
global ilf_fxrstor
ilf_fxrstor:
    fxrstor [rcx]
    ret

; void ilf_xsave(void *area, u64 mask)   area: 64-aligned
global ilf_xsave
ilf_xsave:
    mov     rax, rdx
    mov     r8, rdx
    shr     r8, 32
    mov     edx, r8d
    xsave   [rcx]
    ret

; void ilf_xrstor(void *area, u64 mask)
global ilf_xrstor
ilf_xrstor:
    mov     rax, rdx
    mov     r8, rdx
    shr     r8, 32
    mov     edx, r8d
    xrstor  [rcx]
    ret

; uptr ilf_save_irq(void)  -> saved RFLAGS, then CLI
global ilf_save_irq
ilf_save_irq:
    pushfq
    pop     rax
    cli
    ret

; void ilf_restore_irq(uptr flags)
global ilf_restore_irq
ilf_restore_irq:
    push    rcx
    popfq
    ret

; void ilf_emms(void)
global ilf_emms
ilf_emms:
    emms
    ret

; ---- SIMD self-test kernels (amd64 forms). Identical encodings, no '_'.
global ilf_test_sse1
ilf_test_sse1:
    xorps    xmm0, xmm0
    cmpeqps  xmm0, xmm0
    movmskps eax, xmm0
    ret

global ilf_test_sse2
ilf_test_sse2:
    pcmpeqd  xmm0, xmm0
    movd     eax, xmm0
    ret

global ilf_test_avx
ilf_test_avx:
    vxorps    ymm0, ymm0, ymm0
    vcmpeqps  ymm0, ymm0, ymm0
    vmovmskps eax, ymm0
    vzeroupper
    ret

; void ilf_e9out(u8 c)  - QEMU debug console port 0xE9
global ilf_e9out
ilf_e9out:
    mov     al, cl
    out     0xE9, al
    ret
