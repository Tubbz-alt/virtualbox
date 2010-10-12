/** @file
 * VirtualBox File System for Solaris Guests, provider implementation.
 * Portions contributed by: Ronald.
 */

/*
 * Copyright (C) 2008 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
/*
 * Provider interfaces for shared folder file system.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mntent.h>
#include <sys/param.h>
#include <sys/modctl.h>
#include <sys/mount.h>
#include <sys/policy.h>
#include <sys/atomic.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/dirent.h>
#include "vboxfs_prov.h"
#ifdef u
#undef u
#endif
#include "../../common/VBoxGuestLib/VBoxCalls.h"

#define	SFPROV_VERSION	1

static VBSFCLIENT vbox_client;

static int sfprov_vbox2errno(int rc)
{
	if (rc == VERR_ACCESS_DENIED)
		return (EACCES);
	return (RTErrConvertToErrno(rc));
}

/*
 * utility to create strings
 */
static SHFLSTRING *
sfprov_string(char *path, int *sz)
{
	SHFLSTRING *str;
	int len = strlen(path);

	*sz = len + 1 + sizeof (*str) - sizeof (str->String);
	str = kmem_zalloc(*sz, KM_SLEEP);
	str->u16Size = len + 1;
	str->u16Length = len;
	strcpy(str->String.utf8, path);
	return (str);
}

sfp_connection_t *
sfprov_connect(int version)
{
	/*
	 * only one version for now, so must match
	 */
	int rc = -1;
	if (version != SFPROV_VERSION)
	{
		cmn_err(CE_WARN, "sfprov_connect: wrong version");
		return NULL;
	}
	rc = vboxInit();
	if (RT_SUCCESS(rc))
	{
		rc = vboxConnect(&vbox_client);
		if (RT_SUCCESS(rc))
		{
			rc = vboxCallSetUtf8(&vbox_client);
			if (RT_SUCCESS(rc))
			{
				return ((sfp_connection_t *)&vbox_client);
			}
			else
				cmn_err(CE_WARN, "sfprov_connect: vboxCallSetUtf8() failed");

			vboxDisconnect(&vbox_client);
		}
		else
			cmn_err(CE_WARN, "sfprov_connect: vboxConnect() failed rc=%d", rc);
		vboxUninit();
	}
	else
		cmn_err(CE_WARN, "sfprov_connect: vboxInit() failed rc=%d", rc);
}

void
sfprov_disconnect(sfp_connection_t *conn)
{
	if (conn != (sfp_connection_t *)&vbox_client)
		cmn_err(CE_WARN, "sfprov_disconnect: bad argument");
	vboxDisconnect(&vbox_client);
	vboxUninit();
}


/*
 * representation of an active mount point
 */
struct sfp_mount {
	VBSFMAP	map;
};

int
sfprov_mount(sfp_connection_t *conn, char *path, sfp_mount_t **mnt)
{
	sfp_mount_t *m;
	SHFLSTRING *str;
	int size;
	int rc;

	m = kmem_zalloc(sizeof (*m), KM_SLEEP);
	str = sfprov_string(path, &size);
	rc = vboxCallMapFolder(&vbox_client, str, &m->map);
	if (!RT_SUCCESS(rc)) {
		cmn_err(CE_WARN, "sfprov_mount: vboxCallMapFolder() failed");
		kmem_free(m, sizeof (*m));
		*mnt = NULL;
		rc = EINVAL;
	} else {
		*mnt = m;
		rc = 0;
	}
	kmem_free(str, size);
	return (rc);
}

int
sfprov_unmount(sfp_mount_t *mnt)
{
	int rc;

	rc = vboxCallUnmapFolder(&vbox_client, &mnt->map);
	if (!RT_SUCCESS(rc)) {
		cmn_err(CE_WARN, "sfprov_mount: vboxCallUnmapFolder() failed");
		rc = EINVAL;
	} else {
		rc = 0;
	}
	kmem_free(mnt, sizeof (*mnt));
	return (rc);
}

