/* $Id: log.cpp 37818 2011-07-07 13:25:03Z vboxsync $ */
/** @file
 * Runtime VBox - Logger.
 */

/*
 * Copyright (C) 2006-2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/log.h>
#include "internal/iprt.h"

#ifndef IN_RC
# include <iprt/alloc.h>
# include <iprt/process.h>
# include <iprt/semaphore.h>
# include <iprt/thread.h>
# include <iprt/mp.h>
#endif
#ifdef IN_RING3
# include <iprt/env.h>
# include <iprt/file.h>
# include <iprt/lockvalidator.h>
# include <iprt/path.h>
#endif
#include <iprt/time.h>
#include <iprt/asm.h>
#if defined(RT_ARCH_AMD64) || defined(RT_ARCH_X86)
# include <iprt/asm-amd64-x86.h>
#endif
#include <iprt/assert.h>
#include <iprt/err.h>
#include <iprt/param.h>

#include <iprt/stdarg.h>
#include <iprt/string.h>
#include <iprt/ctype.h>
#ifdef IN_RING3
# include <iprt/alloca.h>
# include <stdio.h>
#endif


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * Arguments passed to the output function.
 */
typedef struct RTLOGOUTPUTPREFIXEDARGS
{
    /** The logger instance. */
    PRTLOGGER               pLogger;
    /** The flags. (used for prefixing.) */
    unsigned                fFlags;
    /** The group. (used for prefixing.) */
    unsigned                iGroup;
} RTLOGOUTPUTPREFIXEDARGS, *PRTLOGOUTPUTPREFIXEDARGS;

/**
 * Internal logger data.
 *
 * @remarks Don't make casual changes to this structure.
 */
typedef struct RTLOGGERINTERNAL
{
    /** The structure revision (RTLOGGERINTERNAL_REV). */
    uint32_t                uRevision;
    /** The size of the internal logger structure. */
    uint32_t                cbSelf;

    /** Spinning mutex semaphore.  Can be NIL. */
    RTSEMSPINMUTEX          hSpinMtx;
    /** Pointer to the flush function. */
    PFNRTLOGFLUSH           pfnFlush;

    /** Custom prefix callback. */
    PFNRTLOGPREFIX          pfnPrefix;
    /** Prefix callback argument. */
    void                   *pvPrefixUserArg;
    /** This is set if a prefix is pending. */
    bool                    fPendingPrefix;
    /** Alignment padding. */
    bool                    afPadding1[3];

    /** The max number of groups that there is room for in afGroups and papszGroups.
     * Used by RTLogCopyGroupAndFlags(). */
    uint32_t                cMaxGroups;
    /** Pointer to the group name array.
     * (The data is readonly and provided by the user.) */
    const char * const     *papszGroups;

    /** The number of log entries per group.  NULL if
     * RTLOGFLAGS_RESTRICT_GROUPS is not specified. */
    uint32_t               *pacEntriesPerGroup;
    /** The max number of entries per group. */
    uint32_t                cMaxEntriesPerGroup;
    /** Padding.  */
    uint32_t                u32Padding2;

#ifdef IN_RING3 /* Note! Must be at the end! */
    /** @name File logging bits for the logger.
     * @{ */
    /** Pointer to the function called when starting logging, and when
     * ending or starting a new log file as part of history rotation.
     * This can be NULL. */
    PFNRTLOGPHASE           pfnPhase;

    /** Handle to log file (if open). */
    RTFILE                  hFile;
    /** Log file history settings: maximum amount of data to put in a file. */
    uint64_t                cbHistoryFileMax;
    /** Log file history settings: current amount of data in a file. */
    uint64_t                cbHistoryFileWritten;
    /** Log file history settings: maximum time to use a file (in seconds). */
    uint32_t                cSecsHistoryTimeSlot;
    /** Log file history settings: in what time slot was the file created. */
    uint32_t                uHistoryTimeSlotStart;
    /** Log file history settings: number of older files to keep.
     * 0 means no history. */
    uint32_t                cHistory;
    /** Pointer to filename. */
    char                    szFilename[RTPATH_MAX];
    /** @} */
#endif /* IN_RING3 */
} RTLOGGERINTERNAL;

/** The revision of the internal logger structure. */
#define RTLOGGERINTERNAL_REV    UINT32_C(9)

#ifdef IN_RING3
/** The size of the RTLOGGERINTERNAL structure in ring-0.  */
# define RTLOGGERINTERNAL_R0_SIZE       RT_OFFSETOF(RTLOGGERINTERNAL, pfnPhase)
AssertCompileMemberAlignment(RTLOGGERINTERNAL, hFile, sizeof(void *));
AssertCompileMemberAlignment(RTLOGGERINTERNAL, cbHistoryFileMax, sizeof(uint64_t));
#endif

/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
#ifndef IN_RC
static unsigned rtlogGroupFlags(const char *psz);
#endif
#ifdef IN_RING0
static void rtR0LogLoggerExFallback(uint32_t fDestFlags, uint32_t fFlags, const char *pszFormat, va_list va);
#endif
#ifdef IN_RING3
static int rtlogFileOpen(PRTLOGGER pLogger, char *pszErrorMsg, size_t cchErrorMsg);
static void rtlogRotate(PRTLOGGER pLogger, uint32_t uTimeSlot, bool fFirst);
#endif
static void rtlogFlush(PRTLOGGER pLogger);
static DECLCALLBACK(size_t) rtLogOutput(void *pv, const char *pachChars, size_t cbChars);
static DECLCALLBACK(size_t) rtLogOutputPrefixed(void *pv, const char *pachChars, size_t cbChars);
static void rtlogLoggerExVLocked(PRTLOGGER pLogger, unsigned fFlags, unsigned iGroup, const char *pszFormat, va_list args);
static void rtlogLoggerExFLocked(PRTLOGGER pLogger, unsigned fFlags, unsigned iGroup, const char *pszFormat, ...);


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
#ifdef IN_RC
/** Default logger instance. */
extern "C" DECLIMPORT(RTLOGGERRC)   g_Logger;
#else /* !IN_RC */
/** Default logger instance. */
static PRTLOGGER                    g_pLogger;
#endif /* !IN_RC */
#ifdef IN_RING3
/** The RTThreadGetWriteLockCount() change caused by the logger mutex semaphore. */
static uint32_t volatile            g_cLoggerLockCount;
#endif

#ifdef IN_RING0
/** Number of per-thread loggers. */
static int32_t volatile             g_cPerThreadLoggers;
/** Per-thread loggers.
 * This is just a quick TLS hack suitable for debug logging only.
 * If we run out of entries, just unload and reload the driver. */
static struct RTLOGGERPERTHREAD
{
    /** The thread. */
    RTNATIVETHREAD volatile NativeThread;
    /** The (process / session) key. */
    uintptr_t volatile      uKey;
    /** The logger instance.*/
    PRTLOGGER volatile      pLogger;
} g_aPerThreadLoggers[8] =
{
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0},
    { NIL_RTNATIVETHREAD, 0, 0}
};
#endif /* IN_RING0 */

/**
 * Logger flags instructions.
 */
static struct
{
    const char *pszInstr;               /**< The name  */
    size_t      cchInstr;               /**< The size of the name. */
    uint32_t    fFlag;                  /**< The flag value. */
    bool        fInverted;              /**< Inverse meaning? */
} const s_aLogFlags[] =
{
    { "disabled",     sizeof("disabled"    ) - 1,   RTLOGFLAGS_DISABLED,            false },
    { "enabled",      sizeof("enabled"     ) - 1,   RTLOGFLAGS_DISABLED,            true  },
    { "buffered",     sizeof("buffered"    ) - 1,   RTLOGFLAGS_BUFFERED,            false },
    { "unbuffered",   sizeof("unbuffered"  ) - 1,   RTLOGFLAGS_BUFFERED,            true  },
    { "usecrlf",      sizeof("usecrlf"     ) - 1,   RTLOGFLAGS_USECRLF,             false },
    { "uself",        sizeof("uself"       ) - 1,   RTLOGFLAGS_USECRLF,             true  },
    { "append",       sizeof("append"      ) - 1,   RTLOGFLAGS_APPEND,              false },
    { "overwrite",    sizeof("overwrite"   ) - 1,   RTLOGFLAGS_APPEND,              true  },
    { "rel",          sizeof("rel"         ) - 1,   RTLOGFLAGS_REL_TS,              false },
    { "abs",          sizeof("abs"         ) - 1,   RTLOGFLAGS_REL_TS,              true  },
    { "dec",          sizeof("dec"         ) - 1,   RTLOGFLAGS_DECIMAL_TS,          false },
    { "hex",          sizeof("hex"         ) - 1,   RTLOGFLAGS_DECIMAL_TS,          true  },
    { "writethru",    sizeof("writethru"   ) - 1,   RTLOGFLAGS_WRITE_THROUGH,       false },
    { "writethrough", sizeof("writethrough") - 1,   RTLOGFLAGS_WRITE_THROUGH,       false },
    { "flush",        sizeof("flush"       ) - 1,   RTLOGFLAGS_FLUSH,               false },
    { "lockcnts",     sizeof("lockcnts"    ) - 1,   RTLOGFLAGS_PREFIX_LOCK_COUNTS,  false },
    { "cpuid",        sizeof("cpuid"       ) - 1,   RTLOGFLAGS_PREFIX_CPUID,        false },
    { "pid",          sizeof("pid"         ) - 1,   RTLOGFLAGS_PREFIX_PID,          false },
    { "flagno",       sizeof("flagno"      ) - 1,   RTLOGFLAGS_PREFIX_FLAG_NO,      false },
    { "flag",         sizeof("flag"        ) - 1,   RTLOGFLAGS_PREFIX_FLAG,         false },
    { "groupno",      sizeof("groupno"     ) - 1,   RTLOGFLAGS_PREFIX_GROUP_NO,     false },
    { "group",        sizeof("group"       ) - 1,   RTLOGFLAGS_PREFIX_GROUP,        false },
    { "tid",          sizeof("tid"         ) - 1,   RTLOGFLAGS_PREFIX_TID,          false },
    { "thread",       sizeof("thread"      ) - 1,   RTLOGFLAGS_PREFIX_THREAD,       false },
    { "custom",       sizeof("custom"      ) - 1,   RTLOGFLAGS_PREFIX_CUSTOM,       false },
    { "timeprog",     sizeof("timeprog"    ) - 1,   RTLOGFLAGS_PREFIX_TIME_PROG,    false },
    { "time",         sizeof("time"        ) - 1,   RTLOGFLAGS_PREFIX_TIME,         false },
    { "msprog",       sizeof("msprog"      ) - 1,   RTLOGFLAGS_PREFIX_MS_PROG,      false },
    { "tsc",          sizeof("tsc"         ) - 1,   RTLOGFLAGS_PREFIX_TSC,          false }, /* before ts! */
    { "ts",           sizeof("ts"          ) - 1,   RTLOGFLAGS_PREFIX_TS,           false },
    /* We intentionally omit RTLOGFLAGS_RESTRICT_GROUPS. */
};

/**
 * Logger destination instructions.
 */
static struct
{
    const char *pszInstr;               /**< The name. */
    size_t      cchInstr;               /**< The size of the name. */
    uint32_t    fFlag;                  /**< The corresponding destination flag. */
} const s_aLogDst[] =
{
    { "file",     sizeof("file"    ) - 1,  RTLOGDEST_FILE }, /* Must be 1st! */
    { "dir",      sizeof("dir"     ) - 1,  RTLOGDEST_FILE }, /* Must be 2nd! */
    { "history",  sizeof("history" ) - 1,  0 },              /* Must be 3rd! */
    { "histsize", sizeof("histsize") - 1,  0 },              /* Must be 4th! */
    { "histtime", sizeof("histtime") - 1,  0 },              /* Must be 5th! */
    { "stdout",   sizeof("stdout"  ) - 1,  RTLOGDEST_STDOUT },
    { "stderr",   sizeof("stderr"  ) - 1,  RTLOGDEST_STDERR },
    { "debugger", sizeof("debugger") - 1,  RTLOGDEST_DEBUGGER },
    { "com",      sizeof("com"     ) - 1,  RTLOGDEST_COM },
    { "user",     sizeof("user"    ) - 1,  RTLOGDEST_USER },
};


/**
 * Locks the logger instance.
 *
 * @returns See RTSemSpinMutexRequest().
 * @param   pLogger     The logger instance.
 */
DECLINLINE(int) rtlogLock(PRTLOGGER pLogger)
{
#ifndef IN_RC
    PRTLOGGERINTERNAL pInt = pLogger->pInt;
    AssertMsgReturn(pInt->uRevision == RTLOGGERINTERNAL_REV, ("%#x != %#x\n", pInt->uRevision, RTLOGGERINTERNAL_REV),
                    VERR_LOG_REVISION_MISMATCH);
    AssertMsgReturn(pInt->cbSelf == sizeof(*pInt), ("%#x != %#x\n", pInt->cbSelf, sizeof(*pInt)),
                    VERR_LOG_REVISION_MISMATCH);
    if (pInt->hSpinMtx != NIL_RTSEMSPINMUTEX)
    {
        int rc = RTSemSpinMutexRequest(pInt->hSpinMtx);
        if (RT_FAILURE(rc))
            return rc;
    }
#endif
    return VINF_SUCCESS;
}


/**
 * Unlocks the logger instance.
 * @param   pLogger     The logger instance.
 */
DECLINLINE(void) rtlogUnlock(PRTLOGGER pLogger)
{
#ifndef IN_RC
    if (pLogger->pInt->hSpinMtx != NIL_RTSEMSPINMUTEX)
        RTSemSpinMutexRelease(pLogger->pInt->hSpinMtx);
#endif
    return;
}

#ifndef IN_RC
# ifdef IN_RING3

/**
 * Logging to file, output callback.
 *
 * @param  pvArg        User argument.
 * @param  pachChars    Pointer to an array of utf-8 characters.
 * @param  cbChars      Number of bytes in the character array pointed to by pachChars.
 */
static DECLCALLBACK(size_t) rtlogPhaseWrite(void *pvArg, const char *pachChars, size_t cbChars)
{
    PRTLOGGER pLogger = (PRTLOGGER)pvArg;
    RTFileWrite(pLogger->pInt->hFile, pachChars, cbChars, NULL);
    return cbChars;
}


/**
 * Callback to format VBox formatting extentions.
 * See @ref pg_rt_str_format for a reference on the format types.
 *
 * @returns The number of bytes formatted.
 * @param   pvArg           Formatter argument.
 * @param   pfnOutput       Pointer to output function.
 * @param   pvArgOutput     Argument for the output function.
 * @param   ppszFormat      Pointer to the format string pointer. Advance this till the char
 *                          after the format specifier.
 * @param   pArgs           Pointer to the argument list. Use this to fetch the arguments.
 * @param   cchWidth        Format Width. -1 if not specified.
 * @param   cchPrecision    Format Precision. -1 if not specified.
 * @param   fFlags          Flags (RTSTR_NTFS_*).
 * @param   chArgSize       The argument size specifier, 'l' or 'L'.
 */
