/*
 * interdiff - create incremental patch between two against a common source
 * combinediff - create cumulative patch from two incremental patches
 * flipdiff - exchange the order of two incremental patches
 * Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005, 2009, 2011 Tim Waugh <twaugh@redhat.com>
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

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#endif /* HAVE_ALLOCA_H */
#include <assert.h>
#ifdef HAVE_ERROR_H
# include <error.h>
#endif /* HAVE_ERROR_H */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <getopt.h>
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */

#include "util.h"
#include "diff.h"

#ifndef DIFF
#define DIFF "diff"
#endif

#ifndef PATCH
#define PATCH "patch"
#endif

/* Line type for coloring */
enum line_type {
	LINE_FILE,
	LINE_HEADER,
	LINE_HUNK,
	LINE_ADDED,
	LINE_REMOVED,
	LINE_TYPE_MAX
};

/* ANSI color codes for diff output */
static const char *const color_codes[LINE_TYPE_MAX] = {

	[LINE_FILE] = "\033[1m",      /* Bold for filenames */
	[LINE_HEADER] = "\033[1m",    /* Bold for headers */
	[LINE_HUNK] = "\033[36m",     /* Cyan for hunk headers */
	[LINE_ADDED] = "\033[32m",    /* Green for added lines */
	[LINE_REMOVED] = "\033[31m"   /* Red for removed lines */
};

/* This can be invoked as interdiff, combinediff, or flipdiff. */
static enum {
	mode_inter,
	mode_combine,
	mode_flip,
} mode;
static int flipdiff_inplace = 0;

struct file_list {
	char *file;
	long pos;
	struct file_list *next;
	struct file_list *tail;
};

struct lines {
	char *line;
	size_t length;
	unsigned long n;
	struct lines *next;
	struct lines *prev;
};

struct lines_info {
	char *unline;
	unsigned long first_offset;
	unsigned long min_context;
	struct lines *head;
	struct lines *tail;
};

static int human_readable = 1;
static char *diff_opts[100];
static int num_diff_opts = 0;
static unsigned int max_context_real = 3, max_context = 3;
static int context_specified = 0;
static int ignore_components = 0;
static int ignore_components_specified = 0;
static int unzip = 0;
static int no_revert_omitted = 0;
static int use_colors = 0;
static int color_option_specified = 0;
static int debug = 0;
static int fuzzy = 0;

static struct patlist *pat_drop_context = NULL;

static struct file_list *files_done = NULL;
static struct file_list *files_in_patch2 = NULL;
static struct file_list *files_in_patch1 = NULL;

/*
 * Print colored output using a variadic format string.
 */
static void __attribute__((__format__(printf, 3, 4)))
print_color (FILE *output_file, enum line_type type, const char *format, ...)
{
	const char *color_start = NULL;
	va_list args;

	/* Only colorize if colors are enabled AND we're outputting to stdout */
	if (use_colors && output_file == stdout)
		color_start = color_codes[type];

	/* Print color start code */
	if (color_start)
		fputs (color_start, output_file);

	/* Print the formatted content */
	va_start (args, format);
	vfprintf (output_file, format, args);
	va_end (args);

	/* Print color end code */
	if (color_start)
		fputs ("\033[0m", output_file);
}

 /* checks whether file needs processing and sets context */
static int
check_filename (const char *fn)
{
	if (patlist_match(pat_drop_context, fn))
		max_context = 0;
	else
		max_context = max_context_real;

	return 1;
}

static void
add_to_list (struct file_list **list, const char *file, long pos)
{
	struct file_list *make;
	make = xmalloc (sizeof *make);
	make->next = NULL;
	make->tail = NULL;
	make->file = xstrdup (file);
	make->pos = pos;

	if (*list) {
		(*list)->tail->next = make;
		(*list)->tail = make;
	} else {
		make->tail = make;
		*list = make;
	}
}

static long
file_in_list (struct file_list *list, const char *file)
{
	file = stripped (file, ignore_components);
	while (list) {
		if (!strcmp (stripped (list->file, ignore_components), file))
			return list->pos;
		list = list->next;
	}
	return -1;
}

/* Determine the best ignore_components value when none was specified.
 * Try different values from 0 up to a reasonable maximum, and find the
 * smallest value where at least one file from patch1 matches a file from patch2.
 */
static int
determine_ignore_components (struct file_list *list1, struct file_list *list2)
{
	int max_components = 0;
	int p;

	/* Find the maximum number of path components in any file */
	for (struct file_list *l = list1; l; l = l->next) {
		if (strcmp(l->file, "/dev/null") != 0) {
			int components = num_pathname_components(l->file);
			if (components > max_components)
				max_components = components;
		}
	}
	for (struct file_list *l = list2; l; l = l->next) {
		if (strcmp(l->file, "/dev/null") != 0) {
			int components = num_pathname_components(l->file);
			if (components > max_components)
				max_components = components;
		}
	}

	/* Try different -p values, starting from 0 */
	for (p = 0; p <= max_components; p++) {
		for (struct file_list *l1 = list1; l1; l1 = l1->next) {
			const char *stripped1 = stripped(l1->file, p);
			for (struct file_list *l2 = list2; l2; l2 = l2->next) {
				const char *stripped2 = stripped(l2->file, p);
				if (!strcmp(stripped1, stripped2))
					return p;
			}
		}
	}

	/* If no match found, return 0 as default */
	return 0;
}

static void
free_list (struct file_list *list)
{
	struct file_list *next;
	for (; list; list = next) {
		next = list->next;
		free (list->file);
		free (list);
	}
}

static void
insert_line_before (struct lines_info *lines,
		    struct lines *at, struct lines *make)
{
	make->next = at;
	make->prev = at->prev;
	if (at->prev)
		at->prev->next = make;
	else
		lines->head = make;
	at->prev = make;
}

/* Returns 1 is there is a conflict. */
static int
add_line (struct lines_info *lines, const char *line, size_t length,
	  unsigned long n)
{
	struct lines *make;
	struct lines *at;

	make = xmalloc (sizeof *make);
	make->n = n;
	make->line = xmalloc (length + 1);
	memcpy (make->line, line, length);
	make->line[length] = '\0';
	make->length = length;

	if (!lines->tail) {
		/* New */
		make->prev = NULL;
		make->next = NULL;
		lines->head = make;
		lines->tail = make;
		return 0;
	}
	if (lines->tail->n < n) {
		/* End */
		lines->tail->next = make;
		make->next = NULL;
		make->prev = lines->tail;
		lines->tail = make;
		return 0;
	}

	if (!lines->head)
		error (EXIT_FAILURE, 0, "List corrupted: no head");

	at = lines->head;
	while (at) {
		if (n < at->n) {
			insert_line_before (lines, at, make);
			return 0;
		}
		if (n == at->n) {
			free (make->line);
			free (make);
			if (at->length == length &&
			    !memcmp (at->line, line, length))
				/* Already there. */
				return 0;

			/* Clash: first one wins. */
			if (at->length < length)
				return -1;
			if (at->length > length)
				return 1;
			return memcmp (at->line, line, length);
		}
		make->prev = at;
		at = at->next;
	}

	if (!at)
		error (EXIT_FAILURE, 0, "List corrupted: ordering");

	return 1;
}

static void
merge_lines (struct lines_info *lines1, struct lines_info *lines2)
{
	struct lines *at, *at_prev, *make;


	/*
	 * Note: we don't care about the tail member here - not needed
	 * anymore.
	 */

	if (lines2->first_offset < lines1->first_offset)
		lines1->first_offset = lines2->first_offset;

	if (!lines1->head) {
		/* first list empty - only take second */
		lines1->head = lines2->head;
		return;
	}

	/* merge lines in one pass */
	at = lines1->head;
	make = lines2->head;
	while (at) {
		while (make) {
			if (make->n > at->n)
				break;

			if (make->n < at->n) {
				struct lines *next = make->next;

				insert_line_before (lines1, at, make);
				make = next;
				continue;
			}

			make = make->next; /* line number equal - first wins */
		}
		at_prev = at;
		at = at->next;
	}

	/* append the rest of list 2 to list 1 */
	if (make)
		at_prev->next = make;
}

static void
free_lines (struct lines *list)
{
        struct lines *next;
	for (; list; list = next) {
		next = list->next;
		free (list->line);
		free (list);
	}
}

static void
clear_lines_info (struct lines_info *info)
{
        free_lines (info->head);
        info->tail = info->head = NULL;
        free (info->unline);
        info->unline = NULL;
}

static void
whitespace_damage (const char *which)
{
        error (EXIT_FAILURE, 0, "Whitespace damage detected in %s", which);
}


