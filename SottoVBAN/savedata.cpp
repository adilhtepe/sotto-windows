#pragma warning (disable : 4127)

#include "scream.h"
#include "savedata.h"

//=============================================================================
// Defines
//=============================================================================
// VBAN (Sotto fork of Scream): every datagram is a VBAN AUDIO packet. The header is 28 bytes,
// the payload at most 1436 bytes and 256 frames (VBAN specification, rev. 13).
#define VBAN_HEADER_SIZE    28
#define VBAN_DATA_MAX       1436
#define VBAN_SAMPLES_MAX    256
#define NUM_CHUNKS          800                         // How many packets fit in the ring buffer

//=============================================================================
// Statics
//=============================================================================

// Client-level callback table
const WSK_CLIENT_DISPATCH WskSampleClientDispatch = {
    MAKE_WSK_VERSION(1, 0), // This sample uses WSK version 1.0
    0, // Reserved
    NULL // WskClientEvent callback is not required in WSK version 1.0
};

//=============================================================================
// Helper Functions
//=============================================================================
// IRP completion routine used for synchronously waiting for completion
NTSTATUS WskSampleSyncIrpCompletionRoutine(__in PDEVICE_OBJECT Reserved, __in PIRP Irp, __in PVOID Context) {    
    PKEVENT compEvent = (PKEVENT)Context;
    
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(Irp);
    
    KeSetEvent(compEvent, 2, FALSE);    

    return STATUS_MORE_PROCESSING_REQUIRED;
}

#pragma code_seg("PAGE")
//=============================================================================
// CSaveData
//=============================================================================

//=============================================================================
CSaveData::CSaveData() : m_pBuffer(NULL), m_ulOffset(0), m_ulSendOffset(0), m_fWriteDisabled(FALSE), m_socket(NULL), m_pWorkItem(NULL), m_ulChunkSize(0), m_ulBufferSize(0) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::CSaveData]"));
    
    if (!g_UseIVSHMEM) {
        WSK_CLIENT_NPI   wskClientNpi;

        // allocate work item for this stream
        m_pWorkItem = (PSAVEWORKER_PARAM)ExAllocatePoolWithTag(NonPagedPool, sizeof(SAVEWORKER_PARAM), MSVAD_POOLTAG);
        if (m_pWorkItem) {
            m_pWorkItem->WorkItem = IoAllocateWorkItem(GetDeviceObject());
            KeInitializeEvent(&(m_pWorkItem->EventDone), NotificationEvent, TRUE);
        }

        // get us an IRP
        m_irp = IoAllocateIrp(1, FALSE);

        // initialize io completion sychronization event
        KeInitializeEvent(&m_syncEvent, SynchronizationEvent, FALSE);

        // Register with WSK.
        wskClientNpi.ClientContext = NULL;
        wskClientNpi.Dispatch = &WskSampleClientDispatch;
        WskRegister(&wskClientNpi, &m_wskSampleRegistration);
    }
} // CSaveData

//=============================================================================
CSaveData::~CSaveData() {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::~CSaveData]"));

    if (!g_UseIVSHMEM) {
        // Wait for a send still in flight, then free this stream's own work item. (Upstream Scream
        // kept one static work item for all streams and freed it here, so a stream that was still
        // playing queued a NULL work item at DISPATCH_LEVEL: bugcheck 0xA.)
        if (m_pWorkItem) {
            KeWaitForSingleObject(&(m_pWorkItem->EventDone), Executive, KernelMode, FALSE, NULL);
            if (m_pWorkItem->WorkItem != NULL) {
                IoFreeWorkItem(m_pWorkItem->WorkItem);
            }
            ExFreePoolWithTag(m_pWorkItem, MSVAD_POOLTAG);
            m_pWorkItem = NULL;
        }

        // close socket
        if (m_socket) {
            IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
            IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);
            ((PWSK_PROVIDER_BASIC_DISPATCH)m_socket->Dispatch)->WskCloseSocket(m_socket, m_irp);
            KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
        }

        // Deregister with WSK. This call will wait until all the references to
        // the WSK provider NPI are released and all the sockets are closed. Note
        // that if the worker thread has not started yet, then when it eventually
        // starts, its WskCaptureProviderNPI call will fail and the work queue
        // will be flushed and cleaned up properly.
        WskDeregister(&m_wskSampleRegistration);

        // free irp
        IoFreeIrp(m_irp);

        if (m_pBuffer) {
            ExFreePoolWithTag(m_pBuffer, MSVAD_POOLTAG);
            IoFreeMdl(m_pMdl);
        }
    }
} // CSaveData

