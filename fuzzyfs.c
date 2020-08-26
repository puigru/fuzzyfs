/*
 * fuzzyfs: Case-insensitive FUSE file system
 * Copyright (C) 2020  Joel Puig Rubio <joel.puig.rubio@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define FUSE_USE_VERSION 26
#define TRUE 1
#define FALSE 0

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* DOT = ".";

const char* root = NULL;

const char* fix_path(const char* path)
{
	const char *p = path;

	if (p[0] == '/')
	{
		if (p[1] == '\0')
			return DOT;
		p++;
	}

	return p;
}

char* fix_path_case(const char* path)
{
	char *p;
	DIR *dp;
	struct dirent *de;
	struct stat s = { 0 };
	int len, found;
	char *token, *parent, *saveptr;

	p = strdup(path);
	token = strtok_r(p, "/", &saveptr);
	while (token != NULL)
	{
		len = token - p;
		if (len)
			*(token - 1) = '/'; // restore delimiter

		if (lstat(p, &s))
		{
			if (len)
			{
				parent = (char*)malloc(len + 1);
				strncpy(parent, p, len);
				parent[len] = '\0';
			}
			else
				parent = strdup(DOT);

			dp = opendir(parent);
			if (dp == NULL)
			{
				free(p);
				free(parent);
				return NULL;
			}

			found = FALSE;
			while ((de = readdir(dp)) != NULL)
			{
				if (strcasecmp(de->d_name, token) == 0)
				{
					printf("%s --> %s\n", token, de->d_name);
					strcpy(token, de->d_name);
					found = TRUE;
					break;
				}
			}
			closedir(dp);

			if (!found)
			{
				free(p);
				free(parent);
				return NULL;
			}
		}

		token = strtok_r(NULL, "/", &saveptr);
	}

	return p;
}

static int fuzzyfs_getattr(const char *path, struct stat *stbuf)
{
	int res;
	char *p;

	p = (char*)fix_path(path);
	res = lstat(p, stbuf);
	if (!res)
		return 0;

	if (errno != ENOENT)
		return -errno;

	if (!(p = fix_path_case(p)))
		return -ENOENT;

	res = lstat(p, stbuf);
	free(p);
	assert(res != -1);
	return 0;
}

static int fuzzyfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			   off_t offset, struct fuse_file_info *fi)
{
	DIR *dp;
	struct dirent *de;
	char *p;

	(void) offset;
	(void) fi;

	p = (char*)fix_path(path);
	dp = opendir(p);
	if (dp == NULL)
	{
		if (errno != ENOENT)
			return -errno;

		if (!(p = fix_path_case(p)))
			return -ENOENT;

		dp = opendir(p);
		free(p);
		assert(dp != NULL);
	}

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0))
			break;
	}

	closedir(dp);
	return 0;
}

static int fuzzyfs_open(const char *path, struct fuse_file_info *fi)
{
	int res;
	char *p;

	p = (char*)fix_path(path);
	res = open(p, fi->flags);
	if (res != -1) {
		close(res);
		return 0;
	}

	if (errno != ENOENT)
		return -errno;

	if (!(p = fix_path_case(p)))
		return -ENOENT;

	res = open(p, fi->flags);
	free(p);
	close(res);
	assert(res != -1);
	return 0;
}

static int fuzzyfs_read(const char *path, char *buf, size_t size, off_t offset,
			struct fuse_file_info *fi)
{
	int fd;
	int res;
	char *p;

	(void) fi;

	p = (char*)fix_path(path);
	fd = open(p, O_RDONLY);
	if (fd == -1)
	{
		if (errno != ENOENT)
			return -errno;

		if (!(p = fix_path_case(p)))
			return -ENOENT;

		fd = open(p, O_RDONLY);
		free(p);
		assert(fd != -1);
	}

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	close(fd);
	return res;
}

static void *fuzzyfs_init(struct fuse_conn_info *conn)
{
	if (chdir(root) == -1)
	{
		perror("chdir");
		exit(1);
	}

	return NULL;
}

static int fuzzyfs_opt_parse(void *data, const char *arg, int key,
			     struct fuse_args *outargs)
{
	if (!root && key == FUSE_OPT_KEY_NONOPT)
	{
		// when fuse starts, it changes the workdir to the root
		// must resolve relative paths beforehand
		if (!(root = realpath(arg, NULL)))
		{
			perror(outargs->argv[0]);
			exit(1);
		}
		return 0;
	}

	return 1;
}

static struct fuse_operations fuzzyfs_oper = {
	.getattr	= fuzzyfs_getattr,
	.readdir	= fuzzyfs_readdir,
	.open		= fuzzyfs_open,
	.read		= fuzzyfs_read,
	.init		= fuzzyfs_init,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	fuse_opt_parse(&args, NULL, NULL, fuzzyfs_opt_parse);
	umask(0);
	return fuse_main(args.argc, args.argv, &fuzzyfs_oper, NULL);
}
