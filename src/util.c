/*
 * interdiff - create incremental patch between two against a common source
 *
 * Utility functions.
 * Copyright (C) 2001  Marko Kreen
 * Copyright (C) 2001, 2003, 2009, 2011  Tim Waugh <twaugh@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#ifdef HAVE_ERROR_H
# include <error.h>
#endif /* HAVE_ERROR_H */
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */

#include "util.h"
#include "diff.h"
#include "patch_scanner.h"

/* safe malloc */
void *xmalloc (size_t size)
{
	void *res = malloc(size);
	if (!res)
		error (EXIT_FAILURE, errno, "malloc");
	return res;
}

void *xrealloc (void *ptr, size_t size)
{
	void *res = realloc (ptr, size);
	if (!res)
		error (EXIT_FAILURE, errno, "realloc");
	return res;
}

/* safe strdup */
char *xstrdup (const char *s)
{
	size_t len = strlen (s) + 1;
	char *result = xmalloc (len);
	memcpy (result, s, len);
	return result;
}

/* only copy the first n characters of s */
char *xstrndup (const char *s, const size_t n)
{
	char *result;
	result = xmalloc(n + 1);
	strncpy(result, s, n);
	result[n] = '\0';
	return result;
}

int xmkstemp (char *pattern)
{
	int fd = mkstemp (pattern);
	if (fd < 0)
		error (EXIT_FAILURE, errno, "mkstemp: %s", pattern);
	return fd;
}

FILE *xtmpfile (void)
{
	FILE *ret;
	char *tmpfname;
	char *tmpdir = getenv ("TMPDIR");
	size_t tmpdirlen;
	int fd;

	if (tmpdir == NULL || !strcmp (tmpdir, P_tmpdir))
		return tmpfile ();

	tmpdirlen = strlen (tmpdir);
	tmpfname = xmalloc (tmpdirlen + 8);
	strcpy (tmpfname, tmpdir);
	strcpy (tmpfname + tmpdirlen, "/XXXXXX");
	fd = mkstemp (tmpfname);
	ret = fdopen (fd, "w+b");
	if (ret == NULL)
		error (EXIT_FAILURE, errno, "fdopen");
	unlink (tmpfname);
	free (tmpfname);
	return ret;
}

FILE *redirectfd (FILE* fd)
{
	FILE *ret;
	char *tmpfname;
	char *tmpdir = getenv ("TMPDIR");
	size_t tmpdirlen;

	if (tmpdir == NULL) {
		tmpdir = P_tmpdir;
	}

	tmpdirlen = strlen (tmpdir);
	tmpfname = xmalloc (tmpdirlen + 8);
	strcpy (tmpfname, tmpdir);
	strcpy (tmpfname + tmpdirlen, "/XXXXXX");
	close(mkstemp(tmpfname));
	ret = freopen (tmpfname, "w+b", fd);
	if (ret == NULL)
		error (EXIT_FAILURE, errno, "freopen");
	unlink (tmpfname);
	free (tmpfname);
	return ret;
}

/*
 * Pattern operations.
 */
/* list of patterns */

struct patlist {
	struct patlist *next;
	char *pattern;
};

void patlist_add(struct patlist **dst, const char *s)
{
	struct patlist *item = xmalloc (sizeof *item);
	item->pattern = xstrdup (s);
	item->next = *dst;
	*dst = item;
}

void patlist_add_file(struct patlist **dst, const char *fn)
{
	FILE *fd;
	char *line = NULL;
	size_t linelen = 0;
	size_t len;

	fd = fopen (fn, "r");
	if (NULL == fd)
		return;

	while ((len = getline (&line, &linelen, fd)) != -1) {
		if (len < 1)
			/* Shouldn't really happen */
			continue;

		/* Remove '\n' from pattern */
		if ('\n' == line[len - 1]) {
			if (len == 1) /* only '\n' present */
				continue;

			line[len - 1] = '\0';
		}
		patlist_add (dst, line);
	}
	fclose (fd);
}

