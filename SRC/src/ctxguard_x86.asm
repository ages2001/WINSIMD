; ctxguard_x86.asm - vector 7 (#NM) hook and register save/restore for the
; generic context guard (see ctxguard.c). One source for the NT driver and,
; with -DVXD, for WINSIMD.VXD.
bits 32
section .text

extern _cg_mask_lo, _cg_mask_hi, _cg_use_xsave, _cg_orig_nm, _cg_armed
extern _cg_on_nm
extern _cg_apic_index

; u32 cg_cpu_index(void) - this CPU's index from its initial APIC ID (CPUID 1)
global _cg_cpu_index
_cg_cpu_index:
    push    ebx
    mov     eax, 1
    cpuid
    shr     ebx, 24
    movzx   eax, byte [_cg_apic_index+ebx]
    pop     ebx
    ret

; u32 cg_apic_id_now(void) - initial APIC ID of this CPU
global _cg_apic_id_now
_cg_apic_id_now:
    push    ebx
    mov     eax, 1
    cpuid
    mov     eax, ebx
    shr     eax, 24
    pop     ebx
    ret

; u32 cg_rdmsr_lo(u32 msr, u32 *hi)
global _cg_rdmsr
_cg_rdmsr:
    mov     ecx, [esp+4]
    rdmsr
    mov     ecx, [esp+8]
    mov     [ecx], edx
    ret

; u32 cg_clts(void) - return CR0 and clear CR0.TS and CR0.EM (some NT
; kernels keep EM set while no thread owns the FPU)
global _cg_clts
_cg_clts:
    mov     eax, cr0
    mov     edx, eax
    and     edx, 0FFFFFFF3h
    mov     cr0, edx
    ret

; void cg_set_cr0(u32)
global _cg_set_cr0
_cg_set_cr0:
    mov     eax, [esp+4]
    mov     cr0, eax
    ret

; void cg_idt_read8(u32 vector, u32 out[2]) / cg_idt_write8(u32 vector, u32 lo, u32 hi)
global _cg_idt_read8
_cg_idt_read8:
    sub     esp, 8
    sidt    [esp+2]
    mov     ecx, [esp+4]
    add     esp, 8
    mov     eax, [esp+4]
    lea     ecx, [ecx+eax*8]
    mov     edx, [esp+8]
    mov     eax, [ecx]
    mov     [edx], eax
    mov     eax, [ecx+4]
    mov     [edx+4], eax
    ret
global _cg_idt_write8
_cg_idt_write8:
    push    ebx
    pushfd
    cli
    sub     esp, 8
    sidt    [esp+2]
    mov     ecx, [esp+4]
    add     esp, 8
    mov     eax, [esp+12]
    lea     ecx, [ecx+eax*8]
    mov     ebx, cr0
    push    ebx
    and     ebx, 0FFFEFFFFh
    mov     cr0, ebx
    mov     eax, [esp+20]
    mov     [ecx], eax
    mov     eax, [esp+24]
    mov     [ecx+4], eax
    pop     ebx
    mov     cr0, ebx
    popfd
    pop     ebx
    ret

; void cg_save(void *area) - store the guarded components
global _cg_save
_cg_save:
    mov     ecx, [esp+4]
    cmp     dword [_cg_use_xsave], 0
    je      .sse
    mov     eax, [_cg_mask_lo]
    mov     edx, [_cg_mask_hi]
    xsave   [ecx]
    ret
.sse:
%assign n 0
%rep 8
    movups  [ecx+n*16], xmm %+ n
%assign n n+1
%endrep
    stmxcsr [ecx+128]
    ret

; void cg_restore(void *area)
global _cg_restore
_cg_restore:
    mov     ecx, [esp+4]
    cmp     dword [_cg_use_xsave], 0
    je      .sse
    mov     eax, [_cg_mask_lo]
    mov     edx, [_cg_mask_hi]
    xrstor  [ecx]
    ret
.sse:
%assign n 0
%rep 8
    movups  xmm %+ n, [ecx+n*16]
%assign n n+1
%endrep
    ldmxcsr [ecx+128]
    ret

; u32 cg_idt_gate(u32 vector, u32 *handler) - read a gate of this CPU's IDT.
; Returns the gate type byte (P/DPL/type), handler offset in *handler.
global _cg_idt_gate
_cg_idt_gate:
    sub     esp, 8
    sidt    [esp+2]
    mov     ecx, [esp+4]            ; IDT base
    add     esp, 8
    mov     eax, [esp+4]
    lea     ecx, [ecx+eax*8]
    movzx   eax, word [ecx]
    movzx   edx, word [ecx+6]
    shl     edx, 16
    or      eax, edx
    mov     edx, [esp+8]
    mov     [edx], eax
    movzx   eax, byte [ecx+5]
    ret

; void cg_idt_set(u32 vector, u32 handler) - point a gate of this CPU's IDT
; at handler (offset only; selector/type unchanged). Clears CR0.WP briefly in
; case the IDT page is mapped read-only.
global _cg_idt_set
_cg_idt_set:
    push    ebx
    pushfd
    cli
    sub     esp, 8
    sidt    [esp+2]
    mov     ecx, [esp+4]
    add     esp, 8
    mov     eax, [esp+12]
    lea     ecx, [ecx+eax*8]
    mov     edx, [esp+16]
    mov     ebx, cr0
    push    ebx
    and     ebx, 0FFFEFFFFh
    mov     cr0, ebx
    mov     [ecx], dx
    shr     edx, 16
    mov     [ecx+6], dx
    pop     ebx
    mov     cr0, ebx
    popfd
    pop     ebx
    ret

%ifndef VXD
; ---------------------------------------------------------------- NT ----
; Entered directly from the CPU on #NM, before the kernel's trap handler.
; From user mode or V86 mode FS/DS/ES are not the kernel's: load SS (the
; ring-0 flat data selector) into DS/ES and 30h (KPCR, constant on every
; x86 NT from 3.1 to XP) into FS. Thread = KPCR.Prcb->CurrentThread
; ([fs:20h] -> +4, also constant on every x86 NT).
global _cg_nm_hook
_cg_nm_hook:
    cmp     dword [cs:_cg_armed], 0
    je      .chain
    push    ds
    push    es
    push    fs
    pushad
    pushfd
    cli
    mov     ax, ss
    mov     ds, ax
    mov     es, ax
    mov     ax, 30h
    mov     fs, ax
    mov     ebx, cr0
    mov     eax, ebx
    and     eax, 0FFFFFFF3h         ; clear TS and EM: the SIMD below must not fault
    mov     cr0, eax
    mov     eax, [fs:20h]
    push    dword [eax+4]
    call    _cg_on_nm
    add     esp, 4
    mov     cr0, ebx                ; CR0.TS back as the OS left it
    popfd
    popad
    pop     fs
    pop     es
    pop     ds
