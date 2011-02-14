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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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
	char *line;
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
	char *p, *zprog = NULL;
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
	fi = xpipe(zprog, &pid, "r", zprog, name, NULL);
	
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
FILE * xpipe(const char * cmd, pid_t *pid, const char *mode, ...)
{
	va_list ap;
	int fildes[2];
	int child;
	int nargs = 0;
	char *argv[128], *arg;
	FILE *res;
	
	if (!mode || (*mode != 'r' && *mode != 'w'))
		error (EXIT_FAILURE, 0, "xpipe: bad mode: %s", mode);
	
	va_start(ap, mode);
	do {
		arg = va_arg(ap, char *);
		argv[nargs++] = arg;
		if (nargs >= 128)
			error (EXIT_FAILURE, 0, "xpipe: too many args");
	} while (arg != NULL);
	va_end(ap);
	
	fflush (NULL);
	pipe (fildes);
	child = fork ();
	if (child == -1) {
		perror ("fork");
		exit (1);
	}
	if (child == 0) {
		if (*mode == 'r') {
			close (fildes[0]);
			close (1);
			dup (fildes[1]);
			close (fildes[1]);
		} else {
			close (fildes[1]);
			close (1);
			dup(2);
			close (0);
			dup (fildes[0]);
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

/*
 * stuff needed for non-GNU systems
 */

#ifndef HAVE_GETLINE
#define GLSTEP 512

/* suboptimal implementation of glibc's getline() */
ssize_t getline(char **line, size_t *n, FILE *f)
{
	char *p;
	size_t len;
	
	if (*line == NULL || *n < 2) {
		p = realloc(*line, GLSTEP);
		if (!p)
			return -1;
		*line = p;
		*n = GLSTEP;
	}
	
	p = fgets(*line, *n, f);
	if (!p)
		return -1;
	
	len = strlen(p);
	while ((*line)[len - 1] != '\n') {
		p = realloc(*line, *n + GLSTEP);
		if (!p)
			return -1;
		*line = p;
		*n += GLSTEP;
		
		p = fgets(p + len, *n - len, f);
		if (!p)
			break;

		len = len + strlen(p);
	}
	return (ssize_t) len;
}

#endif

char *progname = "(null)"; /* for error() */
void set_progname(const char *s)
{
	progname = xstrdup(s);
}

