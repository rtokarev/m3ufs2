#ifndef _M3UFS2_H_
#define _M3UFS2_H_
/*
 * Copyright (c) 2011 Roman Tokarev <roman.s.tokarev@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *        may be used to endorse or promote products derived from this software
 *        without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CONTRIBUTORS ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <dirent.h>
#include <features.h>
#include <sys/syscall.h>
#include <unistd.h>


#ifndef __USE_ATFILE

#include <fcntl.h>
#include <stdarg.h>

int openat(int dirfd, const char *pathname, int flags, ...)
{
	va_list ap;
	mode_t mode = 0000;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return syscall(__NR_openat, dirfd, pathname, flags, mode);
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags)
{
	return syscall(__NR_fstatat64, dirfd, pathname, buf, flags);
}

#endif // __USE_ATFILE

struct linux_dirent {
	unsigned long  d_ino;		/* Inode number */
	unsigned long  d_off;		/* Offset to next linux_dirent */
	unsigned short d_reclen;	/* Length of this linux_dirent */
	char	       d_name[];	/* Filename (null-terminated) */
					/* length is actually (d_reclen - 2 -
					   offsetof(struct linux_dirent, d_name) */
};

int getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{
	return syscall(__NR_getdents, fd, dirp, count);
}

#endif // _M3UFS2_H_