static struct lines *
create_orig (FILE *f, struct lines_info *file,
	     int reverted, int *clash)
{
	unsigned long linenum;
	char *line = NULL, first_char;
	size_t linelen = 0;
	int last_was_add;
	long pos = ftell (f);
	unsigned long min_context = (unsigned long) -1;

	do {
		if (getline (&line, &linelen, f) == -1)
			break;
	} while (strncmp (line, "@@ ", 3));

	while (!feof (f)) {
		/* Find next hunk */
		unsigned long orig_lines, new_lines, newline;
		int file_is_removed = 0;
		unsigned long context;
		int leading_context;
		char *p, *q;

		if (strncmp (line, "@@", 2)) {
			fseek (f, pos, SEEK_SET);
			break;
		}

		if (reverted) {
			new_lines = orig_num_lines (line);
			orig_lines = new_num_lines (line);
		} else {
			orig_lines = orig_num_lines (line);
			new_lines = new_num_lines (line);
		}

		p = strchr (line, reverted ? '+' : '-');
		if (!p)
			break;

		p++;
		linenum = strtoul (p, &q, 10);
		if (p == q)
			break;
		if (linenum == 0) {
			file_is_removed = 1;
			file->first_offset = 0;
		} else if (!file->first_offset ||
			   linenum < file->first_offset) {
			file->first_offset = linenum;
			if (!orig_lines)
				file->first_offset++;
		}

		/* Note the amount of leading context, in case the user
		 * supplied a -U option. */
		context = 0;
		leading_context = 1;

		/* Now copy the relevant bits of the hunk */
		last_was_add = 0;
		newline = 1;
		while (orig_lines || new_lines || newline) {
			ssize_t got;
			pos = ftell (f);
			got = getline (&line, &linelen, f);
			if (got == -1)
				break;

			if (!orig_lines && !new_lines &&
			    line[0] != '\\')
				break;

			first_char = line[0];
			if (reverted) {
				if (first_char == '-')
					first_char = '+';
				else if (first_char == '+')
					first_char = '-';
			}

			switch (first_char) {
                        case '\n':
                                whitespace_damage("input");
			case ' ':
				if (leading_context) context++;
				if (new_lines) new_lines--;
			case '-':
				if (first_char == '-')
				    leading_context = 0;

				if (orig_lines) orig_lines--;
				if (!file_is_removed)
					if (add_line (file, line + 1,
						      (size_t) got - 1,
						      linenum++) && clash) {
						++*clash;
						if (debug)
							printf ("clash at "
								"line %lu\n",
								linenum - 1);
					}
				break;
			case '+':
				leading_context = 0;
				if (new_lines) new_lines--;
				break;
			case '\\':
				leading_context = 0;
				newline = 0;
				if (!file_is_removed && !last_was_add) {
					struct lines *at, *prev = NULL;
					for (at = file->head; at; at = at->next)
						prev = at;
					if (!prev)
						error (EXIT_FAILURE, 0,
						       "Garbled patch");
					if (prev->length >= 1 &&
					    prev->line[prev->length-1] == '\n')
						prev->length--;
				}
				break;
			}

			last_was_add = (line[0] == '+');
		}

		if (!newline) {
			pos = ftell (f);
			if (getline (&line, &linelen, f) == -1)
				break;
		}

		if (context < min_context)
			min_context = context;
	}

	if (line)
		free (line);

	file->min_context = min_context;
	return file->head;
}

static void
construct_unline (struct lines_info *file_info)
{
	char *un;
	struct lines *at;
	size_t i;

	if (file_info->unline)
		/* Already done. */
		return;

	un = file_info->unline = xmalloc (7);

	/* First pass: construct a small line not in the file. */
	for (i = 0, at = file_info->head; at && i < 5; i++, at = at->next) {
		size_t j = strlen (at->line) - 1;
		if (i < j)
			j = i;
		un[i] = at->line[j] + 1;
		if (iscntrl (un[i]))
			un[i] = '!';
	}
	for (; i < 5; i++)
		un[i] = '!';
	un[i++] = '\n';
	un[i] = '\0';

	for (i = 4; i > 0; i--) {
		/* Is it unique yet? */
		for (at = file_info->head; at; at = at->next) {
			if (!strcmp (at->line, un))
				break;
		}
		if (!at)
			/* Yes! */
			break;

		un[i] = '\n';
		un[i + 1] = '\0';
	}

	if (i == 0) {
		/* Okay, we'll do it the hard way, and generate a long line. */
		size_t maxlength = 0;
		for (at = file_info->head; at; at = at->next) {
			size_t len = strlen (at->line);
			if (len > maxlength)
				maxlength = len;
		}

		free (un);
		un = file_info->unline = xmalloc (maxlength + 3);
		for (i = 0; i < maxlength; i++)
			un[i] = '!';
		un[i++] = '\n';
		un[i] = '\0';
	}
}

static int
write_file (struct lines_info *file_info, int fd)
{
	unsigned long linenum;
	struct lines *at;
	FILE *fout = fdopen (fd, "w");

	construct_unline (file_info);
	for (linenum = 1; linenum < file_info->first_offset; linenum++)
		fputs (file_info->unline, fout);

	for (at = file_info->head; at; at = at->next) {
		unsigned long i = at->n - linenum;
		while (i--) {
			fprintf (fout, "%s", file_info->unline);
			linenum++;
		}
		fwrite (at->line, at->length, 1, fout);
		linenum++;
	}
	fclose (fout);
	return 0;
}

static int
do_output_patch1_only (FILE *p1, FILE *out, int not_reverted)
{
	char *line;
	char *oldname;
	char first_char;
	size_t linelen;
	ssize_t got;
	long pos;

	oldname = NULL;
	linelen = 0;
	if (getline (&oldname, &linelen, p1) < 0)
		error (EXIT_FAILURE, errno, "Bad patch #1");

	if (strncmp (oldname, "--- ", 4))
		error (EXIT_FAILURE, 0, "Bad patch #1");

	line = NULL;
	linelen = 0;
	if (getline (&line, &linelen, p1) < 0)
		error (EXIT_FAILURE, errno, "Bad patch #1");

	if (strncmp (line, "+++ ", 4))
		error (EXIT_FAILURE, 0, "Bad patch #1");

	if (not_reverted) {
		/* Combinediff: copy patch */
		if (human_readable && mode != mode_flip)
			fprintf (out, "unchanged:\n");
		fputs (oldname, out);
		fputs (line, out);
	} else if (!no_revert_omitted) {
		if (human_readable)
			fprintf (out, "reverted:\n");
		fprintf (out, "--- %s", line + 4);
		fprintf (out, "+++ %s", oldname + 4);
	}
	free (oldname);

	pos = ftell (p1);
	got = getline (&line, &linelen, p1);
	if (got < 0)
		error (EXIT_FAILURE, errno, "Bad patch #1");

	for (;;) {
		unsigned long orig_lines;
		unsigned long new_lines;
		unsigned long newline;
		char *d1, *d2, *p;
		size_t h;

		if (strncmp (line, "@@ ", 3)) {
			fseek (p1, pos, SEEK_SET);
			break;
		}

		p = line + 3;
		h = strcspn (p, " \t");
		d1 = xstrndup (p, h);
		p += h;
		p += strspn (p, " \t");
		h = strcspn (p, " \t");
		d2 = xstrndup (p, h);
		if (!*d1 || !*d2)
			error (EXIT_FAILURE, 0, "Bad patch #1");

		if (not_reverted) {
			/* Combinediff: copy patch */
			fwrite (line, (size_t) got, 1, out);
			new_lines = orig_num_lines (d1);
			orig_lines = new_num_lines (d2);
		} else {
			/* Interdiff: revert patch */
			if (!no_revert_omitted) {
				/* When reversing, we need to swap the line counts too */
				unsigned long orig_offset, orig_count, new_offset, new_count;
				if (read_atatline (line, &orig_offset, &orig_count, &new_offset, &new_count) == 0) {
					/* Swap the offsets and counts for the reversed patch */
					fprintf (out, "@@ -%lu", new_offset);
					if (new_count != 1)
						fprintf (out, ",%lu", new_count);
					fprintf (out, " +%lu", orig_offset);
					if (orig_count != 1)
						fprintf (out, ",%lu", orig_count);
					fprintf (out, " @@\n");
				} else {
					/* Fallback to the old method if parsing fails */
					fprintf (out, "@@ -%s +%s @@\n",
						 d2 + 1, d1 + 1);
				}
			}
			orig_lines = orig_num_lines (d1);
			new_lines = new_num_lines (d2);
		}


		free (d1);
		free (d2);

		newline = 1;
		while (orig_lines || new_lines || newline) {
			pos = ftell (p1);
			got = getline (&line, &linelen, p1);
			if (got == -1)
				break;

			if (!orig_lines && !new_lines &&
			    line[0] != '\\')
				break;

			first_char = line[0];
			if (not_reverted) {
				if (first_char == '+')
					first_char = '-';
				else if (first_char == '-')
					first_char = '+';
			}

			switch (first_char) {
                        case '\n':
                                whitespace_damage("patch #1");
			case ' ':
				if (orig_lines) orig_lines--;
				if (new_lines) new_lines--;
				if (not_reverted || !no_revert_omitted)
					fwrite (line, (size_t) got, 1, out);
				break;
			case '+':
				if (new_lines) new_lines--;
				if (not_reverted || !no_revert_omitted) {
					fputc ('-', out);
					fwrite (line + 1, (size_t) got - 1,
						1, out);
				}
				break;
			case '-':
				if (orig_lines) orig_lines--;
				if (not_reverted || !no_revert_omitted) {
					fputc ('+', out);
					fwrite (line + 1, (size_t) got - 1,
						1, out);
				}
				break;
			case '\\':
				newline = 0;
				if (not_reverted || !no_revert_omitted)
					fwrite (line, (size_t) got, 1, out);
				break;
			}
		}

		if (!newline) {
			pos = ftell (p1);
			if (getline (&line, &linelen, p1) == -1)
				break;
		}
	}

	if (line)
		free (line);

	return 0;
}