static DECLCALLBACK(size_t) rtlogPhaseFormatStr(void *pvArg, PFNRTSTROUTPUT pfnOutput, void *pvArgOutput,
                                                const char **ppszFormat, va_list *pArgs, int cchWidth,
                                                int cchPrecision, unsigned fFlags, char chArgSize)
{
    char ch = *(*ppszFormat)++;

    AssertMsgFailed(("Invalid logger phase format type '%%%c%.10s'!\n", ch, *ppszFormat)); NOREF(ch);

    return 0;
}


/**
 * Log phase callback function, assumes the lock is already held
 *
 * @param   pLogger     The logger instance.
 * @param   pszFormat   Format string.
 * @param   ...         Optional arguments as specified in the format string.
 */
static DECLCALLBACK(void) rtlogPhaseMsgLocked(PRTLOGGER pLogger, const char *pszFormat, ...)
{
    va_list args;
    AssertPtrReturnVoid(pLogger);
    AssertPtrReturnVoid(pLogger->pInt);
    Assert(pLogger->pInt->hSpinMtx != NIL_RTSEMSPINMUTEX);

    va_start(args, pszFormat);
    rtlogLoggerExVLocked(pLogger, 0, ~0, pszFormat, args);
    va_end(args);
}


/**
 * Log phase callback function, assumes the lock is not held.
 *
 * @param   pLogger     The logger instance.
 * @param   pszFormat   Format string.
 * @param   ...         Optional arguments as specified in the format string.
 */
static DECLCALLBACK(void) rtlogPhaseMsgNormal(PRTLOGGER pLogger, const char *pszFormat, ...)
{
    va_list args;
    AssertPtrReturnVoid(pLogger);
    AssertPtrReturnVoid(pLogger->pInt);
    Assert(pLogger->pInt->hSpinMtx != NIL_RTSEMSPINMUTEX);

    va_start(args, pszFormat);
    RTLogLoggerExV(pLogger, 0, ~0, pszFormat, args);
    va_end(args);
}

# endif /* IN_RING3 */

RTDECL(int) RTLogCreateExV(PRTLOGGER *ppLogger, uint32_t fFlags, const char *pszGroupSettings,
                           const char *pszEnvVarBase, unsigned cGroups, const char * const *papszGroups,
                           uint32_t fDestFlags, PFNRTLOGPHASE pfnPhase, uint32_t cHistory,
                           uint64_t cbHistoryFileMax, uint32_t cSecsHistoryTimeSlot,
                           char *pszErrorMsg, size_t cchErrorMsg, const char *pszFilenameFmt, va_list args)
{
    int         rc;
    size_t      offInternal;
    size_t      cbLogger;
    PRTLOGGER   pLogger;

    /*
     * Validate input.
     */
    if (    (cGroups && !papszGroups)
        ||  !VALID_PTR(ppLogger) )
    {
        AssertMsgFailed(("Invalid parameters!\n"));
        return VERR_INVALID_PARAMETER;
    }
    *ppLogger = NULL;

    if (pszErrorMsg)
        RTStrPrintf(pszErrorMsg, cchErrorMsg, N_("unknown error"));

    AssertMsgReturn(cHistory < _1M, ("%#x", cHistory), VERR_OUT_OF_RANGE);

    /*
     * Allocate a logger instance.
     */
    offInternal = RT_OFFSETOF(RTLOGGER, afGroups[cGroups]);
    offInternal = RT_ALIGN_Z(offInternal, sizeof(uint64_t));
    cbLogger = offInternal + sizeof(RTLOGGERINTERNAL);
    if (fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
        cbLogger += cGroups * sizeof(uint32_t);
    pLogger = (PRTLOGGER)RTMemAllocZVar(cbLogger);
    if (pLogger)
    {
# if defined(RT_ARCH_X86) && (!defined(LOG_USE_C99) || !defined(RT_WITHOUT_EXEC_ALLOC))
        uint8_t *pu8Code;
# endif
        pLogger->u32Magic       = RTLOGGER_MAGIC;
        pLogger->cGroups        = cGroups;
        pLogger->fFlags         = fFlags;
        pLogger->fDestFlags     = fDestFlags;
        pLogger->pInt           = (PRTLOGGERINTERNAL)((uintptr_t)pLogger + offInternal);
        pLogger->pInt->uRevision                = RTLOGGERINTERNAL_REV;
        pLogger->pInt->cbSelf                   = sizeof(RTLOGGERINTERNAL);
        pLogger->pInt->hSpinMtx                 = NIL_RTSEMSPINMUTEX;
        pLogger->pInt->pfnFlush                 = NULL;
        pLogger->pInt->pfnPrefix                = NULL;
        pLogger->pInt->pvPrefixUserArg          = NULL;
        pLogger->pInt->afPadding1[0]            = false;
        pLogger->pInt->afPadding1[1]            = false;
        pLogger->pInt->afPadding1[2]            = false;
        pLogger->pInt->cMaxGroups               = cGroups;
        pLogger->pInt->papszGroups              = papszGroups;
        if (fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
            pLogger->pInt->pacEntriesPerGroup   = (uint32_t *)(pLogger->pInt + 1);
        else
            pLogger->pInt->pacEntriesPerGroup   = NULL;
        pLogger->pInt->cMaxEntriesPerGroup      = UINT32_MAX;
# ifdef IN_RING3
        pLogger->pInt->pfnPhase                 = pfnPhase;
        pLogger->pInt->hFile                    = NIL_RTFILE;
        pLogger->pInt->cHistory                 = cHistory;
        if (cbHistoryFileMax == 0)
            pLogger->pInt->cbHistoryFileMax     = UINT64_MAX;
        else
            pLogger->pInt->cbHistoryFileMax     = cbHistoryFileMax;
        if (cSecsHistoryTimeSlot == 0)
            pLogger->pInt->cSecsHistoryTimeSlot = UINT32_MAX;
        else
            pLogger->pInt->cSecsHistoryTimeSlot = cSecsHistoryTimeSlot;
# endif  /* IN_RING3 */
        if (pszGroupSettings)
            RTLogGroupSettings(pLogger, pszGroupSettings);

# if defined(RT_ARCH_X86) && (!defined(LOG_USE_C99) || !defined(RT_WITHOUT_EXEC_ALLOC))
        /*
         * Emit wrapper code.
         */
        pu8Code = (uint8_t *)RTMemExecAlloc(64);
        if (pu8Code)
        {
            pLogger->pfnLogger = *(PFNRTLOGGER*)&pu8Code;
            *pu8Code++ = 0x68;          /* push imm32 */
            *(void **)pu8Code = pLogger;
            pu8Code += sizeof(void *);
            *pu8Code++ = 0xe8;          /* call rel32 */
            *(uint32_t *)pu8Code = (uintptr_t)RTLogLogger - ((uintptr_t)pu8Code + sizeof(uint32_t));
            pu8Code += sizeof(uint32_t);
            *pu8Code++ = 0x8d;          /* lea esp, [esp + 4] */
            *pu8Code++ = 0x64;
            *pu8Code++ = 0x24;
            *pu8Code++ = 0x04;
            *pu8Code++ = 0xc3;          /* ret near */
            AssertMsg((uintptr_t)pu8Code - (uintptr_t)pLogger->pfnLogger <= 64,
                      ("Wrapper assembly is too big! %d bytes\n", (uintptr_t)pu8Code - (uintptr_t)pLogger->pfnLogger));
            rc = VINF_SUCCESS;
        }
        else
        {
#  ifdef RT_OS_LINUX
            if (pszErrorMsg) /* Most probably SELinux causing trouble since the larger RTMemAlloc succeeded. */
                RTStrPrintf(pszErrorMsg, cchErrorMsg, N_("mmap(PROT_WRITE | PROT_EXEC) failed -- SELinux?"));
#  endif
            rc = VERR_NO_MEMORY;
        }
        if (RT_SUCCESS(rc))
# endif /* X86 wrapper code*/
        {
# ifdef IN_RING3 /* files and env.vars. are only accessible when in R3 at the present time. */
            /*
             * Format the filename.
             */
            if (pszFilenameFmt)
            {
                /** @todo validate the length, fail on overflow. */
                RTStrPrintfV(pLogger->pInt->szFilename, sizeof(pLogger->pInt->szFilename), pszFilenameFmt, args);
                pLogger->fDestFlags |= RTLOGDEST_FILE;
            }

            /*
             * Parse the environment variables.
             */
            if (pszEnvVarBase)
            {
                /* make temp copy of environment variable base. */
                size_t  cchEnvVarBase = strlen(pszEnvVarBase);
                char   *pszEnvVar = (char *)alloca(cchEnvVarBase + 16);
                memcpy(pszEnvVar, pszEnvVarBase, cchEnvVarBase);

                /*
                 * Destination.
                 */
                strcpy(pszEnvVar + cchEnvVarBase, "_DEST");
                const char *pszVar = RTEnvGet(pszEnvVar);
                if (pszVar)
                    RTLogDestinations(pLogger, pszVar);

                /*
                 * The flags.
                 */
                strcpy(pszEnvVar + cchEnvVarBase, "_FLAGS");
                pszVar = RTEnvGet(pszEnvVar);
                if (pszVar)
                    RTLogFlags(pLogger, pszVar);

                /*
                 * The group settings.
                 */
                pszEnvVar[cchEnvVarBase] = '\0';
                pszVar = RTEnvGet(pszEnvVar);
                if (pszVar)
                    RTLogGroupSettings(pLogger, pszVar);
            }
# endif /* IN_RING3 */

            /*
             * Open the destination(s).
             */
            rc = VINF_SUCCESS;
# ifdef IN_RING3
            if (pLogger->fDestFlags & RTLOGDEST_FILE)
            {
                if (pLogger->fFlags & RTLOGFLAGS_APPEND)
                {
                    rc = rtlogFileOpen(pLogger, pszErrorMsg, cchErrorMsg);

                    /* Rotate in case of appending to a too big log file,
                       otherwise this simply doesn't do anything. */
                    rtlogRotate(pLogger, 0, true /* fFirst */);
                }
                else
                {
                    /* Force rotation if it is configured. */
                    pLogger->pInt->cbHistoryFileWritten = UINT64_MAX;
                    rtlogRotate(pLogger, 0, true /* fFirst */);

                    /* If the file is not open then rotation is not set up. */
                    if (pLogger->pInt->hFile == NIL_RTFILE)
                    {
                        pLogger->pInt->cbHistoryFileWritten = 0;
                        rc = rtlogFileOpen(pLogger, pszErrorMsg, cchErrorMsg);
                    }
                }
            }
# endif  /* IN_RING3 */

            /*
             * Create mutex and check how much it counts when entering the lock
             * so that we can report the values for RTLOGFLAGS_PREFIX_LOCK_COUNTS.
             */
            if (RT_SUCCESS(rc))
            {
                rc = RTSemSpinMutexCreate(&pLogger->pInt->hSpinMtx, RTSEMSPINMUTEX_FLAGS_IRQ_SAFE);
                if (RT_SUCCESS(rc))
                {
# ifdef IN_RING3 /** @todo do counters in ring-0 too? */
                    RTTHREAD Thread = RTThreadSelf();
                    if (Thread != NIL_RTTHREAD)
                    {
                        int32_t c = RTLockValidatorWriteLockGetCount(Thread);
                        RTSemSpinMutexRequest(pLogger->pInt->hSpinMtx);
                        c = RTLockValidatorWriteLockGetCount(Thread) - c;
                        RTSemSpinMutexRelease(pLogger->pInt->hSpinMtx);
                        ASMAtomicWriteU32(&g_cLoggerLockCount, c);
                    }

                    /* Use the callback to generate some initial log contents. */
                    Assert(VALID_PTR(pLogger->pInt->pfnPhase) || pLogger->pInt->pfnPhase == NULL);
                    if (pLogger->pInt->pfnPhase)
                        pLogger->pInt->pfnPhase(pLogger, RTLOGPHASE_BEGIN, rtlogPhaseMsgNormal);
# endif
                    *ppLogger = pLogger;
                    return VINF_SUCCESS;
                }

                if (pszErrorMsg)
                    RTStrPrintf(pszErrorMsg, cchErrorMsg, N_("failed to create semaphore"));
            }
# ifdef IN_RING3
            RTFileClose(pLogger->pInt->hFile);
# endif
# if defined(LOG_USE_C99) && defined(RT_WITHOUT_EXEC_ALLOC)
            RTMemFree(*(void **)&pLogger->pfnLogger);
# else
            RTMemExecFree(*(void **)&pLogger->pfnLogger, 64);
# endif
        }
        RTMemFree(pLogger);
    }
    else
        rc = VERR_NO_MEMORY;

    return rc;
}
RT_EXPORT_SYMBOL(RTLogCreateExV);


RTDECL(int) RTLogCreate(PRTLOGGER *ppLogger, uint32_t fFlags, const char *pszGroupSettings,
                        const char *pszEnvVarBase, unsigned cGroups, const char * const * papszGroups,
                        uint32_t fDestFlags, const char *pszFilenameFmt, ...)
{
    va_list args;
    int rc;

    va_start(args, pszFilenameFmt);
    rc = RTLogCreateExV(ppLogger, fFlags, pszGroupSettings, pszEnvVarBase, cGroups, papszGroups,
                        fDestFlags, NULL /*pfnPhase*/, 0 /*cHistory*/, 0 /*cbHistoryFileMax*/, 0 /*cSecsHistoryTimeSlot*/,
                        NULL /*pszErrorMsg*/, 0 /*cchErrorMsg*/, pszFilenameFmt, args);
    va_end(args);
    return rc;
}
RT_EXPORT_SYMBOL(RTLogCreate);


RTDECL(int) RTLogCreateEx(PRTLOGGER *ppLogger, uint32_t fFlags, const char *pszGroupSettings,
                          const char *pszEnvVarBase, unsigned cGroups, const char * const * papszGroups,
                          uint32_t fDestFlags, PFNRTLOGPHASE pfnPhase, uint32_t cHistory,
                          uint64_t cbHistoryFileMax, uint32_t cSecsHistoryTimeSlot,
                          char *pszErrorMsg, size_t cchErrorMsg, const char *pszFilenameFmt, ...)
{
    va_list args;
    int rc;

    va_start(args, pszFilenameFmt);
    rc = RTLogCreateExV(ppLogger, fFlags, pszGroupSettings, pszEnvVarBase, cGroups, papszGroups,
                        fDestFlags, pfnPhase, cHistory, cbHistoryFileMax, cSecsHistoryTimeSlot,
                        pszErrorMsg, cchErrorMsg, pszFilenameFmt, args);
    va_end(args);
    return rc;
}
RT_EXPORT_SYMBOL(RTLogCreateEx);


/**
 * Destroys a logger instance.
 *
 * The instance is flushed and all output destinations closed (where applicable).
 *
 * @returns iprt status code.
 * @param   pLogger             The logger instance which close destroyed. NULL is fine.
 */
RTDECL(int) RTLogDestroy(PRTLOGGER pLogger)
{
    int             rc;
    uint32_t        iGroup;
    RTSEMSPINMUTEX  hSpinMtx;

    /*
     * Validate input.
     */
    if (!pLogger)
        return VINF_SUCCESS;
    AssertPtrReturn(pLogger, VERR_INVALID_POINTER);
    AssertReturn(pLogger->u32Magic == RTLOGGER_MAGIC, VERR_INVALID_MAGIC);
    AssertPtrReturn(pLogger->pInt, VERR_INVALID_POINTER);

    /*
     * Acquire logger instance sem and disable all logging. (paranoia)
     */
    rc = rtlogLock(pLogger);
    AssertMsgRCReturn(rc, ("%Rrc\n", rc), rc);

    pLogger->fFlags |= RTLOGFLAGS_DISABLED;
    iGroup = pLogger->cGroups;
    while (iGroup-- > 0)
        pLogger->afGroups[iGroup] = 0;

    /*
     * Flush it.
     */
    rtlogFlush(pLogger);

# ifdef IN_RING3
    /*
     * Add end of logging message.
     */
    if (   (pLogger->fDestFlags & RTLOGDEST_FILE)
        && pLogger->pInt->hFile != NIL_RTFILE)
        pLogger->pInt->pfnPhase(pLogger, RTLOGPHASE_END, rtlogPhaseMsgLocked);

    /*
     * Close output stuffs.
     */
    if (pLogger->pInt->hFile != NIL_RTFILE)
    {
        int rc2 = RTFileClose(pLogger->pInt->hFile);
        AssertRC(rc2);
        if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
            rc = rc2;
        pLogger->pInt->hFile = NIL_RTFILE;
    }
# endif

    /*
     * Free the mutex, the wrapper and the instance memory.
     */
    hSpinMtx = pLogger->pInt->hSpinMtx;
    pLogger->pInt->hSpinMtx = NIL_RTSEMSPINMUTEX;
    if (hSpinMtx != NIL_RTSEMSPINMUTEX)
    {
        int rc2;
        RTSemSpinMutexRelease(hSpinMtx);
        rc2 = RTSemSpinMutexDestroy(hSpinMtx);
        AssertRC(rc2);
        if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
            rc = rc2;
    }

    if (pLogger->pfnLogger)
    {
# if defined(LOG_USE_C99) && defined(RT_WITHOUT_EXEC_ALLOC)
        RTMemFree(*(void **)&pLogger->pfnLogger);
# else
        RTMemExecFree(*(void **)&pLogger->pfnLogger, 64);
# endif
        pLogger->pfnLogger = NULL;
    }
    RTMemFree(pLogger);

    return rc;
}
RT_EXPORT_SYMBOL(RTLogDestroy);


/**
 * Create a logger instance clone for RC usage.
 *
 * @returns iprt status code.
 *
 * @param   pLogger             The logger instance to be cloned.
 * @param   pLoggerRC           Where to create the RC logger instance.
 * @param   cbLoggerRC          Amount of memory allocated to for the RC logger
 *                              instance clone.
 * @param   pfnLoggerRCPtr      Pointer to logger wrapper function for this
 *                              instance (RC Ptr).
 * @param   pfnFlushRCPtr       Pointer to flush function (RC Ptr).
 * @param   fFlags              Logger instance flags, a combination of the RTLOGFLAGS_* values.
 */
RTDECL(int) RTLogCloneRC(PRTLOGGER pLogger, PRTLOGGERRC pLoggerRC, size_t cbLoggerRC,
                         RTRCPTR pfnLoggerRCPtr, RTRCPTR pfnFlushRCPtr, uint32_t fFlags)
{
    /*
     * Validate input.
     */
   if (    !pLoggerRC
       ||  !pfnFlushRCPtr
       ||  !pfnLoggerRCPtr)
    {
       AssertMsgFailed(("Invalid parameters!\n"));
       return VERR_INVALID_PARAMETER;
    }
    if (cbLoggerRC < sizeof(*pLoggerRC))
    {
        AssertMsgFailed(("%d min=%d\n", cbLoggerRC, sizeof(*pLoggerRC)));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Initialize GC instance.
     */
    pLoggerRC->offScratch   = 0;
    pLoggerRC->fPendingPrefix = false;
    pLoggerRC->pfnLogger    = pfnLoggerRCPtr;
    pLoggerRC->pfnFlush     = pfnFlushRCPtr;
    pLoggerRC->u32Magic     = RTLOGGERRC_MAGIC;
    pLoggerRC->fFlags       = fFlags | RTLOGFLAGS_DISABLED;
    pLoggerRC->cGroups      = 1;
    pLoggerRC->afGroups[0]  = 0;

    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }

    /*
     * Check if there's enough space for the groups.
     */
    if (cbLoggerRC < (size_t)RT_OFFSETOF(RTLOGGERRC, afGroups[pLogger->cGroups]))
    {
        AssertMsgFailed(("%d req=%d cGroups=%d\n", cbLoggerRC, RT_OFFSETOF(RTLOGGERRC, afGroups[pLogger->cGroups]), pLogger->cGroups));
        return VERR_BUFFER_OVERFLOW;
    }
    memcpy(&pLoggerRC->afGroups[0], &pLogger->afGroups[0], pLogger->cGroups * sizeof(pLoggerRC->afGroups[0]));
    pLoggerRC->cGroups = pLogger->cGroups;

    /*
     * Copy bits from the HC instance.
     */
    pLoggerRC->fPendingPrefix = pLogger->pInt->fPendingPrefix;
    pLoggerRC->fFlags |= pLogger->fFlags;

    /*
     * Check if we can remove the disabled flag.
     */
    if (    pLogger->fDestFlags
        &&  !((pLogger->fFlags | fFlags) & RTLOGFLAGS_DISABLED))
        pLoggerRC->fFlags &= ~RTLOGFLAGS_DISABLED;

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogCloneRC);


/**
 * Flushes a RC logger instance to a R3 logger.
 *
 *
 * @returns iprt status code.
 * @param   pLogger     The R3 logger instance to flush pLoggerRC to. If NULL
 *                      the default logger is used.
 * @param   pLoggerRC   The RC logger instance to flush.
 */
RTDECL(void) RTLogFlushRC(PRTLOGGER pLogger, PRTLOGGERRC pLoggerRC)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
        {
            pLoggerRC->offScratch = 0;
            return;
        }
    }

    /*
     * Any thing to flush?
     */
    if (    pLogger->offScratch
        ||  pLoggerRC->offScratch)
    {
        /*
         * Acquire logger instance sem.
         */
        int rc = rtlogLock(pLogger);
        if (RT_FAILURE(rc))
            return;

        /*
         * Write whatever the GC instance contains to the HC one, and then
         * flush the HC instance.
         */
        if (pLoggerRC->offScratch)
        {
            rtLogOutput(pLogger, pLoggerRC->achScratch, pLoggerRC->offScratch);
            rtLogOutput(pLogger, NULL, 0);
            pLoggerRC->offScratch = 0;
        }

        /*
         * Release the semaphore.
         */
        rtlogUnlock(pLogger);
    }
}
RT_EXPORT_SYMBOL(RTLogFlushRC);

