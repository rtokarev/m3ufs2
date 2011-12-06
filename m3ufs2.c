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

#define FUSE_USE_VERSION 26

#include <m3ufs2.h>

#include <ctype.h>
#include <errno.h>
#include <fuse.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>


#define EXTM3U "#EXTM3U"
#define EXTINF "#EXTINF:"


#define SWAP(a, b) ({		\
	__typeof__(*(a)) t;	\
				\
	t = *b;			\
	*b = *a;		\
	*a = t;			\
})

#define IS_M3U(path) ({							\
	char *ext;							\
	bool is_m3u = false;						\
									\
	ext = strrchr(path, '.');					\
	if (ext != NULL &&						\
	    (strcmp(ext, ".m3u") == 0 || strcmp(ext, ".M3U") == 0))	\
		is_m3u = true;						\
									\
	is_m3u;								\
})

#define RPATH(path) ({			\
	const char *rpath = path;	\
					\
	while ((++rpath)[0] == '/');	\
	if (rpath[0] == '\0')		\
		rpath = ".";		\
					\
	rpath;				\
})


struct m3ufs2 {
	char *dir;
	int dirfd;

	char *mountpoint;

	int shuffle;
};

struct m3u_entry {
	char *name;
	char *path;
};

struct m3u {
	char *fname;

	time_t mtime;

	struct m3u_entry *entry;
	uint32_t size;
	uint32_t len;

	uint32_t count_order;

	struct m3u *next;
};

enum {
	KEY_HELP,
	KEY_VERSION
};


static struct m3ufs2 m3ufs2;

#define M3UFS2_OPT(t, p, v) {t, offsetof(struct m3ufs2, p), v }
static struct fuse_opt m3ufs2_opts[] = {
	M3UFS2_OPT("shuffle",		shuffle, 1),

	FUSE_OPT_KEY("-h",		KEY_HELP),
	FUSE_OPT_KEY("--help",		KEY_HELP),
	FUSE_OPT_KEY("-V",		KEY_VERSION),
	FUSE_OPT_KEY("--version",	KEY_VERSION),
	FUSE_OPT_END
};

static pthread_mutex_t m3u_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct m3u *m3u = NULL;


static void m3u_free(struct m3u *m)
{
	unsigned i;
	struct m3u_entry *entry;

	for (i = 0, entry = m->entry; i < m->len; i++, entry++) {
		if (entry->path != NULL)
			free(entry->path);
		if (entry->name != NULL)
			free(entry->name);
	}

	if (m->fname != NULL)
		free(m->fname);
	if (m->entry != NULL)
		free(m->entry);

	free(m);
}

static void close_(int *fd)
{
	if (*fd != -1)
		close(*fd);
}

static void fclose_(FILE **f)
{
	if (*f != NULL)
		fclose(*f);
}

static void m3u_free_(struct m3u **m)
{
	if (*m == NULL)
		return;

	m3u_free(*m);
}

static void str_free_(char **str)
{
	if (*str == NULL)
		return;

	free(*str);
}

static uint32_t num_order(uint32_t num)
{
	uint32_t order = 1;

	while ((num /= 10) != 0) order++;

	return order;
}

#define M3U_ENTRY_ALLOC_BY 8
static struct m3u *m3u_process(const char *path)
{
	struct stat stbuf;
	int fd __attribute__((cleanup(close_))) = -1;
	FILE *f __attribute__((cleanup(fclose_))) = NULL;
	const char *rpath;
	char *dir, *c;
	struct m3u *m __attribute__((cleanup(m3u_free_))) = NULL;
	char *line __attribute__((cleanup(str_free_))) = NULL;
	size_t line_size = 0;
	char *firstline = NULL;
	bool is_extm3u = false;
	char *extinf_name __attribute__((cleanup(str_free_))) = NULL;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;
	m->fname = strdup(path);
	if (m->fname == NULL)
		return NULL;

	rpath = RPATH(path);

	c = strrchr(path, '/');
	dir = strndupa(path, c - path + 1);

	if (fstatat(m3ufs2.dirfd, rpath, &stbuf, 0) != 0)
		return NULL;
	m->mtime = stbuf.st_mtime;

