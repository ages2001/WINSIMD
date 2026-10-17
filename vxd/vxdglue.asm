; vxdglue.asm - WINSIMD.VXD: Device Descriptor Block + VMM control procedure.
; Assembled as win32 COFF and linked (lld-link) together with the shared C core;
; scripts/pe2le.py then turns the PE image into the LE file Win95 loads.
;   nasm -f win32 vxd/vxdglue.asm
bits 32

Sys_Critical_Init   equ 00h
W32_DEVICEIOCONTROL equ 23h

extern _vxd_init                ; void __cdecl vxd_init(void)
extern _vxd_ioctl               ; u32  __cdecl vxd_ioctl(DIOCPARAMS *)

section .text
    times 16 db 0CCh            ; keep code off object offset 0
; VMM control procedure. In: EAX = message, EBX = VM handle, ESI = DIOCParams
; (for W32_DEVICEIOCONTROL). Out: CF clear = success; for W32_DEVICEIOCONTROL
; EAX = Win32 error code (0 = success).
global _ControlProc
_ControlProc:
    cmp     eax, W32_DEVICEIOCONTROL
    je      .dioc
    cmp     eax, Sys_Critical_Init
    jne     .ok
    pushad
    cld
%ifdef E9TRACE
    mov     al, '@'
    out     0E9h, al
%endif
    call    _vxd_init
    popad
.ok:
    clc
    ret
.dioc:
    push    ebx
    push    ecx
    push    edx
    push    esi
    push    edi
    push    ebp
    cld
    push    esi
    call    _vxd_ioctl
    add     esp, 4
    pop     ebp
    pop     edi
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    clc
    ret

section .data
; ===== Device Descriptor Block (exported as LE ordinal 1 by pe2le.py) =====
; Win95's loader validates the SDK 4.0 tail written by the DDK's
; Declare_Virtual_Device ('Prev', size 50h, 'Rsv1'..'Rsv3'); zeros there make
; BOOTLOG.TXT report LoadFailed.
align 16
global _WINSIMD_DDB
_WINSIMD_DDB:
    dd  0                       ; DDB_Next
    dw  0400h                   ; DDB_SDK_Version
    dw  0                       ; DDB_Req_Device_Number (Undefined_Device_ID)
    db  1, 0                    ; DDB_Dev_Major/Minor_Version
    dw  0                       ; DDB_Flags
    db  'WINSIMD '              ; DDB_Name
    dd  80000000h               ; DDB_Init_Order (Undefined_Init_Order)
    dd  _ControlProc            ; DDB_Control_Proc
    dd  0                       ; DDB_V86_API_Proc
    dd  0                       ; DDB_PM_API_Proc
    dd  0                       ; DDB_V86_API_CSIP
    dd  0                       ; DDB_PM_API_CSIP
    dd  0                       ; DDB_Reference_Data
    dd  0                       ; DDB_Service_Table_Ptr
    dd  0                       ; DDB_Service_Table_Size
    dd  0                       ; DDB_Win32_Service_Table
    db  'verP'                  ; DDB_Prev        (MASM: dd 'Prev')
    dd  50h                     ; DDB_Size
    db  '1vsR'                  ; DDB_Reserved1   (MASM: dd 'Rsv1')
    db  '2vsR'                  ; DDB_Reserved2
    db  '3vsR'                  ; DDB_Reserved3
