 * Copyright (C) 2006-2019 Oracle Corporation
#include <iprt/utf16.h>
/**
 * Helper for cooking up a path string for rtDirOpenRelativeOrHandle.
 *
 * @returns IPRT status code.
 * @param   pszDst              The destination buffer.
 * @param   cbDst               The size of the destination buffer.
 * @param   pThis               The directory this is relative to.
 * @param   pNtPath             The NT path with a possibly relative path.
 * @param   fRelative           Whether @a pNtPath is relative or not.
 * @param   pszPath             The input path.
 */
static int rtDirRelJoinPathForDirOpen(char *pszDst, size_t cbDst, PRTDIRINTERNAL pThis,
                                      PUNICODE_STRING pNtPath, bool fRelative, const char *pszPath)
{
    int rc;
    if (fRelative)
    {
        size_t cchRel = 0;
        rc = RTUtf16CalcUtf8LenEx(pNtPath->Buffer, pNtPath->Length / sizeof(RTUTF16), &cchRel);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            if (pThis->cchPath + cchRel < cbDst)
            {
                size_t cchBase = pThis->cchPath;
                memcpy(pszDst, pThis->pszPath, cchBase);
                pszDst += cchBase;
                cbDst  -= cchBase;
                rc = RTUtf16ToUtf8Ex(pNtPath->Buffer, pNtPath->Length / sizeof(RTUTF16), &pszDst, cbDst, NULL);
            }
            else
                rc = VERR_FILENAME_TOO_LONG;
        }
    }
    else
    {
        /** @todo would be better to convert pNtName to DOS/WIN path here,
         *        as it is absolute and doesn't need stuff resolved. */
        rc = RTPathJoin(pszDst, cbDst, pThis->pszPath, pszPath);
    }
    return rc;
}
        char szAbsDirAndFilter[RTPATH_MAX];
        rc = rtDirRelJoinPathForDirOpen(szAbsDirAndFilter, sizeof(szAbsDirAndFilter), pThis,
                                        &NtName, hRoot != NULL, pszDirAndFilter);
        if (RT_SUCCESS(rc))
        {
            /* Drop the filter from the NT name. */
            switch (enmFilter)
            {
                case RTDIRFILTER_NONE:
                    break;
                case RTDIRFILTER_WINNT:
                case RTDIRFILTER_UNIX:
                case RTDIRFILTER_UNIX_UPCASED:
                {
                    size_t cwc = NtName.Length / sizeof(RTUTF16);
                    while (   cwc > 0
                           && NtName.Buffer[cwc - 1] != '\\')
                        cwc--;
                    NtName.Buffer[cwc] = '\0';
                    NtName.Length = (uint16_t)(cwc * sizeof(RTUTF16));
                    break;
                }
                default:
                    AssertFailedBreak();
            }

            rc = rtDirOpenRelativeOrHandle(phDir, szAbsDirAndFilter, enmFilter, fFlags, (uintptr_t)hRoot, &NtName);
        }
                char szAbsDirAndFilter[RTPATH_MAX];
                rc = rtDirRelJoinPathForDirOpen(szAbsDirAndFilter, sizeof(szAbsDirAndFilter), pThis,
                                                &NtName, hRoot != NULL, pszRelPath);
                if (RT_SUCCESS(rc))
                    rc = rtDirOpenRelativeOrHandle(phSubDir, pszRelPath, RTDIRFILTER_NONE, 0 /*fFlags*/,
                                                   (uintptr_t)hNewDir, NULL /*pvNativeRelative*/);
        if (NtName.Length != 0 || hRoot == NULL)
            rc = rtPathNtQueryInfoWorker(hRoot, &NtName, pObjInfo, enmAddAttr, fFlags, pszRelPath);
        else
            rc = RTDirQueryInfo(hDir, pObjInfo, enmAddAttr);
       RTNtPathFree(&NtName, NULL);