//=============================================================================
void CSaveData::DestroyWorkItems(void) {
    PAGED_CODE();
    
    DPF_ENTER(("[CSaveData::DestroyWorkItems]"));

    // Work items are per stream now and freed in ~CSaveData.

} // DestroyWorkItems

//=============================================================================
void CSaveData::Disable(BOOL fDisable) {
    PAGED_CODE();

    m_fWriteDisabled = fDisable;
} // Disable

//=============================================================================
NTSTATUS CSaveData::SetDeviceObject(IN PDEVICE_OBJECT DeviceObject) {
    PAGED_CODE();

    ASSERT(DeviceObject);

    NTSTATUS ntStatus = STATUS_SUCCESS;
    
    m_pDeviceObject = DeviceObject;
    return ntStatus;
}

//=============================================================================
PDEVICE_OBJECT CSaveData::GetDeviceObject(void) {
    PAGED_CODE();

    return m_pDeviceObject;
}

#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS CSaveData::Initialize(DWORD nSamplesPerSec, WORD wBitsPerSample, WORD nChannels, DWORD dwChannelMask) {
    PAGED_CODE();

    NTSTATUS          ntStatus = STATUS_SUCCESS;

    DPF_ENTER(("[CSaveData::Initialize]"));
    
    // VBAN sample-rate table (spec rev. 13); the index goes in format_SR with sub-protocol AUDIO (0)
    static const ULONG vbanRates[21] = { 6000, 12000, 24000, 48000, 96000, 192000, 384000, 8000, 16000, 32000,
                                         64000, 128000, 256000, 512000, 11025, 22050, 44100, 88200, 176400, 352800, 705600 };
    ULONG frameBytes = (ULONG)nChannels * (wBitsPerSample / 8), frames, i;
    UNREFERENCED_PARAMETER(dwChannelMask);

    m_bVbanSr = 0xFF;
    for (i = 0; i < 21; i++) {
        if (vbanRates[i] == nSamplesPerSec) {
            m_bVbanSr = (BYTE)i;
        }
    }
    if (m_bVbanSr == 0xFF || nChannels < 1 || nChannels > 256 || frameBytes == 0 ||
        (wBitsPerSample != 16 && wBitsPerSample != 24 && wBitsPerSample != 32)) {
        DPF(D_TERSE, ("[VBAN: unsupported format %lu Hz %u bits %u ch]", nSamplesPerSec, wBitsPerSample, nChannels));
        return STATUS_INVALID_PARAMETER;
    }
    frames = VBAN_DATA_MAX / frameBytes;
    if (frames > VBAN_SAMPLES_MAX) {
        frames = VBAN_SAMPLES_MAX;
    }
    m_ulPayload    = frames * frameBytes;
    m_ulChunkSize  = VBAN_HEADER_SIZE + m_ulPayload;
    m_ulBufferSize = m_ulChunkSize * NUM_CHUNKS;
    m_ulFrame      = 0;
    m_bVbanNbs     = (BYTE)(frames - 1);
    m_bVbanNbc     = (BYTE)(nChannels - 1);
    m_bVbanBit     = (BYTE)(wBitsPerSample == 16 ? 1 : (wBitsPerSample == 24 ? 2 : 3));

    // Allocate memory for data buffer.
    if (NT_SUCCESS(ntStatus)) {
        m_pBuffer = (PBYTE) ExAllocatePoolWithTag(NonPagedPool, m_ulBufferSize, MSVAD_POOLTAG);
        if (!m_pBuffer) {
            DPF(D_TERSE, ("[Could not allocate memory for sending data]"));
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // Allocate MDL for the data buffer
    if (NT_SUCCESS(ntStatus)) {
        m_pMdl = IoAllocateMdl(m_pBuffer, m_ulBufferSize, FALSE, FALSE, NULL);
        if (m_pMdl == NULL) {
            DPF(D_TERSE, ("[Failed to allocate MDL]"));
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            MmBuildMdlForNonPagedPool(m_pMdl);
        }
    }

    return ntStatus;
} // Initialize

//=============================================================================
IO_WORKITEM_ROUTINE SendDataWorkerCallback;

VOID SendDataWorkerCallback(PDEVICE_OBJECT pDeviceObject, IN  PVOID  Context) {
    UNREFERENCED_PARAMETER(pDeviceObject);

    PAGED_CODE();

    ASSERT(Context);

    PSAVEWORKER_PARAM pParam = (PSAVEWORKER_PARAM) Context;
    PCSaveData        pSaveData;

    ASSERT(pParam->pSaveData);

    if (pParam->WorkItem) {
        pSaveData = pParam->pSaveData;
        pSaveData->SendData();
    }

    KeSetEvent(&(pParam->EventDone), 0, FALSE);
} // SendDataWorkerCallback

// Prototype for the control socket IoCompletion routine
NTSTATUS
ControlSocketComplete(
	PDEVICE_OBJECT DeviceObject,
	PIRP Irp,
	PVOID Context
);

// Function to set socket options
NTSTATUS SetSockOpt (PWSK_SOCKET Socket, ULONG level, ULONG option_name, ULONG option_value)
{
	PWSK_PROVIDER_BASIC_DISPATCH Dispatch;
	PIRP		Irp;
	//ULONG		SocketOptionState;
	NTSTATUS	Status;

	Dispatch = (PWSK_PROVIDER_BASIC_DISPATCH)(Socket->Dispatch);

	// Allocate an IRP
	Irp = IoAllocateIrp(1, FALSE);

	// Check result
	if (!Irp)
	{
		// Return error
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// Set the completion routine for the IRP
	IoSetCompletionRoutine(
		Irp,
		ControlSocketComplete,
		Socket,  // Use the socket object for the context
		TRUE,
		TRUE,
		TRUE
	);

	// Initiate the control operation on the socket
	Status = Dispatch->WskControlSocket(Socket, WskSetOption, option_name, level, sizeof(ULONG), &option_value, 0, NULL, NULL, Irp);

	return Status;
}

// Control socket IoCompletion routine
NTSTATUS
ControlSocketComplete(
	PDEVICE_OBJECT DeviceObject,
	PIRP Irp,
	PVOID Context
)
{
	UNREFERENCED_PARAMETER(DeviceObject);

	PWSK_SOCKET Socket;

	// Check the result of the control operation
	if (Irp->IoStatus.Status == STATUS_SUCCESS)
	{
		// Get the socket object from the context
		Socket = (PWSK_SOCKET)Context;

		// Perform the next operation on the socket
	}

	// Error status
	else
	{
		// Handle error
		//		DPF(D_TERSE, ("WskSetOtion returns: %x", Irp->IoStatus.Status));
	}

	// Free the IRP
	IoFreeIrp(Irp);

	// Always return STATUS_MORE_PROCESSING_REQUIRED to
	// terminate the completion processing of the IRP.
	return STATUS_MORE_PROCESSING_REQUIRED;
}


#pragma code_seg()
//=============================================================================
void CSaveData::CreateSocket(void) {
    NTSTATUS            status;
    WSK_PROVIDER_NPI    pronpi;
    LPCTSTR             terminator;
    SOCKADDR_IN         locaddr4 = { AF_INET, RtlUshortByteSwap((USHORT)g_UnicastSrcPort), 0, 0 };
    SOCKADDR_IN         sockaddr = { AF_INET, RtlUshortByteSwap((USHORT)g_UnicastPort), 0, 0 };
    
    DPF_ENTER(("[CSaveData::CreateSocket]"));
    
    // capture WSK provider
    status = WskCaptureProviderNPI(&m_wskSampleRegistration, WSK_INFINITE_WAIT, &pronpi);
    if(!NT_SUCCESS(status)){
        DPF(D_TERSE, ("Failed to capture provider NPI: 0x%X\n", status));
        return;
    }

    RtlIpv4StringToAddress(g_UnicastSrcIPv4, true, &terminator, &(locaddr4.sin_addr));
    RtlIpv4StringToAddress(g_UnicastIPv4, true, &terminator, &(sockaddr.sin_addr));
    RtlCopyMemory(&m_sServerAddr, &sockaddr, sizeof(SOCKADDR_IN));
    
    // create socket
    IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
    IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);    
    pronpi.Dispatch->WskSocket(
        pronpi.Client,
        m_sServerAddr.ss_family,
        SOCK_DGRAM,
        IPPROTO_UDP,
        WSK_FLAG_DATAGRAM_SOCKET,
        NULL, // socket context
        NULL, // dispatch
        NULL, // Process
        NULL, // Thread
        NULL, // SecurityDescriptor
        m_irp);
    KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
    
    DPF(D_TERSE, ("WskSocket: %x", m_irp->IoStatus.Status));
    
    if (!NT_SUCCESS(m_irp->IoStatus.Status)) {
        DPF(D_TERSE, ("Failed to create socket: %x", m_irp->IoStatus.Status));
        
        if(m_socket) {
            IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
            IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);
            ((PWSK_PROVIDER_BASIC_DISPATCH)m_socket->Dispatch)->WskCloseSocket(m_socket, m_irp);
            KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
        }
        
        // release the provider again, as we are finished with it
        WskReleaseProviderNPI(&m_wskSampleRegistration);
        
        return;
    }
    
    // save the socket
    m_socket = (PWSK_SOCKET)m_irp->IoStatus.Information;
    
    // release the provider again, as we are finished with it
    WskReleaseProviderNPI(&m_wskSampleRegistration);

    // bind the socket
    IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
    IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);

    status = SetSockOpt(m_socket, SOL_SOCKET, SO_REUSEADDR, 1);

    if (g_TTL) {
	// should check for unicasst and set IP_TTL
	status = SetSockOpt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, g_TTL);
    }
	