.chain:
    jmp     dword [cs:_cg_orig_nm]

; NMI (vector 2): a flush request from another CPU (see ctxguard.c), or a
; real NMI, which goes on to the original gate. NT reaches its NMI handler
; through a task gate; that is entered here with a far jump to the TSS.
extern _cg_on_nmi
extern _cg_orig_nmi_far
global _cg_nmi_hook
_cg_nmi_hook:
    push    ds
    push    es
    push    fs
    pushad
    mov     ax, ss
    mov     ds, ax
    mov     es, ax
    mov     ax, 30h
    mov     fs, ax
    call    _cg_on_nmi
    test    eax, eax
    popad
    pop     fs
    pop     es
    pop     ds
    jz      .not_ours
    iretd
.not_ours:
    jmp     far [cs:_cg_orig_nmi_far]

; u32 cg_current_thread(void) - same lookup, for validation at install time
global _cg_current_thread
_cg_current_thread:
    mov     eax, [fs:20h]
    mov     eax, [eax+4]
    ret
%else
; ---------------------------------------------------------------- 9x ----
; VMM is entered with SS = its flat ring-0 data selector; from V86 mode the
; CPU has cleared DS/ES. Thread = VMM Get_Cur_Thread_Handle (VMM service
; 0108h, returns EDI). The call site is patched by VMM into a direct call on
; first use; cg_current_thread is called once during init so that happens
; outside the fault path.
global _cg_nm_hook
_cg_nm_hook:
    cmp     dword [cs:_cg_armed], 0
    je      .chain
    push    ds
    push    es
    pushad
    pushfd
    cli
    mov     ax, ss
    mov     ds, ax
    mov     es, ax
    mov     ebx, cr0
    mov     eax, ebx
    and     eax, 0FFFFFFF3h         ; clear TS and EM
    mov     cr0, eax
    call    _cg_current_thread
    push    eax
    call    _cg_on_nm
    add     esp, 4
    mov     cr0, ebx
    popfd
    popad
    pop     es
    pop     ds
.chain:
    jmp     dword [cs:_cg_orig_nm]

global _cg_current_thread
_cg_current_thread:
    push    edi
    int     20h
    dd      00010108h               ; VMMCall Get_Cur_Thread_Handle
    mov     eax, edi
    pop     edi
    ret

; void *cg_heap_alloc(u32 bytes) - VMMCall _HeapAllocate(bytes, 0)
global _cg_heap_alloc
_cg_heap_alloc:
    push    0
    push    dword [esp+8]
    int     20h
    dd      0001004Fh               ; VMMCall _HeapAllocate
    add     esp, 8
    ret
%endif
