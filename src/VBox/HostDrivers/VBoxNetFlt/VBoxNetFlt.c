/* $Id: VBoxNetFlt.c 18064 2009-03-18 12:52:38Z vboxsync $ */
/** @file
 * VBoxNetFlt - Network Filter Driver (Host), Common Code.
 */

/*
 * Copyright (C) 2008-2009 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

/** @page pg_netflt     VBoxNetFlt - Network Interface Filter
 *
 * This is a kernel module that attaches to a real interface on the host
 * and filters and injects packets.
 *
 * In the big picture we're one of the three trunk interface on the internal
 * network, the one named "NIC Filter Driver": @image html Networking_Overview.gif
 *
 *
 * @section  sec_netflt_msc      Locking / Sequence Diagrams
 *
 * This secion contains a few sequence diagrams describing the problematic
 * transitions of a host interface filter instance.
 *
 * The thing that makes it all a bit problematic is that multiple events may
 * happen at the same time, and that we have to be very careful to avoid
 * deadlocks caused by mixing our locks with the ones in the host kernel.
 * The main events are receive, send, async send completion, disappearance of
 * the host networking interface and it's reappearance. The latter two events
 * are can be caused by driver unloading/loading or the device being physical
 * unplugged (e.g. a USB network device).
 *
 * The strategy for dealing with these issues are:
 *    - Use a simple state machine.
 *    - Require the user (IntNet) to serialize all its calls to us,
 *      while at the same time not owning any lock used by any of the
 *      the callbacks we might call on receive and async send completion.
 *    - Make sure we're 100% idle before disconnecting, and have a
 *      disconnected status on both sides to fend off async calls.
 *    - Protect the host specific interface handle and the state variables
 *      using a spinlock.
 *
 *
 * @subsection subsec_netflt_msc_dis_rel    Disconnect from the network and release
 *
 * @msc
 *      VM, IntNet, NetFlt, Kernel, Wire;
 *
 *      VM->IntNet      [label="pkt0", linecolor="green", textcolor="green"];
 *      IntNet=>IntNet  [label="Lock Network", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Route packet -> wire", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Unlock Network", linecolor="green", textcolor="green" ];
 *      IntNet=>NetFlt  [label="pkt0 to wire", linecolor="green", textcolor="green" ];
 *      NetFlt=>Kernel  [label="pkt0 to wire", linecolor="green", textcolor="green"];
 *      Kernel->Wire    [label="pkt0 to wire", linecolor="green", textcolor="green"];
 *
 *      ---             [label="Suspending the trunk interface"];
 *      IntNet=>IntNet  [label="Lock Network"];
 *
 *      Wire->Kernel    [label="pkt1 - racing us", linecolor="red", textcolor="red"];
 *      Kernel=>>NetFlt [label="pkt1 - racing us", linecolor="red", textcolor="red"];
 *      NetFlt=>>IntNet [label="pkt1 recv - blocks", linecolor="red", textcolor="red"];
 *
 *      IntNet=>IntNet  [label="Mark Trunk Suspended"];
 *      IntNet=>IntNet  [label="Unlock Network"];
 *
 *      IntNet=>NetFlt  [label="pfnSetActive(false)"];
 *      NetFlt=>NetFlt  [label="Mark inactive (atomic)"];
 *      IntNet<<NetFlt;
 *      IntNet=>NetFlt  [label="pfnWaitForIdle(forever)"];
 *
 *      IntNet=>>NetFlt [label="pkt1 to host", linecolor="red", textcolor="red"];
 *      NetFlt=>>Kernel [label="pkt1 to host", linecolor="red", textcolor="red"];
 *
 *      Kernel<-Wire    [label="pkt0 on wire", linecolor="green", textcolor="green"];
 *      NetFlt<<Kernel  [label="pkt0 on wire", linecolor="green", textcolor="green"];
 *      IntNet<<=NetFlt [label="pfnSGRelease", linecolor="green", textcolor="green"];
 *      IntNet<<=IntNet [label="Lock Net, free SG, Unlock Net", linecolor="green", textcolor="green"];
 *      IntNet>>NetFlt  [label="pfnSGRelease", linecolor="green", textcolor="green"];
 *      NetFlt<-NetFlt  [label="idle", linecolor="green", textcolor="green"];
 *
 *      IntNet<<NetFlt  [label="idle (pfnWaitForIdle)"];
 *
 *      Wire->Kernel    [label="pkt2", linecolor="red", textcolor="red"];
 *      Kernel=>>NetFlt [label="pkt2", linecolor="red", textcolor="red"];
 *      NetFlt=>>Kernel [label="pkt2 to host", linecolor="red", textcolor="red"];
 *
 *      VM->IntNet      [label="pkt3", linecolor="green", textcolor="green"];
 *      IntNet=>IntNet  [label="Lock Network", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Route packet -> drop", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Unlock Network", linecolor="green", textcolor="green" ];
 *
 *      ---             [label="The trunk interface is idle now, disconnect it"];
 *      IntNet=>IntNet  [label="Lock Network"];
 *      IntNet=>IntNet  [label="Unlink Trunk"];
 *      IntNet=>IntNet  [label="Unlock Network"];
 *      IntNet=>NetFlt  [label="pfnDisconnectAndRelease"];
 *      NetFlt=>Kernel  [label="iflt_detach"];
 *      NetFlt<<=Kernel [label="iff_detached"];
 *      NetFlt>>Kernel  [label="iff_detached"];
 *      NetFlt<<Kernel  [label="iflt_detach"];
 *      NetFlt=>NetFlt  [label="Release"];
 *      IntNet<<NetFlt  [label="pfnDisconnectAndRelease"];
 *
 * @endmsc
 *
 *
 *
 * @subsection subsec_netflt_msc_hif_rm    Host Interface Removal
 *
 * The ifnet_t (pIf) is a tricky customer as any reference to it can potentially
 * race the filter detaching. The simple way of solving it on Darwin is to guard
 * all access to the pIf member with a spinlock. The other host systems will
 * probably have similar race conditions, so the spinlock is a generic thing.
 *
 * @msc
 *      VM, IntNet, NetFlt, Kernel;
 *
 *      VM->IntNet      [label="pkt0", linecolor="green", textcolor="green"];
 *      IntNet=>IntNet  [label="Lock Network", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Route packet -> wire", linecolor="green", textcolor="green" ];
 *      IntNet=>IntNet  [label="Unlock Network", linecolor="green", textcolor="green" ];
 *      IntNet=>NetFlt  [label="pkt0 to wire", linecolor="green", textcolor="green" ];
 *      NetFlt=>Kernel  [label="ifnet_reference w/ spinlock", linecolor="green", textcolor="green" ];
 *      NetFlt<<Kernel  [label="ifnet_reference", linecolor="green", textcolor="green" ];
 *      NetFlt=>Kernel  [label="pkt0 to wire (blocks)", linecolor="green", textcolor="green" ];
 *
 *      ---             [label="The host interface is being disconnected"];
 *      Kernel->NetFlt  [label="iff_detached"];
 *      NetFlt=>Kernel  [label="ifnet_release w/ spinlock"];
 *      NetFlt<<Kernel  [label="ifnet_release"];
 *      NetFlt=>NetFlt  [label="fDisconnectedFromHost=true"];
 *      NetFlt>>Kernel  [label="iff_detached"];
 *
 *      NetFlt<<Kernel  [label="dropped", linecolor="green", textcolor="green"];
 *      NetFlt=>NetFlt  [label="Acquire spinlock", linecolor="green", textcolor="green"];
 *      NetFlt=>Kernel  [label="ifnet_release", linecolor="green", textcolor="green"];
 *      NetFlt<<Kernel  [label="ifnet_release", linecolor="green", textcolor="green"];
 *      NetFlt=>NetFlt  [label="pIf=NULL", linecolor="green", textcolor="green"];
 *      NetFlt=>NetFlt  [label="Release spinlock", linecolor="green", textcolor="green"];
 *      IntNet<=NetFlt  [label="pfnSGRelease", linecolor="green", textcolor="green"];
 *      IntNet>>NetFlt  [label="pfnSGRelease", linecolor="green", textcolor="green"];
 *      IntNet<<NetFlt  [label="pkt0 to wire", linecolor="green", textcolor="green"];
 *
 * @endmsc
 *
 *
 *
 * @subsection subsec_netflt_msc_hif_rm    Host Interface Rediscovery
 *
 * The rediscovery is performed when we receive a send request and a certain
 * period have elapsed since the last attempt, i.e. we're polling it. We
 * synchronize the rediscovery with disconnection from the internal network
 * by means of the pfnWaitForIdle call, so no special handling is required.
 *
 * @msc
 *      VM2, VM1, IntNet, NetFlt, Kernel, Wire;
 *
 *      ---             [label="Rediscovery conditions are not met"];
 *      VM1->IntNet     [label="pkt0"];
 *      IntNet=>IntNet  [label="Lock Network"];
 *      IntNet=>IntNet  [label="Route packet -> wire"];
 *      IntNet=>IntNet  [label="Unlock Network"];
 *      IntNet=>NetFlt  [label="pkt0 to wire"];
 *      NetFlt=>NetFlt  [label="Read pIf(==NULL) w/ spinlock"];
 *      IntNet<<NetFlt  [label="pkt0 to wire (dropped)"];
 *
 *      ---             [label="Rediscovery conditions"];
 *      VM1->IntNet     [label="pkt1"];
 *      IntNet=>IntNet  [label="Lock Network"];
 *      IntNet=>IntNet  [label="Route packet -> wire"];
 *      IntNet=>IntNet  [label="Unlock Network"];
 *      IntNet=>NetFlt  [label="pkt1 to wire"];
 *      NetFlt=>NetFlt  [label="Read pIf(==NULL) w/ spinlock"];
 *      NetFlt=>NetFlt  [label="fRediscoveryPending=true w/ spinlock"];
 *      NetFlt=>Kernel  [label="ifnet_find_by_name"];
 *      NetFlt<<Kernel  [label="ifnet_find_by_name (success)"];
 *
 *      VM2->IntNet     [label="pkt2", linecolor="red", textcolor="red"];
 *      IntNet=>IntNet  [label="Lock Network", linecolor="red", textcolor="red"];
 *      IntNet=>IntNet  [label="Route packet -> wire", linecolor="red", textcolor="red"];
 *      IntNet=>IntNet  [label="Unlock Network", linecolor="red", textcolor="red"];
 *      IntNet=>NetFlt  [label="pkt2 to wire", linecolor="red", textcolor="red"];
 *      NetFlt=>NetFlt  [label="!pIf || fRediscoveryPending (w/ spinlock)", linecolor="red", textcolor="red"];
 *      IntNet<<NetFlt  [label="pkt2 to wire (dropped)", linecolor="red", textcolor="red"];

 *      NetFlt=>Kernel  [label="iflt_attach"];
 *      NetFlt<<Kernel  [label="iflt_attach (success)"];
 *      NetFlt=>NetFlt  [label="Acquire spinlock"];
 *      NetFlt=>NetFlt  [label="Set pIf and update flags"];
 *      NetFlt=>NetFlt  [label="Release spinlock"];
 *
 *      NetFlt=>Kernel  [label="pkt1 to wire"];
 *      Kernel->Wire    [label="pkt1 to wire"];
 *      NetFlt<<Kernel  [label="pkt1 to wire"];
 *      IntNet<<NetFlt  [label="pkt1 to wire"];
 *
 *
 * @endmsc
 *
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_NET_FLT_DRV
#include "VBoxNetFltInternal.h"

#include <VBox/sup.h>
#include <VBox/log.h>
#include <VBox/err.h>
#include <iprt/assert.h>
#include <iprt/string.h>
#include <iprt/spinlock.h>
#include <iprt/uuid.h>
#include <iprt/mem.h>
#include <iprt/time.h>
#include <iprt/semaphore.h>
#include <iprt/thread.h>


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
#define IFPORT_2_VBOXNETFLTINS(pIfPort) \
    ( (PVBOXNETFLTINS)((uint8_t *)pIfPort - RT_OFFSETOF(VBOXNETFLTINS, MyPort)) )


AssertCompileMemberSize(VBOXNETFLTINS, enmState, sizeof(uint32_t));

/**
 * Sets the enmState member atomically.
 *
 * Used for all updates.
 *
 * @param   pThis           The instance.
 * @param   enmNewState     The new value.
 */
