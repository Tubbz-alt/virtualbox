/* $Id: tstDllMainStub.c 2 2007-11-16 16:07:14Z bird $ */
/** @file
 * kLdr testcase - DLL Stub.
 */

/*
 * Copyright (c) 2006-2007 knut st. osmundsen <bird-kStuff-spam@anduin.net>
 *
 * This file is part of kStuff.
 *
 * kStuff is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * kStuff is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with kStuff; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "tst.h"

#if K_OS == K_OS_OS2
# define INCL_BASE
# include <os2.h>

#elif K_OS == K_OS_WINDOWS
# include <windows.h>

#elif K_OS == K_OS_DARWIN
/* later */

#else
# error "port me"
#endif


#if K_OS == K_OS_OS2
/**
 * OS/2 DLL 'main'
 */
ULONG _System _DLL_InitTerm(HMODULE hmod, ULONG fFlag)
{
    return TRUE;
}

#elif K_OS == K_OS_WINDOWS

/**
 * Window DLL 'main'
 */
BOOL __stdcall DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpReserved)
{
    return TRUE;
}

#elif K_OS == K_OS_DARWIN
/* later */

#else
# error "port me"
#endif