/*
 * query information about a mounted file system
 */
int
sfprov_get_blksize(sfp_mount_t *mnt, uint64_t *blksize)
{
	int rc;
	SHFLVOLINFO info;
	uint32_t bytes = sizeof(SHFLVOLINFO);

	rc = vboxCallFSInfo(&vbox_client, &mnt->map, 0,
	    (SHFL_INFO_GET | SHFL_INFO_VOLUME), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc))
		return (EINVAL);
	*blksize = info.ulBytesPerAllocationUnit;
	return (0);
}

int
sfprov_get_blksused(sfp_mount_t *mnt, uint64_t *blksused)
{
	int rc;
	SHFLVOLINFO info;
	uint32_t bytes = sizeof(SHFLVOLINFO);

	rc = vboxCallFSInfo(&vbox_client, &mnt->map, 0,
	    (SHFL_INFO_GET | SHFL_INFO_VOLUME), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc))
		return (EINVAL);
	*blksused = (info.ullTotalAllocationBytes -
	    info.ullAvailableAllocationBytes) / info.ulBytesPerAllocationUnit;
	return (0);
}

int
sfprov_get_blksavail(sfp_mount_t *mnt, uint64_t *blksavail)
{
	int rc;
	SHFLVOLINFO info;
	uint32_t bytes = sizeof(SHFLVOLINFO);

	rc = vboxCallFSInfo(&vbox_client, &mnt->map, 0,
	    (SHFL_INFO_GET | SHFL_INFO_VOLUME), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc))
		return (EINVAL);
	*blksavail =
	    info.ullAvailableAllocationBytes / info.ulBytesPerAllocationUnit;
	return (0);
}

int
sfprov_get_maxnamesize(sfp_mount_t *mnt, uint32_t *maxnamesize)
{
	int rc;
	SHFLVOLINFO info;
	uint32_t bytes = sizeof(SHFLVOLINFO);

	rc = vboxCallFSInfo(&vbox_client, &mnt->map, 0,
	    (SHFL_INFO_GET | SHFL_INFO_VOLUME), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc))
		return (EINVAL);
	*maxnamesize = info.fsProperties.cbMaxComponent;
	return (0);
}

int
sfprov_get_readonly(sfp_mount_t *mnt, uint32_t *readonly)
{
	int rc;
	SHFLVOLINFO info;
	uint32_t bytes = sizeof(SHFLVOLINFO);

	rc = vboxCallFSInfo(&vbox_client, &mnt->map, 0,
	    (SHFL_INFO_GET | SHFL_INFO_VOLUME), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc))
		return (EINVAL);
	*readonly = info.fsProperties.fReadOnly;
	return (0);
}

/*
 * File operations: open/close/read/write/etc.
 *
 * open/create can return any relevant errno, however ENOENT
 * generally means that the host file didn't exist.
 */
struct sfp_file {
	SHFLHANDLE handle;
	VBSFMAP map;	/* need this again for the close operation */
};

int
sfprov_create(sfp_mount_t *mnt, char *path, sfp_file_t **fp)
{

	int rc;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	int size;
	sfp_file_t *newfp;

	str = sfprov_string(path, &size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_ACT_CREATE_IF_NEW |
	    SHFL_CF_ACT_REPLACE_IF_EXISTS | SHFL_CF_ACCESS_READWRITE;
	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);
	kmem_free(str, size);

	if (RT_FAILURE(rc))
	{
		if (rc != VERR_ACCESS_DENIED && rc != VERR_WRITE_PROTECT)
			cmn_err(CE_WARN, "sfprov_create: vboxCallCreate failed! path=%s rc=%d\n", path, rc);
		return (sfprov_vbox2errno(rc));
	}
	if (parms.Handle == SHFL_HANDLE_NIL) {
		if (parms.Result == SHFL_FILE_EXISTS)
			return (EEXIST);
		return (ENOENT);
	}
	newfp = kmem_alloc(sizeof(sfp_file_t), KM_SLEEP);
	newfp->handle = parms.Handle;
	newfp->map = mnt->map;
	*fp = newfp;
	return (0);
}

