; ntosprobe_x86.asm - locate ntoskrnl via our DbgPrint import (driver only).
bits 32
section .text
extern __imp__DbgPrint
global _ilf_iat_probe
_ilf_iat_probe:
    mov     eax, [__imp__DbgPrint]
    ret
