/**
 * Copyright (C) 2009, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
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
 * Driver specifics
 *
 */

#include <stdio.h>
#include <ntddk.h>

#include "winvblock.h"
#include "portable.h"
#include "irp.h"
#include "driver.h"
#include "disk.h"
#include "registry.h"
#include "protocol.h"
#include "mount.h"
#include "aoe.h"
#include "bus.h"
#include "debug.h"

/* in this file */
static NTSTATUS STDCALL Driver_DispatchNotSupported (
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp
 );

static NTSTATUS STDCALL Driver_Dispatch (
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp
 );

static void STDCALL Driver_Unload (
  IN PDRIVER_OBJECT DriverObject
 );

static void *Driver_Globals_StateHandle;
static winvblock__bool Driver_Globals_Started = FALSE;

PDRIVER_OBJECT driver__obj_ptr = NULL;

/* Contains TXTSETUP.SIF/BOOT.INI-style OsLoadOptions parameters */
LPWSTR os_load_opts = NULL;

static LPWSTR STDCALL
get_opt (
  IN LPWSTR opt_name
 )
{
  LPWSTR our_opts,
   the_opt;
  WCHAR our_sig[] = L"WINVBLOCK=";
/* To produce constant integer expressions */
  enum
  {
    our_sig_len_bytes = sizeof ( our_sig ) - sizeof ( WCHAR ),
    our_sig_len = our_sig_len_bytes / sizeof ( WCHAR )
  };
  size_t opt_name_len,
   opt_name_len_bytes;

  if ( !os_load_opts || !opt_name )
    return NULL;

  /*
   * Find /WINVBLOCK= options
   */
  our_opts = os_load_opts;
  while ( *our_opts != L'\0' )
    {
      if ( RtlCompareMemory ( our_opts, our_sig, our_sig_len_bytes ) !=
	   our_sig_len_bytes )
	{
	  our_opts++;
	  continue;
	}
      our_opts += our_sig_len;
      break;
    }

  /*
   * Search for the specific option
   */
  the_opt = our_opts;
  opt_name_len = wcslen ( opt_name );
  opt_name_len_bytes = opt_name_len * sizeof ( WCHAR );
  while ( *the_opt != L'\0' && *the_opt != L' ' )
    {
      if ( RtlCompareMemory ( the_opt, opt_name, opt_name_len_bytes ) !=
	   opt_name_len_bytes )
	{
	  while ( *the_opt != L'\0' && *the_opt != L' ' && *the_opt != L',' )
	    the_opt++;
	  continue;
	}
      the_opt += opt_name_len;
      break;
    }

  if ( *the_opt == L'\0' || *the_opt == L' ' )
    return NULL;

  /*
   * Next should come "=" 
   */
  if ( *the_opt != L'=' )
    return NULL;

  /*
   * And finally our option's value.  The caller needs
   * to worry about looking past the end of the option 
   */
  the_opt++;
  if ( *the_opt == L'\0' || *the_opt == L' ' )
    return NULL;
  return the_opt;
}

/*
 * Note the exception to the function naming convention.
 * TODO: See if a Makefile change is good enough
 */
NTSTATUS STDCALL
DriverEntry (
  IN PDRIVER_OBJECT DriverObject,
  IN PUNICODE_STRING RegistryPath
 )
{
  NTSTATUS Status;
  int i;
  PDEVICE_OBJECT bus_pdo_ptr = NULL;

  DBG ( "Entry\n" );
  if ( driver__obj_ptr )
    {
      DBG ( "Re-entry not allowed!\n" );
      return STATUS_NOT_SUPPORTED;
    }
  driver__obj_ptr = DriverObject;
  if ( Driver_Globals_Started )
    return STATUS_SUCCESS;
  Debug_Initialize (  );
  if ( !NT_SUCCESS ( Status = registry__note_os_load_opts ( &os_load_opts ) ) )
    return Error ( "registry__note_os_load_opts", Status );

  Driver_Globals_StateHandle = NULL;

  if ( ( Driver_Globals_StateHandle =
	 PoRegisterSystemState ( NULL, ES_CONTINUOUS ) ) == NULL )
    {
      DBG ( "Could not set system state to ES_CONTINUOUS!!\n" );
    }

  /*
   * Set up IRP MajorFunction function table for devices
   * this driver handles
   */
  for ( i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++ )
    DriverObject->MajorFunction[i] = Driver_DispatchNotSupported;
  DriverObject->MajorFunction[IRP_MJ_PNP] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_POWER] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_CREATE] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Driver_Dispatch;
  DriverObject->MajorFunction[IRP_MJ_SCSI] = Driver_Dispatch;
  /*
   * Other functions this driver handles
   */
  DriverObject->DriverExtension->AddDevice = Bus_AddDevice;
  DriverObject->DriverUnload = Driver_Unload;

  /*
   * Always create the root-enumerated bus device 
   */
  IoReportDetectedDevice ( DriverObject, InterfaceTypeUndefined, -1, -1, NULL,
			   NULL, FALSE, &bus_pdo_ptr );
  if ( !NT_SUCCESS ( Status = Bus_AddDevice ( DriverObject, bus_pdo_ptr ) ) )
    {
      Protocol_Stop (  );
      AoE_Stop (  );
      return Error ( "Bus_AddDevice", Status );
    }

  Driver_Globals_Started = TRUE;
  DBG ( "Exit\n" );
  return STATUS_SUCCESS;
}

