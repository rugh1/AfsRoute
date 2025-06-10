#include <fltKernel.h>
//
// The FLT_REGISTRATION structure provides information about a file system minifilter to the filter manager.
//

enum MsgType {
    PID,
    OPEN,
    WRITE,
    CREATE,
    DEL
};

int PID_TO_IGNORE; 

struct AfsRoutePortMessage {
    enum MsgType state;
    USHORT DataLength;
    WCHAR Data[1];
}typedef AfsRoutePortMessage;


struct AfsRouteReplyMsg {
     FILTER_REPLY_HEADER ReplyHeader;
     INT data; // can be a process id or 0 if approved -1 if needs to be loaded , -2 if not approved
}typedef AfsRouteReplyMsg;

PFLT_FILTER g_minifilterHandle = NULL;
CONST FLT_REGISTRATION g_filterRegistration; 

PFLT_PORT g_ServerPort;
PFLT_PORT g_ClientPort;


FLT_PREOP_CALLBACK_STATUS SimRepPreCreate(
    _Inout_  PFLT_CALLBACK_DATA Cbd,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_ PVOID* CompletionContext
);

FLT_PREOP_CALLBACK_STATUS AfsPreWrite(
    _Inout_  PFLT_CALLBACK_DATA Cbd,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_ PVOID* CompletionContext
);


int sendNoData(enum MsgType type) {
    if (g_ClientPort) { // client connected
        USHORT nameLen = 0;
        USHORT len = sizeof(AfsRoutePortMessage) + nameLen;
        AfsRoutePortMessage* msg = (AfsRoutePortMessage*)ExAllocatePool2(
            POOL_FLAG_PAGED, len, 0x31676174); // I really dont know what are tags this one is tag1 reversed cuz idk why
        if (msg) {
            msg->state = type;
            msg->DataLength = 0;
            msg->Data[0] = L'0';
            ULONG lenbuffer = sizeof(AfsRouteReplyMsg);
            LARGE_INTEGER timeout;
            timeout.QuadPart = -10000 * 10000; // 10 sec
            AfsRouteReplyMsg* reply = (AfsRouteReplyMsg*)ExAllocatePool2(
                POOL_FLAG_PAGED, sizeof(AfsRouteReplyMsg), 0x31676174);
            NTSTATUS status = FltSendMessage(g_minifilterHandle, &g_ClientPort, msg, len,
                reply, &lenbuffer, &timeout);
            DbgPrint("AfsRoute1: Raw reply buffer: %02x %02x %02x %02x ...", ((char*)reply)[0], ((char*)reply)[1], ((char*)reply)[2], ((char*)reply)[3]);
            DbgPrint("AfsRoute1: FltSendMessage returned 0x%x\n", status);
            INT data = *(INT*)reply;
            DbgPrint("AfsRoute1: recived data: %d, ", data);
            ExFreePool(msg);
            ExFreePool(reply);
            return data;
        }
        return -1;
    }
    return -1;
}


int send(enum MsgType type, UNICODE_STRING* filepath) {
    if (g_ClientPort) { // client connected
        USHORT nameLen = filepath->Length;
        USHORT len = sizeof(AfsRoutePortMessage) + nameLen;
        AfsRoutePortMessage *msg = (AfsRoutePortMessage*)ExAllocatePool2(
            POOL_FLAG_PAGED, len, 0x31676174); // I really dont know what are tags this one is tag1 reversed cuz idk why
        if (msg) {
            msg->state = type;
            msg->DataLength = nameLen / sizeof(WCHAR);
            RtlCopyMemory(msg->Data, filepath->Buffer, nameLen);
            LARGE_INTEGER timeout;
            timeout.QuadPart = -10000 * 10000; // 10 sec
            ULONG lenbuffer = sizeof(AfsRouteReplyMsg);
            AfsRouteReplyMsg* reply = (AfsRouteReplyMsg*)ExAllocatePool2(
                POOL_FLAG_PAGED, sizeof(AfsRouteReplyMsg), 0x31676174);
            NTSTATUS status = FltSendMessage(g_minifilterHandle, &g_ClientPort, msg, len,
                reply, &lenbuffer, &timeout);
            DbgPrint("AfsRoute1: FltSendMessage returned 0x%x\n", status);
            INT data = *(INT*)reply;
            DbgPrint("AfsRoute1: recived data: %d", data);
            ExFreePool(msg);
            ExFreePool(reply);
            return data;
        }
        return 1;
    }
    return 1;
}