static int
output_patch1_only (FILE *p1, FILE *out, int not_reverted)
{
	const char *tmpdir;
	unsigned int tmplen;
	const char tail1[] = "/interdiff-1.XXXXXX";
	const char tail2[] = "/interdiff-2.XXXXXX";
	char *tmpp1, *tmpp2;
	char options[100];
	int tmpp1fd, tmpp2fd;
	long pos;
	char *oldname = NULL, *newname = NULL;
	size_t oldnamelen = 0, newnamelen = 0;
	struct lines_info file_orig = { NULL, 0, 0, NULL, NULL };
	struct lines_info file_new = { NULL, 0, 0, NULL, NULL };
	pid_t child;
	FILE *in;
	int diff_is_empty = 1;
	unsigned int use_context = max_context;

	if (!num_diff_opts && !context_specified)
		return do_output_patch1_only (p1, out, not_reverted);

	/* We want to redo the diff using the supplied options. */
	tmpdir = getenv ("TMPDIR");
	if (!tmpdir)
		tmpdir = P_tmpdir;

	tmplen = strlen (tmpdir);
	tmpp1 = alloca (tmplen + sizeof (tail1));
	memcpy (tmpp1, tmpdir, tmplen);
	memcpy (tmpp1 + tmplen, tail1, sizeof (tail1));
	tmpp2 = alloca (tmplen + sizeof (tail2));
	memcpy (tmpp2, tmpdir, tmplen);
	memcpy (tmpp2 + tmplen, tail2, sizeof (tail2));

	tmpp1fd = xmkstemp (tmpp1);
	tmpp2fd = xmkstemp (tmpp2);

	pos = ftell (p1);
	do {
		if (oldname) {
			free (oldname);
			oldname = NULL;
			oldnamelen = 0;
		}
		if (getline (&oldname, &oldnamelen, p1) < 0)
			error (EXIT_FAILURE, errno, "Bad patch #1");

	} while (strncmp (oldname, "--- ", 4));
	oldname[strlen (oldname) - 1] = '\0';

	if (getline (&newname, &newnamelen, p1) < 0)
		error (EXIT_FAILURE, errno, "Bad patch #1");
	if (strncmp (newname, "+++ ", 4))
		error (EXIT_FAILURE, errno, "Bad patch #1");
	newname[strlen (newname) - 1] = '\0';

	/* Recreate the original and modified state. */
	fseek (p1, pos, SEEK_SET);
	create_orig (p1, &file_orig, !not_reverted, NULL);
	fseek (p1, pos, SEEK_SET);
	create_orig (p1, &file_new, not_reverted, NULL);

	/* Decide how much context to use. */
	if (file_orig.min_context < use_context)
		use_context = file_orig.min_context;

	if (use_context == 3)
		strcpy (options, "-u");
	else
		sprintf (options, "-U%d", use_context);

	/* Write it out. */
	write_file (&file_orig, tmpp1fd);
        free (file_new.unline);
	file_new.unline = xstrdup (file_orig.unline);
	write_file (&file_new, tmpp2fd);
	clear_lines_info (&file_orig);
	clear_lines_info (&file_new);

	fflush (NULL);
	char *argv[2 + num_diff_opts + 2 + 1];
	memcpy (argv, ((const char *[]) { DIFF, options }), 2 * sizeof (char *));
	memcpy (argv + 2, diff_opts, num_diff_opts * sizeof (char *));
	memcpy (argv + 2 + num_diff_opts, ((char *[]) { tmpp1, tmpp2, NULL }), (2 + 1) * sizeof (char *));
	in = xpipe (DIFF, &child, "r", argv);

	/* Eat the first line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
		diff_is_empty = 0;
	}

	/* Eat the second line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
	}

	if (!diff_is_empty) {
		char *line = NULL;
		size_t linelen = 0;
		if (not_reverted) {
			fprintf (out, "--- %s\n", oldname + 4);
			fprintf (out, "+++ %s\n", newname + 4);
		} else {
			fprintf (out, "--- %s\n", newname + 4);
			fprintf (out, "+++ %s\n", oldname + 4);
		}
		for (;;) {
			ssize_t got = getline (&line, &linelen, in);
			if (got < 0)
				break;
			fwrite (line, (size_t) got, 1, out);
		}
                free (line);
	}

	fclose (in);
	waitpid (child, NULL, 0);
	if (debug)
		printf ("reconstructed orig1=%s orig2=%s\n", tmpp1, tmpp2);
	else {
		unlink (tmpp1);
		unlink (tmpp2);
	}
	free (oldname);
	free (newname);
	return 0;
}

static int
apply_patch (FILE *patch, const char *file, int reverted, int max_fuzz_no_rej)
{
#define MAX_PATCH_ARGS 7
	const char *argv[MAX_PATCH_ARGS];
	int argc = 0;
	const char *basename;
	unsigned long orig_lines, new_lines;
	size_t linelen;
	char *line;
	pid_t child;
	int status;
	FILE *w;

	basename = strrchr (file, '/');
	if (basename)
	    basename++;
	else
	    basename = file;

	/* Check if -w option is present in diff_opts */
	int has_ignore_all_space = 0;
	for (int i = 0; i < num_diff_opts; i++) {
		if (strcmp(diff_opts[i], "-w") == 0) {
			has_ignore_all_space = 1;
			break;
		}
	}

	/* Add up to MAX_PATCH_ARGS arguments for the patch execution */
	argv[argc++] = PATCH;
	argv[argc++] = reverted ? (has_ignore_all_space ? "-Rlsp0" : "-Rsp0")
				: (has_ignore_all_space ? "-lsp0" : "-sp0");
	if (fuzzy) {
		/* Don't generate .orig files when we expect rejected hunks */
		argv[argc++] = "--no-backup-if-mismatch";

		/* Ignore all context lines by setting fuzz to INT_MAX, and
		 * don't generate a reject file even if any hunks fail. */
		if (max_fuzz_no_rej) {
			argv[argc++] = "--fuzz=2147483647";
			argv[argc++] = "--reject-file=-";
		}
	}
	argv[argc++] = file;
	argv[argc++] = NULL;

	w = xpipe(PATCH, &child, "w", (char **) argv);

	fprintf (w, "--- %s\n+++ %s\n", basename, basename);
	line = NULL;
	linelen = 0;
	orig_lines = new_lines = 0;
	for (;;) {
		ssize_t got = getline (&line, &linelen, patch);
		if (got == -1)
			break;

		if (!orig_lines && !new_lines) {
			/* FIXME: Should this compare with '@@ ' instead of '--- '? */
			if (!strncmp (line, "--- ", 4))
				break;
			if (!strncmp (line, "diff ", 5)
			    || !strncmp (line, "new file mode ", 14)
			    || !strncmp (line, "index ", 6))
				continue;
		}

		fwrite (line, (size_t) got, 1, w);

		if (!strncmp (line, "@@ ", 3)) {
			orig_lines = orig_num_lines (line);
			new_lines = new_num_lines (line);
			continue;
		}

		if (orig_lines && line[0] != '+')
			orig_lines--;
		if (new_lines && line[0] != '-')
			new_lines--;
	}
	fclose (w);
	waitpid (child, &status, 0);

	if (line)
		free (line);

	return WEXITSTATUS (status);
}