	fd = openat(m3ufs2.dirfd, rpath, O_RDONLY);
	if (fd == -1)
		return NULL;
	f = fdopen(fd, "r");
	if (f == NULL)
		return NULL;
	fd = -1;

	errno = 0;
	while (getline(&line, &line_size, f) != -1) {
		struct m3u_entry *entry;
		char str[PATH_MAX];
		char *fname;

		line[strlen(line) - 1] = '\0';

		// Skip leading white space.
		for (fname = line; *fname != '\0' && isspace(*fname) != 0; fname++);

		if (firstline == NULL) {
			// Check for extended M3U format.
			firstline = line;
			if (strncmp(firstline, EXTM3U, sizeof(EXTM3U) - 1) == 0) {
				is_extm3u = true;

				continue;
			}
		}

		if (is_extm3u == true &&
		    strncmp(line, EXTINF, sizeof(EXTINF) - 1) == 0) {
			c = strchr(line, ',');
			if (c != NULL) {
				while (isspace(*(++c)) != 0);

				if (extinf_name != NULL)
					free(extinf_name);
				extinf_name = strdup(c);
			}

			continue;
		}

		// Skip empty lines and comments.
		if (fname[0] == '\0' || fname[0] == '#')
			continue;

		// Skip absolute path.
		if (fname[0] == '/')
			continue;

		if (m->len == m->size) {
			struct m3u_entry *new_entry;

			m->size += M3U_ENTRY_ALLOC_BY;
			new_entry = realloc(m->entry, m->size * sizeof(*entry));
			if (new_entry == NULL)
				return NULL;

			m->entry = new_entry;
		}
		entry = &m->entry[m->len++];
		memset(entry, 0, sizeof(*entry));

		snprintf(str, sizeof(str), "%s%s", dir, fname);
		entry->path = strdup(str);
		if (entry->path == NULL)
			return NULL;

		if (extinf_name != NULL) {
			entry->name = extinf_name;
			extinf_name = NULL;

			continue;
		}

		c = strrchr(fname, '/');
		if (c != NULL)
			c++;
		else
			c = fname;
		entry->name = strdup(c);
		if (entry->name == NULL)
			return NULL;
	};

	if (errno != 0)
		return NULL;

	m->count_order = num_order(m->len);

	m->next = m3u;
	m3u = m;
	m = NULL;

	return m3u;
}

static void m3u_unlock_(pthread_mutex_t **mutex)
{
	if (*mutex == NULL)
		return;

	pthread_mutex_unlock(*mutex);
}

static pthread_mutex_t *m3u_lock(void)
{
	if (pthread_mutex_lock(&m3u_mutex) != 0)
		return NULL;

	return &m3u_mutex;
}

static struct m3u *m3u_lookup(const char *path)
{
	pthread_mutex_t *mutex __attribute__((cleanup(m3u_unlock_))) = NULL;

	mutex = m3u_lock();
	if (mutex == NULL)
		return NULL;

	for (struct m3u *m = m3u, *m_prev = NULL; m != NULL; m_prev = m, m = m->next) {
		const char *rpath;
		struct stat stbuf;

		if (strcmp(m3u->fname, path) != 0)
			continue;

		rpath = RPATH(path);
		if (fstatat(m3ufs2.dirfd, rpath, &stbuf, 0) != 0)
			return NULL;

		if (stbuf.st_mtime == m->mtime)
			return m;

		// We are to update.
		if (m_prev != NULL)
			m_prev->next = m->next;
		else
			m3u = m->next;

		m3u_free(m);

		break;
	}

	return m3u_process(path);;
}

static struct m3u *is_m3u_entry(const char *path)
{
	char *dir;
	char *c;

	c = strrchr(path, '/');

	dir = strndupa(path, c - path);
	if (IS_M3U(dir) == true)
		return m3u_lookup(dir);

	return NULL;
}

static struct m3u_entry *m3u_entry_lookup(struct m3u *m, const char *name)
{
	pthread_mutex_t *mutex __attribute__((cleanup(m3u_unlock_))) = NULL;
	char *endptr = NULL;
	uint32_t num;

