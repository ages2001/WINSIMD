/* Exact NT31 ABI only. No kernel addresses or mutable controls are exposed. */
#include <ddk/ntddk.h>
#include "../include/guard_query.h"
#undef IoCompleteRequest
/* NT31 exports the stdcall routine, not the later fastcall IofCompleteRequest. */
__declspec(dllimport) void __stdcall Nt31CompleteRequest(PIRP, CCHAR);
static PDEVICE_OBJECT query_device;
static FpGuardStatus capability;
static UNICODE_STRING link_name;
typedef char check_irp_status[FIELD_OFFSET(IRP, IoStatus)==0x18 ? 1 : -1];
typedef char check_irp_stack[FIELD_OFFSET(IRP, Tail.Overlay.CurrentStackLocation)==0x60 ? 1 : -1];
typedef char check_stack_size[sizeof(IO_STACK_LOCATION)==0x24 ? 1 : -1];

static NTSTATUS NTAPI dispatch(PDEVICE_OBJECT device, PIRP irp) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;
    ULONG bytes=0;
    (void)device;
    if(stack->MajorFunction==IRP_MJ_CREATE || stack->MajorFunction==IRP_MJ_CLOSE ||
       stack->MajorFunction==IRP_MJ_CLEANUP) status=STATUS_SUCCESS;
    else if(stack->MajorFunction==IRP_MJ_DEVICE_CONTROL &&
            stack->Parameters.DeviceIoControl.IoControlCode==FP_GUARD_IOCTL) {
        if(stack->Parameters.DeviceIoControl.InputBufferLength!=0)
            status=STATUS_INVALID_PARAMETER;
        else if(stack->Parameters.DeviceIoControl.OutputBufferLength<sizeof(capability))
            status=STATUS_BUFFER_TOO_SMALL;
        else if(!capability.flags) status=STATUS_DEVICE_NOT_READY;
        else {
            *(FpGuardStatus*)irp->AssociatedIrp.SystemBuffer=capability;
            bytes=sizeof(capability);
            status=STATUS_SUCCESS;
        }
    }
    irp->IoStatus.Status=status;
    irp->IoStatus.Information=bytes;
    Nt31CompleteRequest(irp,0);
    return status;
}

long __cdecl guard_query_init(void *object) {
    PDRIVER_OBJECT driver=object;
    UNICODE_STRING name;
    NTSTATUS status;
    RtlInitUnicodeString(&name,L"\\Device\\FpXmmGuard");
    RtlInitUnicodeString(&link_name,L"\\DosDevices\\FpXmmGuard");
    /* The NT31 dispatch table predates the modern DDK's final major codes.
       Do not overwrite beyond the native object's declared size. */
    if(driver->Size < FIELD_OFFSET(DRIVER_OBJECT,MajorFunction)+
                      (IRP_MJ_CLEANUP+1)*sizeof(PDRIVER_DISPATCH))
        return STATUS_NOT_SUPPORTED;
    driver->MajorFunction[IRP_MJ_CREATE]=dispatch;
    driver->MajorFunction[IRP_MJ_CLOSE]=dispatch;
    driver->MajorFunction[IRP_MJ_CLEANUP]=dispatch;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL]=dispatch;
    status=IoCreateDevice(driver,0,&name,FILE_DEVICE_UNKNOWN,0,FALSE,&query_device);
    if(!NT_SUCCESS(status))return status;
    query_device->Flags|=DO_BUFFERED_IO;
    status=IoCreateSymbolicLink(&link_name,&name);
    if(!NT_SUCCESS(status)){IoDeleteDevice(query_device);query_device=0;return status;}
    query_device->Flags&=~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
void __cdecl guard_query_abort(void) {
    IoDeleteSymbolicLink(&link_name);
    if(query_device)IoDeleteDevice(query_device);
    query_device=0;
}
void __cdecl guard_query_activate(unsigned long processors,unsigned long timestamp) {
    capability.size=sizeof(capability);
    capability.magic=FP_GUARD_MAGIC;
    capability.abi=FP_GUARD_ABI;
    capability.processors=processors;
    capability.kernel_timestamp=timestamp;
    capability.thread_capacity=512;
    /* Publish readiness only after the context-switch adapter is installed. */
    __asm__ volatile("":::"memory");
    capability.flags=FP_GUARD_XMM_MXCSR;
}