// if (g_DSCP) status = SetSockOpt(m_socket, IPPROTO_IP, IP_TOS, (g_DSCP << 2) & 0xff);  // no support in kernel - raw socket and IP_HDRINCL?

    status = ((PWSK_PROVIDER_DATAGRAM_DISPATCH)(m_socket->Dispatch))->WskBind(m_socket, (PSOCKADDR)(&locaddr4), 0, m_irp);
    KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
    
    DPF(D_TERSE, ("WskBind: %x", m_irp->IoStatus.Status));
    
    if (!NT_SUCCESS(m_irp->IoStatus.Status)) {
        DPF(D_TERSE, ("Failed to bind socket: %x", m_irp->IoStatus.Status));
        if(m_socket) {
            IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
            IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);
            ((PWSK_PROVIDER_BASIC_DISPATCH)m_socket->Dispatch)->WskCloseSocket(m_socket, m_irp);
            KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
        }
        
        return;
    }
}

//=============================================================================
void CSaveData::SendData() {
    WSK_BUF wskbuf;

    ULONG storeOffset;
    
    if (!m_socket) {
        CreateSocket();
    }
    
    if (m_socket) {
        while (1) {
            // Read latest storeOffset. There might be new data.
            storeOffset = m_ulOffset;

            // Abort if there's nothing to send. Note: When storeOffset < sendOffset, we can always send a chunk.
            if ((storeOffset >= m_ulSendOffset) && ((storeOffset - m_ulSendOffset) < m_ulChunkSize))
                break;

            // Send a chunk
            wskbuf.Mdl = m_pMdl;
            wskbuf.Length = m_ulChunkSize;
            wskbuf.Offset = m_ulSendOffset;
            IoReuseIrp(m_irp, STATUS_UNSUCCESSFUL);
            IoSetCompletionRoutine(m_irp, WskSampleSyncIrpCompletionRoutine, &m_syncEvent, TRUE, TRUE, TRUE);
            ((PWSK_PROVIDER_DATAGRAM_DISPATCH)(m_socket->Dispatch))->WskSendTo(m_socket, &wskbuf, 0, (PSOCKADDR)&m_sServerAddr, 0, NULL, m_irp);
            KeWaitForSingleObject(&m_syncEvent, Executive, KernelMode, FALSE, NULL);
            DPF(D_TERSE, ("WskSendTo: %x", m_irp->IoStatus.Status));

            m_ulSendOffset += m_ulChunkSize; if (m_ulSendOffset >= m_ulBufferSize) m_ulSendOffset = 0;
        }
    }
}

