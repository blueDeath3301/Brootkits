/*++

Module Name:

	DelProtect.c

Abstract:

	This is the main module of the DelProtect miniFilter driver.

Environment:

	Kernel mode

--*/

#include <fltKernel.h>
#include <dontuse.h>
#include "FastMutex.h"
#include "AutoLock.h"
#include "DelProtectCommon.h"

extern "C" NTSTATUS ZwQueryInformationProcess(
	_In_      HANDLE           ProcessHandle,
	_In_      PROCESSINFOCLASS ProcessInformationClass,
	_Out_     PVOID            ProcessInformation,
	_In_      ULONG            ProcessInformationLength,
	_Out_opt_ PULONG           ReturnLength
);

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

#define DRIVER_TAG 'PleD'

PFLT_FILTER gFilterHandle;
ULONG_PTR OperationStatusCtx = 1;

#define PTDBG_TRACE_ROUTINES            0x00000001
#define PTDBG_TRACE_OPERATION_STATUS    0x00000002

ULONG gTraceFlags = 0;

const int MaxExecutables = 32;

WCHAR* ExeNames[MaxExecutables];
int ExeNamesCount;
FastMutex ExeNamesLock;


#define PT_DBG_PRINT( _dbgLevel, _string )          \
	(FlagOn(gTraceFlags,(_dbgLevel)) ?              \
		DbgPrint _string :                          \
		((int)0))

/*************************************************************************
	Prototypes
*************************************************************************/

bool FindExecutable(PCWSTR name);


EXTERN_C_START

DRIVER_DISPATCH DelProtectCreateClose, DelProtectDeviceControl;
DRIVER_UNLOAD DelProtectUnloadDriver;

void ClearAll();


FLT_PREOP_CALLBACK_STATUS DelProtectPreCreate(_Inout_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, PVOID*);

FLT_PREOP_CALLBACK_STATUS DelProtectPreSetInformation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_Flt_CompletionContext_Outptr_ PVOID *CompletionContext);

DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_ PUNICODE_STRING RegistryPath
);

NTSTATUS
DelProtectInstanceSetup(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_SETUP_FLAGS Flags,
	_In_ DEVICE_TYPE VolumeDeviceType,
	_In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
);

VOID
DelProtectInstanceTeardownStart(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
);

VOID
DelProtectInstanceTeardownComplete(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
);

NTSTATUS
DelProtectUnload(
	_In_ FLT_FILTER_UNLOAD_FLAGS Flags
);

NTSTATUS
DelProtectInstanceQueryTeardown(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
);

FLT_PREOP_CALLBACK_STATUS
DelProtectPreOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_Flt_CompletionContext_Outptr_ PVOID *CompletionContext
);

VOID
DelProtectOperationStatusCallback(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ PFLT_IO_PARAMETER_BLOCK ParameterSnapshot,
	_In_ NTSTATUS OperationStatus,
	_In_ PVOID RequesterContext
);

FLT_POSTOP_CALLBACK_STATUS
DelProtectPostOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_opt_ PVOID CompletionContext,
	_In_ FLT_POST_OPERATION_FLAGS Flags
);

FLT_PREOP_CALLBACK_STATUS
DelProtectPreOperationNoPostOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_Flt_CompletionContext_Outptr_ PVOID *CompletionContext
);

BOOLEAN
DelProtectDoRequestOperationStatus(
	_In_ PFLT_CALLBACK_DATA Data
);

EXTERN_C_END

//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DelProtectUnload)
#pragma alloc_text(PAGE, DelProtectInstanceQueryTeardown)
#pragma alloc_text(PAGE, DelProtectInstanceSetup)
#pragma alloc_text(PAGE, DelProtectInstanceTeardownStart)
#pragma alloc_text(PAGE, DelProtectInstanceTeardownComplete)
#endif

//
//  operation registration
//

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {
	{ IRP_MJ_CREATE, 0, DelProtectPreCreate, nullptr },
	{ IRP_MJ_SET_INFORMATION, 0, DelProtectPreSetInformation, nullptr },
	{ IRP_MJ_OPERATION_END }
};

