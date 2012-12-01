/**
 * Copyright (C) 2009-2012, Shao Miller <sha0.miller@gmail.com>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, originally derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Main bus specifics
 */

#include <stdio.h>
#include <ntddk.h>
#include <scsi.h>

#include "portable.h"
#include "winvblock.h"
#include "thread.h"
#include "wv_stdlib.h"
#include "wv_string.h"
#include "irp.h"
#include "driver.h"
#include "bus.h"
#include "device.h"
#include "disk.h"
#include "registry.h"
#include "mount.h"
#include "x86.h"
#include "safehook.h"
#include "filedisk.h"
#include "dummy.h"
#include "memdisk.h"
#include "debug.h"

/* Names for the main bus. */
#define WV_M_BUS_NAME (L"\\Device\\" WVL_M_WLIT)
#define WV_M_BUS_DOSNAME (L"\\DosDevices\\" WVL_M_WLIT)

/** Object types */
typedef struct S_WV_MAIN_BUS S_WV_MAIN_BUS;

/** Public functions */
NTSTATUS STDCALL WvBusEstablish(IN PUNICODE_STRING);
NTSTATUS STDCALL WvBusDevCtl(IN PIRP, IN ULONG POINTER_ALIGNMENT);
WVL_F_BUS_PNP WvBusPnpQueryDevText;

/** Public objects */
UNICODE_STRING WvBusName = {
    sizeof WV_M_BUS_NAME - sizeof (WCHAR),
    sizeof WV_M_BUS_NAME - sizeof (WCHAR),
    WV_M_BUS_NAME
  };
UNICODE_STRING WvBusDosname = {
    sizeof WV_M_BUS_DOSNAME - sizeof (WCHAR),
    sizeof WV_M_BUS_DOSNAME - sizeof (WCHAR),
    WV_M_BUS_DOSNAME
  };
/* The main bus. */
WVL_S_BUS_T WvBus = {0};
WV_S_DEV_T WvBusDev = {0};

/** Private function declarations */

/* TODO: DRIVER_ADD_DEVICE isn't available in DDK 3790.1830, it seems */
static DRIVER_ADD_DEVICE WvMainBusDriveDevice;
static DRIVER_UNLOAD WvMainBusUnload;
static F_WVL_DEVICE_THREAD_FUNCTION WvMainBusDriveDeviceInThread;
static VOID WvMainBusOldProbe(DEVICE_OBJECT * DeviceObject);

/** Struct/union type definitions */

/** The bus extension type */
struct S_WV_MAIN_BUS {
    /** This must be the first member of all extension types */
    WV_S_DEV_EXT DeviceExtension[1];

    /** The lower device this FDO is attached to, if any */
    DEVICE_OBJECT * LowerDeviceObject;
  };

/** Private objects */

static WVL_S_BUS_NODE WvBusSafeHookChild;

/** This mini-driver */
static S_WVL_MINI_DRIVER * WvMainBusMiniDriver;

/** The main bus */
static DEVICE_OBJECT * WvMainBusDevice;

/**
 * The mini-driver entry-point
 *
 * @param drv_obj
 *   The driver object provided by the caller
 *
 * @param reg_path
 *   The Registry path provided by the caller
 *
 * @return
 *   The status
 */
NTSTATUS STDCALL WvMainBusDriverEntry(
    IN DRIVER_OBJECT * drv_obj,
    IN UNICODE_STRING * reg_path
  ) {
    NTSTATUS status;

    /* Register this mini-driver */
    status = WvlRegisterMiniDriver(
        &WvMainBusMiniDriver,
        drv_obj,
        WvMainBusDriveDevice,
        WvMainBusUnload
      );
    if (!NT_SUCCESS(status))
      goto err_register;
    ASSERT(WvMainBusMiniDriver);

    /* Create the bus FDO and possibly the PDO */
    status = WvBusEstablish(reg_path);
    if (!NT_SUCCESS(status))
      goto err_bus;

    /* Do not introduce more resource acquisition, here */

    return status;

    /* Nothing to do.  See comment, above */
    err_bus:

    WvlDeregisterMiniDriver(WvMainBusMiniDriver);
    err_register:

    return status;
  }

/**
 * Drive a PDO with the main bus FDO
 *
 * @param DriverObject
 *   The driver object provided by the caller
 *
 * @param PhysicalDeviceObject
 *   The PDO to probe and attach the main bus FDO to
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS STDCALL WvMainBusDriveDevice(
    IN DRIVER_OBJECT * drv_obj,
    IN DEVICE_OBJECT * pdo
  ) {
    ASSERT(drv_obj);
    (VOID) drv_obj;
    ASSERT(WvMainBusDevice);
    return WvlCallFunctionInDeviceThread(
        WvMainBusDevice,
        WvMainBusDriveDeviceInThread,
        pdo,
        TRUE
      );
  }

/**
 * Release resources and unwind state
 *
 * @param DriverObject
 *   The driver object provided by the caller
 */