NTSTATUS PortConnectNotify(
    PFLT_PORT ClientPort, PVOID ServerPortCookie,
    PVOID ConnectionContext, ULONG SizeOfContext,
    PVOID* ConnectionPortCookie) {
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);
    UNREFERENCED_PARAMETER(ConnectionPortCookie);
    g_ClientPort = ClientPort;
    DbgPrint("AfsRoute1:recived connection");
    return STATUS_SUCCESS;
}


void PortDisconnectNotify(PVOID ConnectionCookie) {
    UNREFERENCED_PARAMETER(ConnectionCookie);
    FltCloseClientPort(g_minifilterHandle, &g_ClientPort);
    g_ClientPort = NULL;
    DbgPrint("AfsRoute1:disconnected connection");
}


NTSTATUS PfltMessageNotify(
    PVOID PortCookie,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnOutputBufferLength
)
{
    return STATUS_SUCCESS;
}

CONST FLT_OPERATION_REGISTRATION g_callbacks[] =
{
    {
        IRP_MJ_CREATE,
        0,
        SimRepPreCreate,
        0
    },
    { IRP_MJ_WRITE,
        0,
        AfsPreWrite,
        0
    },

    
         

    { IRP_MJ_OPERATION_END }
};

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    DbgPrint("AfsRoute1: drive entry");
    NTSTATUS status = FltRegisterFilter(DriverObject, &g_filterRegistration, &g_minifilterHandle);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    DbgPrint("AfsRoute1: started creating connection");
    PSECURITY_DESCRIPTOR sd;
    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        return STATUS_FAILED_DRIVER_ENTRY;
    }
    UNICODE_STRING portName = RTL_CONSTANT_STRING(L"\\AfsRoute");
    OBJECT_ATTRIBUTES portAttr;
    InitializeObjectAttributes(&portAttr, &portName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, sd);
    status = FltCreateCommunicationPort(g_minifilterHandle, &g_ServerPort, &portAttr, NULL,
        PortConnectNotify, PortDisconnectNotify, PfltMessageNotify, 1);
    FltFreeSecurityDescriptor(sd);
    if (!NT_SUCCESS(status))
        return STATUS_FAILED_DRIVER_ENTRY;
    
    while (g_ClientPort == NULL) {
        continue;
    }
    DbgPrint("AfsRoute1: continue drive entry");
    int pid = sendNoData(PID);
    if (pid == -1) {
        DbgPrint("AfsRoute1: failed drive entry");
        return STATUS_FAILED_DRIVER_ENTRY;
    }
    DbgPrint("AfsRoute1: pid to ignore: %d", pid);
    PID_TO_IGNORE = pid;
    status = FltStartFiltering(g_minifilterHandle);
    if (!NT_SUCCESS(status))
    {
        FltUnregisterFilter(g_minifilterHandle);
    }


    return status;

}

NTSTATUS FLTAPI InstanceFilterUnloadCallback(_In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    //
    // This is called before a filter is unloaded.
    // If NULL is specified for this routine, then the filter can never be unloaded.
    //
    DbgPrint("AfsRoute1:started unloading");
    if (NULL != g_ServerPort)
    {
        DbgPrint("AfsRoute1:in fltclosecomm ");
        FltCloseCommunicationPort(g_ServerPort);
    }
    if (NULL != g_minifilterHandle)
    {
        FltUnregisterFilter(g_minifilterHandle);
    }
   
    DbgPrint("AfsRoute1:finished unloading");

    return STATUS_SUCCESS;
}


