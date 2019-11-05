/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    The per UDP binding (local IP/port and optionally remote IP) state. This
    includes the lookup state for processing a received packet and the list of
    listeners registered.

--*/

#include "precomp.h"

#ifdef QUIC_LOGS_WPP
#include "binding.tmh"
#endif

//
// Make sure we will always have enough room to fit our Version Negotiation packet,
// which includes both the global, constant list of supported versions and the
// randomly generated version.
//
#define MAX_VER_NEG_PACKET_LENGTH \
( \
    sizeof(QUIC_VERSION_NEGOTIATION_PACKET) + \
    QUIC_MAX_CONNECTION_ID_LENGTH_INVARIANT + \
    QUIC_MAX_CONNECTION_ID_LENGTH_INVARIANT + \
    sizeof(uint32_t) + \
    sizeof(QuicSupportedVersionList) \
)
QUIC_STATIC_ASSERT(
    QUIC_DEFAULT_PATH_MTU - 48 >= MAX_VER_NEG_PACKET_LENGTH,
    "Too many supported version numbers! Requires too big of buffer for response!");

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicBindingInitialize(
#ifdef QUIC_COMPARTMENT_ID
    _In_ QUIC_COMPARTMENT_ID CompartmentId,
#endif
    _In_ BOOLEAN ShareBinding,
    _In_opt_ const QUIC_ADDR * LocalAddress,
    _In_opt_ const QUIC_ADDR * RemoteAddress,
    _Out_ PQUIC_BINDING* NewBinding
    )
{
    QUIC_STATUS Status;
    PQUIC_BINDING Binding;
    uint8_t HashSalt[20];

    Binding = QUIC_ALLOC_NONPAGED(sizeof(QUIC_BINDING));
    if (Binding == NULL) {
        EventWriteQuicAllocFailure("QUIC_BINDING", sizeof(QUIC_BINDING));
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    Binding->RefCount = 1;
    Binding->Exclusive = !ShareBinding;
    Binding->Connected = RemoteAddress == NULL ? FALSE : TRUE;
    Binding->HandshakeConnections = 0;
    Binding->StatelessOperCount = 0;
    QuicDispatchRwLockInitialize(&Binding->RwLock);
    QuicDispatchLockInitialize(&Binding->ResetTokenLock);
    QuicDispatchLockInitialize(&Binding->StatelessOperLock);
    QuicListInitializeHead(&Binding->Listeners);
    QuicLookupInitialize(&Binding->Lookup);
    QuicHashtableInitializeEx(&Binding->StatelessOperTable, QUIC_HASH_MIN_SIZE);
    QuicListInitializeHead(&Binding->StatelessOperList);

    //
    // Random reserved version number for version negotation.
    //
    QuicRandom(sizeof(uint32_t), (uint8_t*)&Binding->RandomReservedVersion);
    Binding->RandomReservedVersion =
        (Binding->RandomReservedVersion & ~QUIC_VERSION_RESERVED_MASK) |
        QUIC_VERSION_RESERVED;
    
    QuicRandom(sizeof(HashSalt), HashSalt);
    Status =
        QuicHashCreate(
            QUIC_HASH_SHA256,
            HashSalt,
            sizeof(HashSalt),
            &Binding->ResetTokenHash);
    if (QUIC_FAILED(Status)) {
        EventWriteQuicBindingErrorStatus(Binding, Status, "Create reset token hash");
        goto Error;
    }

#ifdef QUIC_COMPARTMENT_ID
    Binding->CompartmentId = CompartmentId;

    BOOLEAN RevertCompartmentId = FALSE;
    QUIC_COMPARTMENT_ID PrevCompartmentId = QuicCompartmentIdGetCurrent();
    if (PrevCompartmentId != CompartmentId) {
        Status = QuicCompartmentIdSetCurrent(CompartmentId);
        if (QUIC_FAILED(Status)) {
            EventWriteQuicBindingErrorStatus(Binding, Status, "Set current compartment Id");
            goto Error;
        }
        RevertCompartmentId = TRUE;
    }
#endif

    Status =
        QuicDataPathBindingCreate(
            MsQuicLib.Datapath,
            LocalAddress,
            RemoteAddress,
            Binding,
            &Binding->DatapathBinding);

#ifdef QUIC_COMPARTMENT_ID
    if (RevertCompartmentId) {
        (void)QuicCompartmentIdSetCurrent(PrevCompartmentId);
    }
#endif

    if (QUIC_FAILED(Status)) {
        EventWriteQuicBindingErrorStatus(Binding, Status, "Create datapath binding");
        goto Error;
    }

    QUIC_ADDR DatapathLocalAddr, DatapathRemoteAddr;
    QuicDataPathBindingGetLocalAddress(Binding->DatapathBinding, &DatapathLocalAddr);
    QuicDataPathBindingGetRemoteAddress(Binding->DatapathBinding, &DatapathRemoteAddr);
    EventWriteQuicBindingCreated(
        Binding, Binding->DatapathBinding,
        LOG_ADDR_LEN(DatapathLocalAddr), LOG_ADDR_LEN(DatapathRemoteAddr),
        (uint8_t*)&DatapathLocalAddr, (uint8_t*)&DatapathRemoteAddr);

    *NewBinding = Binding;
    Status = QUIC_STATUS_SUCCESS;

Error:

    if (QUIC_FAILED(Status)) {
        if (Binding != NULL) {
            QuicHashFree(Binding->ResetTokenHash);
            QuicLookupUninitialize(&Binding->Lookup);
            QuicHashtableUninitialize(&Binding->StatelessOperTable);
            QuicDispatchLockUninitialize(&Binding->StatelessOperLock);
            QuicDispatchLockUninitialize(&Binding->ResetTokenLock);
            QuicDispatchRwLockUninitialize(&Binding->RwLock);
            QUIC_FREE(Binding);
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingUninitialize(
    _In_ PQUIC_BINDING Binding
    )
{
    EventWriteQuicBindingCleanup(Binding);

    QUIC_TEL_ASSERT(Binding->RefCount == 0);
    QUIC_TEL_ASSERT(Binding->HandshakeConnections == 0);
    QUIC_TEL_ASSERT(QuicListIsEmpty(&Binding->Listeners));

    //
    // Delete the datapath binding. This function blocks until all receive
    // upcalls have completed.
    //
    QuicDataPathBindingDelete(Binding->DatapathBinding);

    //
    // Clean up any leftover stateless operations being tracked.
    //
    while (!QuicListIsEmpty(&Binding->StatelessOperList)) {
        QUIC_STATELESS_CONTEXT* StatelessCtx =
            QUIC_CONTAINING_RECORD(
                QuicListRemoveHead(&Binding->StatelessOperList),
                QUIC_STATELESS_CONTEXT,
                ListEntry);
        Binding->StatelessOperCount--;
        QuicHashtableRemove(
            &Binding->StatelessOperTable,
            &StatelessCtx->TableEntry,
            NULL);
        QUIC_DBG_ASSERT(StatelessCtx->IsProcessed);
        QuicPoolFree(
            &StatelessCtx->Worker->StatelessContextPool,
            StatelessCtx);
    }
    QUIC_DBG_ASSERT(Binding->StatelessOperCount == 0);
    QUIC_DBG_ASSERT(QuicHashtableGetTotalEntryCount(&Binding->StatelessOperTable) == 0);

    QuicHashFree(Binding->ResetTokenHash);
    QuicLookupUninitialize(&Binding->Lookup);
    QuicDispatchLockUninitialize(&Binding->StatelessOperLock);
    QuicHashtableUninitialize(&Binding->StatelessOperTable);
    QuicDispatchLockUninitialize(&Binding->ResetTokenLock);
    QuicDispatchRwLockUninitialize(&Binding->RwLock);

    QUIC_FREE(Binding);
    EventWriteQuicBindingDestroyed(Binding);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingTraceRundown(
    _In_ PQUIC_BINDING Binding
    )
{
    // TODO - Trace datapath binding

    QUIC_ADDR DatapathLocalAddr, DatapathRemoteAddr;
    QuicDataPathBindingGetLocalAddress(Binding->DatapathBinding, &DatapathLocalAddr);
    QuicDataPathBindingGetRemoteAddress(Binding->DatapathBinding, &DatapathRemoteAddr);
    EventWriteQuicBindingRundown(
        Binding, Binding->DatapathBinding,
        LOG_ADDR_LEN(DatapathLocalAddr), LOG_ADDR_LEN(DatapathRemoteAddr),
        (uint8_t*)&DatapathLocalAddr, (uint8_t*)&DatapathRemoteAddr);

    QuicDispatchRwLockAcquireShared(&Binding->RwLock);

    for (QUIC_LIST_ENTRY* Link = Binding->Listeners.Flink;
        Link != &Binding->Listeners;
        Link = Link->Flink) {
        QuicListenerTraceRundown(
            QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link));
    }

    QuicDispatchRwLockReleaseShared(&Binding->RwLock);
}

//
// Returns TRUE if there are any registered listeners on this binding.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingHasListenerRegistered(
    _In_ const QUIC_BINDING* const Binding
    )
{
    return !QuicListIsEmpty(&Binding->Listeners);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicBindingRegisterListener(
    _In_ PQUIC_BINDING Binding,
    _In_ PQUIC_LISTENER NewListener
    )
{
    BOOLEAN AddNewListener = TRUE;
    BOOLEAN MaximizeLookup = FALSE;

    const QUIC_ADDR* NewAddr = &NewListener->LocalAddress;
    const BOOLEAN NewWildCard = NewListener->WildCard;
    const QUIC_ADDRESS_FAMILY NewFamily = QuicAddrGetFamily(NewAddr);
    const char* NewAlpn = NewListener->Session->Alpn;
    const uint8_t NewAlpnLength = NewListener->Session->AlpnLength;

    QuicDispatchRwLockAcquireExclusive(&Binding->RwLock);

    //
    // For a single binding, listeners are saved in a linked list, sorted by
    // family first, in decending order {AF_INET6, AF_INET, AF_UNSPEC}, and then
    // specific addresses followed by wild card addresses. Insertion of a new
    // listener with a given IP/ALPN go at the end of the existing family group,
    // only if there isn't a direct match prexisting in the list.
    //

    QUIC_LIST_ENTRY* Link;
    for (Link = Binding->Listeners.Flink;
        Link != &Binding->Listeners;
        Link = Link->Flink) {

        const QUIC_LISTENER* ExistingListener =
            QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link);
        const QUIC_ADDR* ExistingAddr = &ExistingListener->LocalAddress;
        const BOOLEAN ExistingWildCard = ExistingListener->WildCard;
        const QUIC_ADDRESS_FAMILY ExistingFamily = QuicAddrGetFamily(ExistingAddr);
        const char* ExistingAlpn = ExistingListener->Session->Alpn;
        const uint8_t ExistingAlpnLength = ExistingListener->Session->AlpnLength;

        if (NewFamily > ExistingFamily) {
            break; // End of possible family matches. Done searching.
        } else if (NewFamily != ExistingFamily) {
            continue;
        }

        if (!NewWildCard && ExistingWildCard) {
            break; // End of specific address matches. Done searching.
        } else if (NewWildCard != ExistingWildCard) {
            continue;
        }

        if (NewFamily != AF_UNSPEC && !QuicAddrCompareIp(NewAddr, ExistingAddr)) {
            continue;
        }

        if (NewAlpnLength == ExistingAlpnLength &&
            memcmp(NewAlpn, ExistingAlpn, NewAlpnLength) == 0) { // Pre-existing match found.
            LogWarning("[bind][%p] Listener (%p) already registered on ALPN %s",
                Binding, ExistingListener, NewAlpn);
            AddNewListener = FALSE;
            break;
        }
    }

    if (AddNewListener) {
        MaximizeLookup = QuicListIsEmpty(&Binding->Listeners);

        //
        // If we search all the way back to the head of the list, just insert
        // the new listener at the end of the list. Otherwise, we terminated
        // prematurely based on sort order. Insert the new listener right before
        // the current Link.
        //
        if (Link == &Binding->Listeners) {
            QuicListInsertTail(&Binding->Listeners, &NewListener->Link);
        } else {
            NewListener->Link.Flink = Link;
            NewListener->Link.Blink = Link->Blink;
            NewListener->Link.Blink->Flink = &NewListener->Link;
            Link->Blink = &NewListener->Link;
        }
    }

    QuicDispatchRwLockReleaseExclusive(&Binding->RwLock);

    if (MaximizeLookup &&
        !QuicLookupMaximizePartitioning(&Binding->Lookup)) {
        QuicBindingUnregisterListener(Binding, NewListener);
        AddNewListener = FALSE;
    }

    return AddNewListener;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(return != NULL)
PQUIC_LISTENER
QuicBindingGetListener(
    _In_ PQUIC_BINDING Binding,
    _In_ const QUIC_NEW_CONNECTION_INFO* Info
    )
{
    PQUIC_LISTENER Listener = NULL;

    const QUIC_ADDR* Addr = Info->LocalAddress;
    const QUIC_ADDRESS_FAMILY Family = QuicAddrGetFamily(Addr);
    const uint8_t* AlpnList = Info->AlpnList;
    uint16_t AlpnListLength = Info->AlpnListLength;

    //
    // The ALPN list has been prevalidated. We have a few asserts/assumes to
    // ensure this and get OACR to not complain.
    //
    QUIC_DBG_ASSERT(AlpnListLength >= 2);

    QuicDispatchRwLockAcquireShared(&Binding->RwLock);

    while (AlpnListLength != 0) {

        QUIC_DBG_ASSERT(AlpnListLength >= 2);
        uint8_t Length = AlpnList[0];

        AlpnList++;
        AlpnListLength--;
        QUIC_DBG_ASSERT(Length <= AlpnListLength);

        for (QUIC_LIST_ENTRY* Link = Binding->Listeners.Flink;
            Link != &Binding->Listeners;
            Link = Link->Flink) {
                
            QUIC_LISTENER* ExistingListener =
                QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link);
            const QUIC_ADDR* ExistingAddr = &ExistingListener->LocalAddress;
            const BOOLEAN ExistingWildCard = ExistingListener->WildCard;
            const QUIC_ADDRESS_FAMILY ExistingFamily = QuicAddrGetFamily(ExistingAddr);

            if (ExistingFamily != AF_UNSPEC) {
                if (Family != ExistingFamily ||
                    (!ExistingWildCard && !QuicAddrCompareIp(Addr, ExistingAddr))) {
                    continue; // No IP match.
                }
            }

            if (Length == ExistingListener->Session->AlpnLength &&
                memcmp(AlpnList, ExistingListener->Session->Alpn, Length) == 0) {
                if (QuicRundownAcquire(&ExistingListener->Rundown)) {
                    Listener = ExistingListener;
                }
                goto Done;
            }
        }

        AlpnList += Length;
        AlpnListLength -= Length;
    }

Done:

    QuicDispatchRwLockReleaseShared(&Binding->RwLock);

    return Listener;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingUnregisterListener(
    _In_ PQUIC_BINDING Binding,
    _In_ PQUIC_LISTENER Listener
    )
{
    QuicDispatchRwLockAcquireExclusive(&Binding->RwLock);
    QuicListEntryRemove(&Listener->Link);
    QuicDispatchRwLockReleaseExclusive(&Binding->RwLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingAddSourceConnectionID(
    _In_ PQUIC_BINDING Binding,
    _In_ QUIC_CID_HASH_ENTRY* SourceCID
    )
{
    return QuicLookupAddSourceConnectionID(&Binding->Lookup, SourceCID, NULL);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingRemoveSourceConnectionID(
    _In_ PQUIC_BINDING Binding,
    _In_ QUIC_CID_HASH_ENTRY* SourceCID
    )
{
    QuicLookupRemoveSourceConnectionID(&Binding->Lookup, SourceCID);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingRemoveConnection(
    _In_ PQUIC_BINDING Binding,
    _In_ PQUIC_CONNECTION Connection
    )
{
    QuicLookupRemoveSourceConnectionIDs(&Binding->Lookup, Connection);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingMoveSourceConnectionIDs(
    _In_ PQUIC_BINDING BindingSrc,
    _In_ PQUIC_BINDING BindingDest,
    _In_ PQUIC_CONNECTION Connection
    )
{
    QuicLookupMoveSourceConnectionIDs(
        &BindingSrc->Lookup, &BindingDest->Lookup, Connection);
}

//
// This attempts to add a new stateless operation (for a given remote endpoint)
// to the tracking structures in the binding. It first ages out any old
// operations that might have expired. Then it adds the new operation only if
// the remote address isn't already in the table.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATELESS_CONTEXT*
QuicBindingCreateStatelessOperation(
    _In_ PQUIC_BINDING Binding,
    _In_ PQUIC_WORKER Worker,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    uint32_t TimeMs = QuicTimeMs32();
    const QUIC_ADDR* RemoteAddress = &Datagram->Tuple->RemoteAddress;
    uint32_t Hash = QuicAddrHash(RemoteAddress);
    QUIC_STATELESS_CONTEXT* StatelessCtx = NULL;

    QuicDispatchLockAcquire(&Binding->StatelessOperLock);

    //
    // Age out all expired operation contexts.
    //
    while (!QuicListIsEmpty(&Binding->StatelessOperList)) {
        QUIC_STATELESS_CONTEXT* OldStatelessCtx =
            QUIC_CONTAINING_RECORD(
                Binding->StatelessOperList.Flink,
                QUIC_STATELESS_CONTEXT,
                ListEntry);

        if (QuicTimeDiff32(OldStatelessCtx->CreationTimeMs, TimeMs) <
            QUIC_STATELESS_OPERATION_EXPIRATION_MS) {
            break;
        }

        //
        // The operation is expired. Remove it from the tracking structures.
        //
        OldStatelessCtx->IsExpired = TRUE;
        QuicHashtableRemove(
            &Binding->StatelessOperTable,
            &OldStatelessCtx->TableEntry,
            NULL);
        QuicListEntryRemove(&OldStatelessCtx->ListEntry);
        Binding->StatelessOperCount--;

        //
        // If it's also processed, free it.
        //
        if (OldStatelessCtx->IsProcessed) {
            QuicPoolFree(
                &OldStatelessCtx->Worker->StatelessContextPool,
                OldStatelessCtx);
        }
    }

    if (Binding->StatelessOperCount >= QUIC_MAX_BINDING_STATELESS_OPERATIONS) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Max binding operations reached");
        goto Exit;
    }

    //
    // Check for pre-existing operations already in the tracking structures.
    //

    QUIC_HASHTABLE_LOOKUP_CONTEXT Context;
    QUIC_HASHTABLE_ENTRY* TableEntry =
        QuicHashtableLookup(&Binding->StatelessOperTable, Hash, &Context);

    while (TableEntry != NULL) {
        const QUIC_STATELESS_CONTEXT* ExistingCtx =
            QUIC_CONTAINING_RECORD(TableEntry, QUIC_STATELESS_CONTEXT, TableEntry);

        if (QuicAddrCompare(&ExistingCtx->RemoteAddress, RemoteAddress)) {
            QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
                "Already in stateless oper table");
            goto Exit;
        }

        TableEntry =
            QuicHashtableLookupNext(&Binding->StatelessOperTable, &Context);
    }

    //
    // Not already in the tracking structures, so allocate and insert a new one.
    //

    StatelessCtx =
        (QUIC_STATELESS_CONTEXT*)QuicPoolAlloc(&Worker->StatelessContextPool);
    if (StatelessCtx == NULL) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Alloc failure for stateless oper ctx");
        goto Exit;
    }

    StatelessCtx->Binding = Binding;
    StatelessCtx->Worker = Worker;
    StatelessCtx->Datagram = Datagram;
    StatelessCtx->CreationTimeMs = TimeMs;
    StatelessCtx->HasBindingRef = FALSE;
    StatelessCtx->IsProcessed = FALSE;
    StatelessCtx->IsExpired = FALSE;
    QuicCopyMemory(&StatelessCtx->RemoteAddress, RemoteAddress, sizeof(QUIC_ADDR));

    QuicHashtableInsert(
        &Binding->StatelessOperTable,
        &StatelessCtx->TableEntry,
        Hash,
        NULL); // TODO - Context?

    QuicListInsertTail(
        &Binding->StatelessOperList,
        &StatelessCtx->ListEntry
        );

    Binding->StatelessOperCount++;

Exit:

    QuicDispatchLockRelease(&Binding->StatelessOperLock);

    return StatelessCtx;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingQueueStatelessOperation(
    _In_ PQUIC_BINDING Binding,
    _In_ QUIC_OPERATION_TYPE OperType,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    if (MsQuicLib.WorkerPool == NULL) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "NULL worker pool");
        return FALSE;
    }

    QUIC_WORKER* Worker = QuicLibraryGetWorker();
    if (QuicWorkerIsOverloaded(Worker)) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Worker overloaded (stateless oper)");
        return FALSE;
    }

    QUIC_STATELESS_CONTEXT* Context =
        QuicBindingCreateStatelessOperation(Binding, Worker, Datagram);
    if (Context == NULL) {
        return FALSE;
    }

    QUIC_OPERATION* Oper = QuicOperationAlloc(Worker, OperType);
    if (Oper == NULL) {
        EventWriteQuicAllocFailure("stateless operation", sizeof(QUIC_OPERATION));
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Alloc failure for stateless operation");
        QuicBindingReleaseStatelessOperation(Context, FALSE);
        return FALSE;
    }

    Oper->STATELESS.Context = Context;
    QuicWorkerQueueOperation(Worker, Oper);

    return TRUE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingProcessStatelessOperation(
    _In_ uint32_t OperationType,
    _In_ QUIC_STATELESS_CONTEXT* StatelessCtx
    )
{
    PQUIC_BINDING Binding = StatelessCtx->Binding;
    QUIC_RECV_DATAGRAM* RecvDatagram = StatelessCtx->Datagram;
    QUIC_RECV_PACKET* RecvPacket =
        QuicDataPathRecvDatagramToRecvPacket(RecvDatagram);

    QUIC_DBG_ASSERT(RecvPacket->ValidatedHeaderInv);

    EventWriteQuicBindingExecOper(Binding, OperationType);

    PQUIC_DATAPATH_SEND_CONTEXT SendContext =
        QuicDataPathBindingAllocSendContext(Binding->DatapathBinding, 0);
    if (SendContext == NULL) {
        EventWriteQuicAllocFailure("stateless send context", 0);
        goto Exit;
    }

    if (OperationType == QUIC_OPER_TYPE_VERSION_NEGOTIATION) {

        QUIC_DBG_ASSERT(RecvPacket->DestCID != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCID != NULL);

        uint16_t PacketLength =
            sizeof(QUIC_VERSION_NEGOTIATION_PACKET) +   // Header
            RecvPacket->SourceCIDLen +
            sizeof(uint8_t) +
            RecvPacket->DestCIDLen +
            sizeof(uint32_t) +                          // One random version
            sizeof(QuicSupportedVersionList);           // Our actual supported versions

        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            EventWriteQuicAllocFailure("vn datagram", PacketLength);
            goto Exit;
        }

        PQUIC_VERSION_NEGOTIATION_PACKET VerNeg =
            (PQUIC_VERSION_NEGOTIATION_PACKET)SendDatagram->Buffer;
        QUIC_DBG_ASSERT(SendDatagram->Length == PacketLength);

        VerNeg->IsLongHeader = TRUE;
        VerNeg->Version = QUIC_VERSION_VER_NEG;

        uint8_t* Buffer = VerNeg->DestCID;
        VerNeg->DestCIDLength = RecvPacket->SourceCIDLen;
        memcpy(
            Buffer,
            RecvPacket->SourceCID,
            RecvPacket->SourceCIDLen);
        Buffer += RecvPacket->SourceCIDLen;

        *Buffer = RecvPacket->DestCIDLen;
        Buffer++;
        memcpy(
            Buffer,
            RecvPacket->DestCID,
            RecvPacket->DestCIDLen);
        Buffer += RecvPacket->DestCIDLen;

        uint8_t RandomValue = 0;
        QuicRandom(sizeof(uint8_t), &RandomValue);
        VerNeg->Unused = 0x7F & RandomValue;

        uint32_t* SupportedVersion = (uint32_t*)Buffer;
        SupportedVersion[0] = Binding->RandomReservedVersion;
        QuicCopyMemory(
            &SupportedVersion[1],
            QuicSupportedVersionList,
            sizeof(QuicSupportedVersionList));

        LogPacketInfo("[S][TX][-] VN");

    } else if (OperationType == QUIC_OPER_TYPE_STATELESS_RESET) {

        QUIC_DBG_ASSERT(RecvPacket->DestCID != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCID == NULL);

        //
        // There are a few requirements for sending stateless reset packets:
        //
        //   - It must be smaller than the received packet.
        //   - It must be larger than a spec defined minimum (39 bytes).
        //   - It must be sufficiently random so that a middle box cannot easily
        //     detect that it is a stateless reset packet.
        //

        //
        // Add a bit of randomness (3 bits worth) to the packet length.
        //
        uint8_t PacketLength;
        QuicRandom(sizeof(PacketLength), &PacketLength);
        PacketLength >>= 5; // Only drop 5 of the 8 bits of randomness.
        PacketLength += QUIC_RECOMMENDED_STATELESS_RESET_PACKET_LENGTH;

        if (PacketLength >= RecvPacket->BufferLength) {
            //
            // Can't go over the recieve packet's length.
            //
            PacketLength = (uint8_t)RecvPacket->BufferLength - 1;
        }

        QUIC_DBG_ASSERT(PacketLength >= QUIC_MIN_STATELESS_RESET_PACKET_LENGTH);

        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            EventWriteQuicAllocFailure("reset datagram", PacketLength);
            goto Exit;
        }

        QUIC_SHORT_HEADER_D23* ResetPacket =
            (QUIC_SHORT_HEADER_D23*)SendDatagram->Buffer;
        QUIC_DBG_ASSERT(SendDatagram->Length == PacketLength);

        QuicRandom(
            PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH,
            SendDatagram->Buffer);
        ResetPacket->IsLongHeader = FALSE;
        ResetPacket->FixedBit = 1;
        ResetPacket->KeyPhase = RecvPacket->SH->KeyPhase;
        QuicBindingGenerateStatelessResetToken(
            Binding,
            RecvPacket->DestCID,
            SendDatagram->Buffer + PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH);

        LogPacketInfo("[S][TX][-] SR %s",
            QuicCidBufToStr(
                SendDatagram->Buffer + PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH,
                QUIC_STATELESS_RESET_TOKEN_LENGTH
            ).Buffer);

    } else if (OperationType == QUIC_OPER_TYPE_RETRY) {

        QUIC_DBG_ASSERT(RecvPacket->DestCID != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCID != NULL);

        uint16_t PacketLength = QuicPacketMaxBufferSizeForRetryD23();
        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            EventWriteQuicAllocFailure("retry datagram", PacketLength);
            QuicDataPathBindingFreeSendContext(SendContext);
            goto Exit;
        }

        uint8_t NewDestCID[MSQUIC_CONNECTION_ID_LENGTH];
        QuicRandom(MSQUIC_CONNECTION_ID_LENGTH, NewDestCID);

        QUIC_RETRY_TOKEN_CONTENTS Token;
        Token.RemoteAddress = RecvDatagram->Tuple->RemoteAddress;
        QuicCopyMemory(Token.OrigConnId, RecvPacket->DestCID, RecvPacket->DestCIDLen);
        Token.OrigConnIdLength = RecvPacket->DestCIDLen;

        uint8_t Iv[QUIC_IV_LENGTH];
        QuicCopyMemory(Iv, NewDestCID, MSQUIC_CONNECTION_ID_LENGTH);
        QuicZeroMemory(
            Iv + MSQUIC_CONNECTION_ID_LENGTH,
            QUIC_IV_LENGTH - MSQUIC_CONNECTION_ID_LENGTH);
        QuicEncrypt(
            MsQuicLib.StatelessRetryKey,
            Iv,
            0, NULL,
            sizeof(Token), (uint8_t*)&Token);

        SendDatagram->Length =
            QuicPacketEncodeRetryD23(
                RecvPacket->LH->Version,
                RecvPacket->SourceCID, RecvPacket->SourceCIDLen,
                NewDestCID, MSQUIC_CONNECTION_ID_LENGTH,
                RecvPacket->DestCID, RecvPacket->DestCIDLen,
                sizeof(Token),
                (uint8_t*)&Token,
                (uint16_t)SendDatagram->Length,
                (uint8_t*)SendDatagram->Buffer);
        QUIC_DBG_ASSERT(SendDatagram->Length != 0);

        LogPacketInfo(
            "[S][TX][-] LH Ver:0x%x DestCID:%s SrcCID:%s Type:R OrigDestCID:%s (Token %hu bytes)",
            RecvPacket->LH->Version,
            QuicCidBufToStr(RecvPacket->SourceCID, RecvPacket->SourceCIDLen).Buffer,
            QuicCidBufToStr(NewDestCID, MSQUIC_CONNECTION_ID_LENGTH).Buffer,
            QuicCidBufToStr(RecvPacket->DestCID, RecvPacket->DestCIDLen).Buffer,
            (uint16_t)sizeof(Token));

    } else {
        QUIC_TEL_ASSERT(FALSE); // Should be unreachable code.
        goto Exit;
    }

    QuicBindingSendFromTo(
        Binding,
        &RecvDatagram->Tuple->LocalAddress,
        &RecvDatagram->Tuple->RemoteAddress,
        SendContext);
    SendContext = NULL;

Exit:

    if (SendContext != NULL) {
        QuicDataPathBindingFreeSendContext(SendContext);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingReleaseStatelessOperation(
    _In_ QUIC_STATELESS_CONTEXT* StatelessCtx,
    _In_ BOOLEAN ReturnDatagram
    )
{
    PQUIC_BINDING Binding = StatelessCtx->Binding;

    if (ReturnDatagram) {
        QuicDataPathBindingReturnRecvDatagrams(StatelessCtx->Datagram);
    }
    StatelessCtx->Datagram = NULL;

    QuicDispatchLockAcquire(&Binding->StatelessOperLock);

    StatelessCtx->IsProcessed = TRUE;
    uint8_t FreeCtx = StatelessCtx->IsExpired;

    QuicDispatchLockRelease(&Binding->StatelessOperLock);

    if (StatelessCtx->HasBindingRef) {
        QuicLibraryReleaseBinding(Binding);
    }

    if (FreeCtx) {
        QuicPoolFree(
            &StatelessCtx->Worker->StatelessContextPool,
            StatelessCtx);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingQueueStatelessReset(
    _In_ PQUIC_BINDING Binding,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    //
    // We don't respond to long header packets because the peer generally
    // doesn't even have the stateless reset token yet. We don't respond to
    // small short header packets because it could cause an infinite loop.
    //
    const QUIC_SHORT_HEADER_D23* Header = (QUIC_SHORT_HEADER_D23*)Datagram->Buffer;
    if (Header->IsLongHeader) {
        return FALSE; // No packet drop log, because it was already logged in QuicBindingShouldCreateConnection.
    }

    if (Datagram->BufferLength <= QUIC_MIN_STATELESS_RESET_PACKET_LENGTH) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Packet too short for stateless reset");
        return FALSE;
    }

    if (Binding->Exclusive) {
        //
        // Can't support stateless reset in exclusive mode, because we don't use
        // a connection ID. Without a connection ID, a stateless reset token
        // cannot be generated.
        //
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "No stateless reset on exclusive binding");
        return FALSE;
    }

    return
        QuicBindingQueueStatelessOperation(
            Binding, QUIC_OPER_TYPE_STATELESS_RESET, Datagram);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingPreprocessPacket(
    _In_ PQUIC_BINDING Binding,
    _Inout_ QUIC_RECV_DATAGRAM* Datagram,
    _Out_ BOOLEAN* ReleasePacket
    )
{
    QUIC_RECV_PACKET* Packet = QuicDataPathRecvDatagramToRecvPacket(Datagram);
    QuicZeroMemory(Packet, sizeof(QUIC_RECV_PACKET));
    Packet->Buffer = Datagram->Buffer;
    Packet->BufferLength = Datagram->BufferLength;

    *ReleasePacket = TRUE;

    //
    // Get the destination connection ID from the packet so we can use it for
    // determining partition delivery. All this must be version INDEPENDENT as
    // we haven't done any version validation at this point.
    //

    if (!QuicPacketValidateInvariant(Binding, Packet, !Binding->Exclusive)) {
        return FALSE;
    }

    if (Binding->Exclusive) {
        if (Packet->DestCIDLen != 0) {
            QuicPacketLogDrop(Binding, Packet, "Non-zero length CID on exclusive binding");
            return FALSE;
        }
    } else {
        if (Packet->DestCIDLen == 0) {
            QuicPacketLogDrop(Binding, Packet, "Zero length CID on non-exclusive binding");
            return FALSE;

        } else if (Packet->DestCIDLen < QUIC_MIN_INITIAL_CONNECTION_ID_LENGTH) {
            QuicPacketLogDrop(Binding, Packet, "Less than min length CID on non-exclusive binding");
            return FALSE;
        }
    }

    if (Packet->Invariant->IsLongHeader) {
        //
        // Validate we support this long header packet version.
        //
        if (!QuicIsVersionSupported(Packet->Invariant->LONG_HDR.Version)) {
            if (!QuicBindingHasListenerRegistered(Binding)) {
                QuicPacketLogDrop(Binding, Packet, "No listener to send VN");
            } else {
                *ReleasePacket =
                    !QuicBindingQueueStatelessOperation(
                        Binding, QUIC_OPER_TYPE_VERSION_NEGOTIATION, Datagram);
            }
            return FALSE;
        }
    }

    *ReleasePacket = FALSE;

    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingShouldCreateConnection(
    _In_ const QUIC_BINDING* const Binding,
    _In_ const QUIC_RECV_PACKET* const Packet
    )
{
    if (!Packet->Invariant->IsLongHeader) {
        return FALSE; // Don't log drop. Stateless reset code may or may not.
    }

    if (!QuicBindingHasListenerRegistered(Binding)) {
        QuicPacketLogDrop(Binding, Packet, "LH packet not matched with a connection and no listeners registered");
        return FALSE;
    }

    if (Packet->Invariant->LONG_HDR.Version == QUIC_VERSION_VER_NEG) {
        QuicPacketLogDrop(Binding, Packet, "Version negotiation packet not matched with a connection");
        return FALSE;
    }

    QUIC_DBG_ASSERT(Packet->Invariant->LONG_HDR.Version != QUIC_VERSION_VER_NEG);

    if (!QuicPacketCanCreateNewConnection(Binding, Packet)) {
        return FALSE;
    }

    //
    // We have a listener on the binding and the packet is allowed to create a
    // new connection.
    //
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingProcessRetryToken(
    _In_ const QUIC_BINDING* const Binding,
    _In_ const QUIC_RECV_PACKET* const Packet,
    _In_ uint16_t TokenLength,
    _In_reads_(TokenLength)
        const uint8_t* TokenBuffer
    )
{
    if (TokenLength != sizeof(QUIC_RETRY_TOKEN_CONTENTS)) {
        QuicPacketLogDrop(Binding, Packet, "Invalid Retry Token Length");
        return FALSE;
    }

    QUIC_RETRY_TOKEN_CONTENTS Token;
    if (!QuicRetryTokenDecrypt(Packet, TokenBuffer, &Token)) {
        QuicPacketLogDrop(Binding, Packet, "Retry Token Decryption Failure");
        return FALSE;
    }

    if (Token.OrigConnIdLength > sizeof(Token.OrigConnId)) {
        QuicPacketLogDrop(Binding, Packet, "Invalid Retry Token OrigConnId Length");
        return FALSE;
    }

    const QUIC_RECV_DATAGRAM* Datagram =
        QuicDataPathRecvPacketToRecvDatagram(Packet);
    if (!QuicAddrCompare(&Token.RemoteAddress, &Datagram->Tuple->RemoteAddress)) {
        QuicPacketLogDrop(Binding, Packet, "Retry Token Addr Mismatch");
        return FALSE;
    }

    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingShouldRetryConnection(
    _In_ const QUIC_BINDING* const Binding,
    _In_ QUIC_RECV_PACKET* Packet,
    _Inout_ BOOLEAN* DropPacket
    )
{
    //
    // The function is only called once QuicBindingShouldCreateConnection has
    // already returned TRUE. It checks to see if the binding currently has too
    // many connections in the handshake state already. If so, it requests the
    // client to retry its connection attempt to prove source address ownership.
    //

    uint64_t CurrentMemoryLimit =
        (MsQuicLib.Settings.RetryMemoryLimit * QuicTotalMemory) / UINT16_MAX;

    if (MsQuicLib.CurrentHandshakeMemoryUsage < CurrentMemoryLimit) {
        return FALSE;
    }

    const uint8_t* Token = NULL;
    uint16_t TokenLength = 0;

    if (!QuicPacketValidateLongHeaderD23(
            Binding,
            TRUE,
            Packet,
            &Token,
            &TokenLength)) {
        *DropPacket = TRUE;
        return FALSE;
    }

    if (TokenLength == 0) {
        return TRUE;
    }

    QUIC_DBG_ASSERT(Token != NULL);
    if (!QuicBindingProcessRetryToken(Binding, Packet, TokenLength, Token)) {
        *DropPacket = TRUE;
        return FALSE;
    }

    Packet->ValidToken = TRUE;

    return FALSE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PQUIC_CONNECTION
QuicBindingCreateConnection(
    _In_ PQUIC_BINDING Binding,
    _In_ const QUIC_RECV_DATAGRAM* const Datagram
    )
{
    //
    // This function returns either a new connection, or an existing
    // connection if a collision is discovered on calling
    // QuicLookupAddSourceConnectionID.
    //

    PQUIC_CONNECTION Connection = NULL;

    PQUIC_CONNECTION NewConnection;
    QUIC_STATUS Status = QuicConnInitialize(Datagram, &NewConnection);
    if (QUIC_FAILED(Status)) {
        QuicConnRelease(NewConnection, QUIC_CONN_REF_HANDLE_OWNER);
        QuicPacketLogDropWithValue(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Failed to initialize new connection", Status);
        return NULL;
    }

    BOOLEAN BindingRefAdded = FALSE;
    QUIC_DBG_ASSERT(NewConnection->SourceCIDs.Next != NULL);
    QUIC_CID_HASH_ENTRY* SourceCID =
        QUIC_CONTAINING_RECORD(
            NewConnection->SourceCIDs.Next,
            QUIC_CID_HASH_ENTRY,
            Link);

    QuicConnAddRef(NewConnection, QUIC_CONN_REF_LOOKUP_RESULT);

    //
    // Pick a temporary worker to process the client hello and if successful,
    // the connection will later be moved to the correct registration's worker.
    //
    QUIC_WORKER* Worker = QuicLibraryGetWorker();
    if (QuicWorkerIsOverloaded(Worker)) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Worker overloaded");
        goto Exit;
    }
    QuicWorkerAssignConnection(Worker, NewConnection);

    //
    // Even though the new connection might not end up being put in this
    // binding's lookup table, it must be completely set up before it is
    // inserted into the table. Once in the table, other threads/processors
    // could immediately be queuing new operations.
    //

    if (!QuicLibraryTryAddRefBinding(Binding)) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Clean up in progress");
        goto Exit;
    }

    BindingRefAdded = TRUE;
    NewConnection->Binding = Binding;
    InterlockedIncrement(&Binding->HandshakeConnections);
    InterlockedExchangeAdd64(
        (LONG64*)&MsQuicLib.CurrentHandshakeMemoryUsage,
        (LONG64)QUIC_CONN_HANDSHAKE_MEMORY_USAGE);

    if (!QuicLookupAddSourceConnectionID(
            &Binding->Lookup,
            SourceCID,
            &Connection)) {
        //
        // Collision with an existing connection or a memory failure.
        //
        if (Connection == NULL) {
            QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
                "Failed to insert scid");
        }
        goto Exit;
    }

    QuicWorkerQueueConnection(NewConnection->Worker, NewConnection);

    return NewConnection;

Exit:

    NewConnection->SourceCIDs.Next = NULL;
    QUIC_FREE(SourceCID);
    QuicConnRelease(NewConnection, QUIC_CONN_REF_LOOKUP_RESULT);

    if (BindingRefAdded) {
        //
        // The binding ref cannot be released on the receive thread. So, once
        // it has been acquired, we must queue the connection, only to shut it
        // down.
        //
        if (InterlockedCompareExchange16(
                (SHORT*)&Connection->BackUpOperUsed, 1, 0) == 0) {
            PQUIC_OPERATION Oper = &Connection->BackUpOper;
            Oper->FreeAfterProcess = FALSE;
            Oper->Type = QUIC_OPER_TYPE_API_CALL;
            Oper->API_CALL.Context = &Connection->BackupApiContext;
            Oper->API_CALL.Context->Type = QUIC_API_TYPE_CONN_SHUTDOWN;
            Oper->API_CALL.Context->CONN_SHUTDOWN.Flags = QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT;
            Oper->API_CALL.Context->CONN_SHUTDOWN.ErrorCode = 0;
            QuicConnQueueOper(NewConnection, Oper);
        }

    } else {
        QuicConnRelease(NewConnection, QUIC_CONN_REF_HANDLE_OWNER);
    }

    return Connection;
}

//
// Takes a chain of validated receive packets that all have the same
// destination connection ID (i.e. destined for the same connection) and does
// the look up for the corresponding connection. Returns TRUE if delivered and
// false if the packets weren't delivered and should be dropped.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_RECEIVE_CALLBACK)
BOOLEAN
QuicBindingDeliverPackets(
    _In_ PQUIC_BINDING Binding,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain,
    _In_ uint32_t DatagramChainLength
    )
{
    QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(DatagramChain);
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);

    //
    // The packet's destination connection ID (DestCID) is the key for looking
    // up the corresponding connection object. The DestCID encodes the
    // partition ID (PID) that can be used for partitioning the look up table.
    //
    // The exact type of look up table associated with binding varies on the
    // circumstances, but it allows for quick and easy lookup based on DestCID.
    //
    // If the lookup fails, and if there is a listener on the local 2-Tuple,
    // then a new connection is created and inserted into the binding's lookup
    // table.
    //
    // If a new connection is created, it will then be initially processed by
    // a library worker thread to decode the ALPN and SNI. That information
    // will then be used to find the associated listener. If not found, the
    // connection will be thrown away. Otherwise, the listener will then be
    // invoked to allow it to accept the connection and choose a server
    // certificate.
    //
    // If all else fails, and no connection was found or created for the
    // packet, then the packet is dropped.
    //

    PQUIC_CONNECTION Connection =
        QuicLookupFindConnection(
            &Binding->Lookup,
            Packet->DestCID,
            Packet->DestCIDLen);

    if (Connection == NULL) {
        //
        // Because the packet chain is ordered by control packets first, we
        // don't have to worry about a packet that can't create the connection
        // being in front of a packet that can in the chain. So we can always
        // use the head of the chain to determine if a new connection should
        // be created.
        //
        BOOLEAN DropPacket = FALSE;
        if (!QuicBindingShouldCreateConnection(Binding, Packet)) {
            return QuicBindingQueueStatelessReset(Binding, DatagramChain);

        } else if (QuicBindingShouldRetryConnection(Binding, Packet, &DropPacket)) {
            return
                QuicBindingQueueStatelessOperation(
                    Binding, QUIC_OPER_TYPE_RETRY, DatagramChain);

        } else if (!DropPacket) {
            Connection = QuicBindingCreateConnection(Binding, DatagramChain);
        }
    }

    if (Connection != NULL) {
        QuicConnQueueRecvDatagram(Connection, DatagramChain, DatagramChainLength);
        QuicConnRelease(Connection, QUIC_CONN_REF_LOOKUP_RESULT);
        return TRUE;
    } else {
        return FALSE;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_RECEIVE_CALLBACK)
void
QuicBindingReceive(
    _In_ PQUIC_DATAPATH_BINDING DatapathBinding,
    _In_ void* RecvCallbackContext,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain
    )
{
    UNREFERENCED_PARAMETER(DatapathBinding);
    QUIC_DBG_ASSERT(RecvCallbackContext != NULL);
    QUIC_DBG_ASSERT(DatagramChain != NULL);

    PQUIC_BINDING Binding = (PQUIC_BINDING)RecvCallbackContext;
    QUIC_RECV_DATAGRAM* ReleaseChain = NULL;
    QUIC_RECV_DATAGRAM** ReleaseChainTail = &ReleaseChain;
    QUIC_RECV_DATAGRAM* ConnectionChain = NULL;
    QUIC_RECV_DATAGRAM** ConnectionChainTail = &ConnectionChain;
    QUIC_RECV_DATAGRAM** ConnectionChainDataTail = &ConnectionChain;
    uint32_t ConnectionChainLength = 0;

    //
    // Now the goal is to find the connections that these packets should be
    // delivered to, or if necessary create them.
    //
    // The datapath can indicate a chain of multiple received packets at once.
    // The following code breaks the chain up into subchains, by destination
    // connection ID in each packet. Each subchain is then delivered to the
    // connection with a single operation.
    //

    QUIC_RECV_DATAGRAM* Datagram;
    while ((Datagram = DatagramChain) != NULL) {
        //
        // Remove the recv buffer from the chain.
        //
        DatagramChain = Datagram->Next;
        Datagram->Next = NULL;

        //
        // Perform initial packet validation.
        //
        BOOLEAN ReleasePacket;
        if (!QuicBindingPreprocessPacket(Binding, Datagram, &ReleasePacket)) {
            if (ReleasePacket) {
                *ReleaseChainTail = Datagram;
                ReleaseChainTail = &Datagram->Next;
            }
            continue;
        }

        QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(Datagram);
        QUIC_RECV_PACKET* ConnectionChainRecvCtx =
            ConnectionChain == NULL ?
                NULL : QuicDataPathRecvDatagramToRecvPacket(ConnectionChain);
        QUIC_DBG_ASSERT(Packet->DestCID != NULL);
        QUIC_DBG_ASSERT(Packet->DestCIDLen != 0 || Binding->Exclusive);
        QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);

        //
        // Add the packet to a connection subchain. If a the packet doesn't
        // match the existing subchain, deliver the existing one and start a
        // new one. If this UDP binding is exclusively owned. All packets are
        // delivered to a single connection so there is no need to extra
        // processing to split the chain.
        //
        if (!Binding->Exclusive && ConnectionChain != NULL &&
            (Packet->DestCIDLen != ConnectionChainRecvCtx->DestCIDLen ||
             memcmp(Packet->DestCID, ConnectionChainRecvCtx->DestCID, Packet->DestCIDLen) != 0)) {
            //
            // This packet doesn't match the current connection chain. Deliver
            // the current chain and start a new one.
            //
            if (!QuicBindingDeliverPackets(Binding, ConnectionChain, ConnectionChainLength)) {
                *ReleaseChainTail = ConnectionChain;
                ReleaseChainTail = ConnectionChainDataTail;
            }
            ConnectionChain = NULL;
            ConnectionChainTail = &ConnectionChain;
            ConnectionChainDataTail = &ConnectionChain;
            ConnectionChainLength = 0;
        }

        //
        // Insert the packet in the current chain, with handshake packets first.
        // We do this so that we can more easily determine if the chain of
        // packets can create a new connection.
        //

        ConnectionChainLength++;
        if (!QuicPacketIsHandshake(Packet->Invariant)) {
            //
            // Data packets go at the end of the chain.
            //
            *ConnectionChainDataTail = Datagram;
            ConnectionChainDataTail = &Datagram->Next;
        } else {
            //
            // Other packets are ordered before data packets.
            //
            if (*ConnectionChainTail == NULL) {
                *ConnectionChainTail = Datagram;
                ConnectionChainTail = &Datagram->Next;
                ConnectionChainDataTail = &Datagram->Next;
            } else {
                Datagram->Next = *ConnectionChainTail;
                *ConnectionChainTail = Datagram;
                ConnectionChainTail = &Datagram->Next;
            }
        }
    }

    if (ConnectionChain != NULL) {
        //
        // Deliver the last connection chain of packets.
        //
        if (!QuicBindingDeliverPackets(Binding, ConnectionChain, ConnectionChainLength)) {
            *ReleaseChainTail = ConnectionChain;
            ReleaseChainTail = ConnectionChainTail;
        }
    }

    if (ReleaseChain != NULL) {
        QuicDataPathBindingReturnRecvDatagrams(ReleaseChain);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_UNREACHABLE_CALLBACK)
void
QuicBindingUnreachable(
    _In_ PQUIC_DATAPATH_BINDING DatapathBinding,
    _In_ void* Context,
    _In_ const QUIC_ADDR* RemoteAddress
    )
{
    UNREFERENCED_PARAMETER(DatapathBinding);
    QUIC_DBG_ASSERT(Context != NULL);
    QUIC_DBG_ASSERT(RemoteAddress != NULL);

    PQUIC_BINDING Binding = (PQUIC_BINDING)Context;

    PQUIC_CONNECTION Connection =
        QuicLookupFindConnectionByRemoteAddr(
            &Binding->Lookup,
            RemoteAddress);

    if (Connection != NULL) {
        QuicConnQueueUnreachable(Connection, RemoteAddress);
        QuicConnRelease(Connection, QUIC_CONN_REF_LOOKUP_RESULT);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingSendTo(
    _In_ PQUIC_BINDING Binding,
    _In_ const QUIC_ADDR * RemoteAddress,
    _In_ PQUIC_DATAPATH_SEND_CONTEXT SendContext
    )
{
    QUIC_STATUS Status;

#if QUIC_SEND_FAKE_LOSS
    if (QuicFakeLossCanSend()) {
#endif
        Status =
            QuicDataPathBindingSendTo(
                Binding->DatapathBinding,
                RemoteAddress,
                SendContext);
        if (QUIC_FAILED(Status)) {
            LogWarning("[bind][%p] SendTo failed, 0x%x", Binding, Status);
        }
#if QUIC_SEND_FAKE_LOSS
    } else {
        LogPacketInfo("[bind][%p] Dropped (fake loss) packet", Binding);
        QuicDataPathBindingFreeSendContext(SendContext);
        Status = QUIC_STATUS_SUCCESS;
    }
#endif

    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingSendFromTo(
    _In_ PQUIC_BINDING Binding,
    _In_ const QUIC_ADDR * LocalAddress,
    _In_ const QUIC_ADDR * RemoteAddress,
    _In_ PQUIC_DATAPATH_SEND_CONTEXT SendContext
    )
{
    QUIC_STATUS Status;

#if QUIC_SEND_FAKE_LOSS
    if (QuicFakeLossCanSend()) {
#endif
        Status =
            QuicDataPathBindingSendFromTo(
                Binding->DatapathBinding,
                LocalAddress,
                RemoteAddress,
                SendContext);
        if (QUIC_FAILED(Status)) {
            LogWarning("[bind][%p] SendFromTo failed, 0x%x", Binding, Status);
        }
#if QUIC_SEND_FAKE_LOSS
    } else {
        LogPacketInfo("[bind][%p] Dropped (fake loss) packet", Binding);
        QuicDataPathBindingFreeSendContext(SendContext);
        Status = QUIC_STATUS_SUCCESS;
    }
#endif

    return Status;
}

QUIC_STATIC_ASSERT(
    QUIC_HASH_SHA256_SIZE >= QUIC_STATELESS_RESET_TOKEN_LENGTH,
    "Stateless reset token must be shorter than hash size used");

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingGenerateStatelessResetToken(
    _In_ PQUIC_BINDING Binding,
    _In_reads_(MSQUIC_CONNECTION_ID_LENGTH)
        const uint8_t* const CID,
    _Out_writes_all_(QUIC_STATELESS_RESET_TOKEN_LENGTH)
        uint8_t* ResetToken
    )
{
    uint8_t HashOutput[QUIC_HASH_SHA256_SIZE];
    QuicDispatchLockAcquire(&Binding->ResetTokenLock);
    QUIC_STATUS Status =
        QuicHashCompute(
            Binding->ResetTokenHash,
            CID,
            MSQUIC_CONNECTION_ID_LENGTH,
            sizeof(HashOutput),
            HashOutput);
    QuicDispatchLockRelease(&Binding->ResetTokenLock);
    if (QUIC_SUCCEEDED(Status)) {
        QuicCopyMemory(
            ResetToken,
            HashOutput,
            QUIC_STATELESS_RESET_TOKEN_LENGTH);
    }
    return Status;
}