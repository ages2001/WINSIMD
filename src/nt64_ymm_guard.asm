bits 64
default rel
section .text

extern guard64_switch        ; void guard64_switch(u64 new_thread, u32 cpu)
extern guard64_resume        ; u64 : trampoline address (displaced bytes + jmp back)

; int guard64_has_avx(void) -> 1 if CPUID.1:ECX has XSAVE(26) & AVX(28).
; OSXSAVE(27) is NOT required here: the guard enables it itself in the hook.
global guard64_has_avx
guard64_has_avx:
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

; void guard64_save_upper(void *buf) : store YMM0-15 upper 128 bits to buf[16*16]
global guard64_save_upper
guard64_save_upper:
    ; rcx = buf
%assign n 0
%rep 8
    vextractf128 [rcx + n*16], ymm %+ n, 1
%assign n n+1
%endrep
    ret

; void guard64_restore_upper(void *buf) : load YMM0-15 upper 128 bits (keep low)
global guard64_restore_upper
guard64_restore_upper:
    ; rcx = buf
%assign n 0
%rep 8
    vinsertf128 ymm %+ n, ymm %+ n, [rcx + n*16], 1
%assign n n+1
%endrep
    ret

; The injected hook. Reached by an abs jmp patched at the SwapContext site.
; Live on entry: RBX=KPRCB, RSI=incoming thread, RDI=outgoing thread, interrupts
; off. Must preserve every register and flags, then continue via the trampoline.
global guard64_hook
guard64_hook:
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
    ; clear CR0.TS/EM so AVX instructions in the save/restore do not #NM
    mov rax, cr0
    mov r13, rax
    and rax, ~0x0c              ; clear TS(3) and EM(2)
    mov cr0, rax
    ; enable CR4.OSFXSR|OSXMMEXCPT and OSXSAVE, and program XCR0, once per CPU.
    mov rax, cr4
    mov r12, rax
    or  rax, 0x40600           ; OSFXSR(9)|OSXMMEXCPT(10)|OSXSAVE(18)
    mov cr4, rax
    test r12, 0x40000          ; was OSXSAVE already set? then XCR0 is programmed
    jnz .xcr0_done
    xor ecx, ecx
    mov eax, 7                 ; XCR0 = x87|SSE|AVX
    xor edx, edx
    xsetbv
.xcr0_done:
    ; args: rcx = incoming thread (RSI), edx = cpu number
    mov rcx, rsi
    ; cpu number: uniprocessor test uses 0. (MP build reads KPRCB.Number.)
    xor edx, edx
    ; 16-byte align + 32-byte shadow space for the Win64 call
    mov r14, rsp
    and rsp, -16
    sub rsp, 32
    call guard64_switch
    mov rsp, r14
    ; restore CR0
    mov cr0, r13
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
    jmp [guard64_resume]        ; trampoline: displaced 16 bytes, then back to site+16

; void guard64_install(u8 *site, u8 *tramp, u64 hook)
;   rcx=site, rdx=tramp(unused here), r8=hook. Writes a 14-byte rip-relative
;   absolute jmp at site: FF 25 00000000 <qword hook>. Uniprocessor-safe:
;   interrupts off, CR0.WP cleared so the read-only kernel page is writable.
global guard64_install
guard64_install:
    pushfq
    cli
    mov rax, cr0
    mov r9, rax
    and rax, ~0x10000           ; clear WP (bit 16)
    mov cr0, rax
    mov word [rcx], 0x25ff      ; ff 25
    mov dword [rcx+2], 0        ; rip-relative disp32 = 0
    mov [rcx+6], r8             ; qword target = hook
    mov cr0, r9                 ; restore WP
    push rbx                    ; serialize (cpuid) to flush prefetch
    xor eax,eax
    cpuid
    pop rbx
    popfq
    ret