static NTSTATUS STDCALL
Driver_DispatchNotSupported (
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp
 )
{
  Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  return Irp->IoStatus.Status;
}

static
irp__handler_decl (
  create_close
 )
{
  NTSTATUS status = STATUS_SUCCESS;

  Irp->IoStatus.Status = status;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return status;
}

static
irp__handler_decl (
  not_supported
 )
{
  NTSTATUS status = STATUS_NOT_SUPPORTED;
  Irp->IoStatus.Status = status;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return status;
}

irp__handling driver__handling_table[] = {
  /*
   * Major, minor, any major?, any minor?, handler
   * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   * Note that the fall-through case must come FIRST!
   * Why? It sets completion to true, so others won't be called
   */
  {0, 0, TRUE, TRUE, not_supported}
  ,
  {IRP_MJ_CLOSE, 0, FALSE, TRUE, create_close}
  ,
  {IRP_MJ_CREATE, 0, FALSE, TRUE, create_close}
};

size_t driver__handling_table_size = sizeof ( driver__handling_table );

static NTSTATUS STDCALL
Driver_Dispatch (
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp
 )
{
  NTSTATUS status;
  PIO_STACK_LOCATION Stack;
  driver__dev_ext_ptr DeviceExtension;
  size_t irp_handler_index;
  winvblock__bool completion = FALSE;

#ifdef DEBUGIRPS
  Debug_IrpStart ( DeviceObject, Irp );
#endif
  Stack = IoGetCurrentIrpStackLocation ( Irp );
  DeviceExtension = ( driver__dev_ext_ptr ) DeviceObject->DeviceExtension;

  /*
   * We handle IRP_MJ_POWER as an exception 
   */
  if ( DeviceExtension->State == Deleted )
    {
      if ( Stack->MajorFunction == IRP_MJ_POWER )
	PoStartNextPowerIrp ( Irp );
      Irp->IoStatus.Information = 0;
      Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
      IoCompleteRequest ( Irp, IO_NO_INCREMENT );
#ifdef DEBUGIRPS
      Debug_IrpEnd ( Irp, STATUS_NO_SUCH_DEVICE );
#endif
      return STATUS_NO_SUCH_DEVICE;
    }

  status =
    irp__process ( DeviceObject, Irp, Stack, DeviceExtension, &completion );

#ifdef DEBUGIRPS
  if ( status != STATUS_PENDING )
    Debug_IrpEnd ( Irp, status );
#endif

  return status;
}

static void STDCALL
Driver_Unload (
  IN PDRIVER_OBJECT DriverObject
 )
{
  if ( Driver_Globals_StateHandle != NULL )
    PoUnregisterSystemState ( Driver_Globals_StateHandle );
  Protocol_Stop (  );
  AoE_Stop (  );
  Bus_Stop (  );
  ExFreePool ( os_load_opts );
  Driver_Globals_Started = FALSE;
  DBG ( "Done\n" );
}

winvblock__lib_func void STDCALL
Driver_CompletePendingIrp (
  IN PIRP Irp
 )
{
#ifdef DEBUGIRPS
  Debug_IrpEnd ( Irp, Irp->IoStatus.Status );
#endif
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
}

/*
 * Note the exception to the function naming convention
 */
winvblock__lib_func NTSTATUS STDCALL
Error (
  IN PCHAR Message,
  IN NTSTATUS Status
 )
{
  DBG ( "%s: 0x%08x\n", Message, Status );
  return Status;
}