int
sfprov_open(sfp_mount_t *mnt, char *path, sfp_file_t **fp)
{
	int rc;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	int size;
	sfp_file_t *newfp;

	/*
	 * First we attempt to open it read/write. If that fails we
	 * try read only.
	 */
	bzero(&parms, sizeof(parms));
	str = sfprov_string(path, &size);
	parms.Handle = SHFL_HANDLE_NIL;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READWRITE;
	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);
	if (RT_FAILURE(rc) && rc != VERR_ACCESS_DENIED) {
		kmem_free(str, size);
		return (sfprov_vbox2errno(rc));
	}
	if (parms.Handle == SHFL_HANDLE_NIL) {
		if (parms.Result == SHFL_PATH_NOT_FOUND ||
		    parms.Result == SHFL_FILE_NOT_FOUND) {
			kmem_free(str, size);
			return (ENOENT);
		}
		parms.CreateFlags =
		    SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;
		rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);
		if (RT_FAILURE(rc)) {
			kmem_free(str, size);
			return (sfprov_vbox2errno(rc));
		}
		if (parms.Handle == SHFL_HANDLE_NIL) {
			kmem_free(str, size);
			return (ENOENT);
		}
	}
	newfp = kmem_alloc(sizeof(sfp_file_t), KM_SLEEP);
	newfp->handle = parms.Handle;
	newfp->map = mnt->map;
	*fp = newfp;
	return (0);
}

int
sfprov_trunc(sfp_mount_t *mnt, char *path)
{
	int rc;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	int size;

	/*
	 * open it read/write.
	 */
	str = sfprov_string(path, &size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READWRITE |
	    SHFL_CF_ACT_OVERWRITE_IF_EXISTS;
	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);

	if (RT_FAILURE(rc)) {
		kmem_free(str, size);
		return (EINVAL);
	}
	(void)vboxCallClose(&vbox_client, &mnt->map, parms.Handle);
	return (0);
}

int
sfprov_close(sfp_file_t *fp)
{
	int rc;

	rc = vboxCallClose(&vbox_client, &fp->map, fp->handle);
	kmem_free(fp, sizeof(sfp_file_t));
	return (0);
}

int
sfprov_read(sfp_file_t *fp, char *buffer, uint64_t offset, uint32_t *numbytes)
{
	int rc;

	rc = vboxCallRead(&vbox_client, &fp->map, fp->handle, offset,
	    numbytes, (uint8_t *)buffer, 0);	/* what is that last arg? */
	if (RT_FAILURE(rc))
		return (EINVAL);
	return (0);
}

int
sfprov_write(sfp_file_t *fp, char *buffer, uint64_t offset, uint32_t *numbytes)
{
	int rc;

	rc = vboxCallWrite(&vbox_client, &fp->map, fp->handle, offset,
	    numbytes, (uint8_t *)buffer, 0);	/* what is that last arg? */
	if (RT_FAILURE(rc))
		return (EINVAL);
	return (0);
}

int
sfprov_fsync(sfp_file_t *fp)
{
	int rc;

	rc = vboxCallFlush(&vbox_client, &fp->map, fp->handle);
	if (RT_FAILURE(rc))
		return (EIO);
	return (0);
}


static int
sfprov_getinfo(sfp_mount_t *mnt, char *path, RTFSOBJINFO *info)
{
	int rc;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	int size;

	str = sfprov_string(path, &size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_LOOKUP | SHFL_CF_ACT_FAIL_IF_NEW;
	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);
	kmem_free(str, size);

	if (RT_FAILURE(rc))
		return (EINVAL);
	if (parms.Result != SHFL_FILE_EXISTS)
		return (ENOENT);
	*info = parms.Info;
	return (0);
}