static int
trim_context (FILE *f /* positioned at start of @@ line */,
	      const char *unline /* drop this line */,
	      FILE *out /* where to send output */)
{
	/* For each hunk, trim the context so that the number of
	 * pre-context lines does not exceed the number of
	 * post-context lines.  See the fuzz1 test case. */
	char *line = NULL;
	size_t linelen;

	for (;;) {
		fpos_t pos;
		unsigned long pre = 0, pre_seen = 0, post = 0;
		unsigned long strip_pre = 0, strip_post = 0;
		unsigned long orig_offset, new_offset;
		unsigned long orig_count, orig_orig_count, new_orig_count;
		unsigned long new_count, orig_new_count, new_new_count;
		unsigned long total_count = 0;

		/* Read @@ line. */
		if (getline (&line, &linelen, f) < 0)
			break;

		if (line[0] == '\\') {
			/* Pass '\' lines through unaltered. */
			fputs (line, out);
			continue;
		}

		if (read_atatline (line, &orig_offset, &orig_count,
				   &new_offset, &new_count))
			error (EXIT_FAILURE, 0, "Line not understood: %s",
			       line);

		orig_orig_count = new_orig_count = orig_count;
		orig_new_count = new_new_count = new_count;
		fgetpos (f, &pos);
		while (orig_count || new_count) {
			if (getline (&line, &linelen, f) < 0)
				break;

			total_count++;
			switch (line[0]) {
                        case '\n':
                                whitespace_damage("input");
			case ' ' :
				if (orig_count) orig_count--;
				if (new_count) new_count--;
				if (!pre_seen) {
					pre++;
					if (!strcmp (line + 1, unline))
						strip_pre = pre;
				} else {
					post++;
					if (strip_post ||
					    !strcmp (line + 1, unline))
						strip_post++;
				}
				break;
			case '-':
				if (orig_count) orig_count--;
				if (strip_post)
					goto split_hunk;
				pre_seen = 1;
				post = 0;
				break;
			case '+':
				if (new_count) new_count--;
				if (strip_post)
					goto split_hunk;
				pre_seen = 1;
				post = 0;
				break;
			}
		}

		pre -= strip_pre;
		post -= strip_post;
		if (post && pre > post)
			strip_pre += pre - post;
		orig_offset += strip_pre;
		new_orig_count -= strip_pre + strip_post;
		new_offset += strip_pre;
		new_new_count -= strip_pre + strip_post;
		if (orig_orig_count && !new_orig_count)
			orig_offset--;
		if (orig_new_count && !new_new_count)
			new_offset--;

		if (debug)
			printf ("Trim: %lu,%lu\n", strip_pre, strip_post);

		fsetpos (f, &pos);
		if (new_orig_count != 1 && new_new_count != 1)
			print_color (out, LINE_HUNK, "@@ -%lu,%lu +%lu,%lu @@\n",
				     orig_offset, new_orig_count, new_offset, new_new_count);
		else if (new_orig_count != 1)
			print_color (out, LINE_HUNK, "@@ -%lu,%lu +%lu @@\n",
				     orig_offset, new_orig_count, new_offset);
		else if (new_new_count != 1)
			print_color (out, LINE_HUNK, "@@ -%lu +%lu,%lu @@\n",
				     orig_offset, new_offset, new_new_count);
		else
			print_color (out, LINE_HUNK, "@@ -%lu +%lu @@\n",
				     orig_offset, new_offset);

		while (total_count--) {
			enum line_type type;
			ssize_t got = getline (&line, &linelen, f);
			assert (got > 0);

			if (strip_pre) {
				strip_pre--;
				continue;
			}

			if (total_count < strip_post)
				continue;

			switch (line[0]) {
			case '+':
				type = LINE_ADDED;
				break;
			case '-':
				type = LINE_REMOVED;
				break;
			default:
				fwrite (line, (size_t) got, 1, out);
				continue;
			}
			print_color (out, type, "%s", line);
		}
	}

	free (line);
	return 0;

 split_hunk:
	error (0, 0, "hunk-splitting is required in this case, but is not yet implemented");
	error (1, 0, "use the -U option to work around this");
	return 0;
}

static FILE *
open_rej_file (char *file)
{
	/* Modify the `file` string in-place to look for a .rej file */
	char *end = strchr (file, '\0');
	FILE *f;

	strcpy (end, ".rej");
	f = fopen (file, "r");
	*end = '\0';
	return f;
}

/* Returns a pointer to the .rej file and the line offset of the first hunk */
static FILE *
fuzz_rejected_hunks (char *tmpp2, unsigned long *rej_offset)
{
	/* In fuzzy mode, emit the patch rejects separately instead of
	 * erroring out, since the fuzzy mode is meant for producing an
	 * interdiff for human eyes rather than a functional diff. */
	char *line = NULL;
	size_t line_len;
	long atat_pos;
	FILE *rej;

	if (!fuzzy)
		return NULL;

	/* It's only ever patch2 that can fail in fuzzy mode because patch1 is
	 * applied to its own original file in fuzzy mode. */
	rej = open_rej_file (tmpp2);
	if (!rej)
		return NULL;

	/* Skip (the first two) lines to get to the start of the @@ line */
	do {
		atat_pos = ftell (rej);
		if (getline (&line, &line_len, rej) < 0)
			error (EXIT_FAILURE, errno,
			       "Failed to read line from .rej");
	} while (strncmp (line, "@@ ", 3));
	fseek (rej, atat_pos, SEEK_SET);

	/* Export the line offset of the first rej hunk */
	if (read_atatline (line, rej_offset, NULL, NULL, NULL))
		error (EXIT_FAILURE, 0, "line not understood: %s", line);

	/* Apply the rejected hunks with maximum fuzzing to tmpp2 to increase
	 * the odds of them getting applied. The goal is to get the rejects
	 * applied so that they are excluded from the diff execution. These
	 * rejected hunks will still be shown. It's fine if this fails. */
	apply_patch (rej, tmpp2, 0, 1);

	/* Go back to the @@ after apply_patch() moved the file cursor */
	fseek (rej, atat_pos, SEEK_SET);
	free (line);
	return rej;
}

static void
output_rej_hunk (const char *diff_line, FILE *rej, unsigned long *rej_offset,
		 FILE *out)
{
	unsigned long diff_offset;
	int first_line_done = 0;
	char *line = NULL;
	size_t line_len;
	long atat_pos;
	ssize_t got;

	/* Nothing to do if no rej file or the rej file is finished */
	if (!rej || feof (rej))
		return;

	/* Wait until the current diff line is an @@ line */
	if (strncmp (diff_line, "@@ ", 3))
		return;

	if (read_atatline (diff_line, &diff_offset, NULL, NULL, NULL))
		error (EXIT_FAILURE, 0, "line not understood: %s", diff_line);

	/* Write the rej hunk before the diff hunk if its offset comes before */
	if (*rej_offset > diff_offset)
		return;

	/* Write the rej hunk until EOF or the next @@ line (i.e., next hunk).
	 * Note that rej starts at the current @@ line that we must write, so
	 * don't look for the next @@ until after the first line is written. */
	for (;;) {
		got = getline (&line, &line_len, rej);
		if (got < 0) {
			if (feof (rej))
				goto rej_file_eof;
			error (EXIT_FAILURE, errno,
			       "Failed to read line from .rej");
		}
		if (first_line_done && !strncmp (line, "@@ ", 3))
			break;
		first_line_done = 1;
		fwrite (line, (size_t) got, 1, out);
		atat_pos = ftell (rej);
	}

	/* Record the line offset of the next rej hunk, only if there is one */
	if (read_atatline (line, rej_offset, NULL, NULL, NULL))
		error (EXIT_FAILURE, 0, "line not understood: %s", line);
	fseek (rej, atat_pos, SEEK_SET);

rej_file_eof:
	free (line);
}