	mutex = m3u_lock();
	if (mutex == NULL)
		return NULL;

	num = strtoul(name, &endptr, 10);
	if (endptr == NULL || strncmp(endptr, ". ", 2) != 0)
		return NULL;
	name = endptr + 2;

	if (num > m->len ||
	    strcmp(name, m->entry[num - 1].name) != 0)
		return NULL;

	return &m->entry[num - 1];
}

static void m3u_shuffle(struct m3u *m)
{
	pthread_mutex_t *mutex __attribute__((cleanup(m3u_unlock_))) = NULL;
	uint32_t len = m->len;

	mutex = m3u_lock();
	if (mutex == NULL)
		return;

	while (len != 0) {
		uint32_t i;

		i = rand() % (len--);

		SWAP(&m->entry[i], &m->entry[len]);
	}
}

static int m3ufs2_getattr(const char *path, struct stat *stbuf)
{
	const char *rpath;
	struct m3u *m;

	if (path[0] != '/')
		return -ENOENT;

	m = is_m3u_entry(path);
	if (m != NULL) {
		struct m3u_entry *entry;
		char *entry_name;

		entry_name = strrchr(path, '/') + 1;

		entry = m3u_entry_lookup(m, entry_name);
		if (entry == NULL)
			return -ENOENT;

		return m3ufs2_getattr(entry->path, stbuf);
	}

	rpath = RPATH(path);

	if (fstatat(m3ufs2.dirfd, rpath, stbuf, 0) != 0)
		return -errno;

	if (IS_M3U(path) == true) {
		stbuf->st_mode &= ~S_IFREG;
		stbuf->st_mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
	}

	return 0;
}

#define DIRENT_READAHEAD_SIZE 1024
static int m3ufs2_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			  off_t offset __attribute__((unused)),
			  struct fuse_file_info *fi __attribute__((unused)))
{
	const char *rpath;
	int dirfd __attribute__((cleanup(close_))) = -1;

	if (path[0] != '/')
		return -ENOENT;

	if (IS_M3U(path) == true) {
		struct m3u *m;
		unsigned i;
		struct m3u_entry *entry;

		errno = 0;
		m = m3u_lookup(path);
		if (m == NULL) {
			if (errno == 0)
				return -ENOENT;

			return -errno;
		}
		if (m3ufs2.shuffle != 0)
			m3u_shuffle(m);

		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		for (i = 0, entry = m->entry; i < m->len; i++, entry++) {
			struct stat stbuf;
			char name[PATH_MAX];

			if (m3ufs2_getattr(entry->path, &stbuf) != 0) {
				if (errno != ENOENT)
					return -errno;

				continue;
			}

			snprintf(name, sizeof(name), "%.*u. %s",
				 m->count_order, i + 1, entry->name);
			filler(buf, name, &stbuf, 0);
		}

		return 0;
	}

	rpath = RPATH(path);

	dirfd = openat(m3ufs2.dirfd, rpath, O_DIRECTORY | O_RDONLY);
	if (dirfd == -1)
		return -errno;

	do {
		char dirent_buf[DIRENT_READAHEAD_SIZE];
		int count;

		count = getdents(dirfd, (struct linux_dirent *)dirent_buf,
				 DIRENT_READAHEAD_SIZE);
		if (count < 0)
			return count;

		if (count == 0)
			return 0;

		for (struct linux_dirent *d = (struct linux_dirent *)dirent_buf;
		     (char *)d < dirent_buf + count;
		     d = (struct linux_dirent *)((char *)d + d->d_reclen)) {
			char epath[PATH_MAX];
			struct stat stbuf;

			snprintf(epath, sizeof(epath), "%s/%s", path, d->d_name);

			if (m3ufs2_getattr(epath, &stbuf) != 0)
				return -errno;

			filler(buf, d->d_name, &stbuf, 0);
		};
	} while (1);

	return 0;
}

