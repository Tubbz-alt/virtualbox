/* $Id: efi-fat.h $ */
/** @file
 * IPRT, EFI FAT Binary (used by Apple, contains multiple architectures).
 */

/*
 * Copyright (C) 2019-2020 Oracle Corporation
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

#ifndef IPRT_INCLUDED_formats_efi_fat_h
#define IPRT_INCLUDED_formats_efi_fat_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/types.h>
#include <iprt/assertcompile.h>

/*
 * Definitions come from http://refit.sourceforge.net/info/fat_binary.html
 */

/**
 * The header structure.
 */
typedef struct EFI_FATHDR
{
    /** The magic identifying the header .*/
    uint32_t                u32Magic;
    /** Number of files (one per architecture) embedded into the file. */
    uint32_t                cFilesEmbedded;
} EFI_FATHDR;
AssertCompileSize(EFI_FATHDR, 8);
typedef EFI_FATHDR *PEFI_FATHDR;

/** The magic identifying a FAT header. */
#define EFI_FATHDR_MAGIC UINT32_C(0x0ef1fab9)

/**
 * The direcory entry.
 */
typedef struct EFI_FATDIRENTRY
{
    /** The CPU type the referenced file is for. */
    uint32_t                u32CpuType;
    /** The CPU sub-type the referenced file is for. */
    uint32_t                u32CpuSubType;
    /** Offset in bytes where the file is located. */
    uint32_t                u32OffsetStart;
    /** Length of the file in bytes. */
    uint32_t                cbFile;
    /** Alignment used for the file. */
    uint32_t                u32Alignment;
} EFI_FATDIRENTRY;
AssertCompileSize(EFI_FATDIRENTRY, 20);
typedef EFI_FATDIRENTRY *PEFI_FATDIRENTRY;

#define EFI_FATDIRENTRY_CPU_TYPE_X86         UINT32_C(0x7)
#define EFI_FATDIRENTRY_CPU_TYPE_AMD64       UINT32_C(0x01000007)

#define EFI_FATDIRENTRY_CPU_SUB_TYPE_GENERIC UINT32_C(0x3)


#endif /* !IPRT_INCLUDED_formats_efi_fat_h */

