/*
 * ds4bt: makes a Bluetooth DualShock 4 look like a USB one.
 *
 * Stack on the DS4's HID collection PDO (created by HidClass over HidBth):
 *     HidClass + mshidkmdf   <- function driver, forwards every HID minidriver IOCTL down
 *     ds4bt (this)           <- lower filter: answers with the USB descriptor, translates reports
 *     PDO (HidBth.sys)       <- the real Bluetooth HID transport
 *
 * Design follows imbushuo/mac-precision-touchpad (src/AmtPtpHidFilter), which does the same for
 * the Magic Trackpad 2 over Bluetooth. HidClass owns HidBth's dispatch table, so minidriver IOCTLs
 * sent to the PDO would land in HidClass. The detour below points IRP_MJ_INTERNAL_DEVICE_CONTROL of
 * HidBth's driver object back at HidBth's own handler, which HidClass saved in its driver extension.
 * That patch is global to HidBth (every BT HID device) and relies on undocumented layouts.
 */
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include "ds4_translate.h"
#include "ds4_usb_descriptor.h"

/* ---- undocumented HidClass / IO manager layouts (as used by AmtPtpHidFilter, include/Hac.h) ---- */
typedef struct _HIDCLASS_DRIVER_EXTENSION {
    PDRIVER_OBJECT      MinidriverObject;
    UNICODE_STRING      RegistryPath;
    ULONG               DeviceExtensionSize;
    PDRIVER_DISPATCH    MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    PDRIVER_ADD_DEVICE  AddDevice;
    PDRIVER_UNLOAD      DriverUnload;
    LONG                ReferenceCount;
    LIST_ENTRY          ListEntry;
    BOOLEAN             DevicesArePolled;
} HIDCLASS_DRIVER_EXTENSION, *PHIDCLASS_DRIVER_EXTENSION;

typedef struct _IO_CLIENT_EXTENSION {
    struct _IO_CLIENT_EXTENSION *NextExtension;
    PVOID ClientIdentificationAddress;
} IO_CLIENT_EXTENSION, *PIO_CLIENT_EXTENSION;

typedef struct _DRIVER_EXTENSION_EXT {
    struct _DRIVER_OBJECT *DriverObject;
    PDRIVER_ADD_DEVICE AddDevice;
    ULONG Count;
    UNICODE_STRING ServiceKeyName;
    PIO_CLIENT_EXTENSION IoClientExtension;
} DRIVER_EXTENSION_EXT, *PDRIVER_EXTENSION_EXT;

NTKERNELAPI PDEVICE_OBJECT IoGetLowerDeviceObject(_In_ PDEVICE_OBJECT DeviceObject);

/* ---- contexts ---- */
typedef struct {
    WDFIOTARGET Target;
    WDFQUEUE ReadQueue;     /* upper IOCTL_HID_READ_REPORTs waiting for a translated report */
    BOOLEAN Ready;          /* detour done, lower reads may be issued */
} DEV_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEV_CTX, DevCtx)

typedef struct {
    WDFDEVICE Device;
    UCHAR Buf[1024];        /* > largest DS4 BT input report (0x19, 547 B) */
} READ_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(READ_CTX, ReadCtx)

typedef struct {
    WDFREQUEST Upper;       /* NULL for driver-initiated requests */
    ULONG Ioctl;
    HID_XFER_PACKET Packet;
    UCHAR Buf[DS4_BT_REPORT_SIZE];
} XFER_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XFER_CTX, XferCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT EvtSelfManagedIoInit;
EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtInternalIoctl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtReadComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtXferComplete;

#define LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ds4bt: " fmt "\n", __VA_ARGS__)

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceSelfManagedIoInit = EvtSelfManagedIoInit;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEV_CTX);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;
    DevCtx(device)->Target = WdfDeviceGetIoTarget(device);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoInternalDeviceControl = EvtInternalIoctl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &DevCtx(device)->ReadQueue);
}

static NTSTATUS DetourHidBth(WDFDEVICE Device)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PDEVICE_OBJECT lower = IoGetLowerDeviceObject(WdfDeviceWdmGetDeviceObject(Device));
    if (!lower)
        return status;

    PDRIVER_OBJECT drv = lower->DriverObject;
    PDRIVER_EXTENSION_EXT ext = drv ? (PDRIVER_EXTENSION_EXT)drv->DriverExtension : NULL;
    PIO_CLIENT_EXTENSION client = ext ? ext->IoClientExtension : NULL;
    /* HidRegisterMinidriver tags its client extension with the "HIDCLASS" literal */
    if (client && strncmp("HIDCLASS", client->ClientIdentificationAddress, sizeof("HIDCLASS")) == 0) {
        PHIDCLASS_DRIVER_EXTENSION hce = (PHIDCLASS_DRIVER_EXTENSION)(client + 1);
        drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = hce->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
        status = STATUS_SUCCESS;
    }
    ObDereferenceObject(lower);
    return status;
}