DECLINLINE(void) vboxNetFltSetState(PVBOXNETFLTINS pThis, VBOXNETFTLINSSTATE enmNewState)
{
    ASMAtomicWriteU32((uint32_t volatile *)&pThis->enmState, enmNewState);
}


/**
 * Gets the enmState member atomically.
 *
 * Used for all reads.
 *
 * @returns The enmState value.
 * @param   pThis           The instance.
 */
DECLINLINE(VBOXNETFTLINSSTATE) vboxNetFltGetState(PVBOXNETFLTINS pThis)
{
    return (VBOXNETFTLINSSTATE)ASMAtomicUoReadU32((uint32_t volatile *)&pThis->enmState);
}


/**
 * Finds a instance by its name, the caller does the locking.
 *
 * @returns Pointer to the instance by the given name. NULL if not found.
 * @param   pGlobals        The globals.
 * @param   pszName         The name of the instance.
 */
static PVBOXNETFLTINS vboxNetFltFindInstanceLocked(PVBOXNETFLTGLOBALS pGlobals, const char *pszName)
{
    PVBOXNETFLTINS pCur;
    for (pCur = pGlobals->pInstanceHead; pCur; pCur = pCur->pNext)
        if (!strcmp(pszName, pCur->szName))
            return pCur;
    return NULL;
}


/**
 * Finds a instance by its name, will request the mutex.
 *
 * No reference to the instance is retained, we're assuming the caller to
 * already have one but just for some reason doesn't have the pointer to it.
 *
 * @returns Pointer to the instance by the given name. NULL if not found.
 * @param   pGlobals        The globals.
 * @param   pszName         The name of the instance.
 */
