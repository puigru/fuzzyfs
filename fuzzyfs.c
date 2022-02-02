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

/*
 * If the requested path is '/', returns DOT.
 * If the requested path starts with '/', strips the leading '/' off.
 * Leaves the string otherwise untouched.
 */
const char* fix_path(const char* path)
{
	// Make p, which points to the same thing as path.
	const char *p = path;

	// If the string starts with '/',
	if (p[0] == '/')
	{
		// If the next character is the null terminator
		// (that is, the whole string is just '/'),
		if (p[1] == '\0')
			// Return DOT.
			return DOT;
		// If the next character is not the null terminator,
		// The string only starts with '/'. Skip the first character.
		p++;
	}
	// If the string doesn't start with '/', leave it untouched.
	// Return the string.
	return p;
}

/* Get the correct case for a file path by searching case-insenitively for matches.
 * Input: path - a string holding the path that you want to correct the case of.
 * This will iterate over slash-delimited chunks of path. On each iteration, it corrects
 * the case of the current chunk (if correction is needed) by looking for files in the
 * current chunk's parent directory (constructed from previous case-corrected chunks) that
 * case-insensitively match the current chunk. If one is found, the current chunk is corrected.
 * This repeats until the entire path is case-corrected. The case-corrected path is returned.
*/
char* fix_path_case(const char* path)
{
	char *p;
	DIR *dp;
	struct dirent *de;
	struct stat s = { 0 };
	int len, found;
	char *token, *parent, *saveptr;

	// p is a copy of path. Note: this allocates new memory.
	p = strdup(path);
	// Split p on slashes. saveptr will follow along so that we can call it again to get the next token.
	token = strtok_r(p, "/", &saveptr);
	// Keep going until we get a null back - that is, loop over all slash-delimited chunks of p.
	while (token != NULL)
	{
		// len is how far into the string the current chunk is.
		len = token - p;
		// If we're not on the first chunk, which would have len = 0,
		if (len)
		{
			// strtok_r replaces all the delimiters with nulls so that chunks can be treated as
			// their own separate strings. This will be a useful property for the current chunk,
			// but we will need the delimiter in place on previous chunks. This line restores the
			// delimiter directly before this chunk.
			*(token - 1) = '/'; // restore delimiter
		}

		// If the current capitalization of the path (up to the current chunk) is incorrect,
		// (that is, if getting info about the currently-specified chunk returns a nonzero exit code)
		// Remember, strtok_r will place a null terminator after the current chunk, so we're not
		// doing the whole path, just from the string beginning to the null terminator.
		if (lstat(p, &s))
		{
			// If we're not on the first chunk,
			if (len)
			{
				// Fill parent with the portion of p preceding this chunk.
				// Note: this allocates new memory!
				parent = (char*)malloc(len + 1);
				strncpy(parent, p, len);
				// And remember to null-terminate.
				parent[len] = '\0';
			}
			// Else, we are on the first chunk.
			else
			{
				// parent is just DOT. Also allocates new memory.
				parent = strdup(DOT);
			}

			// Open the directory.
			dp = opendir(parent);
			// If the directory doesn't exist or isn't a directory or we don't have access (unlikely, we're root.)
			if (dp == NULL)
			{
				// Free some memory and return null.
				free(p);
				p = NULL;
				free(parent);
				parent = NULL;
				return NULL;
			}

			// We haven't found the next portion yet. To be fair, we haven't started looking.
			found = FALSE;
			// For each filename in the parent directory,
			// Note: don't free de. It's managed separately.
			while ((de = readdir(dp)) != NULL)
			{
				// See if we can find a filename that case-insensitively matches the current chunk.
				if (strcasecmp(de->d_name, token) == 0)
				{
					// Wow, we found it! Log the name change.
					printf("%s --> %s\n", token, de->d_name);
					// Also, change the current chunk to the case-changed version.
					strcpy(token, de->d_name);
					// We found it, so we can change the variable.
					found = TRUE;
					break;
				}
			}
			// Close the directory.
			closedir(dp);
			// parent isn't needed anymore, free it and make it null.
			free(parent);
			parent = NULL;

			// If we didn't find anything,
			if (!found)
			{
				// The file or directory doesn't exist in any capitalization.
				// Free p, set it to null, and return.
				free(p);
				p = NULL;
				return NULL;
			}
		}

		// Move to the next chunk of the string.
		token = strtok_r(NULL, "/", &saveptr);
	}

	return p;
}

// Gets file attributes.
static int fuzzyfs_getattr(const char *path, struct stat *stbuf)
{
	int res;
	char *p;

	// Fix the path. and try to get the file attributes.
	p = (char*)fix_path(path);
	// The file attributes should be put in stbuf.
	res = lstat(p, stbuf);
	// It worked? Return zero.
	if (!res)
	{
		return 0;
	}

	// If the error code is anything but ENOENT ("file not found"), return it.
	if (errno != ENOENT)
	{
		return -errno;
	}

	// The error was ENOENT.
	// See if fix_path_case finds anything.
	// Note: this allocates new memory for p, unless it returns an error.
	if (!(p = fix_path_case(p)))
	{
		// It doesn't. Return ENOENT.
		return -ENOENT;
	}

	// fix_path_case did find something. Put that thing's file attributes in stbuf.
	res = lstat(p, stbuf);
	// Free the memory for p.
	free(p);
	p = NULL;
	// Unless lstat errored out, return zero.
	assert(res != -1);
	return 0;
}