//
//  This defines what we want to filter with FltMgr
//

CONST FLT_REGISTRATION FilterRegistration = {

	sizeof(FLT_REGISTRATION),
	FLT_REGISTRATION_VERSION,
	0,                       //  Flags

	nullptr,                 //  Context
	Callbacks,               //  Operation callbacks

	DelProtectUnload,                   //  MiniFilterUnload

	DelProtectInstanceSetup,            //  InstanceSetup
	DelProtectInstanceQueryTeardown,    //  InstanceQueryTeardown
	DelProtectInstanceTeardownStart,    //  InstanceTeardownStart
	DelProtectInstanceTeardownComplete, //  InstanceTeardownComplete
};



NTSTATUS
DelProtectInstanceSetup(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_SETUP_FLAGS Flags,
	_In_ DEVICE_TYPE VolumeDeviceType,
	_In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
)
/*++

Routine Description:

	This routine is called whenever a new instance is created on a volume. This
	gives us a chance to decide if we need to attach to this volume or not.

	If this routine is not defined in the registration structure, automatic
	instances are always created.

Arguments:

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance and its associated volume.

	Flags - Flags describing the reason for this attach request.

Return Value:

	STATUS_SUCCESS - attach
	STATUS_FLT_DO_NOT_ATTACH - do not attach

--*/
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);
	UNREFERENCED_PARAMETER(VolumeDeviceType);
	UNREFERENCED_PARAMETER(VolumeFilesystemType);

	PAGED_CODE();

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectInstanceSetup: Entered\n"));

	return STATUS_SUCCESS;
}


NTSTATUS
DelProtectInstanceQueryTeardown(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
)
/*++

Routine Description:

	This is called when an instance is being manually deleted by a
	call to FltDetachVolume or FilterDetach thereby giving us a
	chance to fail that detach request.

	If this routine is not defined in the registration structure, explicit
	detach requests via FltDetachVolume or FilterDetach will always be
	failed.

Arguments:

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance and its associated volume.

	Flags - Indicating where this detach request came from.

Return Value:

	Returns the status of this operation.

--*/
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);

	PAGED_CODE();

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectInstanceQueryTeardown: Entered\n"));

	return STATUS_SUCCESS;
}


VOID
DelProtectInstanceTeardownStart(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
)
/*++

Routine Description:

	This routine is called at the start of instance teardown.

Arguments:

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance and its associated volume.

	Flags - Reason why this instance is being deleted.

Return Value:

	None.

--*/
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);

	PAGED_CODE();

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectInstanceTeardownStart: Entered\n"));
}


VOID
DelProtectInstanceTeardownComplete(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
)
/*++

Routine Description:

	This routine is called at the end of instance teardown.

Arguments:

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance and its associated volume.

	Flags - Reason why this instance is being deleted.

Return Value:

	None.

--*/
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);

	PAGED_CODE();

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectInstanceTeardownComplete: Entered\n"));
}


/*************************************************************************
	MiniFilter initialization and unload routines.
*************************************************************************/