DECLHIDDEN(PVBOXNETFLTINS) vboxNetFltFindInstance(PVBOXNETFLTGLOBALS pGlobals, const char *pszName)
{
    PVBOXNETFLTINS pRet;
    int rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
    AssertRCReturn(rc, NULL);

    pRet = vboxNetFltFindInstanceLocked(pGlobals, pszName);

    rc = RTSemFastMutexRelease(pGlobals->hFastMtx);
    AssertRC(rc);
    return pRet;
}


/**
 * Unlinks an instance from the chain.
 *
 * @param   pGlobals        The globals.
 * @param   pToUnlink       The instance to unlink.
 */
static void vboxNetFltUnlinkLocked(PVBOXNETFLTGLOBALS pGlobals, PVBOXNETFLTINS pToUnlink)
{
    if (pGlobals->pInstanceHead == pToUnlink)
        pGlobals->pInstanceHead = pToUnlink->pNext;
    else
    {
        PVBOXNETFLTINS pCur;
        for (pCur = pGlobals->pInstanceHead; pCur; pCur = pCur->pNext)
            if (pCur->pNext == pToUnlink)
            {
                pCur->pNext = pToUnlink->pNext;
                break;
            }
        Assert(pCur);
    }
    pToUnlink->pNext = NULL;
}


/**
 * Performs interface rediscovery if it was disconnected from the host.
 *
 * @returns true if successfully rediscovered and connected, false if not.
 * @param   pThis           The instance.
 */
static bool vboxNetFltMaybeRediscovered(PVBOXNETFLTINS pThis)
{
    RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;
    uint64_t Now = RTTimeNanoTS();
    bool fRediscovered;
    bool fDoIt;

    /*
     * Rediscovered already? Time to try again?
     */
    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);

    fRediscovered = !ASMAtomicUoReadBool(&pThis->fDisconnectedFromHost);
    fDoIt = !fRediscovered
         && !ASMAtomicUoReadBool(&pThis->fRediscoveryPending)
         && Now - ASMAtomicUoReadU64(&pThis->NanoTSLastRediscovery) > UINT64_C(5000000000); /* 5 sec */
    if (fDoIt)
        ASMAtomicWriteBool(&pThis->fRediscoveryPending, true);

    RTSpinlockRelease(pThis->hSpinlock, &Tmp);

    /*
     * Call the OS specific code to do the job.
     * Update the state when the call returns, that is everything except for
     * the fDisconnectedFromHost flag which the OS specific code shall set.
     */
    if (fDoIt)
    {
        fRediscovered = vboxNetFltOsMaybeRediscovered(pThis);

        Assert(!fRediscovered || !ASMAtomicUoReadBool(&pThis->fDisconnectedFromHost));

        ASMAtomicUoWriteU64(&pThis->NanoTSLastRediscovery, RTTimeNanoTS());
        ASMAtomicWriteBool(&pThis->fRediscoveryPending, false);

        if (fRediscovered)
            vboxNetFltPortOsSetActive(pThis, pThis->fActive);
    }

    return fRediscovered;
}

#ifdef RT_WITH_W64_UNWIND_HACK
# if defined(RT_OS_WINDOWS) && defined(RT_ARCH_AMD64)
#  define NETFLT_DECL_CALLBACK(type) DECLASM(DECLHIDDEN(type))
#  define NETFLT_CALLBACK(_n) netfltNtWrap##_n

NETFLT_DECL_CALLBACK(int)  NETFLT_CALLBACK(vboxNetFltPortXmit)(PINTNETTRUNKIFPORT pIfPort, PINTNETSG pSG, uint32_t fDst);
NETFLT_DECL_CALLBACK(bool) NETFLT_CALLBACK(vboxNetFltPortIsPromiscuous)(PINTNETTRUNKIFPORT pIfPort);
NETFLT_DECL_CALLBACK(void) NETFLT_CALLBACK(vboxNetFltPortGetMacAddress)(PINTNETTRUNKIFPORT pIfPort, PRTMAC pMac);
NETFLT_DECL_CALLBACK(bool) NETFLT_CALLBACK(vboxNetFltPortIsHostMac)(PINTNETTRUNKIFPORT pIfPort, PCRTMAC pMac);
NETFLT_DECL_CALLBACK(int)  NETFLT_CALLBACK(vboxNetFltPortWaitForIdle)(PINTNETTRUNKIFPORT pIfPort, uint32_t cMillies);
NETFLT_DECL_CALLBACK(bool) NETFLT_CALLBACK(vboxNetFltPortSetActive)(PINTNETTRUNKIFPORT pIfPort, bool fActive);
NETFLT_DECL_CALLBACK(void) NETFLT_CALLBACK(vboxNetFltPortDisconnectAndRelease)(PINTNETTRUNKIFPORT pIfPort);
NETFLT_DECL_CALLBACK(void) NETFLT_CALLBACK(vboxNetFltPortRetain)(PINTNETTRUNKIFPORT pIfPort);
NETFLT_DECL_CALLBACK(void) NETFLT_CALLBACK(vboxNetFltPortRelease)(PINTNETTRUNKIFPORT pIfPort);

# else
#  error "UNSUPPORTED (RT_WITH_W64_UNWIND_HACK)"
# endif
#else
# define NETFLT_DECL_CALLBACK(type) static DECLCALLBACK(type)
# define NETFLT_CALLBACK(_n) _n
#endif

/**
 * @copydoc INTNETTRUNKIFPORT::pfnXmit
 */
NETFLT_DECL_CALLBACK(int) vboxNetFltPortXmit(PINTNETTRUNKIFPORT pIfPort, PINTNETSG pSG, uint32_t fDst)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);
    int rc = VINF_SUCCESS;

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    AssertPtr(pSG);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    AssertReturn(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected, VERR_INVALID_STATE);
    Assert(pThis->fActive);

    /*
     * Do a busy retain and then make sure we're connected to the interface
     * before invoking the OS specific code.
     */
    vboxNetFltRetain(pThis, true /* fBusy */);
    if (    !ASMAtomicUoReadBool(&pThis->fDisconnectedFromHost)
        ||  vboxNetFltMaybeRediscovered(pThis))
        rc = vboxNetFltPortOsXmit(pThis, pSG, fDst);
    vboxNetFltRelease(pThis, true /* fBusy */);

    return rc;
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnIsPromiscuous
 */
NETFLT_DECL_CALLBACK(bool) vboxNetFltPortIsPromiscuous(PINTNETTRUNKIFPORT pIfPort)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected);
    Assert(pThis->fActive);

    /*
     * Ask the OS specific code.
     */
    return vboxNetFltPortOsIsPromiscuous(pThis);
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnGetMacAddress
 */
NETFLT_DECL_CALLBACK(void) vboxNetFltPortGetMacAddress(PINTNETTRUNKIFPORT pIfPort, PRTMAC pMac)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected);
    Assert(pThis->fActive);

    /*
     * Forward the question to the OS specific code.
     */
    vboxNetFltPortOsGetMacAddress(pThis, pMac);
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnIsHostMac
 */
