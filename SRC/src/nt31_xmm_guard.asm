bits 32
section .text
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
global _guard_read_cr4
_guard_read_cr4:
    mov eax,cr4
    ret
global _guard_save
_guard_save:
    mov ecx,[esp+4]
%assign n 0
%rep 8
    movups [ecx+n*16],xmm %+ n
%assign n n+1
%endrep
    stmxcsr [ecx+128]
    ret
global _guard_restore
_guard_restore:
    mov ecx,[esp+4]
%assign n 0
%rep 8
    movups xmm %+ n,[ecx+n*16]
%assign n n+1
%endrep
    ldmxcsr [ecx+128]
    ret
extern _guard_switch_state
extern _guard_resume
global _guard_hook
_guard_hook:
    pushfd
    cli
    pushad
    mov ebx,eax
    mov edi,edx
    mov eax,cr4
    mov ecx,eax
    and ecx,600h
    cmp ecx,600h
    je .enabled
    or eax,600h
    mov cr4,eax
.enabled:
    mov eax,cr0
    push eax
    and eax,0fffffff3h
    mov cr0,eax
    push edi
    push ebx
    call _guard_switch_state
    add esp,8
    pop eax
    mov cr0,eax
    popad
    popfd
    inc dword [edx+184h]
    jmp [_guard_resume]
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