/*
 * get information about a file (or directory)
 */
static void
sfprov_mode_from_fmode(mode_t *mode, RTFMODE fMode)
{
	mode_t m = 0;

	if (RTFS_IS_DIRECTORY(fMode))
		m |= S_IFDIR;
	else if (RTFS_IS_FILE(fMode))
		m |= S_IFREG;
	else if (RTFS_IS_FIFO(fMode))
		m |= S_IFIFO;
	else if (RTFS_IS_DEV_CHAR(fMode))
		m |= S_IFCHR;
	else if (RTFS_IS_DEV_BLOCK(fMode))
		m |= S_IFBLK;
	else if (RTFS_IS_SYMLINK(fMode))
		m |= S_IFLNK;
	else if (RTFS_IS_SOCKET(fMode))
		m |= S_IFSOCK;

	if (fMode & RTFS_UNIX_IRUSR)
		m |= S_IRUSR;
	if (fMode & RTFS_UNIX_IWUSR)
		m |= S_IWUSR;
	if (fMode & RTFS_UNIX_IXUSR)
		m |= S_IXUSR;
	if (fMode & RTFS_UNIX_IRGRP)
		m |= S_IRGRP;
	if (fMode & RTFS_UNIX_IWGRP)
		m |= S_IWGRP;
	if (fMode & RTFS_UNIX_IXGRP)
		m |= S_IXGRP;
	if (fMode & RTFS_UNIX_IROTH)
		m |= S_IROTH;
	if (fMode & RTFS_UNIX_IWOTH)
		m |= S_IWOTH;
	if (fMode & RTFS_UNIX_IXOTH)
		m |= S_IXOTH;
	if (fMode & RTFS_UNIX_ISUID)
		m |= S_ISUID;
	if (fMode & RTFS_UNIX_ISGID)
		m |= S_ISGID;
	if (fMode & RTFS_UNIX_ISTXT)
		m |= S_ISVTX;
	*mode = m;
}

/*
 * get information about a file (or directory)
 */
int
sfprov_get_mode(sfp_mount_t *mnt, char *path, mode_t *mode)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);
	sfprov_mode_from_fmode(mode, info.Attr.fMode);
	return (0);
}

int
sfprov_get_size(sfp_mount_t *mnt, char *path, uint64_t *size)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);
	*size = info.cbObject;
	return (0);
}

static void
sfprov_ftime_from_timespec(timestruc_t *time, RTTIMESPEC *ts)
{
	uint64_t nanosec = RTTimeSpecGetNano(ts);
	time->tv_sec = nanosec / UINT64_C(1000000000);
	time->tv_nsec = nanosec % UINT64_C(1000000000);
}

int
sfprov_get_atime(sfp_mount_t *mnt, char *path, timestruc_t *time)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);
	sfprov_ftime_from_timespec(time, &info.AccessTime);
	return (0);
}

int
sfprov_get_mtime(sfp_mount_t *mnt, char *path, timestruc_t *time)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);
	sfprov_ftime_from_timespec(time, &info.ModificationTime);
	return (0);
}

int
sfprov_get_ctime(sfp_mount_t *mnt, char *path, timestruc_t *time)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);
	sfprov_ftime_from_timespec(time, &info.ChangeTime);
	return (0);
}

int
sfprov_get_attr(
	sfp_mount_t *mnt,
	char *path,
	mode_t *mode,
	uint64_t *size,
	timestruc_t *atime,
	timestruc_t *mtime,
	timestruc_t *ctime)
{
	int rc;
	RTFSOBJINFO info;

	rc = sfprov_getinfo(mnt, path, &info);
	if (rc)
		return (rc);

	if (mode)
		sfprov_mode_from_fmode(mode, info.Attr.fMode);
	if (size != NULL)
		*size = info.cbObject;
	if (atime != NULL)
		sfprov_ftime_from_timespec(atime, &info.AccessTime);
	if (mtime != NULL)
		sfprov_ftime_from_timespec(mtime, &info.ModificationTime);
	if (ctime != NULL)
		sfprov_ftime_from_timespec(ctime, &info.ChangeTime);

	return (0);
}