NTSTATUS FLTAPI InstanceSetupCallback(
    _In_ PCFLT_RELATED_OBJECTS  FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS  Flags,
    _In_ DEVICE_TYPE  VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE  VolumeFilesystemType)
{
    //
    // This is called to see if a filter would like to attach an instance to the given volume.
    //

    return STATUS_SUCCESS;
}


NTSTATUS FLTAPI InstanceQueryTeardownCallback(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
)
{
    //
    // This is called to see if the filter wants to detach from the given volume.
    //

    return STATUS_SUCCESS;
}

//
// Constant FLT_REGISTRATION structure for our filter.
// This initializes the callback routines our filter wants to register for.
//


CONST FLT_REGISTRATION g_filterRegistration =
{
    sizeof(FLT_REGISTRATION),      //  Size
    FLT_REGISTRATION_VERSION,      //  Version
    0,                             //  Flags
    NULL,                          //  Context registration
    g_callbacks,                   //  Operation callbacks
    InstanceFilterUnloadCallback,  //  FilterUnload
    InstanceSetupCallback,         //  InstanceSetup
    InstanceQueryTeardownCallback, //  InstanceQueryTeardown
    NULL,                          //  InstanceTeardownStart
    NULL,                          //  InstanceTeardownComplete
    NULL,                          //  GenerateFileName
    NULL,                          //  GenerateDestinationFileName
    NULL                           //  NormalizeNameComponent
};

NTSTATUS GetRedirectedPath(_In_ PFLT_FILE_NAME_INFORMATION fileNameInfo, _Inout_ UNICODE_STRING* newName);