int patlist_match(struct patlist *list, const char *s)
{
	while (list) {
		if (!fnmatch (list->pattern, s, 0))
			return 1;
		list = list->next;
	}
	return 0;
}

void patlist_free(struct patlist **list)
{
	struct patlist *l, *next;
	for (l = *list; l; l = next) {
		next = l->next;
		free (l->pattern);
		free (l);
	}
	*list = NULL;
}


FILE *xopen (const char *name, const char *mode)
{
	FILE *f;

	f = fopen(name, mode);
	if (!f) {
		perror(name);
		exit(1);
	}
	return f;
}

FILE *xopen_seekable (const char *name, const char *mode)
{
	/* If it's seekable, good.  If not, we need to make it seekable. */
	FILE *f = xopen (name, mode);

	if (fseek (f, 0L, SEEK_SET) != 0) {
		const size_t buflen = 64 * 1024;
		char *buffer = xmalloc (buflen);
		FILE *tmp = xtmpfile();

		while (!feof (f)) {
			size_t count = fread (buffer, 1, buflen, f);
			if (count < 1)
				break;
			fwrite (buffer, count, 1, tmp);
		}

		free (buffer);
		fclose (f);

		f = tmp;
		fseek (f, 0L, SEEK_SET);
	}

	return f;
}

/* unzip if needed */
FILE *xopen_unzip (const char *name, const char *mode)
{
	const char *p, *zprog = NULL;
	FILE *fi, *fo;
	const size_t buflen = 64 * 1024;
	char *buffer;
	pid_t pid;
	int status;
	int any_data = 0;

	p = strrchr(name, '.');
	if (p != NULL) {
		if (!strcmp (p, ".bz2"))
			zprog = "bzcat";
		else if (!strcmp(p, ".gz"))
			zprog = "zcat";
	}
	if (zprog == NULL)
		return xopen_seekable (name, mode);

	buffer = xmalloc (buflen);
	fo = xtmpfile();
	fi = xpipe(zprog, &pid, "r", (char **) (const char *[]) { zprog, name, NULL });

	while (!feof (fi)) {
		size_t count = fread (buffer, 1, buflen, fi);
		if (ferror (fi)) {
			perror(name);
			exit(1);
		}
		if (count < 1)
			break;

		fwrite (buffer, count, 1, fo);
		if (ferror (fo))
			error (EXIT_FAILURE, errno, "writing temp file");

		any_data = 1;
	}

	free (buffer);
	fclose (fi);

	waitpid (pid, &status, 0);
	if (any_data == 0 && WEXITSTATUS (status) != 0)
	{
		fclose (fo);
		exit (1);
	}

	fseek (fo, 0L, SEEK_SET);

	return fo;
}

/* safe pipe/popen.  mode is either "r" or "w" */
FILE * xpipe(const char * cmd, pid_t *pid, const char *mode, char *const argv[])
{
	int fildes[2];
	int child;
	FILE *res;

	if (!mode || (*mode != 'r' && *mode != 'w'))
		error (EXIT_FAILURE, 0, "xpipe: bad mode: %s", mode);

	fflush (NULL);
	if (pipe (fildes) == -1)
		error (EXIT_FAILURE, errno, "pipe failed");
	child = fork ();
	if (child == -1) {
		perror ("fork");
		exit (1);
	}
	if (child == 0) {
		if (*mode == 'r') {
			close (fildes[0]);
			close (1);
			if (dup (fildes[1]) == -1)
				error (EXIT_FAILURE, errno, "dup failed");
			close (fildes[1]);
		} else {
			close (fildes[1]);
			close (1);
			if (dup(2) == -1)
				error (EXIT_FAILURE, errno, "dup failed");
			close (0);
			if (dup (fildes[0]) == -1)
				error (EXIT_FAILURE, errno, "dup failed");
			close (fildes[0]);
		}
		execvp (cmd, argv);
		/* Shouldn't get here */
		error (EXIT_FAILURE, errno, "execvp");
	}
	if (pid != NULL)
		*pid = child;

	if (*mode == 'r') {
		close (fildes[1]);
		res = fdopen (fildes[0], "r");
	} else {
		close (fildes[0]);
		res = fdopen (fildes[1], "w");
	}
	if (res == NULL)
		error (EXIT_FAILURE, errno, "fdopen");

	return res;
}