# ifdef IN_RING3

RTDECL(int) RTLogCreateForR0(PRTLOGGER pLogger, size_t cbLogger,
                             RTR0PTR pLoggerR0Ptr, RTR0PTR pfnLoggerR0Ptr, RTR0PTR pfnFlushR0Ptr,
                             uint32_t fFlags, uint32_t fDestFlags)
{
    /*
     * Validate input.
     */
    AssertPtrReturn(pLogger, VERR_INVALID_PARAMETER);
    size_t const cbRequired = sizeof(*pLogger) + RTLOGGERINTERNAL_R0_SIZE;
    AssertReturn(cbLogger >= cbRequired, VERR_BUFFER_OVERFLOW);
    AssertReturn(pLoggerR0Ptr != NIL_RTR0PTR, VERR_INVALID_PARAMETER);
    AssertReturn(pfnLoggerR0Ptr != NIL_RTR0PTR, VERR_INVALID_PARAMETER);

    /*
     * Initialize the ring-0 instance.
     */
    pLogger->achScratch[0]  = 0;
    pLogger->offScratch     = 0;
    pLogger->pfnLogger      = (PFNRTLOGGER)pfnLoggerR0Ptr;
    pLogger->fFlags         = fFlags;
    pLogger->fDestFlags     = fDestFlags & ~RTLOGDEST_FILE;
    pLogger->pInt           = NULL;
    pLogger->cGroups        = 1;
    pLogger->afGroups[0]    = 0;

    uint32_t cMaxGroups     = (uint32_t)((cbLogger - cbRequired) / sizeof(pLogger->afGroups[0]));
    if (fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
        cMaxGroups /= 2;
    PRTLOGGERINTERNAL pInt;
    for (;;)
    {
        AssertReturn(cMaxGroups > 0, VERR_BUFFER_OVERFLOW);
        pInt = (PRTLOGGERINTERNAL)&pLogger->afGroups[cMaxGroups];
        if (!((uintptr_t)pInt & (sizeof(uint64_t) - 1)))
            break;
        cMaxGroups--;
    }
    pLogger->pInt               = (PRTLOGGERINTERNAL)(pLoggerR0Ptr + (uintptr_t)pInt - (uintptr_t)pLogger);
    pInt->uRevision             = RTLOGGERINTERNAL_REV;
    pInt->cbSelf                = RTLOGGERINTERNAL_R0_SIZE;
    pInt->hSpinMtx              = NIL_RTSEMSPINMUTEX; /* Not serialized. */
    pInt->pfnFlush              = (PFNRTLOGFLUSH)pfnFlushR0Ptr;
    pInt->pfnPrefix             = NULL;
    pInt->pvPrefixUserArg       = NULL;
    pInt->fPendingPrefix        = false;
    pInt->cMaxGroups            = cMaxGroups;
    pInt->papszGroups           = NULL;
    pInt->cMaxEntriesPerGroup   = UINT32_MAX;
    if (fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
    {
        memset(pInt + 1, 0, sizeof(uint32_t) * cMaxGroups);
        pInt->pacEntriesPerGroup= (uint32_t *)(pLogger->pInt + 1);
    }
    else
        pInt->pacEntriesPerGroup= NULL;

    pLogger->u32Magic           = RTLOGGER_MAGIC;
    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogCreateForR0);


RTDECL(size_t) RTLogCalcSizeForR0(uint32_t cGroups, uint32_t fFlags)
{
    size_t cb = RT_OFFSETOF(RTLOGGER, afGroups[cGroups]);
    cb = RT_ALIGN_Z(cb, sizeof(uint64_t));
    cb += sizeof(RTLOGGERINTERNAL);
    if (fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
        cb += sizeof(uint32_t) * cGroups;
    return cb;
}
RT_EXPORT_SYMBOL(RTLogCalcSizeForR0);


RTDECL(int) RTLogCopyGroupsAndFlagsForR0(PRTLOGGER pDstLogger, RTR0PTR pDstLoggerR0Ptr,
                                         PCRTLOGGER pSrcLogger, uint32_t fFlagsOr, uint32_t fFlagsAnd)
{
    /*
     * Validate input.
     */
    AssertPtrReturn(pDstLogger, VERR_INVALID_PARAMETER);
    AssertPtrNullReturn(pSrcLogger, VERR_INVALID_PARAMETER);

    /*
     * Resolve defaults.
     */
    if (!pSrcLogger)
    {
        pSrcLogger = RTLogDefaultInstance();
        if (!pSrcLogger)
        {
            pDstLogger->fFlags |= RTLOGFLAGS_DISABLED | fFlagsOr;
            pDstLogger->cGroups = 1;
            pDstLogger->afGroups[0] = 0;
            return VINF_SUCCESS;
        }
    }

    /*
     * Copy flags and group settings.
     */
    pDstLogger->fFlags = (pSrcLogger->fFlags & fFlagsAnd & ~RTLOGFLAGS_RESTRICT_GROUPS) | fFlagsOr;

    PRTLOGGERINTERNAL   pDstInt = (PRTLOGGERINTERNAL)((uintptr_t)pDstLogger->pInt - pDstLoggerR0Ptr + (uintptr_t)pDstLogger);
    int                 rc      = VINF_SUCCESS;
    uint32_t            cGroups = pSrcLogger->cGroups;
    if (cGroups > pDstInt->cMaxGroups)
    {
        AssertMsgFailed(("cMaxGroups=%zd cGroups=%zd (min size %d)\n", pDstInt->cMaxGroups,
                         pSrcLogger->cGroups, RT_OFFSETOF(RTLOGGER, afGroups[pSrcLogger->cGroups]) + RTLOGGERINTERNAL_R0_SIZE));
        rc = VERR_INVALID_PARAMETER;
        cGroups = pDstInt->cMaxGroups;
    }
    memcpy(&pDstLogger->afGroups[0], &pSrcLogger->afGroups[0], cGroups * sizeof(pDstLogger->afGroups[0]));
    pDstLogger->cGroups = cGroups;

    return rc;
}
RT_EXPORT_SYMBOL(RTLogCopyGroupsAndFlagsForR0);


RTDECL(int) RTLogSetCustomPrefixCallbackForR0(PRTLOGGER pLogger, RTR0PTR pLoggerR0Ptr,
                                              RTR0PTR pfnCallbackR0Ptr, RTR0PTR pvUserR0Ptr)
{
    AssertPtrReturn(pLogger, VERR_INVALID_POINTER);
    AssertReturn(pLogger->u32Magic == RTLOGGER_MAGIC, VERR_INVALID_MAGIC);

    /*
     * Do the work.
     */
    PRTLOGGERINTERNAL pInt = (PRTLOGGERINTERNAL)((uintptr_t)pLogger->pInt - pLoggerR0Ptr + (uintptr_t)pLogger);
    AssertReturn(pInt->uRevision == RTLOGGERINTERNAL_REV, VERR_LOG_REVISION_MISMATCH);
    pInt->pvPrefixUserArg = (void *)pvUserR0Ptr;
    pInt->pfnPrefix       = (PFNRTLOGPREFIX)pfnCallbackR0Ptr;

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogSetCustomPrefixCallbackForR0);

RTDECL(void) RTLogFlushR0(PRTLOGGER pLogger, PRTLOGGER pLoggerR0)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
        {
            /* flushing to "/dev/null". */
            if (pLoggerR0->offScratch)
                    pLoggerR0->offScratch = 0;
            return;
        }
    }

    /*
     * Any thing to flush?
     */
    if (    pLoggerR0->offScratch
        ||  pLogger->offScratch)
    {
        /*
         * Acquire logger semaphores.
         */
        int rc = rtlogLock(pLogger);
        if (RT_FAILURE(rc))
            return;
        if (RT_SUCCESS(rc))
        {
            /*
             * Write whatever the GC instance contains to the HC one, and then
             * flush the HC instance.
             */
            if (pLoggerR0->offScratch)
            {
                rtLogOutput(pLogger, pLoggerR0->achScratch, pLoggerR0->offScratch);
                rtLogOutput(pLogger, NULL, 0);
                pLoggerR0->offScratch = 0;
            }
        }
        rtlogUnlock(pLogger);
    }
}
RT_EXPORT_SYMBOL(RTLogFlushR0);

# endif /* IN_RING3 */


/**
 * Flushes the buffer in one logger instance onto another logger.
 *
 * @returns iprt status code.
 *
 * @param   pSrcLogger   The logger instance to flush.
 * @param   pDstLogger   The logger instance to flush onto.
 *                       If NULL the default logger will be used.
 */
RTDECL(void) RTLogFlushToLogger(PRTLOGGER pSrcLogger, PRTLOGGER pDstLogger)
{
    /*
     * Resolve defaults.
     */
    if (!pDstLogger)
    {
        pDstLogger = RTLogDefaultInstance();
        if (!pDstLogger)
        {
            /* flushing to "/dev/null". */
            if (pSrcLogger->offScratch)
            {
                int rc = rtlogLock(pSrcLogger);
                if (RT_SUCCESS(rc))
                {
                    pSrcLogger->offScratch = 0;
                    rtlogUnlock(pSrcLogger);
                }
            }
            return;
        }
    }

    /*
     * Any thing to flush?
     */
    if (    pSrcLogger->offScratch
        ||  pDstLogger->offScratch)
    {
        /*
         * Acquire logger semaphores.
         */
        int rc = rtlogLock(pDstLogger);
        if (RT_FAILURE(rc))
            return;
        rc = rtlogLock(pSrcLogger);
        if (RT_SUCCESS(rc))
        {
            /*
             * Write whatever the GC instance contains to the HC one, and then
             * flush the HC instance.
             */
            if (pSrcLogger->offScratch)
            {
                rtLogOutput(pDstLogger, pSrcLogger->achScratch, pSrcLogger->offScratch);
                rtLogOutput(pDstLogger, NULL, 0);
                pSrcLogger->offScratch = 0;
            }

            /*
             * Release the semaphores.
             */
            rtlogUnlock(pSrcLogger);
        }
        rtlogUnlock(pDstLogger);
    }
}
RT_EXPORT_SYMBOL(RTLogFlushToLogger);


/**
 * Sets the custom prefix callback.
 *
 * @returns IPRT status code.
 * @param   pLogger     The logger instance.
 * @param   pfnCallback The callback.
 * @param   pvUser      The user argument for the callback.
 *  */
RTDECL(int) RTLogSetCustomPrefixCallback(PRTLOGGER pLogger, PFNRTLOGPREFIX pfnCallback, void *pvUser)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }
    AssertReturn(pLogger->u32Magic == RTLOGGER_MAGIC, VERR_INVALID_MAGIC);

    /*
     * Do the work.
     */
    rtlogLock(pLogger);
    pLogger->pInt->pvPrefixUserArg = pvUser;
    pLogger->pInt->pfnPrefix       = pfnCallback;
    rtlogUnlock(pLogger);

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogSetCustomPrefixCallback);


/**
 * Matches a group name with a pattern mask in an case insensitive manner (ASCII).
 *
 * @returns true if matching and *ppachMask set to the end of the pattern.
 * @returns false if no match.
 * @param   pszGrp      The group name.
 * @param   ppachMask   Pointer to the pointer to the mask. Only wildcard supported is '*'.
 * @param   cchMask     The length of the mask, including modifiers. The modifiers is why
 *                      we update *ppachMask on match.
 */
static bool rtlogIsGroupMatching(const char *pszGrp, const char **ppachMask, size_t cchMask)
{
    const char *pachMask;

    if (!pszGrp || !*pszGrp)
        return false;
    pachMask = *ppachMask;
    for (;;)
    {
        if (RT_C_TO_LOWER(*pszGrp) != RT_C_TO_LOWER(*pachMask))
        {
            const char *pszTmp;

            /*
             * Check for wildcard and do a minimal match if found.
             */
            if (*pachMask != '*')
                return false;

            /* eat '*'s. */
            do  pachMask++;
            while (--cchMask && *pachMask == '*');

            /* is there more to match? */
            if (    !cchMask
                ||  *pachMask == '.'
                ||  *pachMask == '=')
                break; /* we're good */

            /* do extremely minimal matching (fixme) */
            pszTmp = strchr(pszGrp, RT_C_TO_LOWER(*pachMask));
            if (!pszTmp)
                pszTmp = strchr(pszGrp, RT_C_TO_UPPER(*pachMask));
            if (!pszTmp)
                return false;
            pszGrp = pszTmp;
            continue;
        }

        /* done? */
        if (!*++pszGrp)
        {
            /* trailing wildcard is ok. */
            do
            {
                pachMask++;
                cchMask--;
            } while (cchMask && *pachMask == '*');
            if (    !cchMask
                ||  *pachMask == '.'
                ||  *pachMask == '=')
                break; /* we're good */
            return false;
        }

        if (!--cchMask)
            return false;
        pachMask++;
    }

    /* match */
    *ppachMask = pachMask;
    return true;
}


/**
 * Updates the group settings for the logger instance using the specified
 * specification string.
 *
 * @returns iprt status code.
 *          Failures can safely be ignored.
 * @param   pLogger     Logger instance.
 * @param   pszVar      Value to parse.
 */
RTDECL(int) RTLogGroupSettings(PRTLOGGER pLogger, const char *pszVar)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }

    /*
     * Iterate the string.
     */
    while (*pszVar)
    {
        /*
         * Skip prefixes (blanks, ;, + and -).
         */
        bool    fEnabled = true;
        char    ch;
        const char *pszStart;
        unsigned i;
        size_t cch;

        while ((ch = *pszVar) == '+' || ch == '-' || ch == ' ' || ch == '\t' || ch == '\n' || ch == ';')
        {
            if (ch == '+' || ch == '-' || ch == ';')
                fEnabled = ch != '-';
            pszVar++;
        }
        if (!*pszVar)
            break;

        /*
         * Find end.
         */
        pszStart = pszVar;
        while ((ch = *pszVar) != '\0' && ch != '+' && ch != '-' && ch != ' ' && ch != '\t')
            pszVar++;

        /*
         * Find the group (ascii case insensitive search).
         * Special group 'all'.
         */
        cch = pszVar - pszStart;
        if (    cch >= 3
            &&  (pszStart[0] == 'a' || pszStart[0] == 'A')
            &&  (pszStart[1] == 'l' || pszStart[1] == 'L')
            &&  (pszStart[2] == 'l' || pszStart[2] == 'L')
            &&  (cch == 3 || pszStart[3] == '.' || pszStart[3] == '='))
        {
            /*
             * All.
             */
            unsigned fFlags = cch == 3
                            ? RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1
                            : rtlogGroupFlags(&pszStart[3]);
            for (i = 0; i < pLogger->cGroups; i++)
            {
                if (fEnabled)
                    pLogger->afGroups[i] |= fFlags;
                else
                    pLogger->afGroups[i] &= ~fFlags;
            }
        }
        else
        {
            /*
             * Specific group(s).
             */
            for (i = 0; i < pLogger->cGroups; i++)
            {
                const char *psz2 = (const char*)pszStart;
                if (rtlogIsGroupMatching(pLogger->pInt->papszGroups[i], &psz2, cch))
                {
                    unsigned fFlags = RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1;
                    if (*psz2 == '.' || *psz2 == '=')
                        fFlags = rtlogGroupFlags(psz2);
                    if (fEnabled)
                        pLogger->afGroups[i] |= fFlags;
                    else
                        pLogger->afGroups[i] &= ~fFlags;
                }
            } /* for each group */
        }

    } /* parse specification */

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogGroupSettings);


/**
 * Interprets the group flags suffix.
 *
 * @returns Flags specified. (0 is possible!)
 * @param   psz     Start of Suffix. (Either dot or equal sign.)
 */
static unsigned rtlogGroupFlags(const char *psz)
{
    unsigned fFlags = 0;

    /*
     * Literal flags.
     */
    while (*psz == '.')
    {
        static struct
        {
            const char *pszFlag;        /* lowercase!! */
            unsigned    fFlag;
        } aFlags[] =
        {
            { "eo",         RTLOGGRPFLAGS_ENABLED },
            { "enabledonly",RTLOGGRPFLAGS_ENABLED },
            { "e",          RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1 },
            { "enabled",    RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1 },
            { "l1",         RTLOGGRPFLAGS_LEVEL_1 },
            { "level1",     RTLOGGRPFLAGS_LEVEL_1 },
            { "l",          RTLOGGRPFLAGS_LEVEL_2 },
            { "l2",         RTLOGGRPFLAGS_LEVEL_2 },
            { "level2",     RTLOGGRPFLAGS_LEVEL_2 },
            { "l3",         RTLOGGRPFLAGS_LEVEL_3 },
            { "level3",     RTLOGGRPFLAGS_LEVEL_3 },
            { "l4",         RTLOGGRPFLAGS_LEVEL_4 },
            { "level4",     RTLOGGRPFLAGS_LEVEL_4 },
            { "l5",         RTLOGGRPFLAGS_LEVEL_5 },
            { "level5",     RTLOGGRPFLAGS_LEVEL_5 },
            { "l6",         RTLOGGRPFLAGS_LEVEL_6 },
            { "level6",     RTLOGGRPFLAGS_LEVEL_6 },
            { "f",          RTLOGGRPFLAGS_FLOW },
            { "flow",       RTLOGGRPFLAGS_FLOW },
            { "restrict",   RTLOGGRPFLAGS_RESTRICT },

            { "lelik",      RTLOGGRPFLAGS_LELIK },
            { "michael",    RTLOGGRPFLAGS_MICHAEL },
            { "sunlover",   RTLOGGRPFLAGS_SUNLOVER },
            { "achim",      RTLOGGRPFLAGS_ACHIM },
            { "achimha",    RTLOGGRPFLAGS_ACHIM },
            { "s",          RTLOGGRPFLAGS_SANDER },
            { "sander",     RTLOGGRPFLAGS_SANDER },
            { "sandervl",   RTLOGGRPFLAGS_SANDER },
            { "klaus",      RTLOGGRPFLAGS_KLAUS },
            { "frank",      RTLOGGRPFLAGS_FRANK },
            { "b",          RTLOGGRPFLAGS_BIRD },
            { "bird",       RTLOGGRPFLAGS_BIRD },
            { "aleksey",    RTLOGGRPFLAGS_ALEKSEY },
            { "dj",         RTLOGGRPFLAGS_DJ },
            { "n",          RTLOGGRPFLAGS_NONAME },
            { "noname",     RTLOGGRPFLAGS_NONAME }
        };
        unsigned    i;
        bool        fFound = false;
        psz++;
        for (i = 0; i < RT_ELEMENTS(aFlags) && !fFound; i++)
        {
            const char *psz1 = aFlags[i].pszFlag;
            const char *psz2 = psz;
            while (*psz1 == RT_C_TO_LOWER(*psz2))
            {
                psz1++;
                psz2++;
                if (!*psz1)
                {
                    if (    (*psz2 >= 'a' && *psz2 <= 'z')
                        ||  (*psz2 >= 'A' && *psz2 <= 'Z')
                        ||  (*psz2 >= '0' && *psz2 <= '9') )
                        break;
                    fFlags |= aFlags[i].fFlag;
                    fFound = true;
                    psz = psz2;
                    break;
                }
            } /* strincmp */
        } /* for each flags */
    }

    /*
     * Flag value.
     */
    if (*psz == '=')
    {
        psz++;
        if (*psz == '~')
            fFlags = ~RTStrToInt32(psz + 1);
        else
            fFlags = RTStrToInt32(psz);
    }

    return fFlags;
}

/**
 * Helper for RTLogGetGroupSettings.
 */
static int rtLogGetGroupSettingsAddOne(const char *pszName, uint32_t fGroup, char **ppszBuf, size_t *pcchBuf, bool *pfNotFirst)
{
# define APPEND_PSZ(psz,cch) do { memcpy(*ppszBuf, (psz), (cch)); *ppszBuf += (cch); *pcchBuf -= (cch); } while (0)
# define APPEND_SZ(sz)       APPEND_PSZ(sz, sizeof(sz) - 1)
# define APPEND_CH(ch)       do { **ppszBuf = (ch); *ppszBuf += 1; *pcchBuf -= 1; } while (0)

    /*
     * Add the name.
     */
    size_t cchName = strlen(pszName);
    if (cchName + 1 + *pfNotFirst > *pcchBuf)
        return VERR_BUFFER_OVERFLOW;
    if (*pfNotFirst)
        APPEND_CH(' ');
    else
        *pfNotFirst = true;
    APPEND_PSZ(pszName, cchName);

    /*
     * Only generate mnemonics for the simple+common bits.
     */
    if (fGroup == (RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1))
        /* nothing */;
    else if (    fGroup == (RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1 | RTLOGGRPFLAGS_LEVEL_2 |  RTLOGGRPFLAGS_FLOW)
             &&  *pcchBuf >= sizeof(".e.l.f"))
        APPEND_SZ(".e.l.f");
    else if (    fGroup == (RTLOGGRPFLAGS_ENABLED | RTLOGGRPFLAGS_LEVEL_1 | RTLOGGRPFLAGS_FLOW)
             &&  *pcchBuf >= sizeof(".e.f"))
        APPEND_SZ(".e.f");
    else if (*pcchBuf >= 1 + 10 + 1)
    {
        size_t cch;
        APPEND_CH('=');
        cch = RTStrFormatNumber(*ppszBuf, fGroup, 16, 0, 0, RTSTR_F_SPECIAL | RTSTR_F_32BIT);
        *ppszBuf += cch;
        *pcchBuf -= cch;
    }
    else
        return VERR_BUFFER_OVERFLOW;

# undef APPEND_PSZ
# undef APPEND_SZ
# undef APPEND_CH
    return VINF_SUCCESS;
}


/**
 * Get the current log group settings as a string.
 *
 * @returns VINF_SUCCESS or VERR_BUFFER_OVERFLOW.
 * @param   pLogger             Logger instance (NULL for default logger).
 * @param   pszBuf              The output buffer.
 * @param   cchBuf              The size of the output buffer. Must be greater
 *                              than zero.
 */
RTDECL(int) RTLogGetGroupSettings(PRTLOGGER pLogger, char *pszBuf, size_t cchBuf)
{
    bool        fNotFirst = false;
    int         rc        = VINF_SUCCESS;
    uint32_t    cGroups;
    uint32_t    fGroup;
    uint32_t    i;

    Assert(cchBuf);

    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
        {
            *pszBuf = '\0';
            return VINF_SUCCESS;
        }
    }

    cGroups = pLogger->cGroups;

    /*
     * Check if all are the same.
     */
    fGroup = pLogger->afGroups[0];
    for (i = 1; i < cGroups; i++)
        if (pLogger->afGroups[i] != fGroup)
            break;
    if (i >= cGroups)
        rc = rtLogGetGroupSettingsAddOne("all", fGroup, &pszBuf, &cchBuf, &fNotFirst);
    else
    {

        /*
         * Iterate all the groups and print all that are enabled.
         */
        for (i = 0; i < cGroups; i++)
        {
            fGroup = pLogger->afGroups[i];
            if (fGroup)
            {
                const char *pszName = pLogger->pInt->papszGroups[i];
                if (pszName)
                {
                    rc = rtLogGetGroupSettingsAddOne(pszName, fGroup, &pszBuf, &cchBuf, &fNotFirst);
                    if (rc)
                        break;
                }
            }
        }
    }

    *pszBuf = '\0';
    return rc;
}
RT_EXPORT_SYMBOL(RTLogGetGroupSettings);

