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

static const char *DOT = ".";

const char *root = NULL;

/*
 * If the requested path is '/', returns a pointer to the static DOT.
 * If the requested path starts with '/', increments the pointer past
 * the slash and returns the incremented pointer.
 * Leaves the string otherwise untouched.
 * Does not allocate any memory.
 */
const char* fix_path(const char *path)
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

/* Get the correct case for a file path by searching case-insenitively for matches.
 * Input: path - a string holding the path that you want to correct the case of.
 * This will iterate over slash-delimited chunks of path. On each iteration, it corrects
 * the case of the current chunk (if correction is needed) by looking for files in the
 * current chunk's parent directory (constructed from previous case-corrected chunks) that
 * case-insensitively match the current chunk. If one is found, the current chunk is corrected.
 * This repeats until the entire path is case-corrected. The case-corrected path is returned.
 *
 * A note on memory management: this allocates new memory for the return value if it succeeds.
 * If it fails, it will free all the memory that it allocated.
*/
char* fix_path_case(const char *path)
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

		// If the current capitalization of the path (up to the current chunk) is incorrect,
		// (that is, if getting info about the currently-specified chunk returns a nonzero exit code)
		// Remember, strtok_r will place a null terminator after the current chunk, so we're not
		// doing the whole path, just from the string beginning to the null terminator.
		if (lstat(p, &s))
		{
			if (len)
			{
				// Note: this allocates new memory!
				parent = (char*)malloc(len + 1);
				strncpy(parent, p, len);
				parent[len] = '\0';
			}
			else
			{
				// Note: allocates new memory.
				parent = strdup(DOT);
			}

			dp = opendir(parent);
			if (dp == NULL)
			{
				free(p);
				p = NULL;
				free(parent);
				parent = NULL;
				return NULL;
			}

			found = FALSE;
			// Note: don't free de. It's managed separately.
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
			// parent isn't needed anymore.
			free(parent);
			parent = NULL;

			if (!found)
			{
				free(p);
				p = NULL;
				return NULL;
			}
		}

		token = strtok_r(NULL, "/", &saveptr);
	}

	return p;
}

// Gets file attributes, correcting the path's capitalization if needed.
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

	// Note: this allocates new memory for p, unless it returns an error.
	if (!(p = fix_path_case(p)))
		return -ENOENT;

	res = lstat(p, stbuf);
	free(p);
	p = NULL;
	assert(res != -1);
	return 0;
}

// Open a directory stream and put it in fi->fh.
static int fuzzyfs_opendir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp;
	char *p;

	p = (char*)fix_path(path);
	dp = opendir(p);
	if (dp == NULL)
	{
		if (errno != ENOENT)
			return -errno;

		// Note: allocates new memory for p.
		if (!(p = fix_path_case(p)))
			return -ENOENT;

		dp = opendir(p);
		// fix_path_case allocated new memory. Free it.
		free(p);
		p = NULL;
	}
	// fi->fh is a uint64_t, so we must cast. Casting directly to uint64_t
	// generates a compiler warning, so we use uintptr_t.
	fi->fh = (uintptr_t) dp;
	assert(dp != NULL);
	return 0;
}

// Reads the contents of a directory.
static int fuzzyfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			   off_t offset, struct fuse_file_info *fi)
{
	(void) path;
	(void) offset;

	struct dirent *de;

	// Including an intermediate unitptr_t cast avoids a compiler warning.
	while ((de = readdir((DIR *)(uintptr_t)fi->fh)) != NULL)
	{
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0))
			break;
	}

	return 0;
}

// Close the directory stream pointed to by fi->fh.
static int fuzzyfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	(void) path;

	int res;

	// Including an intermediate unitptr_t cast avoids a compiler warning.
        res = closedir((DIR *)(uintptr_t)fi->fh);
        if (res == -1)
                res = -errno;

        return res;
}

// Open a file handle and put it in fi->fh.
static int fuzzyfs_open(const char *path, struct fuse_file_info *fi)
{
	int res;
	char *p;

	p = (char*)fix_path(path);
	res = open(p, fi->flags);

	if (res != -1)
	{
		fi->fh = res;
		return 0;
	}

	if (errno != ENOENT)
		return -errno;

	// Allocates new memory for p.
	if (!(p = fix_path_case(p)))
		return -ENOENT;

	res = open(p, fi->flags);
	free(p);
	p = NULL;
	fi->fh = res;
	assert(res != -1);
	return 0;
}

// Read size bytes from the given file descriptor into the buffer buf, beginning offset bytes into the file.
static int fuzzyfs_read(const char *path, char *buf, size_t size, off_t offset,
			struct fuse_file_info *fi)
{
	(void) path;

	int res;

	res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

// Close the file descriptor.
static int fuzzyfs_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;

	int res;

	res = close(fi->fh);
	if (res == -1)
		res = -errno;

	return res;
}

/*
 * A function called at the filesystem startup.
 * Changes directory to the first argument (the source) so that
 * we can assume that the source is '.', instead of doing nasty
 * directory-symlink appending.
 */
static void *fuzzyfs_init(struct fuse_conn_info *conn)
{
	// cd into the root directory, wherever that is.
	if (chdir(root) == -1)
	{
		perror("chdir");
		exit(1);
	}

	return NULL;
}

// Parse the arguments. Notably, sets root to the first argument (the source).
static int fuzzyfs_opt_parse(void *data, const char *arg, int key,
			     struct fuse_args *outargs)
{
	// Note: this will be triggered only by the first argument.
	if (!root && key == FUSE_OPT_KEY_NONOPT)
	{
		// when fuse starts, it changes the workdir to the root
		// so we must resolve relative paths beforehand
		if (!(root = realpath(arg, NULL)))
		{
			perror(outargs->argv[0]);
			exit(1);
		}
		return 0;
	}

	return 1;
}

// Setup the mapping between the fuse functions and the fuzzyfs functions.
static struct fuse_operations fuzzyfs_oper = {
	.getattr	= fuzzyfs_getattr,
	.opendir	= fuzzyfs_opendir,
	.readdir	= fuzzyfs_readdir,
	.releasedir	= fuzzyfs_releasedir,
	.open		= fuzzyfs_open,
	.read		= fuzzyfs_read,
	.release	= fuzzyfs_release,
	.init		= fuzzyfs_init,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	fuse_opt_parse(&args, NULL, NULL, fuzzyfs_opt_parse);
	umask(0);
	return fuse_main(args.argc, args.argv, &fuzzyfs_oper, NULL);
}