char *progname = "(null)"; /* for error() */
void set_progname(const char *s)
{
	progname = xstrdup(s);
}

/* Safe in-place file writing using atomic rename */
int write_file_inplace(const char *filename, FILE *content)
{
	char *temp_name = NULL;
	FILE *temp_file = NULL;
	int temp_fd = -1;
	int ret = -1;
	size_t filename_len;
	const char temp_suffix[] = ".tmp.XXXXXX";

	if (!filename || !content) {
		error(0, 0, "write_file_inplace: invalid arguments");
		return -1;
	}

	/* Create temporary filename */
	filename_len = strlen(filename);
	temp_name = xmalloc(filename_len + sizeof(temp_suffix));
	strcpy(temp_name, filename);
	strcat(temp_name, temp_suffix);

	/* Create temporary file */
	temp_fd = xmkstemp(temp_name);
	temp_file = fdopen(temp_fd, "w");
	if (!temp_file) {
		error(0, errno, "failed to open temporary file %s", temp_name);
		close(temp_fd);
		unlink(temp_name);
		goto cleanup;
	}

	/* Copy content to temporary file */
	rewind(content);
	while (!feof(content)) {
		int ch = fgetc(content);
		if (ch == EOF)
			break;
		if (fputc(ch, temp_file) == EOF) {
			error(0, errno, "failed to write to temporary file %s", temp_name);
			goto cleanup;
		}
	}

	/* Ensure all data is written */
	if (fflush(temp_file) != 0) {
		error(0, errno, "failed to flush temporary file %s", temp_name);
		goto cleanup;
	}

	fclose(temp_file);
	temp_file = NULL;

	/* Atomically replace original file */
	if (rename(temp_name, filename) != 0) {
		error(0, errno, "failed to rename %s to %s", temp_name, filename);
		goto cleanup;
	}

	ret = 0; /* success */

cleanup:
	if (temp_file)
		fclose(temp_file);
	if (temp_name) {
		if (ret != 0)
			unlink(temp_name); /* cleanup on failure */
		free(temp_name);
	}
	return ret;
}

/* Patch-specific utility functions */

/**
 * Check if a file exists based on filename and timestamp.
 *
 * This function determines file existence by:
 * 1. Returning 0 (false) if filename is "/dev/null"
 * 2. Parsing the timestamp and checking if it's an epoch timestamp
 * 3. Returning 0 (false) for epoch timestamps (indicating deleted files)
 * 4. Returning 1 (true) for normal timestamps
 *
 * @param filename The filename from the patch header
 * @param timestamp The timestamp portion from the patch header
 * @return 1 if file exists, 0 if it doesn't exist (deleted)
 */
int patch_file_exists(const char *filename, const char *timestamp)
{
	struct tm t;
	long zone = -1;

	if (!strcmp (filename, "/dev/null"))
		return 0;

	if (read_timestamp (timestamp, &t, &zone))
		return 1;

	/* If the time is less that fifteen hours either side of the
	 * start of 1970, and it's an exact multiple of 15 minutes, it's
	 * very likely to be the result of ctime(&zero). */
	if (t.tm_sec == 0 &&
	    ((t.tm_year == 69 && t.tm_mon == 11 && t.tm_mday == 31 &&
	      t.tm_hour >= 9) ||
	     (t.tm_year == 70 && t.tm_mon == 0 && t.tm_mday == 1 &&
	      t.tm_hour <= 15)) &&
	    (t.tm_min % 15) == 0) {
		if (zone != -1) {
			/* Extra checking, since we know the timezone. */
			long offset = 0;
			if (t.tm_year == 69) {
				offset = 100 * (t.tm_hour - 24);
				if (t.tm_min)
					offset += 100 + t.tm_min - 60;
			} else {
				offset = 100 * t.tm_hour;
				offset += t.tm_min;
			}

			if (offset != zone)
				return 1;
		}

		return 0;
	}

	/* Otherwise, it's a real file timestamp. */
	return 1;
}