#endif /* !IN_RC */

/**
 * Updates the flags for the logger instance using the specified
 * specification string.
 *
 * @returns iprt status code.
 *          Failures can safely be ignored.
 * @param   pLogger     Logger instance (NULL for default logger).
 * @param   pszVar      Value to parse.
 */
RTDECL(int) RTLogFlags(PRTLOGGER pLogger, const char *pszVar)
{
    int rc = VINF_SUCCESS;

    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }

    /*
     * Iterate the string.
     */
    while (*pszVar)
    {
        /* check no prefix. */
        bool fNo = false;
        char ch;
        unsigned i;

        /* skip blanks. */
        while (RT_C_IS_SPACE(*pszVar))
            pszVar++;
        if (!*pszVar)
            return rc;

        while ((ch = *pszVar) != '\0')
        {
            if (ch == 'n' && pszVar[1] == 'o')
            {
                pszVar += 2;
                fNo = !fNo;
            }
            else if (ch == '+')
            {
                pszVar++;
                fNo = true;
            }
            else if (ch == '-' || ch == '!' || ch == '~')
            {
                pszVar++;
                fNo = !fNo;
            }
            else
                break;
        }

        /* instruction. */
        for (i = 0; i < RT_ELEMENTS(s_aLogFlags); i++)
        {
            if (!strncmp(pszVar, s_aLogFlags[i].pszInstr, s_aLogFlags[i].cchInstr))
            {
                if (fNo == s_aLogFlags[i].fInverted)
                    pLogger->fFlags |= s_aLogFlags[i].fFlag;
                else
                    pLogger->fFlags &= ~s_aLogFlags[i].fFlag;
                pszVar += s_aLogFlags[i].cchInstr;
                break;
            }
        }

        /* unknown instruction? */
        if (i >= RT_ELEMENTS(s_aLogFlags))
        {
            AssertMsgFailed(("Invalid flags! unknown instruction %.20s\n", pszVar));
            pszVar++;
        }

        /* skip blanks and delimiters. */
        while (RT_C_IS_SPACE(*pszVar) || *pszVar == ';')
            pszVar++;
    } /* while more environment variable value left */

    return rc;
}
RT_EXPORT_SYMBOL(RTLogFlags);


/**
 * Changes the buffering setting of the specified logger.
 *
 * This can be used for optimizing longish logging sequences.
 *
 * @returns The old state.
 * @param   pLogger         The logger instance (NULL is an alias for the
 *                          default logger).
 * @param   fBuffered       The new state.
 */
RTDECL(bool) RTLogSetBuffering(PRTLOGGER pLogger, bool fBuffered)
{
    bool fOld;

    /*
     * Resolve the logger instance.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return false;
    }

    rtlogLock(pLogger);
    fOld  = !!(pLogger->fFlags & RTLOGFLAGS_BUFFERED);
    if (fBuffered)
        pLogger->fFlags |= RTLOGFLAGS_BUFFERED;
    else
        pLogger->fFlags &= ~RTLOGFLAGS_BUFFERED;
    rtlogUnlock(pLogger);

    return fOld;
}
RT_EXPORT_SYMBOL(RTLogSetBuffering);


#ifdef IN_RING3
RTDECL(uint32_t) RTLogSetGroupLimit(PRTLOGGER pLogger, uint32_t cMaxEntriesPerGroup)
{
    /*
     * Resolve the logger instance.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return UINT32_MAX;
    }

    rtlogLock(pLogger);
    uint32_t cOld = pLogger->pInt->cMaxEntriesPerGroup;
    pLogger->pInt->cMaxEntriesPerGroup = cMaxEntriesPerGroup;
    rtlogUnlock(pLogger);

    return cOld;
}
#endif

#ifndef IN_RC

/**
 * Get the current log flags as a string.
 *
 * @returns VINF_SUCCESS or VERR_BUFFER_OVERFLOW.
 * @param   pLogger             Logger instance (NULL for default logger).
 * @param   pszBuf              The output buffer.
 * @param   cchBuf              The size of the output buffer. Must be greater
 *                              than zero.
 */
RTDECL(int) RTLogGetFlags(PRTLOGGER pLogger, char *pszBuf, size_t cchBuf)
{
    bool        fNotFirst = false;
    int         rc        = VINF_SUCCESS;
    uint32_t    fFlags;
    unsigned    i;

    Assert(cchBuf);

    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
        {
            *pszBuf = '\0';
            return VINF_SUCCESS;
        }
    }

    /*
     * Add the flags in the list.
     */
    fFlags = pLogger->fFlags;
    for (i = 0; i < RT_ELEMENTS(s_aLogFlags); i++)
        if (    !s_aLogFlags[i].fInverted
            ?   (s_aLogFlags[i].fFlag & fFlags)
            :   !(s_aLogFlags[i].fFlag & fFlags))
        {
            size_t cchInstr = s_aLogFlags[i].cchInstr;
            if (cchInstr + fNotFirst + 1 > cchBuf)
            {
                rc = VERR_BUFFER_OVERFLOW;
                break;
            }
            if (fNotFirst)
            {
                *pszBuf++ = ' ';
                cchBuf--;
            }
            memcpy(pszBuf, s_aLogFlags[i].pszInstr, cchInstr);
            pszBuf += cchInstr;
            cchBuf -= cchInstr;
            fNotFirst = true;
        }
    *pszBuf = '\0';
    return rc;
}
RT_EXPORT_SYMBOL(RTLogGetFlags);


/**
 * Updates the logger destination using the specified string.
 *
 * @returns VINF_SUCCESS or VERR_BUFFER_OVERFLOW.
 * @param   pLogger             Logger instance (NULL for default logger).
 * @param   pszVar              The value to parse.
 */
RTDECL(int) RTLogDestinations(PRTLOGGER pLogger, char const *pszVar)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }

    /*
     * Do the parsing.
     */
    while (*pszVar)
    {
        bool fNo;
        unsigned i;

        /* skip blanks. */
        while (RT_C_IS_SPACE(*pszVar))
            pszVar++;
        if (!*pszVar)
            break;

        /* check no prefix. */
        fNo = false;
        if (pszVar[0] == 'n' && pszVar[1] == 'o')
        {
            fNo = true;
            pszVar += 2;
        }

        /* instruction. */
        for (i = 0; i < RT_ELEMENTS(s_aLogDst); i++)
        {
            size_t cchInstr = strlen(s_aLogDst[i].pszInstr);
            if (!strncmp(pszVar, s_aLogDst[i].pszInstr, cchInstr))
            {
                if (!fNo)
                    pLogger->fDestFlags |= s_aLogDst[i].fFlag;
                else
                    pLogger->fDestFlags &= ~s_aLogDst[i].fFlag;
                pszVar += cchInstr;

                /* check for value. */
                while (RT_C_IS_SPACE(*pszVar))
                    pszVar++;
                if (*pszVar == '=' || *pszVar == ':')
                {
                    const char *pszEnd;

                    pszVar++;
                    pszEnd = strchr(pszVar, ';');
                    if (!pszEnd)
                        pszEnd = strchr(pszVar, '\0');
# ifdef IN_RING3
                    size_t cch = pszEnd - pszVar;

                    /* log file name */
                    if (i == 0 /* file */ && !fNo)
                    {
                        AssertReturn(cch < sizeof(pLogger->pInt->szFilename), VERR_OUT_OF_RANGE);
                        memcpy(pLogger->pInt->szFilename, pszVar, cch);
                        pLogger->pInt->szFilename[cch] = '\0';
                    }
                    /* log directory */
                    else if (i == 1 /* dir */ && !fNo)
                    {
                        char        szTmp[sizeof(pLogger->pInt->szFilename)];
                        const char *pszFile = RTPathFilename(pLogger->pInt->szFilename);
                        size_t      cchFile = pszFile ? strlen(pszFile) : 0;
                        AssertReturn(cchFile + cch + 1 < sizeof(pLogger->pInt->szFilename), VERR_OUT_OF_RANGE);
                        memcpy(szTmp, cchFile ? pszFile : "", cchFile + 1);

                        memcpy(pLogger->pInt->szFilename, pszVar, cch);
                        pLogger->pInt->szFilename[cch] = '\0';
                        RTPathStripTrailingSlash(pLogger->pInt->szFilename);

                        cch = strlen(pLogger->pInt->szFilename);
                        pLogger->pInt->szFilename[cch++] = '/';
                        memcpy(&pLogger->pInt->szFilename[cch], szTmp, cchFile);
                        pLogger->pInt->szFilename[cch + cchFile] = '\0';
                    }
                    else if (i == 2 /* history */)
                    {
                        if (!fNo)
                        {
                            uint32_t    cHistory = 0;
                            char        szTmp[32];
                            int rc = RTStrCopyEx(szTmp, sizeof(szTmp), pszVar, cch);
                            if (RT_SUCCESS(rc))
                                rc = RTStrToUInt32Full(szTmp, 0, &cHistory);
                            AssertMsgReturn(RT_SUCCESS(rc) && cHistory < _1M, ("Invalid history value %s (%Rrc)!\n", szTmp, rc), rc);
                            pLogger->pInt->cHistory = cHistory;
                        }
                        else
                            pLogger->pInt->cHistory = 0;
                    }
                    else if (i == 3 /* histsize */)
                    {
                        if (!fNo)
                        {
                            char szTmp[32];
                            int rc = RTStrCopyEx(szTmp, sizeof(szTmp), pszVar, cch);
                            if (RT_SUCCESS(rc))
                                rc = RTStrToUInt64Full(szTmp, 0, &pLogger->pInt->cbHistoryFileMax);
                            AssertMsgRCReturn(rc, ("Invalid history file size value %s (%Rrc)!\n", szTmp, rc), rc);
                            if (pLogger->pInt->cbHistoryFileMax == 0)
                                pLogger->pInt->cbHistoryFileMax = UINT64_MAX;
                        }
                        else
                            pLogger->pInt->cbHistoryFileMax = UINT64_MAX;
                    }
                    else if (i == 4 /* histtime */)
                    {
                        if (!fNo)
                        {
                            char szTmp[32];
                            int rc = RTStrCopyEx(szTmp, sizeof(szTmp), pszVar, cch);
                            if (RT_SUCCESS(rc))
                                rc = RTStrToUInt32Full(szTmp, 0, &pLogger->pInt->cSecsHistoryTimeSlot);
                            AssertMsgRCReturn(rc, ("Invalid history time slot value %s (%Rrc)!\n", szTmp, rc), rc);
                            if (pLogger->pInt->cSecsHistoryTimeSlot == 0)
                                pLogger->pInt->cSecsHistoryTimeSlot = UINT32_MAX;
                        }
                        else
                            pLogger->pInt->cSecsHistoryTimeSlot = UINT32_MAX;
                    }
                    else
                        AssertMsgFailedReturn(("Invalid destination value! %s%s doesn't take a value!\n",
                                               fNo ? "no" : "", s_aLogDst[i].pszInstr),
                                              VERR_INVALID_PARAMETER);
# endif /* IN_RING3 */
                    pszVar = pszEnd + (*pszEnd != '\0');
                }
                break;
            }
        }

        /* assert known instruction */
        AssertMsgReturn(i < RT_ELEMENTS(s_aLogDst),
                        ("Invalid destination value! unknown instruction %.20s\n", pszVar),
                        VERR_INVALID_PARAMETER);

        /* skip blanks and delimiters. */
        while (RT_C_IS_SPACE(*pszVar) || *pszVar == ';')
            pszVar++;
    } /* while more environment variable value left */

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogDestinations);


/**
 * Get the current log destinations as a string.
 *
 * @returns VINF_SUCCESS or VERR_BUFFER_OVERFLOW.
 * @param   pLogger             Logger instance (NULL for default logger).
 * @param   pszBuf              The output buffer.
 * @param   cchBuf              The size of the output buffer. Must be greater
 *                              than 0.
 */
RTDECL(int) RTLogGetDestinations(PRTLOGGER pLogger, char *pszBuf, size_t cchBuf)
{
    bool        fNotFirst = false;
    int         rc        = VINF_SUCCESS;
    uint32_t    fDestFlags;
    unsigned    i;

    AssertReturn(cchBuf, VERR_INVALID_PARAMETER);
    *pszBuf = '\0';

    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return VINF_SUCCESS;
    }

    /*
     * Add the flags in the list.
     */
    fDestFlags = pLogger->fDestFlags;
    for (i = 2; i < RT_ELEMENTS(s_aLogDst); i++)
        if (s_aLogDst[i].fFlag & fDestFlags)
        {
            if (fNotFirst)
            {
                rc = RTStrCopyP(&pszBuf, &cchBuf, " ");
                if (RT_FAILURE(rc))
                    return rc;
            }
            rc = RTStrCopyP(&pszBuf, &cchBuf, s_aLogDst[i].pszInstr);
            if (RT_FAILURE(rc))
                return rc;
            fNotFirst = true;
        }

# ifdef IN_RING3
    /*
     * Add the filename.
     */
    if (fDestFlags & RTLOGDEST_FILE)
    {
        rc = RTStrCopyP(&pszBuf, &cchBuf, fNotFirst ? " file=" : "file=");
        if (RT_FAILURE(rc))
            return rc;
        rc = RTStrCopyP(&pszBuf, &cchBuf, pLogger->pInt->szFilename);
        if (RT_FAILURE(rc))
            return rc;
        fNotFirst = true;
    }

    if (fDestFlags & RTLOGDEST_FILE)
    {
        char szNum[32];
        if (pLogger->pInt->cHistory)
        {
            RTStrPrintf(szNum, sizeof(szNum), fNotFirst ? "history=%u" : " history=%u", pLogger->pInt->cHistory);
            rc = RTStrCopyP(&pszBuf, &cchBuf, szNum);
            if (RT_FAILURE(rc))
                return rc;
        }
        if (pLogger->pInt->cbHistoryFileMax != UINT64_MAX)
        {
            RTStrPrintf(szNum, sizeof(szNum), fNotFirst ? "histsize=%llu" : " histsize=%llu", pLogger->pInt->cbHistoryFileMax);
            rc = RTStrCopyP(&pszBuf, &cchBuf, szNum);
            if (RT_FAILURE(rc))
                return rc;
        }
        if (pLogger->pInt->cSecsHistoryTimeSlot != UINT32_MAX)
        {
            RTStrPrintf(szNum, sizeof(szNum), fNotFirst ? "histtime=%llu" : " histtime=%llu", pLogger->pInt->cSecsHistoryTimeSlot);
            rc = RTStrCopyP(&pszBuf, &cchBuf, szNum);
            if (RT_FAILURE(rc))
                return rc;
        }
    }
# endif /* IN_RING3 */

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTLogGetDestinations);

#endif /* !IN_RC */

/**
 * Flushes the specified logger.
 *
 * @param   pLogger     The logger instance to flush.
 *                      If NULL the default instance is used. The default instance
 *                      will not be initialized by this call.
 */
