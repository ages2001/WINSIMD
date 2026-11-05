/* Versioned, read-only capability query for the exact-kernel NT31 adapter.
   This reports XMM/MXCSR preservation, not YMM/ZMM support. */
#ifndef FP_GUARD_QUERY_H
#define FP_GUARD_QUERY_H
#define FP_GUARD_IOCTL 0x00222000u
#define FP_GUARD_MAGIC 0x584d4d47u
#define FP_GUARD_ABI 1u
#define FP_GUARD_XMM_MXCSR 1u
typedef struct FpGuardStatus {
    unsigned long size, magic, abi, flags;
    unsigned long processors, kernel_timestamp, thread_capacity;
} FpGuardStatus;
#endif