static int
output_delta (FILE *p1, FILE *p2, FILE *out)
{
	const char *tmpdir = getenv ("TMPDIR");
	unsigned int tmplen;
	const char tail1[] = "/interdiff-1.XXXXXX";
	const char tail2[] = "/interdiff-2.XXXXXX\0rej"; /* Add room for .rej */
	char *tmpp1, *tmpp2;
	int tmpp1fd, tmpp2fd;
	struct lines_info file = { NULL, 0, 0, NULL, NULL };
	struct lines_info file2 = { NULL, 0, 0, NULL, NULL };
	char *oldname = NULL, *newname = NULL;
	pid_t child;
	FILE *in, *rej = NULL;
	unsigned long rej_offset;
	size_t namelen;
	long pos1 = ftell (p1), pos2 = ftell (p2);
	long pristine1, pristine2;
	long start1, start2;
	char options[100];
	int diff_is_empty = 1;

	pristine1 = ftell (p1);
	pristine2 = ftell (p2);

	if (!tmpdir)
		tmpdir = P_tmpdir;

	tmplen = strlen (tmpdir);
	tmpp1 = alloca (tmplen + sizeof (tail1));
	memcpy (tmpp1, tmpdir, tmplen);
	memcpy (tmpp1 + tmplen, tail1, sizeof (tail1));
	tmpp2 = alloca (tmplen + sizeof (tail2));
	memcpy (tmpp2, tmpdir, tmplen);
	memcpy (tmpp2 + tmplen, tail2, sizeof (tail2));

	if (max_context == 3)
		strcpy (options, "-u");
	else
		sprintf (options, "-U%d", max_context);

	tmpp1fd = xmkstemp (tmpp1);
	tmpp2fd = xmkstemp (tmpp2);

	if (mode == mode_combine) {
		/* For combinediff, we want the --- line from patch1 (original file) */
		do {
			if (oldname) {
				free (oldname);
				oldname = NULL;
			}
			if (getline (&oldname, &namelen, p1) < 0)
				error (EXIT_FAILURE, errno, "Bad patch #1");

		} while (strncmp (oldname, "--- ", 4));
		oldname[strlen (oldname) - 1] = '\0';
	} else {
		/* For interdiff, use +++ line from patch1 */
		do {
			if (oldname) {
				free (oldname);
				oldname = NULL;
			}
			if (getline (&oldname, &namelen, p1) < 0)
				error (EXIT_FAILURE, errno, "Bad patch #1");

		} while (strncmp (oldname, "+++ ", 4));
		oldname[strlen (oldname) - 1] = '\0';
	}

	do {
		if (newname) {
			free (newname);
			newname = NULL;
		}
		if (getline (&newname, &namelen, p2) < 0)
			error (EXIT_FAILURE, errno, "Bad patch #2");

	} while (strncmp (newname, "+++ ", 4));
	newname[strlen (newname) - 1] = '\0';

	start1 = ftell (p1);
	start2 = ftell (p2);
	fseek (p1, pos1, SEEK_SET);
	fseek (p2, pos2, SEEK_SET);
	create_orig (fuzzy ? p1 : p2, &file, 0, NULL);
	fseek (p1, pos1, SEEK_SET);
	fseek (p2, pos2, SEEK_SET);
	create_orig (p1, &file2, mode == mode_combine, NULL);
	merge_lines(&file, &file2);
	pos1 = ftell (p1);

	/* Write it out. */
	write_file (&file, tmpp1fd);
	write_file (&file, tmpp2fd);

	fseek (p1, start1, SEEK_SET);
	fseek (p2, start2, SEEK_SET);

	if (apply_patch (p1, tmpp1, mode == mode_combine, 0))
		error (EXIT_FAILURE, 0,
		       "Error applying patch1 to reconstructed file");

	if (apply_patch (p2, tmpp2, 0, 0)) {
		/* Cope with a .rej in fuzzy mode */
		rej = fuzz_rejected_hunks (tmpp2, &rej_offset);
		if (!rej)
			error (EXIT_FAILURE, 0,
			       "Error applying patch2 to reconstructed file");
	}

	fseek (p1, pos1, SEEK_SET);

	fflush (NULL);

	char *argv[2 + num_diff_opts + 2 + 1];
	memcpy (argv, ((const char *[]) { DIFF, options }), 2 * sizeof (char *));
	memcpy (argv + 2, diff_opts, num_diff_opts * sizeof (char *));
	memcpy (argv + 2 + num_diff_opts, ((char *[]) { tmpp1, tmpp2, NULL }), (2 + 1) * sizeof (char *));
	in = xpipe (DIFF, &child, "r", argv);

	/* Eat the first line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
		diff_is_empty = 0;
	}

	/* Eat the second line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
	}

	if (!diff_is_empty || rej) {
		/* ANOTHER temporary file!  This is to catch the case
		 * where we just don't have enough context to generate
		 * a proper interdiff. */
		FILE *tmpdiff = xtmpfile ();
		char *line = NULL;
		size_t linelen;
		for (;;) {
			ssize_t got = getline (&line, &linelen, in);
			if (got < 0)
				break;
			output_rej_hunk (line, rej, &rej_offset, tmpdiff);
			fwrite (line, (size_t) got, 1, tmpdiff);
			if (*line != ' ' && !strcmp (line + 1, file.unline)) {
				/* Uh-oh.  We're trying to output a
				 * line that made up (we never saw the
				 * original).  As long as this is at
				 * the end of a hunk we can safely
				 * drop it (done in trim_context
				 * later). */
				got = getline (&line, &linelen, in);
				if (got < 0)
					continue;
				else if (strncmp (line, "@@ ", 3)) {
					/* An interdiff isn't possible.
					 * Evasive action: just revert the
					 * original and copy the new
					 * version. */
					fclose (tmpdiff);
					free (line);
					goto evasive_action;
				}
				fwrite (line, (size_t) got, 1, tmpdiff);
			}
		}
		free (line);

		/* Output any remaining rej hunks */
		if (rej && !feof (rej)) {
			for (;;) {
				int ch = fgetc (rej);
				if (ch == EOF)
					break;
				fputc (ch, tmpdiff);
			}
		}

		/* First character */
		if (human_readable) {
			char *p, *q, c, d;
			c = d = '\0'; /* shut gcc up */
			p = strchr (oldname + 4, '\t');
			if (p) {
				c = *p;
				*p = '\0';
			}
			q = strchr (newname + 4, '\t');
			if (q) {
				d = *q;
				*q = '\0';
			}
			print_color (out, LINE_HEADER, DIFF " %s %s %s\n", options, oldname + 4,
				     newname + 4);
			if (p) *p = c;
			if (q) *q = d;
		}
		print_color (out, LINE_FILE, "--- %s\n", oldname + 4);
		print_color (out, LINE_FILE, "+++ %s\n", newname + 4);
		rewind (tmpdiff);
		trim_context (tmpdiff, file.unline, out);
		fclose (tmpdiff);
	}

	fclose (in);
	waitpid (child, NULL, 0);
	if (debug)
		printf ("reconstructed orig1=%s orig2=%s\n", tmpp1, tmpp2);
	else {
		unlink (tmpp1);
		unlink (tmpp2);
		if (rej) {
			/* Delete the .rej file generated in fuzzy mode */
			strcat (tmpp2, ".rej");
			unlink (tmpp2);
		}
	}
	free (oldname);
	free (newname);
	clear_lines_info (&file);
	return 0;

 evasive_action:
	if (debug)
		printf ("reconstructed orig1=%s orig2=%s\n", tmpp1, tmpp2);
	else {
		unlink (tmpp1);
		unlink (tmpp2);
		if (rej) {
			/* Delete the .rej file generated in fuzzy mode */
			strcat (tmpp2, ".rej");
			unlink (tmpp2);
		}
	}
	if (human_readable)
		fprintf (out, "%s impossible; taking evasive action\n",
			 (mode == mode_combine) ? "merge" : "interdiff");
	fseek (p1, pristine1, SEEK_SET);
	fseek (p2, pristine2, SEEK_SET);
	output_patch1_only (p1, out, mode == mode_combine);
	output_patch1_only (p2, out, 1);
	return 0;
}

static int
copy_residue (FILE *p2, FILE *out)
{
	struct file_list *at;

	for (at = files_in_patch2; at; at = at->next) {

		if (file_in_list (files_done, at->file) != -1)
			continue;

		/* check if we need to process it and init context */
		if (!check_filename(at->file))
			continue;

		fseek (p2, at->pos, SEEK_SET);
		if (human_readable && mode != mode_flip)
			fprintf (out, "only in patch2:\n");

		output_patch1_only (p2, out, 1);
	}

	return 0;
}

/* Generic patch indexing function that can handle both patch1 and patch2 */
static int
index_patch_generic (FILE *patch_file, struct file_list **file_list, int need_skip_content)
{
	char *line = NULL;
	size_t linelen = 0;
	int is_context = 0;
	int file_is_empty = 1;

	/* Index patch */
	while (!feof (patch_file)) {
		unsigned long skip;
		char *names[2];
		char *p, *end;
		long pos = ftell (patch_file);

		if (getline (&line, &linelen, patch_file) == -1)
			break;

		file_is_empty = 0;

		if (strncmp (line, "--- ", 4)) {
			is_context = !strncmp (line, "*** ", 4);
			continue;
		}

		if (is_context)
			error (EXIT_FAILURE, 0,
			       "I don't understand context diffs yet.");

		names[0] = filename_from_header (line + 4);

		if (getline (&line, &linelen, patch_file) == -1) {
			free (names[0]);
			break;
		}

		if (strncmp (line, "+++ ", 4)) {
			free (names[0]);
			continue;
		}

		names[1] = filename_from_header (line + 4);

		/* For patch2, we need to handle the @@ line and skip content */
		if (need_skip_content) {
			if (getline (&line, &linelen, patch_file) == -1) {
				free (names[0]);
				free (names[1]);
				break;
			}

			if (strncmp (line, "@@ ", 3))
				goto try_next;

			p = strchr (line + 3, '+');
			if (!p)
				goto try_next;
			p = strchr (p, ',');
			if (p) {
				/* Like '@@ -1,3 +1,3 @@' */
				p++;
				skip = strtoul (p, &end, 10);
				if (p == end)
					goto try_next;
			} else
				/* Like '@@ -1 +1 @@' */
				skip = 1;

			add_to_list (file_list, best_name (2, names), pos);

			while (skip--) {
				if (getline (&line, &linelen, patch_file) == -1)
					break;

				if (line[0] == '-')
					/* Doesn't count towards skip count */
					skip++;
			}
		} else {
			/* For patch1, just add to list */
			add_to_list (file_list, best_name (2, names), pos);
		}

	try_next:
		free (names[0]);
		free (names[1]);
	}

	if (line)
		free (line);

	if (file_is_empty || *file_list)
		return 0;
	else
		return 1;
}

