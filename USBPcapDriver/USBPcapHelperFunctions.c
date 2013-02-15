/*
 *  Copyright (c) 2013 Tomasz Moń <desowin@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses>.
 */

#include "USBPcapMain.h"
#include "USBPcapHelperFunctions.h"

/*
 * Retrieves PDO for a device.
 *
 * If function succeesses, the pdo must be dereferenced when no longer
 * needed.
 */
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapGetTargetDevicePdo(IN PDEVICE_OBJECT DeviceObject,
                                   OUT PDEVICE_OBJECT *pdo)
{
    KEVENT              event;
    NTSTATUS            status;
    PIRP                irp;
    IO_STATUS_BLOCK     ioStatusBlock;
    PIO_STACK_LOCATION  irpStack;
    PDEVICE_RELATIONS   deviceRelations;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       DeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &event,
                                       &ioStatusBlock);
    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
    irpStack->Parameters.QueryDeviceRelations.Type = TargetDeviceRelation;

    /*
     * Initialize the status to error in case the bus driver decides not to
     * set it correctly.
     */
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        /* Wait without timeout */
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatusBlock.Status;
    }

    if (NT_SUCCESS(status)) {
        deviceRelations = (PDEVICE_RELATIONS)ioStatusBlock.Information;
        ASSERT(deviceRelations);

        /*
         * You must dereference the PDO when it's no longer required.
         */
        *pdo = deviceRelations->Objects[0];
        ExFreePool(deviceRelations);
    }

    return status;
}

/*
 * Retrieves the parent device port for given device.
 * This function is rather a hack. It assumes the location information
 * for the PDO is in form Port_#XXXX.Hub_#YYYY.
 *
 * On Success, writes XXXX into port.
 */
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapGetTargetDevicePort(PDEVICE_OBJECT pdo_device,
                                    PULONG port)
{
    NTSTATUS status;
    UNICODE_STRING str;
    PWCHAR location = NULL;
    ULONG length;
    ULONG idx;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    /* Query location length */
    status = IoGetDeviceProperty(pdo_device,
                                 DevicePropertyLocationInformation,
                                 0, /* Empty buffer */
                                 (PVOID)location,
                                 &length);

    if (status != STATUS_BUFFER_TOO_SMALL)
    {
        DkDbgVal("Expected STATUS_BUFFER_TOO_SMALL", status);

        if (!NT_SUCCESS(status))
            return status;

        /*
         * IoGetDeviceProperty should have failed.
         * Do our best here to not confuse the caller with success status.
         *
         * This return statement should newer be executed.
         */
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    location = (PWCHAR)ExAllocatePoolWithTag(PagedPool,
                                             length,
                                             ' COL');

    status = IoGetDeviceProperty(pdo_device,
                                 DevicePropertyLocationInformation,
                                 length,
                                 (PVOID)location,
                                 &length);

    if (!NT_SUCCESS(status))
    {
        DkDbgVal("Failed to get location information", status);
        ExFreePool((PVOID)location);
        return status;
    }

    RtlInitUnicodeString(&str, (PCWSTR)location);

    DbgPrint("Location: %wZ\n", str);

    /*
     * Remove the text before first # (Port_#)
     */
    for (idx = 0; location[idx] != L'#' && idx < length; ++idx)
    {
        location[idx] = L' ';
    }
    location[idx] = L' '; /* Remove # as well */

    status = RtlUnicodeStringToInteger(&str, 0, port);

    DkDbgVal("Device is connected to port", *port);
    ExFreePool((PVOID)location);

    return status;
}

__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapGetNumberOfPorts(PDEVICE_OBJECT parent,
                                 PULONG numberOfPorts)
{
    KEVENT                event;
    PIRP                  irp;
    IO_STATUS_BLOCK       ioStatus;
    USB_NODE_INFORMATION  info;
    NTSTATUS              status;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    /* FIXME: check if parent is hub or composite device */
    info.NodeType = UsbHub;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_USB_GET_NODE_INFORMATION,
                                        parent,
                                        &info, sizeof(USB_NODE_INFORMATION),
                                        &info, sizeof(USB_NODE_INFORMATION),
                                        FALSE,
                                        &event,
                                        &ioStatus);

    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(parent, irp);

    if (status == STATUS_PENDING)
    {
        /* Wait without timeout */
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (!NT_SUCCESS(status))
    {
        DkDbgVal("IOCTL_USB_GET_NODE_INFORMATION failed", status);
        return status;
    }

    if (info.NodeType == UsbHub)
    {
        *numberOfPorts =
            info.u.HubInformation.HubDescriptor.bNumberOfPorts;
    }
    else
    {
        /* Composite device */
        *numberOfPorts =
            info.u.MiParentInformation.NumberOfInterfaces;
    }

    return status;
}

__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapGetNodeInformation(PDEVICE_OBJECT hub,
                                   ULONG port,
                                   USB_NODE_CONNECTION_INFORMATION *info)
{
    KEVENT event;
    PIRP irp;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    info->ConnectionIndex = port;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_USB_GET_NODE_CONNECTION_INFORMATION,
              hub,
              info, sizeof(USB_NODE_CONNECTION_INFORMATION),
              info, sizeof(USB_NODE_CONNECTION_INFORMATION),
              FALSE,
              &event,
              &ioStatus);

    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(hub, irp);
    if (status == STATUS_PENDING)
    {
        /* Wait for IRP to be completed */
        status = KeWaitForSingleObject(&event,
                                       Executive,  /* wait reason */
                                       KernelMode,
                                       FALSE,      /* not alertable */
                                       NULL);      /* no timeout */

        status = ioStatus.Status;
    }

    if (!NT_SUCCESS(status))
    {
        DkDbgVal("IOCTL_USB_GET_NODE_CONNECTION_INFORMATION failed", status);
    }
    else
    {
        KdPrint(("USB INFORMATION index: %d isHub: %d Address: %d Connection Status: %d \n",
                 info->ConnectionIndex,
                 info->DeviceIsHub,
                 info->DeviceAddress,
                 info->ConnectionStatus));
    }

    return status;
}