FLT_PREOP_CALLBACK_STATUS FLTAPI AfsPreWrite(
    _Inout_  PFLT_CALLBACK_DATA Cbd,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_ PVOID* CompletionContext
)
{
    // 
    // Pre-create callback to get file info during creation or opening
    //

    DbgPrint("AfsWrite: %wZ\n", &Cbd->Iopb->TargetFileObject->FileName);

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS SimRepPreCreate(
    _Inout_  PFLT_CALLBACK_DATA Cbd,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_ PVOID* CompletionContext
)
/*++

Routine Description:

    This routine does the work for SimRep sample. SimRepPreCreate is called in
    the pre-operation path for IRP_MJ_CREATE and IRP_MJ_NETWORK_QUERY_OPEN.
    The function queries the requested file name for  the create and compares
    it to the mapping path. If the file is down the "old mapping path", the
    filter checks to see if the request is fast io based. If it is we cannot
    reparse the create because fast io does not support STATUS_REPARSE.
    Instead we return FLT_PREOP_DISALLOW_FASTIO to force the io to be reissued
    on the IRP path. If the create is IRP based, then we replace the file
    object's file name field with a new path based on the "new mapping path".

    This is pageable because it could not be called on the paging path

Arguments:

    Cbd - Pointer to the filter callbackData that is passed to us.

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance, its associated volume and
        file object.

    CompletionContext - The context for the completion routine for this
        operation.

Return Value:

    The return value is the status of the operation.

--*/
{
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    NTSTATUS status;
    FLT_PREOP_CALLBACK_STATUS callbackStatus;
    UNICODE_STRING newFileName;
    DbgPrint("AfsRoute: pre callback called");
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    PAGED_CODE();

    //DbgPrint("ENTERD precreate");
    /*DebugTrace(DEBUG_TRACE_ALL_IO,
        ("[SimRep]: SimRepPreCreate -> Enter (Cbd = %p, FileObject = %p)\n",
            Cbd,
            FltObjects->FileObject));*/


            //
            // Initialize defaults
            //

    status = STATUS_SUCCESS;
    callbackStatus = FLT_PREOP_SUCCESS_NO_CALLBACK; // pass through - default is no post op callback

    RtlInitUnicodeString(&newFileName, NULL);

    //
    // We only registered for this irp, so thats all we better get!
    //

    NT_ASSERT(Cbd->Iopb->MajorFunction == IRP_MJ_CREATE);

    //
    //  Check if this is a paging file as we don't want to redirect
    //  the location of the paging file.
    //

    if (FlagOn(Cbd->Iopb->OperationFlags, SL_OPEN_PAGING_FILE)) {

        //DbgPrint("Ignoring paging file open");
        /*DebugTrace(DEBUG_TRACE_ALL_IO,
            ("[SimRep]: SimRepPreCreate -> Ignoring paging file open (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject));*/

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
    }

    //
    //  We are not allowing volume opens to be reparsed in the sample.
    //

    if (FlagOn(Cbd->Iopb->TargetFileObject->Flags, FO_VOLUME_OPEN)) {

        //DbgPrint("Ignoring volume open");
       /* DebugTrace(DEBUG_TRACE_ALL_IO,
            ("[SimRep]: SimRepPreCreate -> Ignoring volume open (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject));*/

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()

    }

    //
    //  SimRep does not honor the FILE_OPEN_REPARSE_POINT create option. For a
    //  symbolic the caller would pass this flag, for example, in order to open
    //  the link for deletion. There is no concept of deleting the mapping for
    //  this filter so it is not clear what the purpose of honoring this flag
    //  would be.
    //

    //
    //  Don't reparse an open by ID because it is not possible to determine create path intent.
    //

    if (FlagOn(Cbd->Iopb->Parameters.Create.Options, FILE_OPEN_BY_FILE_ID)) {

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
    }

    if (FlagOn(Cbd->Iopb->OperationFlags, SL_OPEN_TARGET_DIRECTORY)) {

        //
        //  This is a prelude to a rename or hard link creation but the filter
        //  is NOT configured to filter these operations. To perform the operation
        //  successfully and in a consistent manner this create must not trigger
        //  a reparse. Pass through the create without attempting any redirection.
        //

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()

    }

    //
    //  Get the name information.
    //

    if (FlagOn(Cbd->Iopb->OperationFlags, SL_OPEN_TARGET_DIRECTORY)) {

        //
        //  The SL_OPEN_TARGET_DIRECTORY flag indicates the caller is attempting
        //  to open the target of a rename or hard link creation operation. We
        //  must clear this flag when asking fltmgr for the name or the result
        //  will not include the final component. We need the full path in order
        //  to compare the name to our mapping.
        //

        ClearFlag(Cbd->Iopb->OperationFlags, SL_OPEN_TARGET_DIRECTORY);

        //DbgPrint(" Clearing SL_OPEN_TARGET_DIRECTORY ");
        /*DebugTrace(DEBUG_TRACE_RENAME_REDIRECTION_OPERATIONS,
            ("[SimRep]: SimRepPreCreate -> Clearing SL_OPEN_TARGET_DIRECTORY for %wZ (Cbd = %p, FileObject = %p)\n",
                &nameInfo->Name,
                Cbd,
                FltObjects->FileObject));*/


                //
                //  Get the filename as it appears below this filter. Note that we use
                //  FLT_FILE_NAME_QUERY_FILESYSTEM_ONLY when querying the filename
                //  so that the filename as it appears below this filter does not end up
                //  in filter manager's name cache.
                //

        status = FltGetFileNameInformation(Cbd,
            FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_FILESYSTEM_ONLY,
            &nameInfo);

        //
        //  Restore the SL_OPEN_TARGET_DIRECTORY flag so the create will proceed
        //  for the target. The file systems depend on this flag being set in
        //  the target create in order for the subsequent SET_INFORMATION
        //  operation to proceed correctly.
        //

        SetFlag(Cbd->Iopb->OperationFlags, SL_OPEN_TARGET_DIRECTORY);


    }
    else {

        //
        //  Note that we use FLT_FILE_NAME_QUERY_DEFAULT when querying the
        //  filename. In the precreate the filename should not be in filter
        //  manager's name cache so there is no point looking there.
        //

        status = FltGetFileNameInformation(Cbd,
            FLT_FILE_NAME_OPENED |
            FLT_FILE_NAME_QUERY_DEFAULT,
            &nameInfo);
    }

    if (!NT_SUCCESS(status)) {

        //DbgPrint(" Failed to get name information");
        /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
            ("[SimRep]: SimRepPreCreate -> Failed to get name information (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject));*/

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
    }

    //DbgPrint("Processing create for file");

    /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS,
        ("[SimRep]: SimRepPreCreate -> Processing create for file %wZ (Cbd = %p, FileObject = %p)\n",
            &nameInfo->Name,
            Cbd,
            FltObjects->FileObject));*/

            //
            //  Parse the filename information
            //


    ULONG pid = FltGetRequestorProcessId(Cbd);
    DbgPrint("AfsRoute4: pid called is :%lu ", pid);
    if (pid == PID_TO_IGNORE) {
        goto SimRepPreCreateCleanup;
    }

    status = FltParseFileNameInformation(nameInfo);
    if (!NT_SUCCESS(status)) {
        //DbgPrint("Failed to parse name information");

        /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
            ("[SimRep]: SimRepPreCreate -> Failed to parse name information for file %wZ (Cbd = %p, FileObject = %p)\n",
                &nameInfo->Name,
                Cbd,
                FltObjects->FileObject));*/

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
    }

    //
    //  Munge the path from the old mapping to new mapping if the query overlaps
    //  the mapping path. Note: if the create is case sensitive this comparison
    //  must be as well.
    //


    //check if afs and put the new name into &newFileName:


    status = GetRedirectedPath(nameInfo, &newFileName);
        /*status = SimRepMungeName(nameInfo,
            &Globals.Mapping.OldName,
            &Globals.Mapping.NewName,
            !FlagOn(Cbd->Iopb->OperationFlags, SL_CASE_SENSITIVE),
            FALSE,
            &newFileName);*/

        if (!NT_SUCCESS(status)) {

            if (status == STATUS_NOT_FOUND) {
                status = STATUS_SUCCESS;
            }

            goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
        }

    
    DbgPrint("found afs path... rerouting");
    /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS,
        ("[SimRep]: SimRepPreCreate -> File name %wZ matches mapping. (Cbd = %p, FileObject = %p)\n"
            "\tMapping.OldFileName = %wZ\n"
            "\tMapping.NewFileName = %wZ\n",
            &nameInfo->Name,
            Cbd,
            FltObjects->FileObject,
            Globals.Mapping.OldName,
            Globals.Mapping.NewName));*/


    //
    //  replace file name in object using the new name recived 
    //

    /*status = Globals.ReplaceFileNameFunction(Cbd->Iopb->TargetFileObject,
        newFileName.Buffer,
        newFileName.Length);*/
    status = IoReplaceFileObjectName(Cbd->Iopb->TargetFileObject, newFileName.Buffer, newFileName.Length);
    ExFreePool(newFileName.Buffer);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Failed to allocate string for file");
        /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
            ("[SimRep]: SimRepPreCreate -> Failed to allocate string for file %wZ (Cbd = %p, FileObject = %p)\n",
                &nameInfo->Name,
                Cbd,
                FltObjects->FileObject));*/

        goto SimRepPreCreateCleanup; // need to replace with function : -> return cleanup()
    }

    //
    //  Set the status to STATUS_REPARSE
    //

    status = STATUS_REPARSE;

    DbgPrint("Returning STATUS_REPARSE yay!");
    /*DebugTrace(DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_REPARSED_OPERATIONS,
        ("[SimRep]: SimRepPreCreate -> Returning STATUS_REPARSE for file %wZ. (Cbd = %p, FileObject = %p)\n"
            "\tNewName = %wZ\n",
            &nameInfo->Name,
            Cbd,
            FltObjects->FileObject,
            &newFileName));*/
    //call cleanup