static void
sfprov_timespec_from_ftime(RTTIMESPEC *ts, timestruc_t time)
{
	uint64_t nanosec = UINT64_C(1000000000) * time.tv_sec + time.tv_nsec;
	RTTimeSpecSetNano(ts, nanosec);
}

int
sfprov_set_attr(
	sfp_mount_t *mnt,
	char *path,
	uint_t mask,
	mode_t mode,
	timestruc_t atime,
	timestruc_t mtime,
	timestruc_t ctime)
{
	int rc, err;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	RTFSOBJINFO info;
	uint32_t bytes;
	int str_size;

	str = sfprov_string(path, &str_size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_ACT_OPEN_IF_EXISTS
			  | SHFL_CF_ACT_FAIL_IF_NEW
			  | SHFL_CF_ACCESS_ATTR_WRITE;

	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);

	if (RT_FAILURE(rc)) {
		cmn_err(CE_WARN, "sfprov_set_attr: vboxCallCreate(%s) failed rc=%d",
		    path, rc);
		err = EINVAL;
		goto fail2;
	}
	if (parms.Result != SHFL_FILE_EXISTS) {
		err = ENOENT;
		goto fail1;
	}

	RT_ZERO(info);
	if (mask & AT_MODE) {
#define mode_set(r) ((mode & (S_##r)) ? RTFS_UNIX_##r : 0)

		info.Attr.fMode  = mode_set (ISUID);
		info.Attr.fMode |= mode_set (ISGID);
		info.Attr.fMode |= (mode & S_ISVTX) ? RTFS_UNIX_ISTXT : 0;
		info.Attr.fMode |= mode_set (IRUSR);
		info.Attr.fMode |= mode_set (IWUSR);
		info.Attr.fMode |= mode_set (IXUSR);
		info.Attr.fMode |= mode_set (IRGRP);
		info.Attr.fMode |= mode_set (IWGRP);
		info.Attr.fMode |= mode_set (IXGRP);
		info.Attr.fMode |= mode_set (IROTH);
		info.Attr.fMode |= mode_set (IWOTH);
		info.Attr.fMode |= mode_set (IXOTH);

		if (S_ISDIR(mode))
			info.Attr.fMode |= RTFS_TYPE_DIRECTORY;
		else if (S_ISREG(mode))
			info.Attr.fMode |= RTFS_TYPE_FILE;
		else if (S_ISFIFO(mode))
			info.Attr.fMode |= RTFS_TYPE_FIFO;
		else if (S_ISCHR(mode))
			info.Attr.fMode |= RTFS_TYPE_DEV_CHAR;
		else if (S_ISBLK(mode))
			info.Attr.fMode |= RTFS_TYPE_DEV_BLOCK;
		else if (S_ISLNK(mode))
			info.Attr.fMode |= RTFS_TYPE_SYMLINK;
		else if (S_ISSOCK(mode))
			info.Attr.fMode |= RTFS_TYPE_SOCKET;
		else
			info.Attr.fMode |= RTFS_TYPE_FILE;
	}

	if (mask & AT_ATIME)
		sfprov_timespec_from_ftime(&info.AccessTime, atime);
	if (mask & AT_MTIME)
		sfprov_timespec_from_ftime(&info.ModificationTime, mtime);
	if (mask & AT_CTIME)
		sfprov_timespec_from_ftime(&info.ChangeTime, ctime);

	bytes = sizeof(info);
	rc = vboxCallFSInfo(&vbox_client, &mnt->map, parms.Handle,
	    (SHFL_INFO_SET | SHFL_INFO_FILE), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc)) {
		if (rc != VERR_ACCESS_DENIED && rc != VERR_WRITE_PROTECT)
		{
			cmn_err(CE_WARN, "sfprov_set_attr: vboxCallFSInfo(%s, FILE) failed rc=%d",
		    path, rc);
		}
		err = sfprov_vbox2errno(rc);
		goto fail1;
	}

	err = 0;

