; ntosprobe_x64.asm - locate ntoskrnl via our DbgPrint import (driver only).
bits 64
section .text
extern __imp_DbgPrint
global ilf_iat_probe
ilf_iat_probe:
    mov     rax, [rel __imp_DbgPrint]
    ret