NETFLT_DECL_CALLBACK(bool) vboxNetFltPortIsHostMac(PINTNETTRUNKIFPORT pIfPort, PCRTMAC pMac)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected);
    Assert(pThis->fActive);

    /*
     * Ask the OS specific code.
     */
    return vboxNetFltPortOsIsHostMac(pThis, pMac);
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnWaitForIdle
 */
NETFLT_DECL_CALLBACK(int) vboxNetFltPortWaitForIdle(PINTNETTRUNKIFPORT pIfPort, uint32_t cMillies)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);
    int rc;

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    AssertReturn(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected, VERR_INVALID_STATE);
    AssertReturn(!pThis->fActive, VERR_INVALID_STATE);

    /*
     * Go to sleep on the semaphore after checking the busy count.
     */
    vboxNetFltRetain(pThis, false /* fBusy */);

    rc = VINF_SUCCESS;
    while (pThis->cBusy && RT_SUCCESS(rc))
        rc = RTSemEventWait(pThis->hEventIdle, cMillies); /** @todo make interruptible? */

    vboxNetFltRelease(pThis, false /* fBusy */);

    return rc;
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnSetActive
 */
NETFLT_DECL_CALLBACK(bool) vboxNetFltPortSetActive(PINTNETTRUNKIFPORT pIfPort, bool fActive)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);

    /*
     * Input validation.
     */
    AssertPtr(pThis);
    AssertPtr(pThis->pGlobals);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    AssertReturn(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected, false);

    /*
     * We're assuming that the caller is serializing the calls, so we don't
     * have to be extremely careful here. Just update first and then call
     * the OS specific code, the update must be serialized for various reasons.
     */
    if (ASMAtomicReadBool(&pThis->fActive) != fActive)
    {
        RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;
        RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
        ASMAtomicWriteBool(&pThis->fActive, fActive);
        RTSpinlockRelease(pThis->hSpinlock, &Tmp);

        vboxNetFltPortOsSetActive(pThis, fActive);
    }
    else
        fActive = !fActive;
    return !fActive;
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnDisconnectAndRelease
 */
NETFLT_DECL_CALLBACK(void) vboxNetFltPortDisconnectAndRelease(PINTNETTRUNKIFPORT pIfPort)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);
    RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;

    /*
     * Serious paranoia.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(pThis->MyPort.u32VersionEnd == INTNETTRUNKIFPORT_VERSION);
    AssertPtr(pThis->pGlobals);
    Assert(pThis->hEventIdle != NIL_RTSEMEVENT);
    Assert(pThis->hSpinlock != NIL_RTSPINLOCK);
    Assert(pThis->szName[0]);

    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Connected);
    Assert(!pThis->fActive);
    Assert(!pThis->fRediscoveryPending);
    Assert(!pThis->cBusy);

    /*
     * Disconnect and release it.
     */
    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
    vboxNetFltSetState(pThis, kVBoxNetFltInsState_Disconnecting);
    RTSpinlockRelease(pThis->hSpinlock, &Tmp);

    vboxNetFltOsDisconnectIt(pThis);
    pThis->pSwitchPort = NULL;

#ifdef VBOXNETFLT_STATIC_CONFIG
    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
    vboxNetFltSetState(pThis, kVBoxNetFltInsState_Unconnected);
    RTSpinlockRelease(pThis->hSpinlock, &Tmp);
#endif

    vboxNetFltRelease(pThis, false /* fBusy */);
}


/**
 * Destroy a device that has been disconnected from the switch.
 *
 * @returns true if the instance is destroyed, false otherwise.
 * @param   pThis               The instance to be destroyed. This is
 *                              no longer valid when this function returns.
 */
static bool vboxNetFltDestroyInstance(PVBOXNETFLTINS pThis)
{
    PVBOXNETFLTGLOBALS pGlobals = pThis->pGlobals;
    uint32_t cRefs = ASMAtomicUoReadU32((uint32_t volatile *)&pThis->cRefs);
    int rc;
    LogFlow(("vboxNetFltDestroyInstance: pThis=%p (%s)\n", pThis, pThis->szName));

    /*
     * Validate the state.
     */
#ifdef VBOXNETFLT_STATIC_CONFIG
    Assert(   vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Disconnecting
           || vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Unconnected);
#else
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Disconnecting);
#endif
    Assert(!pThis->fActive);
    Assert(!pThis->fRediscoveryPending);
    Assert(!pThis->cRefs);
    Assert(!pThis->cBusy);
    Assert(!pThis->pSwitchPort);

    /*
     * Make sure the state is 'disconnecting' / 'destroying' and let the OS
     * specific code do its part of the cleanup outside the mutex.
     */
    rc = RTSemFastMutexRequest(pGlobals->hFastMtx); AssertRC(rc);
    vboxNetFltSetState(pThis, kVBoxNetFltInsState_Disconnecting);
    RTSemFastMutexRelease(pGlobals->hFastMtx);

    vboxNetFltOsDeleteInstance(pThis);

    /*
     * Unlink the instance and free up its resources.
     */
    rc = RTSemFastMutexRequest(pGlobals->hFastMtx); AssertRC(rc);
    vboxNetFltSetState(pThis, kVBoxNetFltInsState_Destroyed);
    vboxNetFltUnlinkLocked(pGlobals, pThis);
    RTSemFastMutexRelease(pGlobals->hFastMtx);

    RTSemEventDestroy(pThis->hEventIdle);
    pThis->hEventIdle = NIL_RTSEMEVENT;
    RTSpinlockDestroy(pThis->hSpinlock);
    pThis->hSpinlock = NIL_RTSPINLOCK;
    RTMemFree(pThis);

    NOREF(cRefs);

    return true;
}


/**
 * Releases a reference to the specified instance.
 *
 * This method will destroy the instance when the count reaches 0.
 * It will also take care of decrementing the counter and idle wakeup.
 *
 * @param   pThis           The instance.
 * @param   fBusy           Whether the busy counter should be decremented too.
 */
DECLHIDDEN(void) vboxNetFltRelease(PVBOXNETFLTINS pThis, bool fBusy)
{
    uint32_t cRefs;

    /*
     * Paranoid Android.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(pThis->MyPort.u32VersionEnd == INTNETTRUNKIFPORT_VERSION);
    Assert(   vboxNetFltGetState(pThis) > kVBoxNetFltInsState_Invalid
           && vboxNetFltGetState(pThis) < kVBoxNetFltInsState_Destroyed);
    AssertPtr(pThis->pGlobals);
    Assert(pThis->hEventIdle != NIL_RTSEMEVENT);
    Assert(pThis->hSpinlock != NIL_RTSPINLOCK);
    Assert(pThis->szName[0]);

    /*
     * Work the busy counter.
     */
    if (fBusy)
    {
        cRefs = ASMAtomicDecU32(&pThis->cBusy);
        if (!cRefs)
        {
            int rc = RTSemEventSignal(pThis->hEventIdle);
            AssertRC(rc);
        }
        else
            Assert(cRefs < UINT32_MAX / 2);
    }

    /*
     * The object reference counting.
     */
    cRefs = ASMAtomicDecU32(&pThis->cRefs);
    if (!cRefs)
        vboxNetFltDestroyInstance(pThis);
    else
        Assert(cRefs < UINT32_MAX / 2);
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnRetain
 */
NETFLT_DECL_CALLBACK(void) vboxNetFltPortRelease(PINTNETTRUNKIFPORT pIfPort)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);
    vboxNetFltRelease(pThis, false /* fBusy */);
}


