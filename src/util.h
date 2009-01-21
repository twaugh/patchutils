/*
 * interdiff - create incremental patch between two against a common source
 *
 * Utility functions.
 * Copyright (C) 2001  Marko Kreen
 * Copyright (C) 2001, 2003, 2009  Tim Waugh <twaugh@redhat.com>
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

/* GCC attributes */
#if !defined(__GNUC__) || __GNUC__ < 2 || \
    (__GNUC__ == 2 && __GNUC_MINOR__ < 5) || __STRICT_ANSI__
# define NORETURN
# define FORMAT(x)
#else /* GNU C: */
# define NORETURN __attribute__ ((__noreturn__))
# define FORMAT(x) __attribute__ ((__format__ x))
#endif

/* safe malloc */
void *xmalloc (size_t size);
void *xrealloc (void *tr, size_t size);
/* safe strdup */
char *xstrdup (const char *s);
/* safe strndup */
char *xstrndup (const char *s, const size_t n);
/* safe mkstemp */
int xmkstemp (char *pattern);
/* safe tmpfile */
FILE *xtmpfile (void);

FILE *xopen(const char *file, const char *mode);
FILE *xopen_seekable(const char *file, const char *mode);
FILE *xopen_unzip(const char *file, const char *mode);
FILE *xpipe(const char *cmd, pid_t *pid, const char *mode, ...);

struct patlist;

/* create a new item */
void patlist_add(struct patlist **dst, const char *s);
/* load whole list from file */
void patlist_add_file(struct patlist **dst, const char *fn);

/* match a string to all patterns */
int patlist_match(struct patlist *list, const char *s);

/* free rxlist */
void patlist_free(struct patlist **list);

extern char *progname;
void set_progname(const char * s);


/* for non-glibc systems */
#ifndef HAVE_GETLINE
ssize_t getline(char **line, size_t *n, FILE *f);
#endif

#ifndef HAVE_ERROR
extern void error (int status, int errnum, const char *format, ...)
	FORMAT ((__printf__, 3, 4));
#endif /* HAVE_ERROR */
