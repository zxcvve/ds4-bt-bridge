/*
 * ds4bt: KMDF lower filter below HidBth.sys for a Bluetooth DualShock 4.
 * Rewrites L2CAP HID payloads so the pad talks the USB protocol to everything above it
 * (input 0x01, output 0x05, calibration feature 0x02).
 *
 * Translation is OFF unless Services\ds4bt\Parameters\Translate = 1. It must stay off until
 * HidBth also gets the USB report descriptor (Phase 0 of the plan); otherwise HidClass
 * rejects the translated reports. With it off, the filter only logs what HidBth sends down.
 */
#include <ntddk.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthioctl.h>
#include <bthddi.h>
#include "ds4_translate.h"

typedef struct {
    BOOLEAN Translate;
} DEV_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEV_CTX, DevCtx)

typedef struct {
    struct _BRB_L2CA_ACL_TRANSFER *Brb;
    PUCHAR Data;            /* HidBth's buffer, system-mapped */
    PVOID OrigBuffer;
    PMDL OrigMdl;
    ULONG OrigSize;
    BOOLEAN In;
    UCHAR Bounce[1024];     /* > largest DS4 BT report (0x19, 547 B) */
} REQ_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQ_CTX, ReqCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtInternalIoctl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtAclComplete;

#define LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ds4bt: " fmt "\n", __VA_ARGS__)

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

static ULONG ReadTranslateFlag(WDFDRIVER Driver)
{
    WDFKEY key;
    ULONG value = 0;
    DECLARE_CONST_UNICODE_STRING(name, L"Translate");

    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        WdfRegistryQueryULong(key, &name, &value);
        WdfRegistryClose(key);
    }
    return value;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    NTSTATUS status;

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, REQ_CTX);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attr);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEV_CTX);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    DevCtx(device)->Translate = ReadTranslateFlag(Driver) != 0;
    LOG("attached, translate=%d", DevCtx(device)->Translate);

    /* Only internal IOCTLs are hooked; KMDF forwards every other request type of a filter automatically. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoInternalDeviceControl = EvtInternalIoctl;
    return WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

static void Forward(WDFREQUEST Request, WDFIOTARGET Target)
{
    WDF_REQUEST_SEND_OPTIONS opts;
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, Target, &opts))
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
}

static void RestoreBrb(REQ_CTX *ctx)
{
    ctx->Brb->Buffer = ctx->OrigBuffer;
    ctx->Brb->BufferMDL = ctx->OrigMdl;
    ctx->Brb->BufferSize = ctx->OrigSize;
}

/* Points the BRB at our bounce buffer. Returns FALSE if the transfer should go down untouched. */
static BOOLEAN PrepareAcl(REQ_CTX *ctx, struct _BRB_L2CA_ACL_TRANSFER *t)
{
    PUCHAR data = t->Buffer;
    if (!data && t->BufferMDL)
        data = MmGetSystemAddressForMdlSafe(t->BufferMDL, NormalPagePriority | MdlMappingNoExecute);
    if (!data)
        return FALSE;

    ctx->Brb = t;
    ctx->Data = data;
    ctx->OrigBuffer = t->Buffer;
    ctx->OrigMdl = t->BufferMDL;
    ctx->OrigSize = t->BufferSize;
    ctx->In = (t->TransferFlags & ACL_TRANSFER_DIRECTION_IN) != 0;

    if (ctx->In) {
        /* HidBth sizes its buffer from the (USB) descriptor; BT reports are bigger. */
        t->BufferSize = sizeof(ctx->Bounce);
    } else {
        ds4_fix_get_feature(data, t->BufferSize);
        size_t n = ds4_usb_out_to_bt(data, t->BufferSize, ctx->Bounce, sizeof(ctx->Bounce));
        if (!n)
            return FALSE;
        t->BufferSize = (ULONG)n;
    }
    t->Buffer = ctx->Bounce;
    t->BufferMDL = NULL;
    return TRUE;
}

VOID EvtInternalIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    WDFIOTARGET target = WdfDeviceGetIoTarget(device);
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    if (Code != IOCTL_INTERNAL_BTH_SUBMIT_BRB) {
        LOG("internal ioctl 0x%08X", Code);     /* Phase 0: how does HidBth fetch the SDP descriptor? */
        Forward(Request, target);
        return;
    }

    PIRP irp = WdfRequestWdmGetIrp(Request);
    PBRB_HEADER hdr = (PBRB_HEADER)IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1;
    if (!hdr || hdr->Type != BRB_L2CA_ACL_TRANSFER) {
        if (hdr)
            LOG("brb type %u", hdr->Type);
        Forward(Request, target);
        return;
    }

    REQ_CTX *ctx = ReqCtx(Request);
    if (!DevCtx(device)->Translate || !PrepareAcl(ctx, (struct _BRB_L2CA_ACL_TRANSFER *)hdr)) {
        Forward(Request, target);
        return;
    }

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, EvtAclComplete, NULL);
    if (!WdfRequestSend(Request, target, WDF_NO_SEND_OPTIONS)) {
        RestoreBrb(ctx);
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

VOID EvtAclComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    REQ_CTX *ctx = ReqCtx(Request);
    NTSTATUS status = Params->IoStatus.Status;
    ULONG got = ctx->Brb->BufferSize;     /* bytes received for IN */
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    RestoreBrb(ctx);

    if (ctx->In && NT_SUCCESS(status)) {
        size_t n = ds4_bt_in_to_usb(ctx->Bounce, got, ctx->Data, ctx->OrigSize);
        if (n) {
            ctx->Brb->BufferSize = (ULONG)n;
        } else if (got <= ctx->OrigSize) {
            RtlCopyMemory(ctx->Data, ctx->Bounce, got);
            ctx->Brb->BufferSize = got;
        } else {
            status = STATUS_BUFFER_OVERFLOW;
            ctx->Brb->BufferSize = 0;
        }
    }

    WdfRequestComplete(Request, status);
}