/**
 * Retains a reference to the specified instance and a busy reference too.
 *
 * @param   pThis           The instance.
 * @param   fBusy           Whether the busy counter should be incremented as well.
 */
DECLHIDDEN(void) vboxNetFltRetain(PVBOXNETFLTINS pThis, bool fBusy)
{
    uint32_t cRefs;

    /*
     * Paranoid Android.
     */
    AssertPtr(pThis);
    Assert(pThis->MyPort.u32Version == INTNETTRUNKIFPORT_VERSION);
    Assert(pThis->MyPort.u32VersionEnd == INTNETTRUNKIFPORT_VERSION);
    Assert(   vboxNetFltGetState(pThis) > kVBoxNetFltInsState_Invalid
           && vboxNetFltGetState(pThis) < kVBoxNetFltInsState_Destroyed);
    AssertPtr(pThis->pGlobals);
    Assert(pThis->hEventIdle != NIL_RTSEMEVENT);
    Assert(pThis->hSpinlock != NIL_RTSPINLOCK);
    Assert(pThis->szName[0]);

    /*
     * Retain the object.
     */
    cRefs = ASMAtomicIncU32(&pThis->cRefs);
    Assert(cRefs > 1 && cRefs < UINT32_MAX / 2);

    /*
     * Work the busy counter.
     */
    if (fBusy)
    {
        cRefs = ASMAtomicIncU32(&pThis->cBusy);
        Assert(cRefs > 0 && cRefs < UINT32_MAX / 2);
    }

    NOREF(cRefs);
}


/**
 * @copydoc INTNETTRUNKIFPORT::pfnRetain
 */
NETFLT_DECL_CALLBACK(void) vboxNetFltPortRetain(PINTNETTRUNKIFPORT pIfPort)
{
    PVBOXNETFLTINS pThis = IFPORT_2_VBOXNETFLTINS(pIfPort);
    vboxNetFltRetain(pThis, false /* fBusy */);
}


/**
 * Connects the instance to the specified switch port.
 *
 * Called while owning the lock. We're ASSUMING that the internal
 * networking code is already owning an recursive mutex, so, there
 * will be no deadlocks when vboxNetFltOsConnectIt calls back into
 * it for setting preferences.
 *
 * @returns VBox status code.
 * @param   pThis               The instance.
 * @param   pSwitchPort         The port on the internal network 'switch'.
 * @param   ppIfPort            Where to return our port interface.
 */
static int vboxNetFltConnectIt(PVBOXNETFLTINS pThis, PINTNETTRUNKSWPORT pSwitchPort, PINTNETTRUNKIFPORT *ppIfPort)
{
    int rc;

    /*
     * Validate state.
     */
    Assert(!pThis->fActive);
    Assert(!pThis->fRediscoveryPending);
    Assert(!pThis->cBusy);
#ifdef VBOXNETFLT_STATIC_CONFIG
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Unconnected);
#else
    Assert(vboxNetFltGetState(pThis) == kVBoxNetFltInsState_Initializing);
#endif

    /*
     * Do the job.
     * Note that we're calling the os stuff while owning the semaphore here.
     */
    pThis->pSwitchPort = pSwitchPort;
    rc = vboxNetFltOsConnectIt(pThis);
    if (RT_SUCCESS(rc))
    {
        vboxNetFltSetState(pThis, kVBoxNetFltInsState_Connected);
        *ppIfPort = &pThis->MyPort;
    }
    else
        pThis->pSwitchPort = NULL;

    Assert(!pThis->fActive);
    return rc;
}


/**
 * Creates a new instance.
 *
 * The new instance will be in the suspended state in a dynamic config and in
 * the inactive in a static one.
 *
 * Called without owning the lock, but will request is several times.
 *
 * @returns VBox status code.
 * @param   pGlobals            The globals.
 * @param   pszName             The instance name.
 * @param   pSwitchPort         The port on the switch that we're connected with (dynamic only).
 * @param   fNoPromisc          Do not attempt going into promiscuous mode.
 * @param   pvContext           Context argument for vboxNetFltOsInitInstance.
 * @param   ppIfPort            Where to store the pointer to our port interface (dynamic only).
 */