RTDECL(void) RTLogFlush(PRTLOGGER pLogger)
{
    /*
     * Resolve defaults.
     */
    if (!pLogger)
    {
#ifdef IN_RC
        pLogger = &g_Logger;
#else
        pLogger = g_pLogger;
#endif
        if (!pLogger)
            return;
    }

    /*
     * Any thing to flush?
     */
    if (pLogger->offScratch)
    {
#ifndef IN_RC
        /*
         * Acquire logger instance sem.
         */
        int rc = rtlogLock(pLogger);
        if (RT_FAILURE(rc))
            return;
#endif
        /*
         * Call worker.
         */
        rtlogFlush(pLogger);

#ifndef IN_RC
        /*
         * Release the semaphore.
         */
        rtlogUnlock(pLogger);
#endif
    }
}
RT_EXPORT_SYMBOL(RTLogFlush);


/**
 * Gets the default logger instance, creating it if necessary.
 *
 * @returns Pointer to default logger instance.
 * @returns NULL if no default logger instance available.
 */
RTDECL(PRTLOGGER)   RTLogDefaultInstance(void)
{
#ifdef IN_RC
    return &g_Logger;

#else /* !IN_RC */
# ifdef IN_RING0
    /*
     * Check per thread loggers first.
     */
    if (g_cPerThreadLoggers)
    {
        const RTNATIVETHREAD Self = RTThreadNativeSelf();
        int32_t i = RT_ELEMENTS(g_aPerThreadLoggers);
        while (i-- > 0)
            if (g_aPerThreadLoggers[i].NativeThread == Self)
                return g_aPerThreadLoggers[i].pLogger;
    }
# endif /* IN_RING0 */

    /*
     * If no per thread logger, use the default one.
     */
    if (!g_pLogger)
        g_pLogger = RTLogDefaultInit();
    return g_pLogger;
#endif /* !IN_RC */
}
RT_EXPORT_SYMBOL(RTLogDefaultInstance);


/**
 * Gets the default logger instance.
 *
 * @returns Pointer to default logger instance.
 * @returns NULL if no default logger instance available.
 */
RTDECL(PRTLOGGER)   RTLogGetDefaultInstance(void)
{
#ifdef IN_RC
    return &g_Logger;
#else
# ifdef IN_RING0
    /*
     * Check per thread loggers first.
     */
    if (g_cPerThreadLoggers)
    {
        const RTNATIVETHREAD Self = RTThreadNativeSelf();
        int32_t i = RT_ELEMENTS(g_aPerThreadLoggers);
        while (i-- > 0)
            if (g_aPerThreadLoggers[i].NativeThread == Self)
                return g_aPerThreadLoggers[i].pLogger;
    }
# endif /* IN_RING0 */

    return g_pLogger;
#endif
}
RT_EXPORT_SYMBOL(RTLogGetDefaultInstance);


#ifndef IN_RC
/**
 * Sets the default logger instance.
 *
 * @returns iprt status code.
 * @param   pLogger     The new default logger instance.
 */
RTDECL(PRTLOGGER) RTLogSetDefaultInstance(PRTLOGGER pLogger)
{
    return ASMAtomicXchgPtrT(&g_pLogger, pLogger, PRTLOGGER);
}
RT_EXPORT_SYMBOL(RTLogSetDefaultInstance);
#endif /* !IN_RC */


#ifdef IN_RING0
/**
 * Changes the default logger instance for the current thread.
 *
 * @returns IPRT status code.
 * @param   pLogger     The logger instance. Pass NULL for deregistration.
 * @param   uKey        Associated key for cleanup purposes. If pLogger is NULL,
 *                      all instances with this key will be deregistered. So in
 *                      order to only deregister the instance associated with the
 *                      current thread use 0.
 */
RTDECL(int) RTLogSetDefaultInstanceThread(PRTLOGGER pLogger, uintptr_t uKey)
{
    int             rc;
    RTNATIVETHREAD  Self = RTThreadNativeSelf();
    if (pLogger)
    {
        int32_t i;
        unsigned j;

        AssertReturn(pLogger->u32Magic == RTLOGGER_MAGIC, VERR_INVALID_MAGIC);

        /*
         * Iterate the table to see if there is already an entry for this thread.
         */
        i = RT_ELEMENTS(g_aPerThreadLoggers);
        while (i-- > 0)
            if (g_aPerThreadLoggers[i].NativeThread == Self)
            {
                ASMAtomicWritePtr((void * volatile *)&g_aPerThreadLoggers[i].uKey, (void *)uKey);
                g_aPerThreadLoggers[i].pLogger = pLogger;
                return VINF_SUCCESS;
            }

        /*
         * Allocate a new table entry.
         */
        i = ASMAtomicIncS32(&g_cPerThreadLoggers);
        if (i > (int32_t)RT_ELEMENTS(g_aPerThreadLoggers))
        {
            ASMAtomicDecS32(&g_cPerThreadLoggers);
            return VERR_BUFFER_OVERFLOW; /* horrible error code! */
        }

        for (j = 0; j < 10; j++)
        {
            i = RT_ELEMENTS(g_aPerThreadLoggers);
            while (i-- > 0)
            {
                AssertCompile(sizeof(RTNATIVETHREAD) == sizeof(void*));
                if (    g_aPerThreadLoggers[i].NativeThread == NIL_RTNATIVETHREAD
                    &&  ASMAtomicCmpXchgPtr((void * volatile *)&g_aPerThreadLoggers[i].NativeThread, (void *)Self, (void *)NIL_RTNATIVETHREAD))
                {
                    ASMAtomicWritePtr((void * volatile *)&g_aPerThreadLoggers[i].uKey, (void *)uKey);
                    ASMAtomicWritePtr(&g_aPerThreadLoggers[i].pLogger, pLogger);
                    return VINF_SUCCESS;
                }
            }
        }

        ASMAtomicDecS32(&g_cPerThreadLoggers);
        rc = VERR_INTERNAL_ERROR;
    }
    else
    {
        /*
         * Search the array for the current thread.
         */
        int32_t i = RT_ELEMENTS(g_aPerThreadLoggers);
        while (i-- > 0)
            if (    g_aPerThreadLoggers[i].NativeThread == Self
                ||  g_aPerThreadLoggers[i].uKey == uKey)
            {
                ASMAtomicWriteNullPtr((void * volatile *)&g_aPerThreadLoggers[i].uKey);
                ASMAtomicWriteNullPtr(&g_aPerThreadLoggers[i].pLogger);
                ASMAtomicWriteHandle(&g_aPerThreadLoggers[i].NativeThread, NIL_RTNATIVETHREAD);
                ASMAtomicDecS32(&g_cPerThreadLoggers);
            }

        rc = VINF_SUCCESS;
    }
    return rc;
}
RT_EXPORT_SYMBOL(RTLogSetDefaultInstanceThread);
#endif /* IN_RING0 */


/**
 * Write to a logger instance.
 *
 * @param   pLogger     Pointer to logger instance.
 * @param   pszFormat   Format string.
 * @param   args        Format arguments.
 */
RTDECL(void) RTLogLoggerV(PRTLOGGER pLogger, const char *pszFormat, va_list args)
{
    RTLogLoggerExV(pLogger, 0, ~0U, pszFormat, args);
}
RT_EXPORT_SYMBOL(RTLogLoggerV);


/**
 * Write to a logger instance.
 *
 * This function will check whether the instance, group and flags makes up a
 * logging kind which is currently enabled before writing anything to the log.
 *
 * @param   pLogger     Pointer to logger instance. If NULL the default logger instance will be attempted.
 * @param   fFlags      The logging flags.
 * @param   iGroup      The group.
 *                      The value ~0U is reserved for compatibility with RTLogLogger[V] and is
 *                      only for internal usage!
 * @param   pszFormat   Format string.
 * @param   args        Format arguments.
 */
RTDECL(void) RTLogLoggerExV(PRTLOGGER pLogger, unsigned fFlags, unsigned iGroup, const char *pszFormat, va_list args)
{
    int rc;

    /*
     * A NULL logger means default instance.
     */
    if (!pLogger)
    {
        pLogger = RTLogDefaultInstance();
        if (!pLogger)
            return;
    }

    /*
     * Validate and correct iGroup.
     */
    if (iGroup != ~0U && iGroup >= pLogger->cGroups)
        iGroup = 0;

    /*
     * If no output, then just skip it.
     */
    if (    (pLogger->fFlags & RTLOGFLAGS_DISABLED)
#ifndef IN_RC
        || !pLogger->fDestFlags
#endif
        || !pszFormat || !*pszFormat)
        return;
    if (    iGroup != ~0U
        &&  (pLogger->afGroups[iGroup] & (fFlags | RTLOGGRPFLAGS_ENABLED)) != (fFlags | RTLOGGRPFLAGS_ENABLED))
        return;

    /*
     * Acquire logger instance sem.
     */
    rc = rtlogLock(pLogger);
    if (RT_FAILURE(rc))
    {
#ifdef IN_RING0
        if (pLogger->fDestFlags & ~RTLOGDEST_FILE)
            rtR0LogLoggerExFallback(pLogger->fDestFlags, pLogger->fFlags, pszFormat, args);
#endif
        return;
    }

    /*
     * Check restrictions and call worker.
     */
#ifndef IN_RC
    if (RT_UNLIKELY(   (pLogger->fFlags & RTLOGFLAGS_RESTRICT_GROUPS)
                    && iGroup < pLogger->cGroups
                    && (pLogger->afGroups[iGroup] & RTLOGGRPFLAGS_RESTRICT)
                    && ++pLogger->pInt->pacEntriesPerGroup[iGroup] >= pLogger->pInt->cMaxEntriesPerGroup ))
    {
        uint32_t cEntries = pLogger->pInt->pacEntriesPerGroup[iGroup];
        if (cEntries > pLogger->pInt->cMaxEntriesPerGroup)
            pLogger->pInt->pacEntriesPerGroup[iGroup] = cEntries - 1;
        else
        {
            rtlogLoggerExVLocked(pLogger, fFlags, iGroup, pszFormat, args);
            if (   pLogger->pInt->papszGroups
                && pLogger->pInt->papszGroups[iGroup])
                rtlogLoggerExFLocked(pLogger, fFlags, iGroup, "%u messages from group %s (#%u), muting it.\n",
                                     cEntries, pLogger->pInt->papszGroups[iGroup], iGroup);
            else
                rtlogLoggerExFLocked(pLogger, fFlags, iGroup, "%u messages from group #%u, muting it.\n",
                                     cEntries, iGroup);
        }
    }
    else
#endif
        rtlogLoggerExVLocked(pLogger, fFlags, iGroup, pszFormat, args);

    /*
     * Release the semaphore.
     */
    rtlogUnlock(pLogger);
}
RT_EXPORT_SYMBOL(RTLogLoggerExV);


#ifdef IN_RING0
/**
 * For rtR0LogLoggerExFallbackOutput and rtR0LogLoggerExFallbackFlush.
 */
typedef struct RTR0LOGLOGGERFALLBACK
{
    /** The current scratch buffer offset. */
    uint32_t offScratch;
    /** The destination flags. */
    uint32_t fDestFlags;
    /** The scratch buffer. */
    char achScratch[80];
} RTR0LOGLOGGERFALLBACK;
/** Pointer to RTR0LOGLOGGERFALLBACK which is used by
 * rtR0LogLoggerExFallbackOutput. */
typedef RTR0LOGLOGGERFALLBACK *PRTR0LOGLOGGERFALLBACK;


/**
 * Flushes the fallback buffer.
 *
 * @param   pThis       The scratch buffer.
 */
static void rtR0LogLoggerExFallbackFlush(PRTR0LOGLOGGERFALLBACK pThis)
{
    if (!pThis->offScratch)
        return;

    if (pThis->fDestFlags & RTLOGDEST_USER)
        RTLogWriteUser(pThis->achScratch, pThis->offScratch);

    if (pThis->fDestFlags & RTLOGDEST_DEBUGGER)
        RTLogWriteDebugger(pThis->achScratch, pThis->offScratch);

    if (pThis->fDestFlags & RTLOGDEST_STDOUT)
        RTLogWriteStdOut(pThis->achScratch, pThis->offScratch);

    if (pThis->fDestFlags & RTLOGDEST_STDERR)
        RTLogWriteStdErr(pThis->achScratch, pThis->offScratch);

# ifndef LOG_NO_COM
    if (pThis->fDestFlags & RTLOGDEST_COM)
        RTLogWriteCom(pThis->achScratch, pThis->offScratch);
# endif

    /* empty the buffer. */
    pThis->offScratch = 0;
}


/**
 * Callback for RTLogFormatV used by rtR0LogLoggerExFallback.
 * See PFNLOGOUTPUT() for details.
 */
static DECLCALLBACK(size_t) rtR0LogLoggerExFallbackOutput(void *pv, const char *pachChars, size_t cbChars)
{
    PRTR0LOGLOGGERFALLBACK pThis = (PRTR0LOGLOGGERFALLBACK)pv;
    if (cbChars)
    {
        size_t cbRet = 0;
        for (;;)
        {
            /* how much */
            uint32_t cb = sizeof(pThis->achScratch) - pThis->offScratch - 1; /* minus 1 - for the string terminator. */
            if (cb > cbChars)
                cb = (uint32_t)cbChars;

            /* copy */
            memcpy(&pThis->achScratch[pThis->offScratch], pachChars, cb);

            /* advance */
            pThis->offScratch += cb;
            cbRet += cb;
            cbChars -= cb;

            /* done? */
            if (cbChars <= 0)
                return cbRet;

            pachChars += cb;

            /* flush */
            pThis->achScratch[pThis->offScratch] = '\0';
            rtR0LogLoggerExFallbackFlush(pThis);
        }

        /* won't ever get here! */
    }
    else
    {
        /*
         * Termination call, flush the log.
         */
        pThis->achScratch[pThis->offScratch] = '\0';
        rtR0LogLoggerExFallbackFlush(pThis);
        return 0;
    }
}


/**
 * Ring-0 fallback for cases where we're unable to grab the lock.
 *
 * This will happen when we're at a too high IRQL on Windows for instance and
 * needs to be dealt with or we'll drop a lot of log output. This fallback will
 * only output to some of the log destinations as a few of them may be doing
 * dangerous things. We won't be doing any prefixing here either, at least not
 * for the present, because it's too much hassle.
 *
 * @param   fDestFlags  The destination flags.
 * @param   fFlags      The logger flags.
 * @param   pszFormat   The format string.
 * @param   va          The format arguments.
 */