static VOID STDCALL WvMainBusUnload(IN DRIVER_OBJECT * drv_obj) {
    ASSERT(drv_obj);
    (VOID) drv_obj;
    WvlDeregisterMiniDriver(WvMainBusMiniDriver);
    return;
  }

/**
 * Drive a PDO with the main bus FDO
 *
 * @param DeviceObject
 *   The main bus FDO
 *
 * @param Context
 *   The PDO to probe and attach the main bus FDO to
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS WvMainBusDriveDeviceInThread(
    DEVICE_OBJECT * dev_obj,
    VOID * context
  ) {
    DEVICE_OBJECT * const pdo = context;
    S_WV_MAIN_BUS * bus;
    NTSTATUS status;
    DEVICE_OBJECT * lower;
    

    ASSERT(dev_obj);
    bus = dev_obj->DeviceExtension;
    ASSERT(bus);
    ASSERT(pdo);

    /* Assume failure */
    status = STATUS_NOT_SUPPORTED;

    /*
     * If we're already attached, reject the request.  If the PDO
     * belongs to us, also reject the request
     */
    if (bus->LowerDeviceObject || pdo->DriverObject == dev_obj->DriverObject) {
        DBG("Refusing to drive PDO %p\n", (VOID *) pdo);
        goto err_params;
      }

    /* Attach the FDO to the PDO */
    lower = IoAttachDeviceToDeviceStack(dev_obj, pdo);
    if (!lower) {
        DBG("Error driving PDO %p!\n", (VOID *) pdo);
        goto err_lower;
      }
    bus->LowerDeviceObject = lower;

    /* TODO: Remove these once this when the modules are mini-drivers */
    WvBus.LowerDeviceObject = lower;
    WvBus.Pdo = pdo;
    WvBus.State = WvlBusStateStarted;
    WvMainBusOldProbe(dev_obj);

    DBG("Driving PDO %p\n", (VOID *) pdo);
    return STATUS_SUCCESS;

    IoDetachDevice(lower);
    err_lower:

    err_params:

    return status;
  }

/**
 * Probe for default children of the main bus
 *
 * @param DeviceObject
 *   The main bus FDO
 */
static VOID WvMainBusOldProbe(DEVICE_OBJECT * dev_obj) {
    S_X86_SEG16OFF16 int_13h;
    PDEVICE_OBJECT safe_hook_child;

    ASSERT(dev_obj);

    /* Probe for children */
    int_13h.Segment = 0;
    int_13h.Offset = 0x13 * sizeof int_13h;
    safe_hook_child = WvSafeHookProbe(&int_13h, dev_obj);
    if (safe_hook_child) {
        WvlBusInitNode(&WvBusSafeHookChild, safe_hook_child);
        WvlBusAddNode(&WvBus, &WvBusSafeHookChild);
      }
    WvMemdiskFind();
  }

/* Handle an IRP. */
static NTSTATUS WvBusIrpDispatch(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    PIO_STACK_LOCATION io_stack_loc;

    io_stack_loc = IoGetCurrentIrpStackLocation(irp);
    switch (io_stack_loc->MajorFunction) {
        case IRP_MJ_PNP:
          return WvlBusPnp(&WvBus, irp);

        case IRP_MJ_DEVICE_CONTROL: {
            ULONG POINTER_ALIGNMENT code;

            code = io_stack_loc->Parameters.DeviceIoControl.IoControlCode;
            return WvBusDevCtl(irp, code);
          }

        case IRP_MJ_POWER:
          return WvlIrpPassPowerToLower(WvBus.LowerDeviceObject, irp);

        case IRP_MJ_CREATE:
        case IRP_MJ_CLOSE:
          /* Always succeed with nothing to do. */
          return WvlIrpComplete(irp, 0, STATUS_SUCCESS);

        case IRP_MJ_SYSTEM_CONTROL:
          return WvlIrpPassToLower(WvBus.LowerDeviceObject, irp);

        default:
          ;
      }
    return WvlIrpComplete(irp, 0, STATUS_NOT_SUPPORTED);
  }

/**
 * Establish the bus FDO, thread, and possibly PDO
 *
 * @param RegistryPath
 *   Points to the unicode string for the driver's parameters
 *
 * @return
 *   The status of the operation
 */