#if DBG
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapPrintUSBPChildrenInformation(PDEVICE_OBJECT hub)
{
    USB_NODE_CONNECTION_INFORMATION info;
    NTSTATUS status;
    ULONG maxIndex;
    ULONG idx;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    status = USBPcapGetNumberOfPorts(hub, &maxIndex);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("Failed to get number of ports. Code 0x%x\n", status));
        return status;
    }
    DkDbgVal("Got maximum index", maxIndex);

    for (idx = 1; idx <= maxIndex; idx++)
    {
        USBPcapGetNodeInformation(hub, idx, &info);
    }

    return STATUS_SUCCESS;
}
#endif

__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS USBPcapGetDeviceUSBAddress(PDEVICE_OBJECT hub,
                                    PDEVICE_OBJECT device,
                                    PUSHORT address)
{
    USB_NODE_CONNECTION_INFORMATION info;
    NTSTATUS status;
    ULONG maxIndex;
    ULONG idx;
    ULONG port;

    PDEVICE_OBJECT devicePdo;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    status = USBPcapGetTargetDevicePdo(device, &devicePdo);
    if (!NT_SUCCESS(status))
    {
        DkDbgStr("Failed to get target device PDO!");
        return status;
    }

    status = USBPcapGetTargetDevicePort(devicePdo, &port);
    ObDereferenceObject((PVOID)devicePdo);

    if (!NT_SUCCESS(status))
    {
        DkDbgStr("Failed to get target device Port!");
        return status;
    }

    status = USBPcapGetNodeInformation(hub, port, &info);

    if (NT_SUCCESS(status))
    {
        DkDbgVal("", info.DeviceAddress);
        *address = info.DeviceAddress;
    }
    else
    {
        DkDbgStr("Failed to get device address");
    }

    return status;
}