static int vboxNetFltNewInstance(PVBOXNETFLTGLOBALS pGlobals, const char *pszName, PINTNETTRUNKSWPORT pSwitchPort,
                                 bool fNoPromisc, void *pvContext, PINTNETTRUNKIFPORT *ppIfPort)
{
    /*
     * Allocate and initialize a new instance before requesting the mutex.
     */
    int             rc;
    size_t const    cchName = strlen(pszName);
    PVBOXNETFLTINS  pNew = (PVBOXNETFLTINS)RTMemAllocZ(RT_OFFSETOF(VBOXNETFLTINS, szName[cchName + 1]));
    if (!pNew)
        return VERR_INTNET_FLT_IF_FAILED;
    pNew->pNext                         = NULL;
    pNew->MyPort.u32Version             = INTNETTRUNKIFPORT_VERSION;
    pNew->MyPort.pfnRetain              = NETFLT_CALLBACK(vboxNetFltPortRetain);
    pNew->MyPort.pfnRelease             = NETFLT_CALLBACK(vboxNetFltPortRelease);
    pNew->MyPort.pfnDisconnectAndRelease= NETFLT_CALLBACK(vboxNetFltPortDisconnectAndRelease);
    pNew->MyPort.pfnSetActive           = NETFLT_CALLBACK(vboxNetFltPortSetActive);
    pNew->MyPort.pfnWaitForIdle         = NETFLT_CALLBACK(vboxNetFltPortWaitForIdle);
    pNew->MyPort.pfnGetMacAddress       = NETFLT_CALLBACK(vboxNetFltPortGetMacAddress);
    pNew->MyPort.pfnIsHostMac           = NETFLT_CALLBACK(vboxNetFltPortIsHostMac);
    pNew->MyPort.pfnIsPromiscuous       = NETFLT_CALLBACK(vboxNetFltPortIsPromiscuous);
    pNew->MyPort.pfnXmit                = NETFLT_CALLBACK(vboxNetFltPortXmit);
    pNew->MyPort.u32VersionEnd          = INTNETTRUNKIFPORT_VERSION;
    pNew->pSwitchPort                   = NULL;
    pNew->pGlobals                      = pGlobals;
    pNew->hSpinlock                     = NIL_RTSPINLOCK;
    pNew->enmState                      = kVBoxNetFltInsState_Initializing;
    pNew->fActive                       = false;
    pNew->fDisconnectedFromHost         = false;
    pNew->fRediscoveryPending           = false;
    pNew->fDisablePromiscuous           = fNoPromisc;
    pNew->NanoTSLastRediscovery         = INT64_MAX;
    pNew->cRefs                         = 1;
    pNew->cBusy                         = 0;
    pNew->hEventIdle                    = NIL_RTSEMEVENT;
    memcpy(pNew->szName, pszName, cchName + 1);

    rc = RTSpinlockCreate(&pNew->hSpinlock);
    if (RT_SUCCESS(rc))
    {
        rc = RTSemEventCreate(&pNew->hEventIdle);
        if (RT_SUCCESS(rc))
        {
            rc = vboxNetFltOsPreInitInstance(pNew);
            if (RT_SUCCESS(rc))
            {
                /*
                 * Insert the instance into the chain, checking for
                 * duplicates first of course (race).
                 */
                rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
                if (RT_SUCCESS(rc))
                {
                    if (!vboxNetFltFindInstanceLocked(pGlobals, pszName))
                    {
                        pNew->pNext = pGlobals->pInstanceHead;
                        pGlobals->pInstanceHead = pNew;
                        RTSemFastMutexRelease(pGlobals->hFastMtx);

                        /*
                         * Call the OS specific initialization code.
                         */
                        rc = vboxNetFltOsInitInstance(pNew, pvContext);
                        RTSemFastMutexRequest(pGlobals->hFastMtx);
                        if (RT_SUCCESS(rc))
                        {
#ifdef VBOXNETFLT_STATIC_CONFIG
                            /*
                             * Static instances are unconnected at birth.
                             */
                            Assert(!pSwitchPort);
                            pNew->enmState = kVBoxNetFltInsState_Unconnected;
                            RTSemFastMutexRelease(pGlobals->hFastMtx);
                            *ppIfPort = &pNew->MyPort;
                            return rc;

#else  /* !VBOXNETFLT_STATIC_CONFIG */
                            /*
                             * Connect it as well, the OS specific bits has to be done outside
                             * the lock as they may call back to into intnet.
                             */
                            rc = vboxNetFltConnectIt(pNew, pSwitchPort, ppIfPort);
                            if (RT_SUCCESS(rc))
                            {
                                RTSemFastMutexRelease(pGlobals->hFastMtx);
                                Assert(*ppIfPort == &pNew->MyPort);
                                return rc;
                            }

                            /* Bail out (failed). */
                            vboxNetFltOsDeleteInstance(pNew);
#endif /* !VBOXNETFLT_STATIC_CONFIG */
                        }
                        vboxNetFltUnlinkLocked(pGlobals, pNew);
                    }
                    else
                        rc = VERR_INTNET_FLT_IF_BUSY;
                    RTSemFastMutexRelease(pGlobals->hFastMtx);
                }
            }
            RTSemEventDestroy(pNew->hEventIdle);
        }
        RTSpinlockDestroy(pNew->hSpinlock);
    }

    RTMemFree(pNew);
    return rc;
}


#ifdef VBOXNETFLT_STATIC_CONFIG
/**
 * Searches for the NetFlt instance by its name and creates the new one if not found.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS and *ppInstance if a new instance was created.
 * @retval  VINF_ALREADY_INITIALIZED and *ppInstance if an instance already exists.
 *
 * @param   pGlobal     Pointer to the globals.
 * @param   pszName     The instance name.
 * @param   ppInstance  Where to return the instance pointer on success.
 * @param   pvContext   Context which needs to be passed along to vboxNetFltOsInitInstance.
 */
DECLHIDDEN(int) vboxNetFltSearchCreateInstance(PVBOXNETFLTGLOBALS pGlobals, const char *pszName, PVBOXNETFLTINS *ppInstance, void *pvContext)
{
    PINTNETTRUNKIFPORT pIfPort;
    PVBOXNETFLTINS pCur;
    VBOXNETFTLINSSTATE enmState;
    int rc;

    *ppInstance = NULL;
    rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
    AssertRCReturn(rc, rc);

    /*
     * Look for an existing instance in the list.
     *
     * There might be an existing one in the list if the driver was unbound
     * while it was connected to an internal network. We're running into
     * a destruction race that is a bit similar to the one in
     * vboxNetFltFactoryCreateAndConnect, only the roles are reversed
     * and we're not in a position to back down. Instead of backing down
     * we'll delay a bit giving the other thread time to complete the
     * destructor.
     */
    pCur = vboxNetFltFindInstanceLocked(pGlobals, pszName);
    while (pCur)
    {
        uint32_t cRefs = ASMAtomicIncU32(&pCur->cRefs);
        if (cRefs > 1)
        {
            enmState = vboxNetFltGetState(pCur);
            switch (enmState)
            {
                case kVBoxNetFltInsState_Unconnected:
                case kVBoxNetFltInsState_Connected:
                case kVBoxNetFltInsState_Disconnecting:
                    if (pCur->fDisconnectedFromHost)
                    {
                        /* Wait for it to exit the transitional disconnecting
                           state. It might otherwise be running the risk of
                           upsetting the OS specific code...  */
                        /** @todo This reconnect stuff should be serialized correctly for static
                         *        devices. Shouldn't it? In the dynamic case we're using the INTNET
                         *        outbound thrunk lock, but that doesn't quite cut it here, or does
                         *        it? We could either transition to initializing  or make a callback
                         *        while owning the mutext here... */
                        if (enmState == kVBoxNetFltInsState_Disconnecting)
                        {
                            do
                            {
                                RTSemFastMutexRelease(pGlobals->hFastMtx);
                                RTThreadSleep(2); /* (2ms) */
                                RTSemFastMutexRequest(pGlobals->hFastMtx);
                                enmState = vboxNetFltGetState(pCur);
                            }
                            while (enmState == kVBoxNetFltInsState_Disconnecting);
                            AssertMsg(enmState == kVBoxNetFltInsState_Unconnected, ("%d\n", enmState));
                            Assert(pCur->fDisconnectedFromHost);
                        }

                        RTSemFastMutexRelease(pGlobals->hFastMtx);
                        *ppInstance = pCur;
                        return VINF_ALREADY_INITIALIZED;
                    }
                    /* fall thru */

                default:
                {
                    bool fDfH = pCur->fDisconnectedFromHost;
                    RTSemFastMutexRelease(pGlobals->hFastMtx);
                    vboxNetFltRelease(pCur, false /* fBusy */);
                    LogRel(("VBoxNetFlt: Huh? An instance of '%s' already exists! [pCur=%p cRefs=%d fDfH=%RTbool enmState=%d]\n",
                            pszName, pCur, cRefs - 1, fDfH, enmState));
                    *ppInstance = NULL;
                    return VERR_INTNET_FLT_IF_BUSY;
                }
            }
        }

        /* Zero references, it's being destroyed. Delay a bit so the destructor
           can finish its work and try again. (vboxNetFltNewInstance will fail
           with duplicate name if we don't.) */
# ifdef RT_STRICT
        Assert(cRefs == 1);
        enmState = vboxNetFltGetState(pCur);
        AssertMsg(   enmState == kVBoxNetFltInsState_Unconnected
                  || enmState == kVBoxNetFltInsState_Disconnecting
                  || enmState == kVBoxNetFltInsState_Destroyed, ("%d\n", enmState));
# endif
        ASMAtomicDecU32(&pCur->cRefs);
        RTSemFastMutexRelease(pGlobals->hFastMtx);
        RTThreadSleep(2); /* (2ms) */
        rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
        AssertRCReturn(rc, rc);

        /* try again */
        pCur = vboxNetFltFindInstanceLocked(pGlobals, pszName);
    }

    RTSemFastMutexRelease(pGlobals->hFastMtx);

    /*
     * Try create a new instance.
     * (fNoPromisc is overridden in the vboxNetFltFactoryCreateAndConnect path, so pass true here.)
     */
    rc = vboxNetFltNewInstance(pGlobals, pszName, NULL, true /* fNoPromisc */, pvContext, &pIfPort);
    if (RT_SUCCESS(rc))
        *ppInstance = IFPORT_2_VBOXNETFLTINS(pIfPort);
    else
        *ppInstance = NULL;

    return rc;
}
#endif /* VBOXNETFLT_STATIC_CONFIG */