NTSTATUS
DriverEntry(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_ PUNICODE_STRING RegistryPath
)
/*++

Routine Description:

	This is the initialization routine for this miniFilter driver.  This
	registers with FltMgr and initializes all global data structures.

Arguments:

	DriverObject - Pointer to driver object created by the system to
		represent this driver.

	RegistryPath - Unicode string identifying where the parameters for this
		driver are located in the registry.

Return Value:

	Routine can return non success error codes.

--*/
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(RegistryPath);

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DriverEntry: Entered\n"));

	// create a standard device object and symbolic link

	PDEVICE_OBJECT DeviceObject = nullptr;
	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\device\\delprotect");
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\delprotect");
	auto symLinkCreated = false;

	do {
		status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
		if (!NT_SUCCESS(status))
			break;

		status = IoCreateSymbolicLink(&symLink, &devName);
		if (!NT_SUCCESS(status))
			break;

		symLinkCreated = true;

		//
		//  Register with FltMgr to tell it our callback routines
		//

		status = FltRegisterFilter(DriverObject, &FilterRegistration, &gFilterHandle);

		FLT_ASSERT(NT_SUCCESS(status));
		if (!NT_SUCCESS(status))
			break;

		DriverObject->DriverUnload = DelProtectUnloadDriver;
		DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = DelProtectCreateClose;
		DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DelProtectDeviceControl;
		ExeNamesLock.Init();

		//
		//  Start filtering i/o
		//

		status = FltStartFiltering(gFilterHandle);
	} while (false);

	if (!NT_SUCCESS(status)) {
		if (gFilterHandle)
			FltUnregisterFilter(gFilterHandle);
		if (symLinkCreated)
			IoDeleteSymbolicLink(&symLink);
		if (DeviceObject)
			IoDeleteDevice(DeviceObject);
	}

	return status;
}

NTSTATUS
DelProtectUnload(
	_In_ FLT_FILTER_UNLOAD_FLAGS Flags
)
/*++

Routine Description:

	This is the unload routine for this miniFilter driver. This is called
	when the minifilter is about to be unloaded. We can fail this unload
	request if this is not a mandatory unload indicated by the Flags
	parameter.

Arguments:

	Flags - Indicating if this is a mandatory unload.

Return Value:

	Returns STATUS_SUCCESS.

--*/
{
	UNREFERENCED_PARAMETER(Flags);

	PAGED_CODE();

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectUnload: Entered\n"));

	FltUnregisterFilter(gFilterHandle);

	return STATUS_SUCCESS;
}


/*************************************************************************
	MiniFilter callback routines.
*************************************************************************/
FLT_PREOP_CALLBACK_STATUS
DelProtectPreOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
/*++

Routine Description:

	This routine is a pre-operation dispatch routine for this miniFilter.

	This is non-pageable because it could be called on the paging path

Arguments:

	Data - Pointer to the filter callbackData that is passed to us.

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance, its associated volume and
		file object.

	CompletionContext - The context for the completion routine for this
		operation.

Return Value:

	The return value is the status of the operation.

--*/
{

	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectPreOperation: Entered\n"));

	return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}



VOID
DelProtectOperationStatusCallback(
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_ PFLT_IO_PARAMETER_BLOCK ParameterSnapshot,
	_In_ NTSTATUS OperationStatus,
	_In_ PVOID RequesterContext
)
/*++

Routine Description:

	This routine is called when the given operation returns from the call
	to IoCallDriver.  This is useful for operations where STATUS_PENDING
	means the operation was successfully queued.  This is useful for OpLocks
	and directory change notification operations.

	This callback is called in the context of the originating thread and will
	never be called at DPC level.  The file object has been correctly
	referenced so that you can access it.  It will be automatically
	dereferenced upon return.

	This is non-pageable because it could be called on the paging path

Arguments:

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance, its associated volume and
		file object.

	RequesterContext - The context for the completion routine for this
		operation.

	OperationStatus -

Return Value:

	The return value is the status of the operation.

--*/
{
	UNREFERENCED_PARAMETER(FltObjects);

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectOperationStatusCallback: Entered\n"));

	PT_DBG_PRINT(PTDBG_TRACE_OPERATION_STATUS,
		("DelProtect!DelProtectOperationStatusCallback: Status=%08x ctx=%p IrpMj=%02x.%02x \"%s\"\n",
			OperationStatus,
			RequesterContext,
			ParameterSnapshot->MajorFunction,
			ParameterSnapshot->MinorFunction,
			FltGetIrpName(ParameterSnapshot->MajorFunction)));
}