#pragma code_seg("PAGE")
//=============================================================================
void CSaveData::WaitAllWorkItems(void) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::WaitAllWorkItems]"));

    DPF(D_VERBOSE, ("[Waiting for WorkItem]"));
    if (m_pWorkItem) {
        KeWaitForSingleObject(&(m_pWorkItem->EventDone), Executive, KernelMode, FALSE, NULL);
    }
    
} // WaitAllWorkItems

#pragma code_seg()
//=============================================================================
void CSaveData::WriteData(IN PBYTE pBuffer, IN ULONG ulByteCount) {
    ASSERT(pBuffer);

    LARGE_INTEGER timeOut = { 0 };
    NTSTATUS ntStatus;
    ULONG offset;
    ULONG toWrite;
    ULONG w;
    
    if (m_fWriteDisabled) {
        return;
    }

    // Nothing to write into or nothing to send with (initialization failed)
    if (m_pBuffer == NULL || m_pWorkItem == NULL || m_pWorkItem->WorkItem == NULL || m_ulChunkSize == 0) {
        return;
    }

    DPF_ENTER(("[CSaveData::WriteData ulByteCount=%lu]", ulByteCount));

    // Undersized (paranoia)
    if (0 == ulByteCount) {
        return;
    }

    // Oversized (paranoia)
    if (ulByteCount > (m_ulBufferSize / 2)) {
        return;
    }

    // Append to ring buffer. Don't write intermediate states to m_ulOffset,
    // but update it once at the end.
    offset = m_ulOffset;
    toWrite = ulByteCount;
    while (toWrite > 0) {
        w = offset % m_ulChunkSize;
        if (w > 0) {
            // Fill up last packet
            w = (m_ulChunkSize - w);
            w = (toWrite < w) ? toWrite : w;
            RtlCopyMemory(&(m_pBuffer[offset]), &(pBuffer[ulByteCount - toWrite]), w);
        }
        else {
            // Start a new packet: VBAN header, little endian
            PBYTE h = &(m_pBuffer[offset]);
            h[0] = 'V'; h[1] = 'B'; h[2] = 'A'; h[3] = 'N';
            h[4] = m_bVbanSr;                    // sub-protocol AUDIO (0x00) | sample-rate index
            h[5] = m_bVbanNbs;
            h[6] = m_bVbanNbc;
            h[7] = m_bVbanBit;                   // codec PCM (0x00) | data type
            RtlCopyMemory(&h[8], g_StreamName, 16);
            h[24] = (BYTE)(m_ulFrame);
            h[25] = (BYTE)(m_ulFrame >> 8);
            h[26] = (BYTE)(m_ulFrame >> 16);
            h[27] = (BYTE)(m_ulFrame >> 24);
            m_ulFrame++;
            offset += VBAN_HEADER_SIZE;
            w = ((m_ulBufferSize - offset) < toWrite) ? (m_ulBufferSize - offset) : toWrite;
            w = (w > m_ulPayload) ? m_ulPayload : w;
            RtlCopyMemory(&(m_pBuffer[offset]), &(pBuffer[ulByteCount - toWrite]), w);
        }
        toWrite -= w;
        offset += w;  if (offset >= m_ulBufferSize) offset = 0;
    }
    m_ulOffset = offset;

    // If I/O worker was done, relaunch it
    ntStatus = KeWaitForSingleObject(&(m_pWorkItem->EventDone), Executive, KernelMode, FALSE, &timeOut);
    if (STATUS_SUCCESS == ntStatus) {
            m_pWorkItem->pSaveData = this;
            KeResetEvent(&(m_pWorkItem->EventDone));
            IoQueueWorkItem(m_pWorkItem->WorkItem, SendDataWorkerCallback, CriticalWorkQueue, (PVOID)m_pWorkItem);
    }
} // WriteData