fail1:
	rc = vboxCallClose(&vbox_client, &mnt->map, parms.Handle);
	if (RT_FAILURE(rc)) {
		cmn_err(CE_WARN, "sfprov_set_attr: vboxCallClose(%s) failed rc=%d",
		    path, rc);
	}
fail2:
	kmem_free(str, str_size);
	return err;
}

int
sfprov_set_size(sfp_mount_t *mnt, char *path, uint64_t size)
{
	int rc, err;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	RTFSOBJINFO info;
	uint32_t bytes;
	int str_size;

	str = sfprov_string(path, &str_size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_ACT_OPEN_IF_EXISTS
			  | SHFL_CF_ACT_FAIL_IF_NEW
			  | SHFL_CF_ACCESS_WRITE;

	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);

	if (RT_FAILURE(rc)) {
		cmn_err(CE_WARN, "sfprov_set_size: vboxCallCreate(%s) failed rc=%d",
		    path, rc);
		err = EINVAL;
		goto fail2;
	}
	if (parms.Result != SHFL_FILE_EXISTS) {
		err = ENOENT;
		goto fail1;
	}

	RT_ZERO(info);
	info.cbObject = size;
	bytes = sizeof(info);
	rc = vboxCallFSInfo(&vbox_client, &mnt->map, parms.Handle,
	    (SHFL_INFO_SET | SHFL_INFO_SIZE), &bytes, (SHFLDIRINFO *)&info);
	if (RT_FAILURE(rc)) {
		cmn_err(CE_WARN, "sfprov_set_size: vboxCallFSInfo(%s, SIZE) failed rc=%d",
		    path, rc);
		err = sfprov_vbox2errno(rc);
		goto fail1;
	}

	err = 0;

fail1:
	rc = vboxCallClose(&vbox_client, &mnt->map, parms.Handle);
	if (RT_FAILURE(rc)) {
		cmn_err(CE_WARN, "sfprov_set_size: vboxCallClose(%s) failed rc=%d",
		    path, rc);
	}
fail2:
	kmem_free(str, str_size);
	return err;
}

/*
 * Directory operations
 */
int
sfprov_mkdir(sfp_mount_t *mnt, char *path, sfp_file_t **fp)
{
	int rc;
	SHFLCREATEPARMS parms;
	SHFLSTRING *str;
	int size;
	sfp_file_t *newfp;

	str = sfprov_string(path, &size);
	parms.Handle = 0;
	parms.Info.cbObject = 0;
	parms.CreateFlags = SHFL_CF_DIRECTORY | SHFL_CF_ACT_CREATE_IF_NEW |
	    SHFL_CF_ACT_FAIL_IF_EXISTS | SHFL_CF_ACCESS_READ;
	rc = vboxCallCreate(&vbox_client, &mnt->map, str, &parms);
	kmem_free(str, size);

	if (RT_FAILURE(rc))
		return (sfprov_vbox2errno(rc));
	if (parms.Handle == SHFL_HANDLE_NIL) {
		if (parms.Result == SHFL_FILE_EXISTS)
			return (EEXIST);
		return (ENOENT);
	}
	newfp = kmem_alloc(sizeof(sfp_file_t), KM_SLEEP);
	newfp->handle = parms.Handle;
	newfp->map = mnt->map;
	*fp = newfp;
	return (0);
}