/**
 * @copydoc INTNETTRUNKFACTORY::pfnCreateAndConnect
 */
static DECLCALLBACK(int) vboxNetFltFactoryCreateAndConnect(PINTNETTRUNKFACTORY pIfFactory, const char *pszName,
                                                           PINTNETTRUNKSWPORT pSwitchPort, uint32_t fFlags,
                                                           PINTNETTRUNKIFPORT *ppIfPort)
{
    PVBOXNETFLTGLOBALS pGlobals = (PVBOXNETFLTGLOBALS)((uint8_t *)pIfFactory - RT_OFFSETOF(VBOXNETFLTGLOBALS, TrunkFactory));
    PVBOXNETFLTINS pCur;
    int rc;

    LogFlow(("vboxNetFltFactoryCreateAndConnect: pszName=%p:{%s} fFlags=%#x\n", pszName, pszName, fFlags));
    Assert(pGlobals->cFactoryRefs > 0);
    AssertMsgReturn(!(fFlags & ~(INTNETTRUNKFACTORY_FLAG_NO_PROMISC)),
                    ("%#x\n", fFlags), VERR_INVALID_PARAMETER);

    /*
     * Static: Find instance, check if busy, connect if not.
     * Dynamic: Check for duplicate / busy interface instance.
     */
    rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
    AssertRCReturn(rc, rc);

//#if defined(VBOXNETADP) && defined(RT_OS_WINDOWS)
//    /* temporary hack to pick up the first adapter */
//    pCur = pGlobals->pInstanceHead; /** @todo Don't for get to remove this temporary hack... :-) */
//#else
    pCur = vboxNetFltFindInstanceLocked(pGlobals, pszName);
//#endif
    if (pCur)
    {
#ifdef VBOXNETFLT_STATIC_CONFIG
        /* Try grab a reference. If the count had already reached zero we're racing the
           destructor code and must back down. */
        uint32_t cRefs = ASMAtomicIncU32(&pCur->cRefs);
        if (cRefs > 1)
        {
            if (vboxNetFltGetState(pCur) == kVBoxNetFltInsState_Unconnected)
            {
                pCur->fDisablePromiscuous = !!(fFlags & INTNETTRUNKFACTORY_FLAG_NO_PROMISC);
                rc = vboxNetFltConnectIt(pCur, pSwitchPort, ppIfPort);
                if (RT_SUCCESS(rc))
                    pCur = NULL; /* Don't release it, reference given to the caller. */
            }
            else
                rc = VERR_INTNET_FLT_IF_BUSY;
        }
        else
        {
            Assert(cRefs == 1);
            ASMAtomicDecU32(&pCur->cRefs);
            pCur = NULL; /* nothing to release */
            rc = VERR_INTNET_FLT_IF_NOT_FOUND;
        }

        RTSemFastMutexRelease(pGlobals->hFastMtx);
        if (pCur)
            vboxNetFltRelease(pCur, false /* fBusy */);
#else
        rc = VERR_INTNET_FLT_IF_BUSY;
        RTSemFastMutexRelease(pGlobals->hFastMtx);
#endif
        LogFlow(("vboxNetFltFactoryCreateAndConnect: returns %Rrc\n", rc));
        return rc;
    }

    RTSemFastMutexRelease(pGlobals->hFastMtx);

#ifdef VBOXNETFLT_STATIC_CONFIG
    rc = VERR_INTNET_FLT_IF_NOT_FOUND;
#else
    /*
     * Dynamically create a new instance.
     */
    rc = vboxNetFltNewInstance(pGlobals,
                               pszName,
                               pSwitchPort,
                               !!(fFlags & INTNETTRUNKFACTORY_FLAG_NO_PROMISC),
                               NULL,
                               ppIfPort);
#endif
    LogFlow(("vboxNetFltFactoryCreateAndConnect: returns %Rrc\n", rc));
    return rc;
}


/**
 * @copydoc INTNETTRUNKFACTORY::pfnRelease
 */
static DECLCALLBACK(void) vboxNetFltFactoryRelease(PINTNETTRUNKFACTORY pIfFactory)
{
    PVBOXNETFLTGLOBALS pGlobals = (PVBOXNETFLTGLOBALS)((uint8_t *)pIfFactory - RT_OFFSETOF(VBOXNETFLTGLOBALS, TrunkFactory));

    int32_t cRefs = ASMAtomicDecS32(&pGlobals->cFactoryRefs);
    Assert(cRefs >= 0); NOREF(cRefs);
    LogFlow(("vboxNetFltFactoryRelease: cRefs=%d (new)\n", cRefs));
}


/**
 * Implements the SUPDRV component factor interface query method.
 *
 * @returns Pointer to an interface. NULL if not supported.
 *
 * @param   pSupDrvFactory      Pointer to the componet factory registration structure.
 * @param   pSession            The session - unused.
 * @param   pszInterfaceUuid    The factory interface id.
 */
static DECLCALLBACK(void *) vboxNetFltQueryFactoryInterface(PCSUPDRVFACTORY pSupDrvFactory, PSUPDRVSESSION pSession, const char *pszInterfaceUuid)
{
    PVBOXNETFLTGLOBALS pGlobals = (PVBOXNETFLTGLOBALS)((uint8_t *)pSupDrvFactory - RT_OFFSETOF(VBOXNETFLTGLOBALS, SupDrvFactory));

    /*
     * Convert the UUID strings and compare them.
     */
    RTUUID UuidReq;
    int rc = RTUuidFromStr(&UuidReq, pszInterfaceUuid);
    if (RT_SUCCESS(rc))
    {
        if (!RTUuidCompareStr(&UuidReq, INTNETTRUNKFACTORY_UUID_STR))
        {
            ASMAtomicIncS32(&pGlobals->cFactoryRefs);
            return &pGlobals->TrunkFactory;
        }
#ifdef LOG_ENABLED
        /* log legacy queries */
        /* else if (!RTUuidCompareStr(&UuidReq, INTNETTRUNKFACTORY_V1_UUID_STR))
            Log(("VBoxNetFlt: V1 factory query\n"));
        */
        else
            Log(("VBoxNetFlt: unknown factory interface query (%s)\n", pszInterfaceUuid));
#endif
    }
    else
        Log(("VBoxNetFlt: rc=%Rrc, uuid=%s\n", rc, pszInterfaceUuid));

    return NULL;
}