static int m3ufs2_open(const char *path, struct fuse_file_info *fi)
{
	const char *rpath;
	struct m3u *m;

	if (path[0] != '/')
		return -ENOENT;

	m = is_m3u_entry(path);
	if (m != NULL) {
		struct m3u_entry *entry;
		char *entry_name;

		entry_name = strrchr(path, '/') + 1;

		entry = m3u_entry_lookup(m, entry_name);
		if (entry == NULL)
			return -ENOENT;

		return m3ufs2_open(entry->path, fi);
	}

	rpath = RPATH(path);

	fi->fh = openat(m3ufs2.dirfd, rpath, O_RDONLY);
	if ((int)fi->fh == -1)
		return -errno;

	return 0;
}

static int m3ufs2_read(const char *path __attribute__((unused)),
		       char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	return pread(fi->fh, buf, size, offset);
}

static int m3ufs2_release(const char *path __attribute__((unused)), struct fuse_file_info *fi)
{
	if (close(fi->fh) != 0)
		return -errno;

	return 0;
}

static struct fuse_operations m3ufs2_op = {
	.getattr	= m3ufs2_getattr,
	.readdir	= m3ufs2_readdir,
	.open		= m3ufs2_open,
	.read		= m3ufs2_read,
	.release	= m3ufs2_release
};

static void usage(const char *progname)
{
	printf("usage: %s dir mountpoint [options]\n"
	       "\n"
	       "general options:\n"
	       "    -o opt,[opt...]        mount options\n"
	       "    -h   --help            print help\n"
	       "    -V   --version         print version\n"
	       "\n"
	       "M3UFS2 options:\n"
	       "    -o shuffle             shuffle playlist\n"
	       "\n",
	       progname);
}

#define OPT_MAX 256
static int m3ufs2_opt_proc(void *data, const char *arg, int key,
			   struct fuse_args *outargs __attribute__((unused)))
{
	struct m3ufs2 *m3ufs2 = data;

	switch (key) {
	case FUSE_OPT_KEY_NONOPT:
		if (m3ufs2->dir == NULL)
			m3ufs2->dir = strdup(arg);
		else if (m3ufs2->mountpoint == NULL) {
			m3ufs2->mountpoint = strdup(arg);

			fuse_opt_add_arg(outargs, arg);
		} else {
			fprintf(stderr, "invalid options\n");

			exit(EXIT_FAILURE);
		}

		return 0;
	case FUSE_OPT_KEY_OPT: {
		char opt[OPT_MAX];

		snprintf(opt, sizeof(opt), "-o%s", arg);
		fuse_opt_add_arg(outargs, opt);

		return 0;
	}
	case KEY_HELP:
		usage(outargs->argv[0]);

		fuse_opt_add_arg(outargs, "-ho");
		fuse_main(outargs->argc, outargs->argv, &m3ufs2_op, NULL);

		exit(EXIT_SUCCESS);
	case KEY_VERSION:
		printf("M3UFS2 version: %s\n", VERSION);

		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &m3ufs2_op, NULL);

		exit(EXIT_SUCCESS);
	default:
		fprintf(stderr, "internal error\n");

		exit(EXIT_FAILURE);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char real_dir[PATH_MAX], real_mountpoint[PATH_MAX];

	if (fuse_opt_parse(&args, &m3ufs2, m3ufs2_opts, m3ufs2_opt_proc) != 0)
		exit(EXIT_FAILURE);

	if (realpath(m3ufs2.dir, real_dir) == NULL) {
		fprintf(stderr, "bad dir `%s': %m\n", m3ufs2.dir);

		exit(EXIT_FAILURE);
	}
	if (realpath(m3ufs2.mountpoint, real_mountpoint) == NULL) {
		fprintf(stderr, "bad mountpoint `%s': %m\n", m3ufs2.mountpoint);

		exit(EXIT_FAILURE);
	}

	if (strcmp(real_dir, real_mountpoint) == 0)
		fuse_opt_add_arg(&args, "-ononempty");

	m3ufs2.dirfd = open(m3ufs2.dir, O_DIRECTORY | O_RDONLY);
	if (m3ufs2.dirfd == -1)
		exit(EXIT_FAILURE);

	return fuse_main(args.argc, args.argv, &m3ufs2_op, NULL);
}