/**
 * Determine file status character from patch headers.
 *
 * @param headers Parsed patch headers
 * @param empty_as_absent Whether empty files should be treated as absent (-E flag)
 * @return Status character: '+' (new), '-' (deleted), '!' (modified)
 */
char patch_determine_file_status(const struct patch_headers *headers, int empty_as_absent)
{
	int old_file_exists = 1;
	int new_file_exists = 1;

	if (headers->type == PATCH_TYPE_GIT_EXTENDED) {
		/* For Git diffs, use the git_type to determine existence */
		switch (headers->git_type) {
		case GIT_DIFF_NEW_FILE:
			old_file_exists = 0;
			new_file_exists = 1;
			break;
		case GIT_DIFF_DELETED_FILE:
			old_file_exists = 1;
			new_file_exists = 0;
			break;
		case GIT_DIFF_RENAME:
		case GIT_DIFF_PURE_RENAME:
		case GIT_DIFF_COPY:
		case GIT_DIFF_MODE_ONLY:
		case GIT_DIFF_MODE_CHANGE:
		case GIT_DIFF_NORMAL:
		case GIT_DIFF_BINARY:
		default:
			old_file_exists = 1;
			new_file_exists = 1;
			break;
		}
	} else {
		/* For unified/context diffs, check filenames and timestamps */

		/* First check for /dev/null filenames */
		if (headers->old_name && !strcmp(headers->old_name, "/dev/null")) {
			old_file_exists = 0;
		}
		if (headers->new_name && !strcmp(headers->new_name, "/dev/null")) {
			new_file_exists = 0;
		}

		/* Then check timestamps if both files have real names */
		if (headers->old_name && headers->new_name &&
		    strcmp(headers->old_name, "/dev/null") != 0 &&
		    strcmp(headers->new_name, "/dev/null") != 0) {

			int found_timestamp = 0;
			for (unsigned int i = 0; i < headers->num_headers; i++) {
				const char *line = headers->header_lines[i];
				if (strncmp(line, "--- ", 4) == 0) {
					/* Skip past "--- " and filename, find timestamp */
					const char *tab = strchr(line + 4, '\t');
					if (tab) {
						found_timestamp = 1;
						if (headers->type == PATCH_TYPE_CONTEXT) {
							/* In context diffs, --- refers to the new file */
							new_file_exists = patch_file_exists(headers->new_name, tab + 1);
						} else {
							/* In unified diffs, --- refers to the old file */
							old_file_exists = patch_file_exists(headers->old_name, tab + 1);
						}
					}
				} else if (strncmp(line, "+++ ", 4) == 0) {
					/* Skip past "+++ " and filename, find timestamp */
					const char *tab = strchr(line + 4, '\t');
					if (tab) {
						found_timestamp = 1;
						new_file_exists = patch_file_exists(headers->new_name, tab + 1);
					}
				} else if (strncmp(line, "*** ", 4) == 0 && headers->type == PATCH_TYPE_CONTEXT) {
					/* Context diff old file header: *** old_file timestamp */
					const char *tab = strchr(line + 4, '\t');
					if (tab) {
						found_timestamp = 1;
						old_file_exists = patch_file_exists(headers->old_name, tab + 1);
					}
				}
			}

			/* For context diffs without timestamps, use filename heuristics */
			if (!found_timestamp && headers->type == PATCH_TYPE_CONTEXT) {
				/* If filenames are different, this might be a rename/new/delete case */
				if (strcmp(headers->old_name, headers->new_name) != 0) {
					/* Use empty-as-absent logic to determine the actual status */
					/* This will be handled below in the empty_as_absent section */
					/* For now, keep both as existing and let empty analysis decide */
				}
			}
		}
	}

	/* Handle empty_as_absent logic */
	if (empty_as_absent && old_file_exists && new_file_exists) {
		/* Both files exist, but check if one is effectively empty based on hunk data */
		int old_is_empty = 1;  /* Assume empty until proven otherwise */
		int new_is_empty = 1;  /* Assume empty until proven otherwise */

		/* Parse hunk headers from the patch to determine if files are empty */
		for (unsigned int i = 0; i < headers->num_headers; i++) {
			char *line = headers->header_lines[i];

			/* Look for unified diff hunk headers: @@ -offset,count +offset,count @@ */
			if (strncmp(line, "@@ ", 3) == 0) {
				unsigned long orig_count = 1, new_count = 1;  /* Default counts */
				char *p;

				/* Find original count after '-' */
				p = strchr(line, '-');
				if (p) {
					p++;
					/* Skip offset */
					strtoul(p, &p, 10);
					/* Look for count after comma */
					if (*p == ',') {
						p++;
						orig_count = strtoul(p, NULL, 10);
					}
					/* If no comma, count is 1 (already set) */
				}

				/* Find new count after '+' */
				p = strchr(line, '+');
				if (p) {
					p++;
					/* Skip offset */
					strtoul(p, &p, 10);
					/* Look for count after comma */
					if (*p == ',') {
						p++;
						new_count = strtoul(p, NULL, 10);
					}
					/* If no comma, count is 1 (already set) */
				}

				/* If any hunk has content, the file is not empty */
				if (orig_count > 0) {
					old_is_empty = 0;
				}
				if (new_count > 0) {
					new_is_empty = 0;
				}
			}
			/* Handle context diff hunk headers: *** offset,count **** */
			else if (strncmp(line, "*** ", 4) == 0 && strstr(line, " ****")) {
				char *comma = strchr(line + 4, ',');
				unsigned long orig_count;
				if (comma) {
					orig_count = strtoul(comma + 1, NULL, 10);
				} else {
					/* Single number format: *** number **** */
					char *space = strstr(line + 4, " ****");
					if (space) {
						*space = '\0'; /* Temporarily null-terminate */
						orig_count = strtoul(line + 4, NULL, 10);
						*space = ' '; /* Restore the space */
					} else {
						orig_count = 1; /* Fallback */
					}
				}
				if (orig_count > 0) {
					old_is_empty = 0;
				}
			}
			/* Handle context diff new file headers: --- offset,count ---- */
			else if (strncmp(line, "--- ", 4) == 0 && strstr(line, " ----")) {
				char *comma = strchr(line + 4, ',');
				unsigned long new_count;
				if (comma) {
					new_count = strtoul(comma + 1, NULL, 10);
				} else {
					/* Single number format: --- number ---- */
					char *space = strstr(line + 4, " ----");
					if (space) {
						*space = '\0'; /* Temporarily null-terminate */
						new_count = strtoul(line + 4, NULL, 10);
						*space = ' '; /* Restore the space */
					} else {
						new_count = 1; /* Fallback */
					}
				}
				if (new_count > 0) {
					new_is_empty = 0;
				}
			}
		}

		/* Apply empty-as-absent logic */
		if (old_is_empty && !new_is_empty) {
			return '+'; /* Treat as new file (old was empty) */
		} else if (!old_is_empty && new_is_empty) {
			return '-'; /* Treat as deleted file (new is empty) */
		}
		/* If both empty or both non-empty, fall through to normal logic */
	}

	/* Determine status based on file existence */
	if (!old_file_exists && new_file_exists)
		return '+'; /* New file */
	else if (old_file_exists && !new_file_exists)
		return '-'; /* Deleted file */
	else
		return '!'; /* Modified file */
}