SimRepPreCreateCleanup:

    //
    //  Release the references we have acquired
    //

    /*SimRepFreeUnicodeString(&newFileName);*/

    if (nameInfo != NULL) {

        FltReleaseFileNameInformation(nameInfo);
    }
    
    if (status == STATUS_REPARSE) {

        //
        //  Reparse the open
        //

        Cbd->IoStatus.Status = STATUS_REPARSE;
        Cbd->IoStatus.Information = IO_REPARSE;
        callbackStatus = FLT_PREOP_COMPLETE;

    }
    else if (!NT_SUCCESS(status)) {

        //
        //  An error occurred, fail the open
        //
        DbgPrint("An error occurred, fail the open");
        /*DebugTrace(DEBUG_TRACE_ERROR,
            ("[SimRep]: SimRepPreCreate -> Failed with status 0x%x \n",
                status));*/

        Cbd->IoStatus.Status = status;  
        callbackStatus = FLT_PREOP_COMPLETE;
    }
    DbgPrint("Exit precreate");
    /*DebugTrace(DEBUG_TRACE_ALL_IO,
        ("[SimRep]: SimRepPreCreate -> Exit (Cbd = %p, FileObject = %p)\n",
            Cbd,
            FltObjects->FileObject));*/

    return callbackStatus;
}