static void rtR0LogLoggerExFallback(uint32_t fDestFlags, uint32_t fFlags, const char *pszFormat, va_list va)
{
    RTR0LOGLOGGERFALLBACK This;
    This.fDestFlags = fDestFlags;

    /* fallback indicator. */
    This.offScratch = 2;
    This.achScratch[0] = '[';
    This.achScratch[1] = 'F';

    /* selected prefixes */
    if (fFlags & RTLOGFLAGS_PREFIX_PID)
    {
        RTPROCESS Process = RTProcSelf();
        This.achScratch[This.offScratch++] = ' ';
        This.offScratch += RTStrFormatNumber(&This.achScratch[This.offScratch], Process, 16, sizeof(RTPROCESS) * 2, 0, RTSTR_F_ZEROPAD);
    }
    if (fFlags & RTLOGFLAGS_PREFIX_TID)
    {
        RTNATIVETHREAD Thread = RTThreadNativeSelf();
        This.achScratch[This.offScratch++] = ' ';
        This.offScratch += RTStrFormatNumber(&This.achScratch[This.offScratch], Thread, 16, sizeof(RTNATIVETHREAD) * 2, 0, RTSTR_F_ZEROPAD);
    }

    This.achScratch[This.offScratch++] = ']';
    This.achScratch[This.offScratch++] = ' ';

    RTLogFormatV(rtR0LogLoggerExFallbackOutput, &This, pszFormat, va);
}
#endif /* IN_RING0 */


/**
 * vprintf like function for writing to the default log.
 *
 * @param   pszFormat   Printf like format string.
 * @param   args        Optional arguments as specified in pszFormat.
 *
 * @remark The API doesn't support formatting of floating point numbers at the moment.
 */
RTDECL(void) RTLogPrintfV(const char *pszFormat, va_list args)
{
    RTLogLoggerV(NULL, pszFormat, args);
}
RT_EXPORT_SYMBOL(RTLogPrintfV);

#ifdef IN_RING3

/**
 * Opens/creates the log file.
 *
 * @param   pLogger         The logger instance to update. NULL is not allowed!
 * @param   pszErrorMsg     A buffer which is filled with an error message if
 *                          something fails.  May be NULL.
 * @param   cchErrorMsg     The size of the error message buffer.
 */
static int rtlogFileOpen(PRTLOGGER pLogger, char *pszErrorMsg, size_t cchErrorMsg)
{
    uint32_t fOpen = RTFILE_O_WRITE | RTFILE_O_DENY_WRITE;
    if (pLogger->fFlags & RTLOGFLAGS_APPEND)
        fOpen |= RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND;
    else
        fOpen |= RTFILE_O_CREATE_REPLACE;
    if (pLogger->fFlags & RTLOGFLAGS_WRITE_THROUGH)
        fOpen |= RTFILE_O_WRITE_THROUGH;

    int rc = RTFileOpen(&pLogger->pInt->hFile, pLogger->pInt->szFilename, fOpen);
    if (RT_FAILURE(rc))
    {
        pLogger->pInt->hFile = NIL_RTFILE;
        if (pszErrorMsg)
            RTStrPrintf(pszErrorMsg, cchErrorMsg, N_("could not open file '%s' (fOpen=%#x)"), pLogger->pInt->szFilename, fOpen);
    }
    else
    {
        rc = RTFileGetSize(pLogger->pInt->hFile, &pLogger->pInt->cbHistoryFileWritten);
        if (RT_FAILURE(rc))
        {
            /* Don't complain if this fails, assume the file is empty. */
            pLogger->pInt->cbHistoryFileWritten = 0;
            rc = VINF_SUCCESS;
        }
    }
    return rc;
}


/**
 * Closes, rotates and opens the log files if necessary.
 *
 * Used by the rtlogFlush() function as well as RTLogCreateExV.
 *
 * @param   pLogger     The logger instance to update. NULL is not allowed!
 * @param   uTimeSlit   Current time slot (for tikme based rotation).
 * @param   fFirst      Flag whether this is the beginning of logging, i.e.
 *                      called from RTLogCreateExV.  Prevents pfnPhase from
 *                      being called.
 */
static void rtlogRotate(PRTLOGGER pLogger, uint32_t uTimeSlot, bool fFirst)
{
    /* Suppress rotating empty log files simply because the time elapsed. */
    if (RT_UNLIKELY(!pLogger->pInt->cbHistoryFileWritten))
        pLogger->pInt->uHistoryTimeSlotStart = uTimeSlot;

    /* Check rotation condition: file still small enough and not too old? */
    if (RT_LIKELY(   pLogger->pInt->cbHistoryFileWritten < pLogger->pInt->cbHistoryFileMax
                  && uTimeSlot == pLogger->pInt->uHistoryTimeSlotStart))
        return;

    /*
     * Save "disabled" log flag and make sure logging is disabled.
     * The logging in the functions called during log file history
     * rotation would cause severe trouble otherwise.
     */
    uint32_t const fSavedFlags = pLogger->fFlags;
    pLogger->fFlags |= RTLOGFLAGS_DISABLED;

    /*
     * Disable log rotation temporarily, otherwise with extreme settings and
     * chatty phase logging we could run into endless rotation.
     */
    uint32_t const cSavedHistory = pLogger->pInt->cHistory;
    pLogger->pInt->cHistory = 0;

    /*
     * Close the old log file.
     */
    if (pLogger->pInt->hFile != NIL_RTFILE)
    {
        /* Use the callback to generate some final log contents, but only if
         * this is a rotation with a fully set up logger. Leave the other case
         * to the RTLogCreateExV function. */
        if (pLogger->pInt->pfnPhase && !fFirst)
        {
            uint32_t fODestFlags = pLogger->fDestFlags;
            pLogger->fDestFlags &= RTLOGDEST_FILE;
            pLogger->pInt->pfnPhase(pLogger, RTLOGPHASE_PREROTATE, rtlogPhaseMsgLocked);
            pLogger->fDestFlags = fODestFlags;
        }
        RTFileClose(pLogger->pInt->hFile);
        pLogger->pInt->hFile = NIL_RTFILE;
    }

    if (cSavedHistory)
    {
        /*
         * Rotate the log files.
         */
        for (uint32_t i = cSavedHistory - 1; i + 1 > 0; i--)
        {
            char szOldName[sizeof(pLogger->pInt->szFilename) + 32];
            if (i > 0)
                RTStrPrintf(szOldName, sizeof(szOldName), "%s.%u", pLogger->pInt->szFilename, i);
            else
                RTStrCopy(szOldName, sizeof(szOldName), pLogger->pInt->szFilename);

            char szNewName[sizeof(pLogger->pInt->szFilename) + 32];
            RTStrPrintf(szNewName, sizeof(szNewName), "%s.%u", pLogger->pInt->szFilename, i + 1);
            if (   RTFileRename(szOldName, szNewName, RTFILEMOVE_FLAGS_REPLACE)
                == VERR_FILE_NOT_FOUND)
                RTFileDelete(szNewName);
        }

        /*
         * Delete excess log files.
         */
        for (uint32_t i = cSavedHistory + 1; ; i++)
        {
            char szExcessName[sizeof(pLogger->pInt->szFilename) + 32];
            RTStrPrintf(szExcessName, sizeof(szExcessName), "%s.%u", pLogger->pInt->szFilename, i);
            int rc = RTFileDelete(szExcessName);
            if (RT_FAILURE(rc))
                break;
        }
    }

    /*
     * Update logger state and create new log file.
     */
    pLogger->pInt->cbHistoryFileWritten = 0;
    pLogger->pInt->uHistoryTimeSlotStart = uTimeSlot;
    rtlogFileOpen(pLogger, NULL, 0);

    /*
     * Use the callback to generate some initial log contents, but only if this
     * is a rotation with a fully set up logger.  Leave the other case to the
     * RTLogCreateExV function.
     */
    if (pLogger->pInt->pfnPhase && !fFirst)
    {
        uint32_t const fSavedDestFlags = pLogger->fDestFlags;
        pLogger->fDestFlags &= RTLOGDEST_FILE;
        pLogger->pInt->pfnPhase(pLogger, RTLOGPHASE_POSTROTATE, rtlogPhaseMsgLocked);
        pLogger->fDestFlags = fSavedDestFlags;
    }

    /* Restore saved values. */
    pLogger->pInt->cHistory = cSavedHistory;
    pLogger->fFlags          = fSavedFlags;
}

#endif /* IN_RING3 */

/**
 * Writes the buffer to the given log device without checking for buffered
 * data or anything.
 * Used by the RTLogFlush() function.
 *
 * @param   pLogger     The logger instance to write to. NULL is not allowed!
 */
static void rtlogFlush(PRTLOGGER pLogger)
{
    if (pLogger->offScratch == 0)
        return; /* nothing to flush. */

#ifndef IN_RC
    if (pLogger->fDestFlags & RTLOGDEST_USER)
        RTLogWriteUser(pLogger->achScratch, pLogger->offScratch);

    if (pLogger->fDestFlags & RTLOGDEST_DEBUGGER)
        RTLogWriteDebugger(pLogger->achScratch, pLogger->offScratch);

# ifdef IN_RING3
    if (pLogger->fDestFlags & RTLOGDEST_FILE)
    {
        if (pLogger->pInt->hFile != NIL_RTFILE)
        {
            RTFileWrite(pLogger->pInt->hFile, pLogger->achScratch, pLogger->offScratch, NULL);
            if (pLogger->fFlags & RTLOGFLAGS_FLUSH)
                RTFileFlush(pLogger->pInt->hFile);
        }
        if (pLogger->pInt->cHistory)
            pLogger->pInt->cbHistoryFileWritten += pLogger->offScratch;
    }
# endif

    if (pLogger->fDestFlags & RTLOGDEST_STDOUT)
        RTLogWriteStdOut(pLogger->achScratch, pLogger->offScratch);

    if (pLogger->fDestFlags & RTLOGDEST_STDERR)
        RTLogWriteStdErr(pLogger->achScratch, pLogger->offScratch);

# if (defined(IN_RING0) || defined(IN_RC)) && !defined(LOG_NO_COM)
    if (pLogger->fDestFlags & RTLOGDEST_COM)
        RTLogWriteCom(pLogger->achScratch, pLogger->offScratch);
# endif
#endif /* !IN_RC */

#ifdef IN_RC
    if (pLogger->pfnFlush)
        pLogger->pfnFlush(pLogger);
#else
    if (pLogger->pInt->pfnFlush)
        pLogger->pInt->pfnFlush(pLogger);
#endif

    /* empty the buffer. */
    pLogger->offScratch = 0;

#ifdef IN_RING3
    /*
     * Rotate the log file if configured.  Must be done after everything is
     * flushed, since this will also use logging/flushing to write the header
     * and footer messages.
     */
    if (   (pLogger->fDestFlags & RTLOGDEST_FILE)
        && pLogger->pInt->cHistory)
        rtlogRotate(pLogger, RTTimeProgramSecTS() / pLogger->pInt->cSecsHistoryTimeSlot, false /* fFirst */);
#endif
}


/**
 * Callback for RTLogFormatV which writes to the com port.
 * See PFNLOGOUTPUT() for details.
 */
static DECLCALLBACK(size_t) rtLogOutput(void *pv, const char *pachChars, size_t cbChars)
{
    PRTLOGGER pLogger = (PRTLOGGER)pv;
    if (cbChars)
    {
        size_t cbRet = 0;
        for (;;)
        {
#if defined(DEBUG) && defined(IN_RING3)
            /* sanity */
            if (pLogger->offScratch >= sizeof(pLogger->achScratch))
            {
                fprintf(stderr, "pLogger->offScratch >= sizeof(pLogger->achScratch) (%#x >= %#x)\n",
                        pLogger->offScratch, (unsigned)sizeof(pLogger->achScratch));
                AssertBreakpoint(); AssertBreakpoint();
            }
#endif

            /* how much */
            size_t cb = sizeof(pLogger->achScratch) - pLogger->offScratch - 1;
            if (cb > cbChars)
                cb = cbChars;

            /* copy */
            memcpy(&pLogger->achScratch[pLogger->offScratch], pachChars, cb);

            /* advance */
            pLogger->offScratch += (uint32_t)cb;
            cbRet += cb;
            cbChars -= cb;

            /* done? */
            if (cbChars <= 0)
                return cbRet;

            pachChars += cb;

            /* flush */
            rtlogFlush(pLogger);
        }

        /* won't ever get here! */
    }
    else
    {
        /*
         * Termination call.
         * There's always space for a terminator, and it's not counted.
         */
        pLogger->achScratch[pLogger->offScratch] = '\0';
        return 0;
    }
}



/**
 * Callback for RTLogFormatV which writes to the logger instance.
 * This version supports prefixes.
 *
 * See PFNLOGOUTPUT() for details.
 */