NTSTATUS STDCALL WvBusEstablish(IN UNICODE_STRING * reg_path) {
    NTSTATUS status;
    S_WV_MAIN_BUS * bus;
    PDEVICE_OBJECT fdo = NULL;
    HANDLE reg_key;
    UINT32 pdo_done = 0;
    PDEVICE_OBJECT pdo = NULL;

    /* Initialize the bus */
    WvlBusInit(&WvBus);
    WvDevInit(&WvBusDev);

    /* Create the bus FDO */
    status = WvlCreateDevice(
        WvMainBusMiniDriver,
        sizeof *bus,
        &WvBusName,
        FILE_DEVICE_CONTROLLER,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &fdo
      );
    if (!NT_SUCCESS(status)) {
        DBG("WvlCreateDevice() failed!\n");
        goto err_fdo;
      }
    ASSERT(fdo);
    WvMainBusDevice = WvBusDev.Self = WvBus.Fdo = fdo;
    bus = fdo->DeviceExtension;
    ASSERT(bus);
    WvBusDev.IsBus = TRUE;
    WvBus.QueryDevText = WvBusPnpQueryDevText;
    WvDevForDevObj(WvBus.Fdo, &WvBusDev);
    WvDevSetIrpHandler(WvBus.Fdo, WvBusIrpDispatch);
    /* TODO: Review the following two settings */
    WvBus.Fdo->Flags |= DO_DIRECT_IO;
    WvBus.Fdo->Flags |= DO_POWER_INRUSH;
    WvBus.Fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    #ifdef RIS
    WvBus.Dev.State = Started;
    #endif

    /* DosDevice symlink */
    status = IoCreateSymbolicLink(
        &WvBusDosname,
        &WvBusName
      );
    if (!NT_SUCCESS(status)) {
        DBG("IoCreateSymbolicLink() failed!\n");
        goto err_dos_symlink;
      }

    /* Open our Registry path */
    status = WvlRegOpenKey(reg_path->Buffer, &reg_key);
    if (!NT_SUCCESS(status)) {
        DBG("Couldn't open Registry path!\n");
        goto err_reg;
      }

    /* Check the Registry to see if we've already got a PDO */
    status = WvlRegFetchDword(reg_key, L"PdoDone", &pdo_done);
    if (NT_SUCCESS(status) && pdo_done) {
        WvlRegCloseKey(reg_key);
        return status;
      }

    /* If not, create a root-enumerated PDO for our bus */
    IoReportDetectedDevice(
        WvDriverObj,
        InterfaceTypeUndefined,
        -1,
        -1,
        NULL,
        NULL,
        FALSE,
        &pdo
      );
    if (pdo == NULL) {
        DBG("IoReportDetectedDevice() went wrong!  Exiting\n");
        status = STATUS_UNSUCCESSFUL;
        goto err_pdo;
      }

    /* Remember that we have a PDO for next time */
    status = WvlRegStoreDword(reg_key, L"PdoDone", 1);
    if (!NT_SUCCESS(status)) {
        DBG("Couldn't save PdoDone to Registry.  Oh well\n");
      }
    /* Attach FDO to PDO */
    status = WvMainBusDriveDevice(WvMainBusDevice->DriverObject, pdo);
    if (!NT_SUCCESS(status)) {
        DBG("WvMainBusDriveDevice() went wrong!\n");
        goto err_attach;
      }
    /* PDO created, FDO attached.  All done */
    return STATUS_SUCCESS;

    err_attach:

    /* TODO: Should we really delete an IoReportDetectedDevice() device? */
    IoDeleteDevice(pdo);
    err_pdo:

    WvlRegCloseKey(reg_key);
    err_reg:

    IoDeleteSymbolicLink(&WvBusDosname);
    err_dos_symlink:

    WvlDeleteDevice(WvBus.Fdo);
    WvBus.Fdo = NULL;
    err_fdo:

    return status;
  }

/* TODO: Cosmetic changes */
BOOLEAN STDCALL WvBusAddDev(
    IN OUT WV_SP_DEV_T Dev
  ) {
    /* The new node's device object. */
    PDEVICE_OBJECT dev_obj;

    DBG("Entry\n");
    if (!WvBus.Fdo || !Dev) {
        DBG("No bus or no device!\n");
        return FALSE;
      }
    /* Create the child device, if needed. */
    dev_obj = Dev->Self;
    if (!dev_obj) {
        dev_obj = WvDevCreatePdo(Dev);
        if (!dev_obj) {
            DBG("PDO creation failed!\n");
            return FALSE;
          }
      }
    WvlBusInitNode(&Dev->BusNode, dev_obj);
    /* Associate the parent bus. */
    Dev->Parent = WvBus.Fdo;
    /*
     * Initialize the device.  For disks, this routine is responsible for
     * determining the disk's geometry appropriately for RAM/file disks.
     */
    if (Dev->Ops.Init)
      Dev->Ops.Init(Dev);
    dev_obj->Flags &= ~DO_DEVICE_INITIALIZING;
    /* Add the new PDO device to the bus' list of children. */
    WvlBusAddNode(&WvBus, &Dev->BusNode);

    DBG("Added device %p with PDO %p.\n", Dev, Dev->Self);
    return TRUE;
  }