/**
 * Checks whether the VBoxNetFlt wossname can be unloaded.
 *
 * This will return false if someone is currently using the module.
 *
 * @returns true if it's relatively safe to unload it, otherwise false.
 * @param   pGlobals        Pointer to the globals.
 */
DECLHIDDEN(bool) vboxNetFltCanUnload(PVBOXNETFLTGLOBALS pGlobals)
{
    int rc = RTSemFastMutexRequest(pGlobals->hFastMtx);
    bool fRc = !pGlobals->pInstanceHead
            && pGlobals->cFactoryRefs <= 0;
    RTSemFastMutexRelease(pGlobals->hFastMtx);
    AssertRC(rc);
    return fRc;
}


/**
 * Try to close the IDC connection to SUPDRV if established.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS on success.
 * @retval  VERR_WRONG_ORDER if we're busy.
 *
 * @param   pGlobals        Pointer to the globals.
 *
 * @sa      vboxNetFltTryDeleteIdcAndGlobals()
 */
DECLHIDDEN(int) vboxNetFltTryDeleteIdc(PVBOXNETFLTGLOBALS pGlobals)
{
    int rc;

    Assert(pGlobals->hFastMtx != NIL_RTSEMFASTMUTEX);

    /*
     * Check before trying to deregister the factory.
     */
    if (!vboxNetFltCanUnload(pGlobals))
        return VERR_WRONG_ORDER;

    if (!pGlobals->fIDCOpen)
        rc = VINF_SUCCESS;
    else
    {
        /*
         * Disconnect from SUPDRV and check that nobody raced us,
         * reconnect if that should happen.
         */
        rc = SUPR0IdcComponentDeregisterFactory(&pGlobals->SupDrvIDC, &pGlobals->SupDrvFactory);
        AssertRC(rc);
        if (!vboxNetFltCanUnload(pGlobals))
        {
            rc = SUPR0IdcComponentRegisterFactory(&pGlobals->SupDrvIDC, &pGlobals->SupDrvFactory);
            AssertRC(rc);
            return VERR_WRONG_ORDER;
        }

        SUPR0IdcClose(&pGlobals->SupDrvIDC);
        pGlobals->fIDCOpen = false;
    }

    return rc;
}


/**
 * Establishes the IDC connection to SUPDRV and registers our component factory.
 *
 * @returns VBox status code.
 * @param   pGlobals    Pointer to the globals.
 * @sa      vboxNetFltInitGlobalsAndIdc().
 */
DECLHIDDEN(int) vboxNetFltInitIdc(PVBOXNETFLTGLOBALS pGlobals)
{
    int rc;
    Assert(!pGlobals->fIDCOpen);

    /*
     * Establish a connection to SUPDRV and register our component factory.
     */
    rc = SUPR0IdcOpen(&pGlobals->SupDrvIDC, 0 /* iReqVersion = default */, 0 /* iMinVersion = default */, NULL, NULL, NULL);
    if (RT_SUCCESS(rc))
    {
        rc = SUPR0IdcComponentRegisterFactory(&pGlobals->SupDrvIDC, &pGlobals->SupDrvFactory);
        if (RT_SUCCESS(rc))
        {
            pGlobals->fIDCOpen = true;
            Log(("VBoxNetFlt: pSession=%p\n", SUPR0IdcGetSession(&pGlobals->SupDrvIDC)));
            return rc;
        }

        /* bail out. */
        LogRel(("VBoxNetFlt: Failed to register component factory, rc=%Rrc\n", rc));
        SUPR0IdcClose(&pGlobals->SupDrvIDC);
    }

    return rc;
}


/**
 * Deletes the globals.
 *
 * This must be called after the IDC connection has been closed,
 * see vboxNetFltTryDeleteIdc().
 *
 * @param   pGlobals        Pointer to the globals.
 * @sa      vboxNetFltTryDeleteIdcAndGlobals()
 */
DECLHIDDEN(void) vboxNetFltDeleteGlobals(PVBOXNETFLTGLOBALS pGlobals)
{
    Assert(!pGlobals->fIDCOpen);

    /*
     * Release resources.
     */
    RTSemFastMutexDestroy(pGlobals->hFastMtx);
    pGlobals->hFastMtx = NIL_RTSEMFASTMUTEX;
}


/**
 * Initializes the globals.
 *
 * @returns VBox status code.
 * @param   pGlobals        Pointer to the globals.
 * @sa      vboxNetFltInitGlobalsAndIdc().
 */
DECLHIDDEN(int) vboxNetFltInitGlobals(PVBOXNETFLTGLOBALS pGlobals)
{
    /*
     * Initialize the common portions of the structure.
     */
    int rc = RTSemFastMutexCreate(&pGlobals->hFastMtx);
    if (RT_SUCCESS(rc))
    {
        pGlobals->pInstanceHead = NULL;

        pGlobals->TrunkFactory.pfnRelease = vboxNetFltFactoryRelease;
        pGlobals->TrunkFactory.pfnCreateAndConnect = vboxNetFltFactoryCreateAndConnect;
#if defined(RT_OS_WINDOWS) && defined(VBOXNETADP)
        strcpy(pGlobals->SupDrvFactory.szName, "VBoxNetAdp");
#else
        strcpy(pGlobals->SupDrvFactory.szName, "VBoxNetFlt");
#endif
        pGlobals->SupDrvFactory.pfnQueryFactoryInterface = vboxNetFltQueryFactoryInterface;
        pGlobals->fIDCOpen = false;

        return rc;
    }

    return rc;
}


/**
 * Called by the native part when the OS wants the driver to unload.
 *
 * @returns VINF_SUCCESS on success, VERR_WRONG_ORDER if we're busy.
 *
 * @param   pGlobals        Pointer to the globals.
 */
DECLHIDDEN(int) vboxNetFltTryDeleteIdcAndGlobals(PVBOXNETFLTGLOBALS pGlobals)
{
    int rc = vboxNetFltTryDeleteIdc(pGlobals);
    if (RT_SUCCESS(rc))
        vboxNetFltDeleteGlobals(pGlobals);
    return rc;
}


/**
 * Called by the native driver/kext module initialization routine.
 *
 * It will initialize the common parts of the globals, assuming the caller
 * has already taken care of the OS specific bits, and establish the IDC
 * connection to SUPDRV.
 *
 * @returns VBox status code.
 * @param   pGlobals    Pointer to the globals.
 */
DECLHIDDEN(int) vboxNetFltInitGlobalsAndIdc(PVBOXNETFLTGLOBALS pGlobals)
{
    /*
     * Initialize the common portions of the structure.
     */
    int rc = vboxNetFltInitGlobals(pGlobals);
    if (RT_SUCCESS(rc))
    {
        rc = vboxNetFltInitIdc(pGlobals);
        if (RT_SUCCESS(rc))
            return rc;

        /* bail out. */
        vboxNetFltDeleteGlobals(pGlobals);
    }

    return rc;
}