static int
index_patch2 (FILE *p2)
{
	return index_patch_generic (p2, &files_in_patch2, 1);
}

/* With flipdiff we have two patches we want to reorder.  The
 * algorithm is:
 *
 * 1. Reconstruct the file as it looks after patch1, using the context
 * from patch2 as well.
 *
 * 2. Apply patch2, in order to reconstruct the file as it looks after
 * both patches have been applied.  Write this out twice.
 *
 * 3. Analyse patch2, taking note of the additions and subtractions it
 * makes.
 *
 * 4. To one of the copies of the reconstructed final image, undo the
 * changes from patch1 by analysing the patch line by line
 * (ourselves!).  Need to take account of offsets due to patch2, and
 * the fact that patch2 may have changed some lines. */
struct offset {
	unsigned long line;	/* line number after patch1, before patch2 */
	long offset;		/* offset modification */
};

static struct offset *
add_offset (unsigned long line, long offset,
	    struct offset *offsets, unsigned long *allocated,
	    unsigned long *num_offsets)
{
	if (*num_offsets == *allocated) {
		*allocated *= 2;
		offsets = xrealloc (offsets,
				    *allocated * sizeof (struct offset));
	}
	offsets[*num_offsets].line = line;
	offsets[*num_offsets].offset = offset;
	++*num_offsets;
	if (debug)
		printf ("%lu: %ld\n", line, offset);
	return offsets;
}

static void
free_offsets (struct offset *offsets)
{
	free (offsets);
}

static int
patch2_removes_line (unsigned long line,	/* line number after patch1 */
		     struct offset *offsets,
		     unsigned long num_offsets)
{
	unsigned long i;
	for (i = 0; i < num_offsets; i++) {
		unsigned long this_line = offsets[i].line;
		long this_offset = offsets[i].offset;
		if (this_line <= line &&
		    this_offset < 0 &&
		    this_line - this_offset > line) {
			if (debug)
				printf ("@%lu: removed (%ld)\n", line,
					this_offset);
			return 1;
		}
	}

	return 0;
}

static long
offset_at_line (unsigned long line,	/* line number after patch1 */
		struct offset *offsets,
		unsigned long num_offsets)
{
	long offset = 0;
	unsigned long i;
	for (i = 0; i < num_offsets; i++) {
		unsigned long this_line = offsets[i].line;
		long this_offset = offsets[i].offset;

		if (this_line > line)
			break;

		offset += this_offset;
		if (this_line <= line &&
		    this_offset < 0 &&
		    this_line - this_offset > line)
			break;
	}

	if (debug)
		printf ("@%lu: %ld\n", line, offset);

	return offset;
}

static int
insert_line (struct lines_info *lines, const char *line, size_t length,
	     unsigned long n)
{
	struct lines *at;

	/* Renumber subsequent lines. */
	for (at = lines->head; at; at = at->next)
		if (n <= at->n)
			break;

	while (at) {
		at->n++;
		at = at->next;
	}

	/* Insert the line. */
	if (debug)
		printf ("Insert at %lu: %s", n, line);

	return add_line (lines, line, length, n);
}

static void
remove_line (struct lines_info *lines, const char *line,
	     unsigned long n)
{
	struct lines *at, *kill = NULL;

	/* Renumber subsequent lines. */
	for (at = lines->head; at; at = at->next)
		if (n <= at->n)
			break;

	if (at && n == at->n) {
		kill = at;
		if (strcmp (at->line, line) && kill->prev) {
			kill = kill->prev;
			if (debug)
				printf ("Not removing from %lu: %s", n,
					at->line);
		}
	}

	at = kill;
	while (at) {
		at->n--;
		at = at->next;
	}

	/* Remove the line. */
	if (debug)
		printf ("Remove from %lu: %s", n, kill->line);

	if (kill->prev)
		kill->prev->next = kill->next;

	if (kill->next)
		kill->next->prev = kill->prev;

	if (lines->head == kill)
		lines->head = kill->next;

	if (lines->tail == kill)
		lines->tail = kill->prev;

	free (kill->line);
	free (kill);
}

static int
take_diff (const char *f1, const char *f2, char *headers[2],
	   const char *unline, FILE *out)
{
	char options[100];
	pid_t child;
	int diff_is_empty = 1;
	FILE *in;

	if (max_context == 3)
		strcpy (options, "-u");
	else
		sprintf (options, "-U%d", max_context);

	char *argv[2 + num_diff_opts + 2 + 1];
	memcpy (argv, ((const char *[]) { DIFF, options }), 2 * sizeof (char *));
	memcpy (argv + 2, diff_opts, num_diff_opts * sizeof (char *));
	memcpy (argv + 2 + num_diff_opts, ((const char *[]) { f1, f2, NULL }), (2 + 1) * sizeof (char *));
	if (debug) {
		fputs ("+", stdout);
		for (int i = 0; argv[i]; i++) {
			printf (" %s", argv[i]);
		}
		puts ("");
	}

	in = xpipe (DIFF, &child, "r", argv);

	/* Eat the first line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
		diff_is_empty = 0;
	}

	/* Eat the second line */
	for (;;) {
		int ch = fgetc (in);
		if (ch == EOF || ch == '\n')
			break;
	}

	if (!diff_is_empty) {
		FILE *tmpdiff = xtmpfile ();
		while (!feof (in)) {
			int ch = fgetc (in);
			if (ch != EOF)
				fputc (ch, tmpdiff);
		}
		rewind (tmpdiff);
		fputs (headers[0], out);
		fputs (headers[1], out);
		trim_context (tmpdiff, unline, out);
		fclose (tmpdiff);
	}

	fclose (in);
	waitpid (child, NULL, 0);
	return 0;
}