/* Sends a HID_XFER_PACKET IOCTL to HidBth. Upper == NULL: synchronous (PASSIVE_LEVEL only);
 * otherwise async, and Upper is completed from EvtXferComplete. */
static NTSTATUS SendXfer(WDFDEVICE Device, ULONG Ioctl, UCHAR ReportId, const UCHAR *Data, ULONG Len, WDFREQUEST Upper)
{
    DEV_CTX *dc = DevCtx(Device);
    WDF_OBJECT_ATTRIBUTES attr;
    WDFREQUEST req;
    WDFMEMORY mem;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, XFER_CTX);
    attr.ParentObject = Device;
    status = WdfRequestCreate(&attr, dc->Target, &req);
    if (!NT_SUCCESS(status))
        return status;

    XFER_CTX *x = XferCtx(req);
    x->Upper = Upper;
    x->Ioctl = Ioctl;
    x->Packet.reportId = ReportId;
    x->Packet.reportBuffer = x->Buf;
    x->Packet.reportBufferLen = Len;
    if (Data)
        RtlCopyMemory(x->Buf, Data, Len);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = req;
    status = WdfMemoryCreatePreallocated(&attr, &x->Packet, sizeof(x->Packet), &mem);
    if (NT_SUCCESS(status)) {
        /* GET_FEATURE carries the packet as output, the writes as input (same as HidClass does) */
        if (Ioctl == IOCTL_HID_GET_FEATURE)
            status = WdfIoTargetFormatRequestForInternalIoctl(dc->Target, req, Ioctl, NULL, NULL, mem, NULL);
        else
            status = WdfIoTargetFormatRequestForInternalIoctl(dc->Target, req, Ioctl, mem, NULL, NULL, NULL);
    }
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(req);
        return status;
    }
    /* HID minidrivers read the packet from Irp->UserBuffer (METHOD_NEITHER) */
    WdfRequestWdmGetIrp(req)->UserBuffer = &x->Packet;

    if (!Upper) {
        WDF_REQUEST_SEND_OPTIONS opts;
        WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
        WdfRequestSend(req, dc->Target, &opts);
        status = WdfRequestGetStatus(req);
        WdfObjectDelete(req);
        return status;
    }

    WdfRequestSetCompletionRoutine(req, EvtXferComplete, NULL);
    if (!WdfRequestSend(req, dc->Target, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(req);
        WdfObjectDelete(req);
        return status;
    }
    return STATUS_PENDING;
}

VOID EvtXferComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    XFER_CTX *x = XferCtx(Request);
    NTSTATUS status = Params->IoStatus.Status;
    PHID_XFER_PACKET up = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(x->Upper)->UserBuffer;
    ULONG_PTR info = up->reportBufferLen;
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    if (x->Ioctl == IOCTL_HID_GET_FEATURE && NT_SUCCESS(status)) {
        info = ds4_bt_in_to_usb(x->Buf, DS4_BT_CALIB_SIZE, up->reportBuffer, up->reportBufferLen);
        if (!info)
            status = STATUS_BUFFER_TOO_SMALL;
    }
    WdfRequestCompleteWithInformation(x->Upper, status, info);
    WdfObjectDelete(Request);
}

/* Posts one IOCTL_HID_READ_REPORT to HidBth; its completion feeds one waiting upper read. */
static void IssueRead(WDFDEVICE Device)
{
    DEV_CTX *dc = DevCtx(Device);
    WDF_OBJECT_ATTRIBUTES attr;
    WDFREQUEST req;
    WDFMEMORY mem;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, READ_CTX);
    attr.ParentObject = Device;
    if (!NT_SUCCESS(WdfRequestCreate(&attr, dc->Target, &req)))
        return;
    READ_CTX *rc = ReadCtx(req);
    rc->Device = Device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = req;
    if (!NT_SUCCESS(WdfMemoryCreatePreallocated(&attr, rc->Buf, sizeof(rc->Buf), &mem)) ||
        !NT_SUCCESS(WdfIoTargetFormatRequestForInternalIoctl(dc->Target, req, IOCTL_HID_READ_REPORT, NULL, NULL, mem, NULL))) {
        WdfObjectDelete(req);
        return;
    }
    WdfRequestSetCompletionRoutine(req, EvtReadComplete, NULL);
    if (!WdfRequestSend(req, dc->Target, WDF_NO_SEND_OPTIONS))
        WdfObjectDelete(req);
    /* ponytail: no recovery timer like AmtPtpHidFilter has; a failed lower read leaves one upper read
     * waiting until the next one arrives. Add a retry timer if reads stall after resume. */
}