FLT_POSTOP_CALLBACK_STATUS
DelProtectPostOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_In_opt_ PVOID CompletionContext,
	_In_ FLT_POST_OPERATION_FLAGS Flags
)
/*++

Routine Description:

	This routine is the post-operation completion routine for this
	miniFilter.

	This is non-pageable because it may be called at DPC level.

Arguments:

	Data - Pointer to the filter callbackData that is passed to us.

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance, its associated volume and
		file object.

	CompletionContext - The completion context set in the pre-operation routine.

	Flags - Denotes whether the completion is successful or is being drained.

Return Value:

	The return value is the status of the operation.

--*/
{
	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);
	UNREFERENCED_PARAMETER(Flags);

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectPostOperation: Entered\n"));

	return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
DelProtectPreOperationNoPostOperation(
	_Inout_ PFLT_CALLBACK_DATA Data,
	_In_ PCFLT_RELATED_OBJECTS FltObjects,
	_Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
/*++

Routine Description:

	This routine is a pre-operation dispatch routine for this miniFilter.

	This is non-pageable because it could be called on the paging path

Arguments:

	Data - Pointer to the filter callbackData that is passed to us.

	FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
		opaque handles to this filter, instance, its associated volume and
		file object.

	CompletionContext - The context for the completion routine for this
		operation.

Return Value:

	The return value is the status of the operation.

--*/
{
	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);

	PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
		("DelProtect!DelProtectPreOperationNoPostOperation: Entered\n"));

	// This template code does not do anything with the callbackData, but
	// rather returns FLT_PREOP_SUCCESS_NO_CALLBACK.
	// This passes the request down to the next miniFilter in the chain.

	return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

_Use_decl_annotations_
FLT_PREOP_CALLBACK_STATUS DelProtectPreCreate(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID*) {
	UNREFERENCED_PARAMETER(FltObjects);

	if (Data->RequestorMode == KernelMode)
		return FLT_PREOP_SUCCESS_NO_CALLBACK;

	auto& params = Data->Iopb->Parameters.Create;
	auto returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;

	if (params.Options & FILE_DELETE_ON_CLOSE) {
		// delete operation
		KdPrint(("Delete on close: %wZ\n", &FltObjects->FileObject->FileName));

		auto size = 512;	// some arbitrary size
		auto processName = (UNICODE_STRING*)ExAllocatePoolWithTag(PagedPool, size, DRIVER_TAG);
		if (processName == nullptr)
			return FLT_PREOP_SUCCESS_NO_CALLBACK;

		RtlZeroMemory(processName, size);	// ensure string will be NULL-terminated
		auto status = ZwQueryInformationProcess(NtCurrentProcess(), ProcessImageFileName,
			processName, size - sizeof(WCHAR), nullptr);

		if (NT_SUCCESS(status)) {
			KdPrint(("Delete operation from %wZ\n", processName));

			auto exeName = ::wcsrchr(processName->Buffer, L'\\');
			NT_ASSERT(exeName);

			if (exeName && FindExecutable(exeName + 1)) {	// skip backslash
				Data->IoStatus.Status = STATUS_ACCESS_DENIED;
				KdPrint(("Prevented delete in IRP_MJ_CREATE\n"));
				returnStatus = FLT_PREOP_COMPLETE;
			}
		}
		ExFreePool(processName);

	}
	return returnStatus;
}