//void ReaLength(UNICODE_STRING *str) 
//{
//    int len = 0;
//    WCHAR temp = L'A';
//    while (temp != UNICODE_NULL) {
//        len++;
//        temp = str->Buffer[len];
//    }
//    UNICODE
//}

int stringcmp(char *a, char *b, int len) {
    //used for when might not be null terminated
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) {            
            return 0;
        }
    }
    return 1;
}
void CopyChars(PWCH dest, PWCH src, int index, int length) {
    DbgPrint("AfsRoute4: copying: %S in index : %d len : %d to dest : %S",src, index, length, dest);
    for (int i = 0; i < length; i++) {
        DbgPrint("AfsRoute4: placing in %d  %c", i+index, src[i]);
        dest[i + index] = src[i];
    }
}
int GetNewPath(UNICODE_STRING oldPath, UNICODE_STRING *newPath, PWCHAR ReRoutePath, int len, int index) {
    DbgPrint("AfsRoute3: old:  buffer : %S Length: %hu max len: %hu", oldPath.Buffer, oldPath.Length, oldPath.MaximumLength);
    NTSTATUS status;
    UNICODE_STRING insidePath;
    
    PWCHAR pathAfterAfs = oldPath.Buffer + index; 
    insidePath.Buffer = pathAfterAfs;
    insidePath.Length = oldPath.Length - index*2;
    insidePath.MaximumLength = insidePath.Length;
    DbgPrint("AfsRoute3: insidePath:  buffer : %S Length: %hu max len: %hu index: %d", insidePath.Buffer , insidePath.Length, insidePath.MaximumLength, index);
    //STRING pathStr;
    //RtlZeroMemory(&pathStr, sizeof(pathStr));
    //RtlUnicodeStringToAnsiString(&pathStr, &oldPath, TRUE);
    //STRING restPath;
    //RtlZeroMemory(&restPath, sizeof(restPath));
    //restPath.Length = pathStr.Length + - index  - 4; // -> ...\ASF\.. get only of the end \..
    //restPath.Buffer = pathStr.Buffer + index + 4;
    //restPath.MaximumLength = restPath.Length + 40;

    UNICODE_STRING processedPath;
    processedPath.Buffer = ReRoutePath;
    processedPath.Length = len;
    processedPath.MaximumLength = processedPath.Length + insidePath.Length; //extra adding 

    //UNICODE_STRING UCachePath; 
    //RtlZeroMemory(&UCachePath, sizeof(UCachePath));
    //RtlAnsiStringToUnicodeString(&UCachePath, &CachePath, TRUE);
    DbgPrint("AfsRoute3: trying to append %S  + %S", processedPath.Buffer, pathAfterAfs);
    //status = RtlAppendUnicodeStringToString(&processedPath, &insidePath);

    newPath->Buffer = (PWCH)ExAllocatePoolZero(PagedPool, processedPath.MaximumLength, 0x68746170); // tag: path
    if (newPath->Buffer == NULL) {
        ExFreePool(newPath->Buffer);
        DbgPrint("AfsRoute2: failed to allocate");
        return 0;
    }
    CopyChars(newPath->Buffer, processedPath.Buffer, 0, processedPath.Length/2);
    CopyChars(newPath->Buffer, insidePath.Buffer, processedPath.Length/2, insidePath.Length/2);
    newPath->Length = processedPath.MaximumLength;
    newPath->MaximumLength = newPath->Length;

    DbgPrint("AfsRoute2: fullpath:  buffer : %S Length: %hu max len: %hu", newPath->Buffer, newPath->Length, newPath->MaximumLength);
    //*newPath = UCachePath;
    //RtlFreeAnsiString(&pathStr);
    return 0;
}