static int
flipdiff (FILE *p1, FILE *p2, FILE *flip1, FILE *flip2)
{
	char *line = NULL;
	size_t linelen = 0;
	const char tail1[] = "/flipdiff-1.XXXXXX";
	const char tail2[] = "/flipdiff-2.XXXXXX";
	const char tail3[] = "/flipdiff-3.XXXXXX";
	char *tmpdir = getenv ("TMPDIR");
	char *header1[2], *header2[2];
	unsigned int tmplen;
	char *tmpp1, *tmpp2, *tmpp3;
	int tmpfd;
	FILE *f;
	fpos_t at1, at2;
	struct lines_info intermediate = { NULL, 0, 0, NULL, NULL };
	struct offset *offsets = NULL;
	unsigned long offset_alloc = 100;
	unsigned long num_offsets = 0;
	long this_offset = 0;
	unsigned long first_linenum = 0;
	unsigned long linenum;
	int saw_first_offset;
	int clash = 0;
	unsigned long orig_lines, new_lines;

	/* Read headers. */
	header1[0] = header1[1] = NULL;
	linelen = 0;
	if (getline (&header1[0], &linelen, p1) == -1)
		error (EXIT_FAILURE, errno, "Failed to read patch header from first file");
	linelen = 0;
	if (getline (&header1[1], &linelen, p1) == -1)
		error (EXIT_FAILURE, errno, "Failed to read patch header from first file");

	header2[0] = header2[1] = NULL;
	linelen = 0;
	if (getline (&header2[0], &linelen, p2) == -1)
		error (EXIT_FAILURE, errno, "Failed to read patch header from second file");
	linelen = 0;
	if (getline (&header2[1], &linelen, p2) == -1)
		error (EXIT_FAILURE, errno, "Failed to read patch header from second file");
	linelen = 0;

	fgetpos (p1, &at1);
	fgetpos (p2, &at2);

	/* Generate temporary file name templates. */
	if (!tmpdir)
		tmpdir = P_tmpdir;

	tmplen = strlen (tmpdir);
	tmpp1 = alloca (tmplen + sizeof (tail1));
	memcpy (tmpp1, tmpdir, tmplen);
	memcpy (tmpp1 + tmplen, tail1, sizeof (tail1));

	tmpp2 = alloca (tmplen + sizeof (tail2));
	memcpy (tmpp2, tmpdir, tmplen);
	memcpy (tmpp2 + tmplen, tail2, sizeof (tail2));

	tmpp3 = alloca (tmplen + sizeof (tail3));
	memcpy (tmpp3, tmpdir, tmplen);
	memcpy (tmpp3 + tmplen, tail3, sizeof (tail3));

	/* Reconstruct the file after patch1. */
	create_orig (p1, &intermediate, 1, NULL);

	/* Reconstruct the file before patch2. */
	create_orig (p2, &intermediate, 0, &clash);

	/* If any of the lines grokked from the patches mis-matched, we are
	 * likely to run into problems. */
	if (clash)
		error (EXIT_FAILURE, 0, "patches clashed in %d place%s - "
		       "re-generate them first", clash,
		       clash == 1 ? "" : "s");

	/* Now we have all the context we're going to get.  Write out
	 * the file and apply patch1 in reverse, so we end up with the
	 * file as it should look before applying patches. */
	tmpfd = xmkstemp (tmpp1);
	write_file (&intermediate, tmpfd);
	fsetpos (p1, &at1);
	if (apply_patch (p1, tmpp1, 1, 0))
		error (EXIT_FAILURE, 0,
		       "Error reconstructing original file");

	/* Write it out again and apply patch2, so we end up with the
	 * file as it should look after both patches. */
	tmpfd = xmkstemp (tmpp3);
	write_file (&intermediate, tmpfd);
	fsetpos (p2, &at2);
	if (apply_patch (p2, tmpp3, 0, 0))
		error (EXIT_FAILURE, 0,
		       "Error reconstructing final file");

	/* Examine patch2 to figure out offsets. */
	fsetpos (p2, &at2);
	offsets = xmalloc (offset_alloc * sizeof (struct offset));
	orig_lines = new_lines = 0;
	for (;;) {
		if (getline (&line, &linelen, p2) == -1) {
			if (this_offset) {
				offsets = add_offset (first_linenum,
						      this_offset,
						      offsets,
						      &offset_alloc,
						      &num_offsets);
				this_offset = 0;
			}
			break;
		}

		if (!orig_lines && !new_lines && this_offset) {
			offsets = add_offset (first_linenum,
					      this_offset,
					      offsets,
					      &offset_alloc,
					      &num_offsets);
			this_offset = 0;
		}

		if (!orig_lines && !new_lines && strncmp (line, "@@ ", 3))
			break;

		if (!strncmp (line, "@@ ", 3)) {
			if (read_atatline (line, &linenum, &orig_lines,
					   NULL, &new_lines))
				error (EXIT_FAILURE, 0,
				       "line not understood: %s", line);

			continue;
		}

		if (orig_lines && line[0] != '+')
			orig_lines--;
		if (new_lines && line[0] != '-')
			new_lines--;

		switch (line[0]) {
                case '\n':
                        whitespace_damage("patch #2");
		case ' ':
			if (this_offset) {
				offsets = add_offset (first_linenum,
						      this_offset,
						      offsets,
						      &offset_alloc,
						      &num_offsets);
				this_offset = 0;
			}
			break;

		case '-':
			if (this_offset > 0) {
				offsets = add_offset (first_linenum,
						      this_offset,
						      offsets,
						      &offset_alloc,
						      &num_offsets);
				this_offset = 0;
			}
			if (this_offset == 0)
				first_linenum = linenum;
			this_offset--;
			break;

		case '+':
			if (this_offset < 0) {
				offsets = add_offset (first_linenum,
						      this_offset,
						      offsets,
						      &offset_alloc,
						      &num_offsets);
				this_offset = 0;
			}
			if (this_offset == 0)
				first_linenum = linenum;
			this_offset++;
			break;
		}

		if (line[0] != '+')
			linenum++;
	}

	/* Now read the temporary file we wrote (and patched) back
	 * into memory, into our 'intermediate' structure, in order to
	 * modify it (ourselves!) using patch1. */
	free_lines (intermediate.head);
	intermediate.head = intermediate.tail = NULL;
	f = fopen (tmpp3, "r");
	if (!f)
	    error (EXIT_FAILURE, errno, "error opening temporary file");

	linenum = 0;
	saw_first_offset = 0;
	while (!feof (f)) {
		ssize_t got;
		linenum++;
		got = getline (&line, &linelen, f);
		if (got == -1)
			break;
		if (!strcmp (line, intermediate.unline)) {
			if (!saw_first_offset)
				intermediate.first_offset = linenum + 1;
			continue;
		}

		if (!saw_first_offset) {
			saw_first_offset = 1;
			intermediate.first_offset = linenum;
		}

		add_line (&intermediate, line, (size_t) got, linenum);
	}

	/* Modify it according to patch1, making sure to adjust for
	 * offsets introduced in patch2. */
	fsetpos (p1, &at1);
	this_offset = 0;
	orig_lines = new_lines = 0;
	for (;;) {
		ssize_t got;
		fgetpos (p1, &at1);
		got = getline (&line, &linelen, p1);
		if (got == -1)
			break;

		if (!orig_lines && !new_lines && strncmp (line, "@@ ", 3)) {
			fsetpos (p1, &at1);
			break;
		}

		if (!orig_lines && !new_lines) {
			if (read_atatline (line, NULL, &orig_lines,
					   &linenum, &new_lines))
				error (EXIT_FAILURE, 0,
				       "line not understood: %s", line);

			continue;
		}

		/* We'll track the offset relative to the intermediate
		 * file in linenum. */

		if (orig_lines && line[0] != '+')
			orig_lines--;
		if (new_lines && line[0] != '-')
			new_lines--;

		if (line[0] == '+') {
			if (!patch2_removes_line (linenum, offsets,
						  num_offsets)) {
				unsigned long at = linenum;
				at += offset_at_line (linenum, offsets,
						      num_offsets);
				at += this_offset;
				remove_line (&intermediate, line + 1, at);
				this_offset--;
			}
		} else if (line[0] == '-') {
			if (!patch2_removes_line (linenum, offsets,
						  num_offsets)) {
				unsigned long at = linenum;
				at += offset_at_line (linenum, offsets,
						      num_offsets);
				at += this_offset;
				insert_line (&intermediate, line + 1,
					     (size_t) got - 1, at);
			}
			this_offset++;
		}

		if (line[0] != '-')
			linenum++;
	}

	tmpfd = xmkstemp (tmpp2);
	write_file (&intermediate, tmpfd);

	/* Now tmpp1 is the start point, tmpp3 is the end point, and
	 * tmpp2 is the mid-point once the diffs have been flipped.
	 * So just take diffs and we're done. */
	take_diff (tmpp1, tmpp2, header2, intermediate.unline, flip1);
	take_diff (tmpp2, tmpp3, header1, intermediate.unline, flip2);

	free (line);
	free_offsets (offsets);
	fclose (f);

	if (debug)
		printf ("flipped\n");
	else {
		unlink (tmpp1);
		unlink (tmpp2);
		unlink (tmpp3);
	}

	return 0;
}

static int
copy (FILE *from, FILE *to)
{
	while (!feof (from)) {
		int ch = fgetc (from);
		if (ch == EOF)
			break;
		fputc (ch, to);
	}
	return 0;
}

static int
no_patch (const char *f)
{
	error (0, 0, "%s doesn't contain a patch", f);
	return 0;
}

static int
index_patch1 (FILE *p1)
{
	return index_patch_generic (p1, &files_in_patch1, 0);
}

static int
interdiff (FILE *p1, FILE *p2, const char *patch1, const char *patch2)
{
	char *line = NULL;
	size_t linelen = 0;
	int is_context = 0;
	int patch_found = 0;
	int file_is_empty = 1;
	FILE *flip1 = NULL, *flip2 = NULL;

	if (mode == mode_flip) {
		flip1 = xtmpfile ();
		flip2 = xtmpfile ();
	}

	if (index_patch2 (p2))
		no_patch (patch2);

	/* Index patch1 and determine ignore_components if not specified */
	if (index_patch1 (p1))
		no_patch (patch1);

	/* Determine ignore_components automatically if not explicitly specified */
	if (!ignore_components_specified) {
		ignore_components = determine_ignore_components (files_in_patch1, files_in_patch2);
		if (debug)
			fprintf (stderr, "Auto-determined -p%d\n", ignore_components);
	}

	/* Reset file pointer for patch1 */
	rewind (p1);

	/* Search for next file to patch */
	while (!feof (p1)) {
		char *names[2];
		char *p;
		long pos, start_pos = ftell (p1);

		if (line) {
			free (line);
			line = NULL;
			linelen = 0;
		}

		if (getline (&line, &linelen, p1) == -1)
			break;

		file_is_empty = 0;

		if (strncmp (line, "--- ", 4)) {
			is_context = !strncmp (line, "*** ", 4);
			continue;
		}

		if (is_context)
			error (EXIT_FAILURE, 0,
			       "I don't understand context diffs yet.");

		names[0] = filename_from_header (line + 4);

		if (getline (&line, &linelen, p1) == -1)
			break;

		if (strncmp (line, "+++ ", 4))
			continue;

		names[1] = filename_from_header (line + 4);

		p = xstrdup (best_name (2, names));
		free (names[0]);
		free (names[1]);
		patch_found = 1;

		/* check if we need to process it and init context */
		if (!check_filename(p)) {
			add_to_list (&files_done, p, 0);
			continue;
		}

		fseek (p1, start_pos, SEEK_SET);
		pos = file_in_list (files_in_patch2, p);
		if (pos == -1) {
			output_patch1_only (p1,
					    mode == mode_flip ? flip2 : stdout,
					    mode != mode_inter);
		} else {
			fseek (p2, pos, SEEK_SET);
			if (mode == mode_flip)
				flipdiff (p1, p2, flip1, flip2);
			else
				output_delta (p1, p2, stdout);
		}

		add_to_list (&files_done, p, 0);
                free (p);
	}

	if (!file_is_empty && !patch_found)
		no_patch (patch1);

	copy_residue (p2, mode == mode_flip ? flip1 : stdout);

	if (mode == mode_flip) {
		/* Now we flipped the two patches, show them. */
		rewind (flip1);
		rewind (flip2);

		if (flipdiff_inplace) {
			/* Use atomic in-place writing for safety */
			if (write_file_inplace(patch2, flip1) != 0)
				error (EXIT_FAILURE, errno, "failed to write %s", patch2);
			if (write_file_inplace(patch1, flip2) != 0)
				error (EXIT_FAILURE, errno, "failed to write %s", patch1);
		} else {
			copy (flip1, stdout);
			puts ("\n=== 8< === cut here === 8< ===\n");
			copy (flip2, stdout);
		}
	}

	if (flip1)
		fclose (flip1);
	if (flip2)
		fclose (flip2);
	free_list (files_in_patch1);
	free_list (files_in_patch2);
	free_list (files_done);
	if (line)
		free (line);
	return 0;
}

