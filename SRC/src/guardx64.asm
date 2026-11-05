; guardx64.asm - WINSIMD 1.2 amd64 (Windows XP x64 / Server 2003 x64) YMM guard.
;
; x64 Windows saves XMM0-15 + MXCSR itself (fxsave/fxrstor in SwapContext) but
; not the AVX YMM upper halves, so concurrent AVX threads corrupt each other's
; YMM state across a context switch. This guard preserves ONLY the YMM upper
; halves (YMM0-7) per thread; the OS's XMM state is left untouched.
;
; The hook site is found at run time by a unique byte-signature scan of the
; running kernel (cgx64_scan in guardx64.c) - never a fixed address.
;
; Assemble: nasm -f win64 guardx64.asm
bits 64
default rel
section .text

extern cgx64_switch          ; void cgx64_switch(u64 incoming_thread, u64 kprcb)
extern cgx64_resume          ; u64 : trampoline (displaced bytes + jmp back)

; int cgx64_has_avx(void) -> 1 if CPUID.1:ECX has XSAVE(26) & AVX(28).
global cgx64_has_avx
cgx64_has_avx:
    push rbx
    mov eax,1
    cpuid
    mov eax,ecx
    and eax,(1<<26)|(1<<28)
    cmp eax,(1<<26)|(1<<28)
    sete al
    movzx eax,al
    pop rbx
    ret

; void cgx64_save_upper(void *buf) : store YMM0-7 upper 128 bits to buf[8*16].
; YMM8-15 are deliberately NOT touched (see src/NOTES-xp-x64.md).
global cgx64_save_upper
cgx64_save_upper:
    ; rcx = buf
%assign n 0
%rep 8
    vextractf128 [rcx + n*16], ymm %+ n, 1
%assign n n+1
%endrep
    ret

; void cgx64_restore_upper(void *buf) : load YMM0-7 upper 128 bits (keep low).
global cgx64_restore_upper
cgx64_restore_upper:
    ; rcx = buf
%assign n 0
%rep 8
    vinsertf128 ymm %+ n, ymm %+ n, [rcx + n*16], 1
%assign n n+1
%endrep
    ret

; The injected hook. Reached by an abs jmp patched at the SwapContext site.
; Live on entry: RBX = KPRCB, RSI = incoming thread, interrupts off. Must
; preserve every register and flag, then continue through the trampoline.
global cgx64_hook
cgx64_hook:
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    ; clear CR0.TS/EM so the AVX save/restore below cannot #NM
    mov rax, cr0
    mov r13, rax
    and rax, ~0x0c              ; clear TS(3) and EM(2)
    mov cr0, rax
    ; make sure OSFXSR/OSXMMEXCPT/OSXSAVE are on and XCR0 programmed, once/CPU
    mov rax, cr4
    mov r12, rax
    or  rax, 0x40600           ; OSFXSR(9)|OSXMMEXCPT(10)|OSXSAVE(18)
    mov cr4, rax
    test r12, 0x40000          ; was OSXSAVE already on? then XCR0 is set
    jnz .xcr0_done
    xor ecx, ecx
    mov eax, 7                 ; XCR0 = x87|SSE|AVX
    xor edx, edx
    xsetbv
.xcr0_done:
    ; args: rcx = incoming thread (RSI), rdx = KPRCB (RBX) as CPU identity
    mov rcx, rsi
    mov rdx, rbx
    mov r14, rsp
    and rsp, -16               ; 16-byte align + 32-byte shadow space
    sub rsp, 32
    call cgx64_switch
    mov rsp, r14
    mov cr0, r13               ; CR0 back as the OS left it
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    popfq
    jmp [cgx64_resume]         ; trampoline: displaced 16 bytes, then site+16

; u8 cgx64_write14(u8 *site, u64 hook) - write the 14-byte rip-relative
;   absolute jmp "FF 25 00000000 <qword hook>" at site, with CR0.WP cleared
;   and interrupts off, then serialize this CPU. Returns 1. Intended to run
;   inside a KeIpiGenericCall broadcast (all other CPUs halted at IPI_LEVEL),
;   but is self-contained and UP-safe on its own.
;   rcx = site, rdx = hook.
global cgx64_write14
cgx64_write14:
    pushfq
    cli
    mov rax, cr0
    mov r9, rax
    and rax, ~0x10000          ; clear WP (bit 16)
    mov cr0, rax
    mov word [rcx], 0x25ff     ; ff 25
    mov dword [rcx+2], 0       ; rip-relative disp32 = 0
    mov [rcx+6], rdx           ; qword target = hook
    mov cr0, r9                ; restore WP
    mov r8, rbx
    xor eax,eax
    cpuid                      ; serialize: flush this CPU's prefetch
    mov rbx, r8
    popfq
    mov eax, 1
    ret

; void cgx64_serialize(void) - cpuid, to flush prefetch on a spinning CPU.
global cgx64_serialize
cgx64_serialize:
    mov r8, rbx
    xor eax, eax
    cpuid
    mov rbx, r8
    ret

; u64 cgx64_hook_addr(void) - address of the hook entry, for the C side.
global cgx64_hook_addr
cgx64_hook_addr:
    lea rax, [cgx64_hook]
    ret