/**
 * Remove a device node from the WinVBlock bus.
 *
 * @v Dev               The device to remove.
 * @ret NTSTATUS        The status of the operation.
 */
NTSTATUS STDCALL WvBusRemoveDev(IN WV_SP_DEV_T Dev) {
    NTSTATUS status;

    if (!Dev->BusNode.Linked) {
        WvDevClose(Dev);
        IoDeleteDevice(Dev->Self);
        WvDevFree(Dev);
      } else
        WvlBusRemoveNode(&Dev->BusNode);
    return STATUS_SUCCESS;
  }

static NTSTATUS STDCALL WvBusDevCtlDetach(
    IN PIRP irp
  ) {
    UINT32 unit_num;
    WVL_SP_BUS_NODE walker;
    WV_SP_DEV_T dev = NULL;

    unit_num = *((PUINT32) irp->AssociatedIrp.SystemBuffer);
    DBG("Request to detach unit %d...\n", unit_num);

    walker = NULL;
    /* For each node on the bus... */
    WvlBusLock(&WvBus);
    while (walker = WvlBusGetNextNode(&WvBus, walker)) {
        /* If the unit number matches... */
        if (WvlBusGetNodeNum(walker) == unit_num) {
            dev = WvDevFromDevObj(WvlBusGetNodePdo(walker));
            /* If it's not a boot-time device... */
            if (dev->Boot) {
                DBG("Cannot detach a boot-time device.\n");
                /* Signal error. */
                dev = NULL;
              }
            break;
          }
      }
    WvlBusUnlock(&WvBus);
    if (!dev) {
        DBG("Unit %d not found.\n", unit_num);
        return WvlIrpComplete(irp, 0, STATUS_INVALID_PARAMETER);
      }
    /* Detach the node. */
    WvBusRemoveDev(dev);
    DBG("Removed unit %d.\n", unit_num);
    return WvlIrpComplete(irp, 0, STATUS_SUCCESS);
  }

NTSTATUS STDCALL WvBusDevCtl(
    IN PIRP irp,
    IN ULONG POINTER_ALIGNMENT code
  ) {
    NTSTATUS status;

    switch (code) {
        case IOCTL_FILE_ATTACH:
          status = WvFilediskAttach(irp);
          break;

        case IOCTL_FILE_DETACH:
          return WvBusDevCtlDetach(irp);

        case IOCTL_WV_DUMMY:
          return WvDummyIoctl(irp);

        default:
          irp->IoStatus.Information = 0;
          status = STATUS_INVALID_DEVICE_REQUEST;
      }

    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
  }

NTSTATUS STDCALL WvBusPnpQueryDevText(
    IN WVL_SP_BUS_T bus,
    IN PIRP irp
  ) {
    WCHAR (*str)[512];
    PIO_STACK_LOCATION io_stack_loc = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;
    UINT32 str_len;

    /* Allocate a string buffer. */
    str = wv_mallocz(sizeof *str);
    if (str == NULL) {
        DBG("wv_malloc IRP_MN_QUERY_DEVICE_TEXT\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto alloc_str;
      }
    /* Determine the query type. */
    switch (io_stack_loc->Parameters.QueryDeviceText.DeviceTextType) {
        case DeviceTextDescription:
          str_len = swprintf(*str, WVL_M_WLIT L" Bus") + 1;
          irp->IoStatus.Information =
            (ULONG_PTR) wv_palloc(str_len * sizeof **str);
          if (irp->IoStatus.Information == 0) {
              DBG("wv_palloc DeviceTextDescription\n");
              status = STATUS_INSUFFICIENT_RESOURCES;
              goto alloc_info;
            }
          RtlCopyMemory(
              (PWCHAR) irp->IoStatus.Information,
              str,
              str_len * sizeof **str
            );
          status = STATUS_SUCCESS;
          goto alloc_info;

        default:
          irp->IoStatus.Information = 0;
          status = STATUS_NOT_SUPPORTED;
      }
    /* irp->IoStatus.Information not freed. */
    alloc_info:

    wv_free(str);
    alloc_str:

    return WvlIrpComplete(irp, irp->IoStatus.Information, status);
  }

WVL_M_LIB DEVICE_OBJECT * WvBusFdo(void) {
    return WvBus.Fdo;
  }

/**
 * Signal the main bus that it's time to go away
 */
VOID WvMainBusRemove(void) {
    /* This will be called from the main bus' thread */
    ASSERT(WvMainBusDevice);
    IoDeleteSymbolicLink(&WvBusDosname);
    WvlDeleteDevice(WvMainBusDevice);
  }