static DECLCALLBACK(size_t) rtLogOutputPrefixed(void *pv, const char *pachChars, size_t cbChars)
{
    PRTLOGOUTPUTPREFIXEDARGS    pArgs = (PRTLOGOUTPUTPREFIXEDARGS)pv;
    PRTLOGGER                   pLogger = pArgs->pLogger;
    if (cbChars)
    {
        size_t cbRet = 0;
        for (;;)
        {
            size_t      cb = sizeof(pLogger->achScratch) - pLogger->offScratch - 1;
            const char *pszNewLine;
            char       *psz;
#ifdef IN_RC
            bool       *pfPendingPrefix = &pLogger->fPendingPrefix;
#else
            bool       *pfPendingPrefix = &pLogger->pInt->fPendingPrefix;
#endif

            /*
             * Pending prefix?
             */
            if (*pfPendingPrefix)
            {
                *pfPendingPrefix = false;

#if defined(DEBUG) && defined(IN_RING3)
                /* sanity */
                if (pLogger->offScratch >= sizeof(pLogger->achScratch))
                {
                    fprintf(stderr, "pLogger->offScratch >= sizeof(pLogger->achScratch) (%#x >= %#x)\n",
                            pLogger->offScratch, (unsigned)sizeof(pLogger->achScratch));
                    AssertBreakpoint(); AssertBreakpoint();
                }
#endif

                /*
                 * Flush the buffer if there isn't enough room for the maximum prefix config.
                 * Max is 256, add a couple of extra bytes.
                 */
                if (cb < 256 + 16)
                {
                    rtlogFlush(pLogger);
                    cb = sizeof(pLogger->achScratch) - pLogger->offScratch - 1;
                }

                /*
                 * Write the prefixes.
                 * psz is pointing to the current position.
                 */
                psz = &pLogger->achScratch[pLogger->offScratch];
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_TS)
                {
                    uint64_t     u64    = RTTimeNanoTS();
                    int          iBase  = 16;
                    unsigned int fFlags = RTSTR_F_ZEROPAD;
                    if (pLogger->fFlags & RTLOGFLAGS_DECIMAL_TS)
                    {
                        iBase = 10;
                        fFlags = 0;
                    }
                    if (pLogger->fFlags & RTLOGFLAGS_REL_TS)
                    {
                        static volatile uint64_t s_u64LastTs;
                        uint64_t        u64DiffTs = u64 - s_u64LastTs;
                        s_u64LastTs = u64;
                        /* We could have been preempted just before reading of s_u64LastTs by
                         * another thread which wrote s_u64LastTs. In that case the difference
                         * is negative which we simply ignore. */
                        u64         = (int64_t)u64DiffTs < 0 ? 0 : u64DiffTs;
                    }
                    /* 1E15 nanoseconds = 11 days */
                    psz += RTStrFormatNumber(psz, u64, iBase, 16, 0, fFlags);                       /* +17 */
                    *psz++ = ' ';
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_TSC)
                {
#if defined(RT_ARCH_AMD64) || defined(RT_ARCH_X86)
                    uint64_t     u64    = ASMReadTSC();
#else
                    uint64_t     u64    = RTTimeNanoTS();
#endif
                    int          iBase  = 16;
                    unsigned int fFlags = RTSTR_F_ZEROPAD;
                    if (pLogger->fFlags & RTLOGFLAGS_DECIMAL_TS)
                    {
                        iBase = 10;
                        fFlags = 0;
                    }
                    if (pLogger->fFlags & RTLOGFLAGS_REL_TS)
                    {
                        static volatile uint64_t s_u64LastTsc;
                        int64_t        i64DiffTsc = u64 - s_u64LastTsc;
                        s_u64LastTsc = u64;
                        /* We could have been preempted just before reading of s_u64LastTsc by
                         * another thread which wrote s_u64LastTsc. In that case the difference
                         * is negative which we simply ignore. */
                        u64          = i64DiffTsc < 0 ? 0 : i64DiffTsc;
                    }
                    /* 1E15 ticks at 4GHz = 69 hours */
                    psz += RTStrFormatNumber(psz, u64, iBase, 16, 0, fFlags);                       /* +17 */
                    *psz++ = ' ';
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_MS_PROG)
                {
#if defined(IN_RING3) || defined(IN_RC)
                    uint64_t u64 = RTTimeProgramMilliTS();
#else
                    uint64_t u64 = 0;
#endif
                    /* 1E8 milliseconds = 27 hours */
                    psz += RTStrFormatNumber(psz, u64, 10, 9, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_TIME)
                {
#if defined(IN_RING3) || defined(IN_RING0)
                    RTTIMESPEC TimeSpec;
                    RTTIME Time;
                    RTTimeExplode(&Time, RTTimeNow(&TimeSpec));
                    psz += RTStrFormatNumber(psz, Time.u8Hour, 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ':';
                    psz += RTStrFormatNumber(psz, Time.u8Minute, 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ':';
                    psz += RTStrFormatNumber(psz, Time.u8Second, 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = '.';
                    psz += RTStrFormatNumber(psz, Time.u32Nanosecond / 1000000, 10, 3, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                                   /* +17 (3+1+3+1+3+1+4+1) */
#else
                    memset(psz, ' ', 13);
                    psz += 13;
#endif
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_TIME_PROG)
                {
#if defined(IN_RING3) || defined(IN_RC)
                    uint64_t u64 = RTTimeProgramMilliTS();
                    psz += RTStrFormatNumber(psz, (uint32_t)(u64 / (60 * 60 * 1000)), 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ':';
                    uint32_t u32 = (uint32_t)(u64 % (60 * 60 * 1000));
                    psz += RTStrFormatNumber(psz, u32 / (60 * 1000), 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ':';
                    u32 %= 60 * 1000;
                    psz += RTStrFormatNumber(psz, u32 / 1000, 10, 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = '.';
                    psz += RTStrFormatNumber(psz, u32 % 1000, 10, 3, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                               /* +20 (9+1+2+1+2+1+3+1) */
#else
                    memset(psz, ' ', 13);
                    psz += 13;
#endif
                }
# if 0
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_DATETIME)
                {
                    char szDate[32];
                    RTTIMESPEC Time;
                    RTTimeSpecToString(RTTimeNow(&Time), szDate, sizeof(szDate));
                    size_t cch = strlen(szDate);
                    memcpy(psz, szDate, cch);
                    psz += cch;
                    *psz++ = ' ';                                                               /* +32 */
                }
# endif
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_PID)
                {
#ifndef IN_RC
                    RTPROCESS Process = RTProcSelf();
#else
                    RTPROCESS Process = NIL_RTPROCESS;
#endif
                    psz += RTStrFormatNumber(psz, Process, 16, sizeof(RTPROCESS) * 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                               /* +9 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_TID)
                {
#ifndef IN_RC
                    RTNATIVETHREAD Thread = RTThreadNativeSelf();
#else
                    RTNATIVETHREAD Thread = NIL_RTNATIVETHREAD;
#endif
                    psz += RTStrFormatNumber(psz, Thread, 16, sizeof(RTNATIVETHREAD) * 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                               /* +17 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_THREAD)
                {
#ifdef IN_RING3
                    const char *pszName = RTThreadSelfName();
#elif defined IN_RC
                    const char *pszName = "EMT-RC";
#else
                    const char *pszName = "R0";
#endif
                    size_t cch = 0;
                    if (pszName)
                    {
                        cch = strlen(pszName);
                        cch = RT_MIN(cch, 16);
                        memcpy(psz, pszName, cch);
                        psz += cch;
                    }
                    do
                        *psz++ = ' ';
                    while (cch++ < 8);                                                          /* +17  */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_CPUID)
                {
#if defined(RT_ARCH_AMD64) || defined(RT_ARCH_X86)
                    const uint8_t idCpu = ASMGetApicId();
#else
                    const RTCPUID idCpu = RTMpCpuId();
#endif
                    psz += RTStrFormatNumber(psz, idCpu, 16, sizeof(idCpu) * 2, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                               /* +17 */
                }
#ifndef IN_RC
                if (    (pLogger->fFlags & RTLOGFLAGS_PREFIX_CUSTOM)
                    &&  pLogger->pInt->pfnPrefix)
                {
                    psz += pLogger->pInt->pfnPrefix(pLogger, psz, 31, pLogger->pInt->pvPrefixUserArg);
                    *psz++ = ' ';                                                               /* +32 */
                }
#endif
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_LOCK_COUNTS)
                {
#ifdef IN_RING3 /** @todo implement these counters in ring-0 too? */
                    RTTHREAD Thread = RTThreadSelf();
                    if (Thread != NIL_RTTHREAD)
                    {
                        uint32_t cReadLocks  = RTLockValidatorReadLockGetCount(Thread);
                        uint32_t cWriteLocks = RTLockValidatorWriteLockGetCount(Thread) - g_cLoggerLockCount;
                        cReadLocks  = RT_MIN(0xfff, cReadLocks);
                        cWriteLocks = RT_MIN(0xfff, cWriteLocks);
                        psz += RTStrFormatNumber(psz, cReadLocks,  16, 1, 0, RTSTR_F_ZEROPAD);
                        *psz++ = '/';
                        psz += RTStrFormatNumber(psz, cWriteLocks, 16, 1, 0, RTSTR_F_ZEROPAD);
                    }
                    else
#endif
                    {
                        *psz++ = '?';
                        *psz++ = '/';
                        *psz++ = '?';
                    }
                    *psz++ = ' ';                                                               /* +8 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_FLAG_NO)
                {
                    psz += RTStrFormatNumber(psz, pArgs->fFlags, 16, 8, 0, RTSTR_F_ZEROPAD);
                    *psz++ = ' ';                                                               /* +9 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_FLAG)
                {
#ifdef IN_RING3
                    const char *pszGroup = pArgs->iGroup != ~0U ? pLogger->pInt->papszGroups[pArgs->iGroup] : NULL;
#else
                    const char *pszGroup = NULL;
#endif
                    size_t cch = 0;
                    if (pszGroup)
                    {
                        cch = strlen(pszGroup);
                        cch = RT_MIN(cch, 16);
                        memcpy(psz, pszGroup, cch);
                        psz += cch;
                    }
                    do
                        *psz++ = ' ';
                    while (cch++ < 8);                                                          /* +17 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_GROUP_NO)
                {
                    if (pArgs->iGroup != ~0U)
                    {
                        psz += RTStrFormatNumber(psz, pArgs->iGroup, 16, 3, 0, RTSTR_F_ZEROPAD);
                        *psz++ = ' ';
                    }
                    else
                    {
                        memcpy(psz, "-1  ", sizeof("-1  ") - 1);
                        psz += sizeof("-1  ") - 1;
                    }                                                                           /* +9 */
                }
                if (pLogger->fFlags & RTLOGFLAGS_PREFIX_GROUP)
                {
                    const unsigned fGrp = pLogger->afGroups[pArgs->iGroup != ~0U ? pArgs->iGroup : 0];
                    const char *pszGroup;
                    size_t cch;
                    switch (pArgs->fFlags & fGrp)
                    {
                        case 0:                         pszGroup = "--------";  cch = sizeof("--------") - 1; break;
                        case RTLOGGRPFLAGS_ENABLED:     pszGroup = "enabled" ;  cch = sizeof("enabled" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_1:     pszGroup = "level 1" ;  cch = sizeof("level 1" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_2:     pszGroup = "level 2" ;  cch = sizeof("level 2" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_3:     pszGroup = "level 3" ;  cch = sizeof("level 3" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_4:     pszGroup = "level 4" ;  cch = sizeof("level 4" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_5:     pszGroup = "level 5" ;  cch = sizeof("level 5" ) - 1; break;
                        case RTLOGGRPFLAGS_LEVEL_6:     pszGroup = "level 6" ;  cch = sizeof("level 6" ) - 1; break;
                        case RTLOGGRPFLAGS_FLOW:        pszGroup = "flow"    ;  cch = sizeof("flow"    ) - 1; break;

                        /* personal groups */
                        case RTLOGGRPFLAGS_LELIK:       pszGroup = "lelik"   ;  cch = sizeof("lelik"   ) - 1; break;
                        case RTLOGGRPFLAGS_MICHAEL:     pszGroup = "Michael" ;  cch = sizeof("Michael" ) - 1; break;
                        case RTLOGGRPFLAGS_SUNLOVER:    pszGroup = "sunlover";  cch = sizeof("sunlover") - 1; break;
                        case RTLOGGRPFLAGS_ACHIM:       pszGroup = "Achim"   ;  cch = sizeof("Achim"   ) - 1; break;
                        case RTLOGGRPFLAGS_SANDER:      pszGroup = "Sander"  ;  cch = sizeof("Sander"  ) - 1; break;
                        case RTLOGGRPFLAGS_KLAUS:       pszGroup = "Klaus"   ;  cch = sizeof("Klaus"   ) - 1; break;
                        case RTLOGGRPFLAGS_FRANK:       pszGroup = "Frank"   ;  cch = sizeof("Frank"   ) - 1; break;
                        case RTLOGGRPFLAGS_BIRD:        pszGroup = "bird"    ;  cch = sizeof("bird"    ) - 1; break;
                        case RTLOGGRPFLAGS_NONAME:      pszGroup = "noname"  ;  cch = sizeof("noname"  ) - 1; break;
                        default:                        pszGroup = "????????";  cch = sizeof("????????") - 1; break;
                    }
                    if (pszGroup)
                    {
                        cch = RT_MIN(cch, 16);
                        memcpy(psz, pszGroup, cch);
                        psz += cch;
                    }
                    do
                        *psz++ = ' ';
                    while (cch++ < 8);                                                          /* +17 */
                }

                /*
                 * Done, figure what we've used and advance the buffer and free size.
                 */
                cb = psz - &pLogger->achScratch[pLogger->offScratch];
                AssertMsg(cb <= 223, ("%#zx (%zd) - fFlags=%#x\n", cb, cb, pLogger->fFlags));
                pLogger->offScratch += (uint32_t)cb;
                cb = sizeof(pLogger->achScratch) - pLogger->offScratch - 1;
            }
            else if (cb <= 0)
            {
                rtlogFlush(pLogger);
                cb = sizeof(pLogger->achScratch) - pLogger->offScratch - 1;
            }

#if defined(DEBUG) && defined(IN_RING3)
            /* sanity */
            if (pLogger->offScratch >= sizeof(pLogger->achScratch))
            {
                fprintf(stderr, "pLogger->offScratch >= sizeof(pLogger->achScratch) (%#x >= %#x)\n",
                        pLogger->offScratch, (unsigned)sizeof(pLogger->achScratch));
                AssertBreakpoint(); AssertBreakpoint();
            }
#endif

            /* how much */
            if (cb > cbChars)
                cb = cbChars;

            /* have newline? */
            pszNewLine = (const char *)memchr(pachChars, '\n', cb);
            if (pszNewLine)
            {
                if (pLogger->fFlags & RTLOGFLAGS_USECRLF)
                    cb = pszNewLine - pachChars;
                else
                {
                    cb = pszNewLine - pachChars + 1;
                    *pfPendingPrefix = true;
                }
            }

            /* copy */
            memcpy(&pLogger->achScratch[pLogger->offScratch], pachChars, cb);

            /* advance */
            pLogger->offScratch += (uint32_t)cb;
            cbRet += cb;
            cbChars -= cb;

            if (    pszNewLine
                &&  (pLogger->fFlags & RTLOGFLAGS_USECRLF)
                &&  pLogger->offScratch + 2 < sizeof(pLogger->achScratch))
            {
                memcpy(&pLogger->achScratch[pLogger->offScratch], "\r\n", 2);
                pLogger->offScratch += 2;
                cbRet++;
                cbChars--;
                cb++;
                *pfPendingPrefix = true;
            }

            /* done? */
            if (cbChars <= 0)
                return cbRet;
            pachChars += cb;
        }

        /* won't ever get here! */
    }
    else
    {
        /*
         * Termination call.
         * There's always space for a terminator, and it's not counted.
         */
        pLogger->achScratch[pLogger->offScratch] = '\0';
        return 0;
    }
}


/**
 * Write to a logger instance (worker function).
 *
 * This function will check whether the instance, group and flags makes up a
 * logging kind which is currently enabled before writing anything to the log.
 *
 * @param   pLogger     Pointer to logger instance. Must be non-NULL.
 * @param   fFlags      The logging flags.
 * @param   iGroup      The group.
 *                      The value ~0U is reserved for compatibility with RTLogLogger[V] and is
 *                      only for internal usage!
 * @param   pszFormat   Format string.
 * @param   args        Format arguments.
 */
static void rtlogLoggerExVLocked(PRTLOGGER pLogger, unsigned fFlags, unsigned iGroup, const char *pszFormat, va_list args)
{
    /*
     * Format the message and perhaps flush it.
     */
    if (pLogger->fFlags & (RTLOGFLAGS_PREFIX_MASK | RTLOGFLAGS_USECRLF))
    {
        RTLOGOUTPUTPREFIXEDARGS OutputArgs;
        OutputArgs.pLogger = pLogger;
        OutputArgs.iGroup  = iGroup;
        OutputArgs.fFlags  = fFlags;
        RTLogFormatV(rtLogOutputPrefixed, &OutputArgs, pszFormat, args);
    }
    else
        RTLogFormatV(rtLogOutput, pLogger, pszFormat, args);
    if (    !(pLogger->fFlags & RTLOGFLAGS_BUFFERED)
        &&  pLogger->offScratch)
        rtlogFlush(pLogger);
}


/**
 * For calling rtlogLoggerExVLocked.
 *
 * @param   pLogger     The logger.
 * @param   fFlags      The logging flags.
 * @param   iGroup      The group.
 *                      The value ~0U is reserved for compatibility with RTLogLogger[V] and is
 *                      only for internal usage!
 * @param   pszFormat   Format string.
 * @param   ...         Format arguments.
 */
static void rtlogLoggerExFLocked(PRTLOGGER pLogger, unsigned fFlags, unsigned iGroup, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    rtlogLoggerExVLocked(pLogger, fFlags, iGroup, pszFormat, va);
    va_end(va);
}