int
sfprov_remove(sfp_mount_t *mnt, char *path)
{
	int rc;
	SHFLSTRING *str;
	int size;

	str = sfprov_string(path, &size);
	rc = vboxCallRemove(&vbox_client, &mnt->map, str, SHFL_REMOVE_FILE);
	kmem_free(str, size);
	if (RT_FAILURE(rc))
		return (sfprov_vbox2errno(rc));
	return (0);
}

int
sfprov_rmdir(sfp_mount_t *mnt, char *path)
{
	int rc;
	SHFLSTRING *str;
	int size;

	str = sfprov_string(path, &size);
	rc = vboxCallRemove(&vbox_client, &mnt->map, str, SHFL_REMOVE_DIR);
	kmem_free(str, size);
	if (RT_FAILURE(rc))
		return (sfprov_vbox2errno(rc));
	return (0);
}

int
sfprov_rename(sfp_mount_t *mnt, char *from, char *to, uint_t is_dir)
{
	int rc;
	SHFLSTRING *old, *new;
	int old_size, new_size;

	old = sfprov_string(from, &old_size);
	new = sfprov_string(to, &new_size);
	rc = vboxCallRename(&vbox_client, &mnt->map, old, new,
	    (is_dir ? SHFL_RENAME_DIR : SHFL_RENAME_FILE) |
	    SHFL_RENAME_REPLACE_IF_EXISTS);
	kmem_free(old, old_size);
	kmem_free(new, new_size);
	if (RT_FAILURE(rc))
		return (sfprov_vbox2errno(rc));
	return (0);
}


/*
 * Read all filenames in a directory.
 *
 * - success - all entries read and returned
 * - ENOENT - Couldn't open the directory for reading
 * - EINVAL - Internal error of some kind
 *
 * On successful return, *dirents points to a list of sffs_dirents_t;
 * for each dirent, all fields except the d_ino will be set appropriately.
 * The caller is responsible for freeing the dirents buffer.
 */