NORETURN static void
syntax (int err)
{
	const char *const syntax_str =
"usage: %s [OPTIONS] patch1 patch2\n"
"       %s --version|--help\n"
"OPTIONS are:\n"
"  -U N, --unified=N\n"
"                  max lines of context to carry\n"
"  -i, --ignore-case\n"
"                  Consider upper- and lower-case to be the same\n"
"  -w, --ignore-all-space\n"
"                  ignore whitespace changes in patches\n"
"  -b, --ignore-space-change\n"
"                  ignore changes in the amount of whitespace\n"
"  -B, --ignore-blank-lines\n"
"                  ignore changes whose lines are all blank\n"
"      --color[=WHEN]\n"
"                  colorize the output; WHEN can be 'never', 'always',\n"
"                    or 'auto' (default: auto, use 'never' to disable)\n"
"  -p N, --strip-match=N\n"
"                  pathname components to ignore\n"
"  -q, --quiet\n"
"                  don't add rationale text\n"
"  -d PAT, --drop-context=PAT\n"
"                  drop context on matching files\n"
"  -z, --decompress\n"
"                  decompress .gz and .bz2 files\n"
"  --interpolate   run as 'interdiff'\n"
"  --combine       run as 'combinediff'\n"
"  --flip          run as 'flipdiff'\n"
"  --no-revert-omitted\n"
"                  (interdiff) When a patch from patch1 is not in patch2,\n"
"                  don't revert it\n"
"  --in-place      (flipdiff) Write the output to the original input\n"
"                  files\n"
"  --fuzzy\n"
"                  (interdiff) Perform a fuzzy comparison, which filters\n"
"                  out hunks that the patch utility can apply with fuzz\n";

	fprintf (err ? stderr : stdout, syntax_str, progname, progname);
	exit (err);
}

static void
set_interdiff (void)
{
	/* This is interdiff. */
	set_progname ("interdiff");
	mode = mode_inter;
}

static void
set_combinediff (void)
{
	/* This is combinediff. */
	set_progname ("combinediff");
	mode = mode_combine;
}

static void
set_flipdiff (void)
{
	/* This is flipdiff. */
	set_progname ("flipdiff");
	mode = mode_flip;
}

static void
get_mode_from_name (const char *argv0)
{
	/* This is interdiff, unless it is named 'combinediff' or
	 * 'flipdiff'. */
	const char *p = strrchr (argv0, '/');
	if (!p++)
		p = argv0;
	if (strstr (p, "combine"))
		set_combinediff ();
	else if (strstr (p, "flip"))
		set_flipdiff ();
	else
		set_interdiff ();
}

int
main (int argc, char *argv[])
{
	FILE *p1, *p2;
	int ret;

	get_mode_from_name (argv[0]);
	for (;;) {
		static struct option long_options[] = {
			{"help", 0, 0, 1000 + 'H'},
			{"version", 0, 0, 1000 + 'V'},
			{"interpolate", 0, 0, 1000 + 'I' },
			{"combine", 0, 0, 1000 + 'C' },
			{"flip", 0, 0, 1000 + 'F' },
			{"no-revert-omitted", 0, 0, 1000 + 'R' },
			{"in-place", 0, 0, 1000 + 'i' },
			{"fuzzy", 0, 0, 1000 + 'f' },
			{"debug", 0, 0, 1000 + 'D' },
			{"strip-match", 1, 0, 'p'},
			{"unified", 1, 0, 'U'},
			{"drop-context", 1, 0, 'd'},
			{"ignore-blank-lines", 0, 0, 'B'},
			{"ignore-space-change", 0, 0, 'b'},
			{"ignore-case", 0, 0, 'i'},
			{"ignore-all-space", 0, 0, 'w'},
			{"color", 2, 0, 1000 + 'c'},
			{"decompress", 0, 0, 'z'},
			{"quiet", 0, 0, 'q'},
			{0, 0, 0, 0}
		};
		char *end;
		int c = getopt_long (argc, argv, "BU:bd:hip:qwz",
				     long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 1000 + 'V':
			printf("%s - patchutils version %s\n", progname,
			       VERSION);
			exit(0);
		case 1000 + 'H':
			syntax (0);
			break;
		case 'h':
			break;
		case 'U':
			max_context_real = strtoul (optarg, &end, 0);
			if (optarg == end)
				syntax (1);
			context_specified = 1;
			break;
		case 'p':
			ignore_components = strtoul (optarg, &end, 0);
			if (optarg == end)
				syntax (1);
			ignore_components_specified = 1;
			break;
		case 'q':
			human_readable = 0;
			break;
		case 'd':
			patlist_add (&pat_drop_context, optarg);
			break;
		case 'z':
			unzip = 1;
			break;
		case 'B':
		case 'b':
		case 'i':
		case 'w':
			if (asprintf (diff_opts + num_diff_opts++, "-%c", c) < 0)
				error (EXIT_FAILURE, errno, "Memory allocation failed");
			break;
		case 1000 + 'c': {
			/* Determine the color mode: default to "auto" if no argument given */
			const char *color_mode = optarg ? optarg : "auto";

			/* Handle auto mode: check if stdout is a terminal */
			if (strcmp(color_mode, "auto") == 0)
				color_mode = isatty(STDOUT_FILENO) ? "always" : "never";

			/* Set our internal color flag instead of passing to diff */
			use_colors = (strcmp(color_mode, "always") == 0);
			color_option_specified = 1;
			break;
		}
		case 1000 + 'I':
			set_interdiff ();
			break;
		case 1000 + 'C':
			set_combinediff ();
			break;
		case 1000 + 'F':
			set_flipdiff ();
			break;
		case 1000 + 'R':
			no_revert_omitted = 1;
			break;
		case 1000 + 'i':
			if (mode != mode_flip)
				syntax (1);
			flipdiff_inplace = 1;
			break;
		case 1000 + 'f':
			if (mode != mode_inter)
				syntax (1);
			fuzzy = 1;
			break;
		case 1000 + 'D':
			debug = 1;
			break;
		default:
			syntax(1);
		}
	}

	if (unzip && flipdiff_inplace)
		error (EXIT_FAILURE, 0,
		       "-z and --in-place are mutually exclusive.");

	/* Set default color behavior if no color option was specified */
	if (!color_option_specified && isatty(STDOUT_FILENO))
		use_colors = 1;

	if (optind + 2 != argc)
		syntax (1);

	if (unzip) {
		p1 = xopen_unzip (argv[optind], "rb");
		p2 = xopen_unzip (argv[optind + 1], "rb");
	} else {
		if (strcmp (argv[optind], "-") == 0 && strcmp(argv[optind+1], "-") == 0)
			error (EXIT_FAILURE, 0, "only one input file can come from stdin");
		p1 = strcmp (argv[optind], "-") == 0 ? stdin : xopen (argv[optind], "rbm");
		p2 = strcmp (argv[optind+1], "-") == 0 ? stdin : xopen (argv[optind + 1], "rbm");
	}

	p1 = convert_to_unified (p1, "rb", 1);
	p2 = convert_to_unified (p2, "rb", 1);

	ret = interdiff (p1, p2, argv[optind], argv[optind + 1]);

	fclose (p1);
	fclose (p2);
	patlist_free (&pat_drop_context);
	return ret;
}