int CheckIfAfs(UNICODE_STRING name, int *index) {
    STRING pathStr;
    RtlZeroMemory(&pathStr, sizeof(pathStr));
    int status = 0;
    RtlUnicodeStringToAnsiString(&pathStr, &name, TRUE);
    for (int i = 0; i < pathStr.Length - 5; i++) {
        if (stringcmp("\\AFS\\", pathStr.Buffer + i, 5) == 1) {
            *index = i;
            status = 1;
            goto checkifafscleanup;
        }
    }
    for (int i = 0; i < pathStr.Length - 7; i++) {
        if (stringcmp("\\ToAfs\\", pathStr.Buffer + i, 7) == 1) {
            *index = i;
            status = 2;
            goto checkifafscleanup;
        }
    }

checkifafscleanup:
    RtlFreeAnsiString(&pathStr);
    return status;
}



NTSTATUS GetRedirectedPath(_In_ PFLT_FILE_NAME_INFORMATION fileNameInfo,_Inout_ UNICODE_STRING* newName) {
    //DbgPrint("AfsRoute: found afs file name recived is : %S", fileNameInfo->Name.Buffer);
    int index;
    int status = CheckIfAfs(fileNameInfo->Name, &index);
    if (status == 1) {
        DbgPrint("AfsRoute2: found afs file name recived is : %S status: %i", fileNameInfo->Name.Buffer, index);
        PWCHAR ReRoutePath = L"\\Device\\HarddiskVolume4\\AfsCache";
        index += 4;// 4 to remove AFS/
        status = GetNewPath(fileNameInfo->Name, newName, ReRoutePath,64 ,index);
        DbgPrint("AfsRoute1: sending...");
        UNICODE_STRING insidePath;
        PWCHAR pathAfterAfs = fileNameInfo->Name.Buffer + index;
        insidePath.Buffer = pathAfterAfs;
        insidePath.Length = fileNameInfo->Name.Length - index * 2;
        insidePath.MaximumLength = insidePath.Length;
        send(OPEN, &insidePath);
        DbgPrint("AfsRoute2: new path is : %S", newName->Buffer);
        return STATUS_SUCCESS;
    }
    else if (status == 2) {
        DbgPrint("AfsRoute2: found file to reroute to afs: %S status: %i", fileNameInfo->Name.Buffer, index);
        PWCHAR ReRoutePath = L"\\Device\\HarddiskVolume4\\Users\\vboxuser\\Desktop\\AFS";
        index += 6;// 6 to remove ToAfs/
        status = GetNewPath(fileNameInfo->Name, newName, ReRoutePath, 100, index);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}