_Use_decl_annotations_
FLT_PREOP_CALLBACK_STATUS DelProtectPreSetInformation(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID* CompletionContext) {
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);
	UNREFERENCED_PARAMETER(FltObjects);

	if (Data->RequestorMode == KernelMode)
		return FLT_PREOP_SUCCESS_NO_CALLBACK;

	auto& params = Data->Iopb->Parameters.SetFileInformation;

	if (params.FileInformationClass != FileDispositionInformation && params.FileInformationClass != FileDispositionInformationEx) {
		// not a delete operation
		return FLT_PREOP_SUCCESS_NO_CALLBACK;
	}

	auto info = (FILE_DISPOSITION_INFORMATION*)params.InfoBuffer;
	if (!info->DeleteFile)
		return FLT_PREOP_SUCCESS_NO_CALLBACK;

	// what process did this originate from?
	auto process = PsGetThreadProcess(Data->Thread);
	NT_ASSERT(process);

	// open a handle
	HANDLE hProcess;
	auto status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, nullptr, 0, nullptr, KernelMode, &hProcess);
	if (!NT_SUCCESS(status))
		return FLT_PREOP_SUCCESS_NO_CALLBACK;

	auto returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;

	auto size = 512;	// some arbitrary size
	auto processName = (UNICODE_STRING*)ExAllocatePoolWithTag(PagedPool, size, DRIVER_TAG);
	if (processName) {
		RtlZeroMemory(processName, size);	// ensure string will be NULL-terminated
		status = ZwQueryInformationProcess(hProcess, ProcessImageFileName,
			processName, size - sizeof(WCHAR), nullptr);

		if (NT_SUCCESS(status) && processName->Length > 0) {
			KdPrint(("Delete operation from %wZ\n"));

			auto exeName = ::wcsrchr(processName->Buffer, L'\\');

			if (exeName && FindExecutable(exeName + 1)) {	// skip backslash
				// prevent delete
				Data->IoStatus.Status = STATUS_ACCESS_DENIED;
				returnStatus = FLT_PREOP_COMPLETE;
				KdPrint(("Prevented delete in IRP_MJ_SET_INFORMATION\n"));
			}
		}
		ExFreePool(processName);
	}
	ZwClose(hProcess);

	return returnStatus;
}

NTSTATUS DelProtectCreateClose(PDEVICE_OBJECT, PIRP Irp) {
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS DelProtectDeviceControl(PDEVICE_OBJECT, PIRP Irp) {
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto status = STATUS_SUCCESS;

	switch (stack->Parameters.DeviceIoControl.IoControlCode) {
		case IOCTL_DELPROTECT_ADD_EXE:
		{
			auto name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
			if (!name) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			if (FindExecutable(name)) {
				break;
			}

			AutoLock locker(ExeNamesLock);
			if (ExeNamesCount == MaxExecutables) {
				status = STATUS_TOO_MANY_NAMES;
				break;
			}

			for (int i = 0; i < MaxExecutables; i++) {
				if (ExeNames[i] == nullptr) {
					auto len = (::wcslen(name) + 1) * sizeof(WCHAR);
					auto buffer = (WCHAR*)ExAllocatePoolWithTag(PagedPool, len, DRIVER_TAG);
					if (!buffer) {
						status = STATUS_INSUFFICIENT_RESOURCES;
						break;
					}
					::wcscpy_s(buffer, len / sizeof(WCHAR), name);
					ExeNames[i] = buffer;
					++ExeNamesCount;
					break;
				}
			}
			break;
		}

		case IOCTL_DELPROTECT_REMOVE_EXE:
		{
			auto name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
			if (!name) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			AutoLock locker(ExeNamesLock);
			auto found = false;
			for (int i = 0; i < MaxExecutables; i++) {
				if (::_wcsicmp(ExeNames[i], name) == 0) {
					ExFreePool(ExeNames[i]);
					ExeNames[i] = nullptr;
					--ExeNamesCount;
					found = true;
					break;
				}
			}
			if (!found)
				status = STATUS_NOT_FOUND;
			break;
		}

		case IOCTL_DELPROTECT_CLEAR:
			ClearAll();
			break;

		default:
			status = STATUS_INVALID_DEVICE_REQUEST;
			break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;

}

bool FindExecutable(PCWSTR name) {
	AutoLock locker(ExeNamesLock);
	if (ExeNamesCount == 0)
		return false;

	for (int i = 0; i < MaxExecutables; i++)
		if (ExeNames[i] && ::_wcsicmp(ExeNames[i], name) == 0)
			return true;
	return false;
}

void ClearAll() {
	AutoLock locker(ExeNamesLock);
	for (int i = 0; i < MaxExecutables; i++) {
		if (ExeNames[i]) {
			ExFreePool(ExeNames[i]);
			ExeNames[i] = nullptr;
		}
	}
	ExeNamesCount = 0;
}

void DelProtectUnloadDriver(PDRIVER_OBJECT DriverObject) {
	ClearAll();
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\delprotect");
	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);
}