int
sfprov_readdir(
	sfp_mount_t *mnt,
	char *path,
	sffs_dirents_t **dirents,
	sffs_stats_t **stats)
{
	int error;
	char *cp;
	int len;
	SHFLSTRING *mask_str = NULL;	/* must be path with "/*" appended */
	int mask_size;
	sfp_file_t *fp;
	uint32_t infobuff_alloc = 16384;
	SHFLDIRINFO *infobuff = NULL, *info;
	uint32_t numbytes;
	uint32_t nents;
	uint32_t size;
	uint32_t cnt;
	sffs_dirents_t *cur_buf;
	sffs_stats_t *cur_stats;
	struct dirent64 *dirent;
	sffs_stat_t *stat;
	unsigned short reclen;

	*dirents = NULL;
	*stats = NULL;

	error = sfprov_open(mnt, path, &fp);
	if (error != 0)
		return (ENOENT);

	/*
	 * Allocate the first dirents and stats buffers.
	 */
	*dirents = kmem_alloc(SFFS_DIRENTS_SIZE, KM_SLEEP);
	if (*dirents == NULL) {
		error = (ENOSPC);
		goto done;
	}
	cur_buf = *dirents;
	cur_buf->sf_next = NULL;
	cur_buf->sf_len = 0;

	*stats = kmem_alloc(sizeof(**stats), KM_SLEEP);
	if (*stats == NULL) {
		error = (ENOSPC);
		goto done;
	}
	cur_stats = *stats;
	cur_stats->sf_next = NULL;
	cur_stats->sf_num = 0;

	/*
	 * Create mask that VBox expects. This needs to be the directory path,
	 * plus a "*" wildcard to get all files.
	 */
	len = strlen(path) + 3;
	cp = kmem_alloc(len, KM_SLEEP);
	if (cp == NULL) {
		error = (ENOSPC);
		goto done;
	}
	strcpy(cp, path);
	strcat(cp, "/*");
	mask_str = sfprov_string(cp, &mask_size);
	kmem_free(cp, len);

	/*
	 * Now loop using vboxCallDirInfo
	 */
	infobuff = kmem_alloc(infobuff_alloc, KM_SLEEP);
	if (infobuff == NULL) {
		error = (ENOSPC);
		goto done;
	}

	cnt = 0;
	for (;;) {
		numbytes = infobuff_alloc;
		error = vboxCallDirInfo(&vbox_client, &fp->map, fp->handle,
		    mask_str, 0, 0, &numbytes, infobuff, &nents);
		switch (error) {

		case VINF_SUCCESS:
			/* fallthrough */
		case VERR_NO_MORE_FILES:
			break;

		case VERR_NO_TRANSLATION:
			/* XXX ??? */
			break;

		default:
			error = sfprov_vbox2errno(error);
			goto done;
		}

		/*
		 * Create the dirent_t's and save the stats for each name
		 */
		for (info = infobuff; (char *) info < (char *) infobuff + numbytes; nents--) {
			/* expand buffers if we need more space */
			reclen = DIRENT64_RECLEN(strlen(info->name.String.utf8));
			if (SFFS_DIRENTS_OFF + cur_buf->sf_len + reclen > SFFS_DIRENTS_SIZE) {
				cur_buf->sf_next = kmem_alloc(SFFS_DIRENTS_SIZE, KM_SLEEP);
				if (cur_buf->sf_next == NULL) {
					error = ENOSPC;
					goto done;
				}
				cur_buf = cur_buf->sf_next;
				cur_buf->sf_next = NULL;
				cur_buf->sf_len = 0;
			}

			if (cur_stats->sf_num >= SFFS_STATS_LEN) {
				cur_stats->sf_next = kmem_alloc(sizeof(**stats), KM_SLEEP);
				if (cur_stats->sf_next == NULL) {
					error = (ENOSPC);
					goto done;
				}
				cur_stats = cur_stats->sf_next;
				cur_stats->sf_next = NULL;
				cur_stats->sf_num = 0;
			}

			/* create the dirent with the name, offset, and len */
			dirent = (dirent64_t *)
			    (((char *) &cur_buf->sf_entries[0]) + cur_buf->sf_len);
			strcpy(&dirent->d_name[0], info->name.String.utf8);
			dirent->d_reclen = reclen;
			dirent->d_off = cnt;

			cur_buf->sf_len += reclen;
			++cnt;

			/* save the stats */
			stat = &cur_stats->sf_stats[cur_stats->sf_num];
			++cur_stats->sf_num;

			sfprov_mode_from_fmode(&stat->sf_mode, info->Info.Attr.fMode);
			stat->sf_size = info->Info.cbObject;
			sfprov_ftime_from_timespec(&stat->sf_atime, &info->Info.AccessTime);
			sfprov_ftime_from_timespec(&stat->sf_mtime, &info->Info.ModificationTime);
			sfprov_ftime_from_timespec(&stat->sf_ctime, &info->Info.ChangeTime);

			/* next info */
			size = offsetof (SHFLDIRINFO, name.String) + info->name.u16Size;
			info = (SHFLDIRINFO *) ((uintptr_t) info + size);
		}
		ASSERT(nents == 0);
		ASSERT((char *) info == (char *) infobuff + numbytes);

		if (error == VERR_NO_MORE_FILES)
			break;
	}
	error = 0;

done:
	if (error != 0) {
		while (*dirents) {
			cur_buf = (*dirents)->sf_next;
			kmem_free(*dirents, SFFS_DIRENTS_SIZE);
			*dirents = cur_buf;
		}
		while (*stats) {
			cur_stats = (*stats)->sf_next;
			kmem_free(*stats, sizeof(**stats));
			*stats = cur_stats;
		}
	}
	if (infobuff != NULL)
		kmem_free(infobuff, infobuff_alloc);
	if (mask_str != NULL)
		kmem_free(mask_str, mask_size);
	sfprov_close(fp);
	return (error);
}