VOID EvtReadComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    READ_CTX *rc = ReadCtx(Request);
    WDFDEVICE device = rc->Device;
    NTSTATUS status = Params->IoStatus.Status;
    UCHAR usb[DS4_USB_INPUT_SIZE];
    size_t n = 0;
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    if (NT_SUCCESS(status))
        n = ds4_bt_in_to_usb(rc->Buf, Params->IoStatus.Information, usb, sizeof(usb));
    WdfObjectDelete(Request);

    if (!NT_SUCCESS(status))
        return;                 /* device going away or powering down */
    if (!n) {                   /* not an input report we map (e.g. audio 0x12-0x19): read again */
        IssueRead(device);
        return;
    }

    WDFREQUEST upper;
    PVOID out;
    size_t cap;
    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(DevCtx(device)->ReadQueue, &upper)))
        return;
    status = WdfRequestRetrieveOutputBuffer(upper, n, &out, &cap);
    if (NT_SUCCESS(status))
        RtlCopyMemory(out, usb, n);
    WdfRequestCompleteWithInformation(upper, status, NT_SUCCESS(status) ? n : 0);
}

NTSTATUS EvtSelfManagedIoInit(WDFDEVICE Device)
{
    NTSTATUS status = DetourHidBth(Device);
    if (!NT_SUCCESS(status)) {
        LOG("lower driver is not a HidClass minidriver, status 0x%08X", status);
        return status;
    }

    /* Reading calibration switches the pad from the short 0x01 to the full 0x11 input report. */
    status = SendXfer(Device, IOCTL_HID_GET_FEATURE, 0x05, NULL, DS4_BT_CALIB_SIZE, NULL);
    LOG("extended mode kick, status 0x%08X", status);

    DevCtx(Device)->Ready = TRUE;
    ULONG queued = 0;
    WdfIoQueueGetState(DevCtx(Device)->ReadQueue, &queued, NULL);
    while (queued--)
        IssueRead(Device);      /* reads that arrived before the detour */
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    WDFREQUEST req;
    UNREFERENCED_PARAMETER(TargetState);

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(DevCtx(Device)->ReadQueue, &req)))
        WdfRequestComplete(req, STATUS_CANCELLED);
    return STATUS_SUCCESS;
}

static NTSTATUS CopyOut(WDFREQUEST Request, const void *Src, size_t Len)
{
    WDFMEMORY mem;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(Request, &mem);
    if (NT_SUCCESS(status))
        status = WdfMemoryCopyFromBuffer(mem, 0, (PVOID)Src, Len);
    if (NT_SUCCESS(status))
        WdfRequestSetInformation(Request, Len);
    return status;
}

static BOOLEAN Forward(WDFREQUEST Request, WDFIOTARGET Target)
{
    WDF_REQUEST_SEND_OPTIONS opts;
    WdfRequestFormatRequestUsingCurrentType(Request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    return WdfRequestSend(Request, Target, &opts);
}

VOID EvtInternalIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEV_CTX *dc = DevCtx(device);
    NTSTATUS status;
    PHID_XFER_PACKET pkt;
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR d = { 0 };
        d.bLength = sizeof(d);
        d.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
        d.bcdHID = 0x0111;
        d.bNumDescriptors = 1;
        d.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
        d.DescriptorList[0].wReportLength = sizeof(ds4_usb_report_descriptor);
        status = CopyOut(Request, &d, sizeof(d));
        break;
    }
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = CopyOut(Request, ds4_usb_report_descriptor, sizeof(ds4_usb_report_descriptor));
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:   /* real VID/PID from HidBth */
    case IOCTL_HID_GET_STRING:
    case IOCTL_HID_GET_INDEXED_STRING:
    case IOCTL_HID_SET_FEATURE:
        if (Forward(Request, dc->Target))
            return;
        status = WdfRequestGetStatus(Request);
        break;

    case IOCTL_HID_READ_REPORT:
        status = WdfRequestForwardToIoQueue(Request, dc->ReadQueue);
        if (NT_SUCCESS(status)) {
            if (dc->Ready)
                IssueRead(device);
            return;
        }
        break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT: {
        UCHAR bt[DS4_BT_REPORT_SIZE];
        pkt = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
        size_t n = pkt ? ds4_usb_out_to_bt(pkt->reportBuffer, pkt->reportBufferLen, bt, sizeof(bt)) : 0;
        if (!n) {
            if (Forward(Request, dc->Target))
                return;
            status = WdfRequestGetStatus(Request);
            break;
        }
        status = SendXfer(device, Code, bt[0], bt, (ULONG)n, Request);
        if (status == STATUS_PENDING)
            return;
        break;
    }

    case IOCTL_HID_GET_FEATURE:
        pkt = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
        if (!pkt || pkt->reportId != 0x02) {
            if (Forward(Request, dc->Target))
                return;
            status = WdfRequestGetStatus(Request);
            break;
        }
        /* USB calibration 0x02 is BT 0x05 (same data + CRC) */
        status = SendXfer(device, Code, 0x05, NULL, DS4_BT_CALIB_SIZE, Request);
        if (status == STATUS_PENDING)
            return;
        break;

    default:    /* activate/deactivate, idle notification, GET_INPUT_REPORT: same as AmtPtpHidFilter */
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    WdfRequestComplete(Request, status);
}