// Reads the contents of a directory.
static int fuzzyfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			   off_t offset, struct fuse_file_info *fi)
{
	DIR *dp;
	struct dirent *de;
	char *p;

	(void) offset;
	(void) fi;

	// Increment past the leading slash, if any.
	p = (char*)fix_path(path);
	// Try to open the directory normally.
	dp = opendir(p);
	// If it didn't work,
	if (dp == NULL)
	{
		// If the error was anything but ENOENT, return that error.
		if (errno != ENOENT)
		{
			return -errno;
		}

		// If the error was an ENOENT, check if fix_path_case fixes it.
		if (!(p = fix_path_case(p)))
		{
			// No? Return ENOENT.
			return -ENOENT;
		}

		// Open the fixed directory.
		dp = opendir(p);
		// fix_path_case allocated new memory. Free it.
		free(p);
		p = NULL;
		// If the directory failed to open, bail out.
		assert(dp != NULL);
	}

	// At this point, we have either bailed out or placed the directory stream in dp.
	while ((de = readdir(dp)) != NULL)
	{
		// Make a new stat struct.
		struct stat st;
		// Zero it out.
		memset(&st, 0, sizeof(st));
		// Copy the inode and mode.
		st.st_ino = de->d_ino;
		// TODO: figure out what this is doing. Why are we setting the mode from the type?
		st.st_mode = de->d_type << 12;
		// Call the magic FUSE filler function.
		if (filler(buf, de->d_name, &st, 0))
		{
			break;
		}
	}
	// Close the directory and return zero.
	closedir(dp);
	return 0;
}

// Basic check that a file exists.
static int fuzzyfs_open(const char *path, struct fuse_file_info *fi)
{
	int res;
	char *p;

	// Increment past the leading slash, if any.
	p = (char*)fix_path(path);
	// Try to open the file normally.
	res = open(p, fi->flags);

	// If it worked...
	if (res != -1)
	{
		// Close the file descriptor and return zero.
		close(res);
		return 0;
	}

	// If it gave an error other than ENOENT, return that error.
	if (errno != ENOENT)
	{
		return -errno;
	}

	// If it gave ENOENT, see if fix_path_case can fix it.
	if (!(p = fix_path_case(p)))
	{
		// If it can't, return ENOENT.
		return -ENOENT;
	}

	// If it can, open it.
	res = open(p, fi->flags);
	// Then free p (allocated by fix_path_case) and close the file descriptor.
	free(p);
	p = NULL;
	close(res);
	// As long as there were no errors, return zero.
	assert(res != -1);
	return 0;
}

// Read size bytes from the given file into the buffer buf, beginning offset bytes into the file.
static int fuzzyfs_read(const char *path, char *buf, size_t size, off_t offset,
			struct fuse_file_info *fi)
{
	int fd;
	int res;
	char *p;

	(void) fi;

	// Increment past the leading slash, if any.
	p = (char*)fix_path(path);
	// Try to open it read-only without modifications.
	fd = open(p, O_RDONLY);
	// If it failed...
	if (fd == -1)
	{
		// If the error was not an ENOENT, return that error.
		if (errno != ENOENT)
		{
			return -errno;
		}

		// If it gave ENOENT, see if fix_path_case can fix it.
		if (!(p = fix_path_case(p)))
		{
			// If it can't, return ENOENT.
			return -ENOENT;
		}

		// Open the case_corrected file read-only.
		fd = open(p, O_RDONLY);
		// Free the memory that fix_path_case allocated.
		free(p);
		p = NULL;
		// Assert that we opened it correctly.
		assert(fd != -1);
	}

	// Read from the file descriptor.
	res = pread(fd, buf, size, offset);
	// If there was an error, pass it through.
	if (res == -1)
	{
		res = -errno;
	}

	// Close the file descriptor.
	close(fd);
	// Return whatever retval we get.
	return res;
}

static void *fuzzyfs_init(struct fuse_conn_info *conn)
{
	// cd into the root directory, wherever that is.
	if (chdir(root) == -1)
	{
		// If it didn't work, throw some errors.
		perror("chdir");
		exit(1);
	}

	return NULL;
}

static int fuzzyfs_opt_parse(void *data, const char *arg, int key,
			     struct fuse_args *outargs)
{
	// If root is unset and we're handling a positional argument,
	// Note: this will be triggered only by the first argument.
	if (!root && key == FUSE_OPT_KEY_NONOPT)
	{
		// Set root to the absolute version of that argument.

		// when fuse starts, it changes the workdir to the root
		// must resolve relative paths beforehand
		if (!(root = realpath(arg, NULL)))
		{
			perror(outargs->argv[0]);
			exit(1);
		}
		// It worked. Return success.
		return 0;
	}

	// Return failure.
	return 1;
}

// Setup the mapping between the fuse functions and the fuzzyfs functions.
static struct fuse_operations fuzzyfs_oper = {
	.getattr	= fuzzyfs_getattr,
	.readdir	= fuzzyfs_readdir,
	.open		= fuzzyfs_open,
	.read		= fuzzyfs_read,
	.init		= fuzzyfs_init,
};

int main(int argc, char *argv[])
{
	// Parse the args.
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	fuse_opt_parse(&args, NULL, NULL, fuzzyfs_opt_parse);
	// Set the umask to zero - all permissions are allowed.
	umask(0);
	// Call the fuse_main function to start everything.
	return fuse_main(args.argc, args.argv, &fuzzyfs_oper, NULL);
}
