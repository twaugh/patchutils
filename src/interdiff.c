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
#include <limits.h>
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
#include <sys/param.h>
#include <sys/stat.h>

#include "util.h"
#include "diff.h"

#ifndef DIFF
#define DIFF "diff"
#endif

#ifndef PATCH
#define PATCH "patch"
#endif

/* Fuzzy mode section headers */
#define DELTA_DIFF_HEADER \
	"================================================================================\n" \
	"*    DELTA DIFFERENCES - code changes that differ between the patches          *\n" \
	"================================================================================\n\n"

#define DELTA_REJ_HEADER \
	"################################################################################\n" \
	"!    REJECTED PATCH2 HUNKS - could not be compared; manual review needed       !\n" \
	"################################################################################\n\n"

#define CONTEXT_DIFF_HEADER \
	"================================================================================\n" \
	"*    CONTEXT DIFFERENCES - surrounding code differences between the patches    *\n" \
	"================================================================================\n\n"

#define ONLY_IN_PATCH1_HEADER \
	"================================================================================\n" \
	"*    ONLY IN PATCH1 - files not modified by patch2                             *\n" \
	"================================================================================\n\n"

#define ONLY_IN_PATCH2_HEADER \
	"================================================================================\n" \
	"*    ONLY IN PATCH2 - files not modified by patch1                             *\n" \
	"================================================================================\n\n"

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

struct hunk_info {
	char *s; /* Start of hunk */
	size_t len; /* Length of hunk in bytes */
	unsigned long nstart; /* Starting line number */
	unsigned long nend; /* Ending line number (inclusive) */
	int relocated:1, /* Whether or not this hunk was relocated */
	    discard:1; /* Whether or not to discard this hunk */
};

struct hunk_reloc {
	unsigned long new; /* New starting line number */
	long off; /* Offset from the old starting line number */
	unsigned long fuzz; /* Fuzz amount reported by patch */
	int ignored:1; /* Whether or not this relocation was ignored */
};

struct line_info {
	char *s; /* Start of line */
	size_t len; /* Length of line in bytes */
};

struct xtra_context {
	unsigned long num; /* Number of extra context lines */
	char *s; /* String of extra context lines */
	size_t len; /* Length of extra context string in bytes */
};

struct rej_file {
	FILE *fp;
	unsigned long off;
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
static int max_fuzz_user = -1;

/* Per-file record for fuzzy mode output accumulation */
struct fuzzy_file_record {
	char *oldname;
	char *newname;
	FILE *hunks;  /* Raw hunk data (before trim_context) */
	struct fuzzy_file_record *next;
};

/* Per-hunk change/context line arrays for rejection filtering */
struct strarray {
	struct line_info *lines;
	size_t len;
};

struct hunk_lines {
	struct strarray add, del, ctx;
	unsigned int *ctx_dist; /* Distance from nearest +/- line */
};

struct fuzzy_file_list {
	struct fuzzy_file_record *head;
	struct fuzzy_file_record *tail;
};

/* Accumulators for fuzzy mode output sections */
static struct fuzzy_file_list fuzzy_delta_files = {};
static struct fuzzy_file_list fuzzy_ctx_files = {};
static struct fuzzy_file_list fuzzy_delta_rej_files = {};
static FILE *fuzzy_only_in_patch1 = NULL;
static FILE *fuzzy_only_in_patch2 = NULL;

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

			last_was_add = (first_char == '+');
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
		if (human_readable && !fuzzy && mode != mode_flip)
			fprintf (out, "unchanged:\n");
		fputs (oldname, out);
		fputs (line, out);
	} else if (!no_revert_omitted) {
		if (human_readable && !fuzzy)
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

static void
open_rej_file (const char *file, struct rej_file *rej)
{
	char *rej_file, *line = NULL;
	size_t linelen;
	long atat_pos;

	/* Open the .rej file */
	if (asprintf (&rej_file, "%s.rej", file) < 0)
		error (EXIT_FAILURE, errno, "asprintf failed");
	rej->fp = xopen (rej_file, "r");
	free (rej_file);

	/* Skip (the first two) lines to get to the start of the @@ line */
	do {
		atat_pos = ftell (rej->fp);
		if (getline (&line, &linelen, rej->fp) <= 0)
			error (EXIT_FAILURE, errno,
			       "Failed to read line from .rej");
	} while (strncmp (line, "@@ ", 3));
	fseek (rej->fp, atat_pos, SEEK_SET);

	/* Export the line offset of the first rej hunk */
	if (read_atatline (line, &rej->off, NULL, NULL, NULL))
		error (EXIT_FAILURE, 0, "line not understood: %s", line);
	free (line);

	/* Go back to the @@ after apply_patch() moved the file cursor */
	fseek (rej->fp, atat_pos, SEEK_SET);
}

static int
get_fuzz (void)
{
	if (max_fuzz_user >= 0)
		return max_fuzz_user;

	if (max_context)
		return max_context - 1;

	return 0;
}

static int
apply_patch (FILE *patch, const char *file, int reverted, struct rej_file *rej,
	     FILE **out)
{
#define MAX_PATCH_ARGS 8
	const char *argv[MAX_PATCH_ARGS];
	int argc = 0;
	const char *basename;
	unsigned long orig_lines, new_lines;
	char *line, *fuzz_arg = NULL;
	size_t linelen;
	int fildes[4];
	FILE *r, *w;
	pid_t child;
	int status;

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
	argv[argc++] = reverted ? (has_ignore_all_space ? "-NRlp0" : "-NRp0")
				: (has_ignore_all_space ? "-Nlp0" : "-Np0");
	if (fuzzy) {
		/* Don't generate .orig files when we expect rejected hunks */
		argv[argc++] = "--no-backup-if-mismatch";

		if (asprintf (&fuzz_arg, "--fuzz=%d", get_fuzz ()) < 0)
			error (EXIT_FAILURE, errno, "asprintf failed");
		argv[argc++] = fuzz_arg;
	}
	/* Fuzzy mode needs hunk offset messages. Only silence output when
	 * piping stdout wasn't requested. */
	if (!out)
		argv[argc++] = "--silent";
	argv[argc++] = file;
	argv[argc++] = NULL;

	/* Flush any pending writes, set up two pipes, and then fork */
	fflush (NULL);
	if (pipe (fildes) == -1 || pipe (&fildes[2]) == -1)
		error (EXIT_FAILURE, errno, "pipe failed");
	child = fork ();
	if (child == -1) {
		perror ("fork");
		exit (1);
	}

	if (child == 0) {
		/* Keep two pipes: one open to stdin, one to stdout */
		close (0);
		close (1);
		if (dup (fildes[0]) == -1 || dup (fildes[3]) == -1)
			error (EXIT_FAILURE, errno, "dup failed");
		close (fildes[0]);
		close (fildes[1]);
		close (fildes[2]);
		close (fildes[3]);
		execvp (argv[0], (char **)argv);
	}
	free (fuzz_arg);

	/* Open the read and write ends of the two pipes */
	if (!(r = fdopen (fildes[2], "r")) || !(w = fdopen (fildes[1], "w")))
		error (EXIT_FAILURE, errno, "fdopen");
	close (fildes[0]);
	close (fildes[3]);

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
	free (line);
	waitpid (child, &status, 0);
	status = WEXITSTATUS (status);

	/* Provide the output from patch if requested */
	if (out)
		*out = r;
	else
		fclose (r);

	/* Open the reject file if requested and there are rejects */
	if (status && rej)
		open_rej_file (file, rej);

	return status;
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
					if (unline && !strcmp (line + 1, unline))
						strip_pre = pre;
				} else {
					post++;
					if (strip_post ||
					    (unline && !strcmp (line + 1, unline)))
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
			print_color (out, type, "%.*s", (int) got - 1, line);
			fputc ('\n', out);
		}
	}

	free (line);
	return 0;

 split_hunk:
	error (0, 0, "hunk-splitting is required in this case, but is not yet implemented");
	error (1, 0, "use the -U option to work around this");
	return 0;
}

/* Skip past the two --- / +++ header lines. Returns 1 on EOF. */
static int
skip_header_lines (FILE *f)
{
	int ch;

	for (int i = 0; i < 2; i++)
		while ((ch = fgetc (f)) != '\n' && ch != EOF);

	return ch == EOF;
}

/* Add a file record to a fuzzy output list */
static void
fuzzy_add_file (struct fuzzy_file_list *list, const char *oldname,
		const char *newname, FILE *hunks)
{
	struct fuzzy_file_record *rec = xmalloc (sizeof (*rec));

	rec->oldname = xstrdup (oldname);
	rec->newname = xstrdup (newname);
	rec->hunks = hunks;
	rec->next = NULL;

	if (list->tail)
		list->tail->next = rec;
	else
		list->head = rec;
	list->tail = rec;
}

/* Free a fuzzy file record list */
static void
fuzzy_free_list (struct fuzzy_file_list *list)
{
	struct fuzzy_file_record *rec = list->head;

	while (rec) {
		struct fuzzy_file_record *next = rec->next;

		free (rec->oldname);
		free (rec->newname);
		if (rec->hunks)
			fclose (rec->hunks);
		free (rec);
		rec = next;
	}
}

/* Output a fuzzy file list with colorization through trim_context. If
 * skip_headers is set, skip past the --- / +++ lines (e.g. for reject files
 * which include their own headers). */
static void
fuzzy_output_list (struct fuzzy_file_list *list, int skip_headers, FILE *out)
{
	struct fuzzy_file_record *rec;

	for (rec = list->head; rec; rec = rec->next) {
		if (!rec->hunks)
			continue;

		rewind (rec->hunks);

		/* Skip past --- / +++ headers */
		if (skip_headers && skip_header_lines (rec->hunks))
			error (EXIT_FAILURE, 0, "truncated hunk file");

		print_color (out, LINE_FILE, "--- %s\n", rec->oldname);
		print_color (out, LINE_FILE, "+++ %s\n", rec->newname);
		trim_context (rec->hunks, NULL, out);
	}
}

/* Colorize and output a raw diff (with --- / +++ / @@ headers) */
static void
colorize_diff (FILE *in, FILE *out)
{
	char *line = NULL;
	size_t linelen;
	ssize_t got;

	rewind (in);
	while ((got = getline (&line, &linelen, in)) > 0) {
		enum line_type type;

		if (!strncmp (line, "--- ", 4) || !strncmp (line, "+++ ", 4)) {
			type = LINE_FILE;
		} else if (!strncmp (line, "@@ ", 3)) {
			type = LINE_HUNK;
		} else if (line[0] == '-') {
			type = LINE_REMOVED;
		} else if (line[0] == '+') {
			type = LINE_ADDED;
		} else {
			fwrite (line, got, 1, out);
			continue;
		}
		print_color (out, type, "%.*s", (int) got - 1, line);
		fputc ('\n', out);
	}
	free (line);
}

/* `xctx` must come with `num` initialized and `s` and `len` zeroed */
static void
ctx_lookbehind (const struct line_info *lines, unsigned long start_line_idx,
		struct xtra_context *xctx)
{
	unsigned long i, num = 0;

	for (i = start_line_idx - 1; i < start_line_idx; i--) {
		const struct line_info *line = &lines[i];

		if (*line->s == '+')
			continue;

		/* Copy out the line and ensure the first character is a space,
		 * since it may be a minus. */
		xctx->s = xrealloc (xctx->s, xctx->len + line->len);
		memmove (xctx->s + line->len, xctx->s, xctx->len);
		memcpy (xctx->s, line->s, line->len);
		*xctx->s = ' ';
		xctx->len += line->len;

		/* Quit when we've got the desired number of context lines */
		if (++num == xctx->num)
			return;
	}

	/* Record the actual number of extra content lines found, since it is
	 * less than the number of lines requested. */
	xctx->num = num;
}

/* `xctx` must come with `num` initialized and `s` and `len` zeroed */
static void
ctx_lookahead (const char *hunk, size_t hlen, struct xtra_context *xctx)
{
	const char *line, *next_line;
	unsigned long num = 0;
	size_t linelen;

	/* `hunk` is positioned at the first character of the current line
	 * parsed by split_patch_hunks(). Reduce it by one first to go to the
	 * newline character of the previous line, to make our loop simpler. */
	for (line = hunk - 1;; line = next_line) {
		/* Go to the character _after_ the newline character */
		line++;

		/* Get the next line now to find the length of the line */
		next_line = memchr (line, '\n', hunk + hlen - line);
		if (*line != '+') {
			linelen = next_line + 1 - line;

			/* Copy out the line and ensure the first character is a
			 * space, since it may be a minus. */
			xctx->s = xrealloc (xctx->s, xctx->len + linelen);
			memcpy (xctx->s + xctx->len, line, linelen);
			xctx->s[xctx->len] = ' ';
			xctx->len += linelen;

			/* Quit when we've got the desired amount of context */
			if (++num == xctx->num)
				break;
		}

		/* Stop when this is the end of the hunk, recording the actual
		 * number of extra context lines found. */
		if (!next_line || next_line + 1 == hunk + hlen) {
			xctx->num = num;
			break;
		}
	}
}

/* Squash up to max_context*2 unlines between two hunks */
static int
squash_unline_gap (char **line_ptr, size_t hlen, const char *unline,
		   size_t unline_len)
{
	char *hunk = *line_ptr, *line = hunk, *prev = line;
	unsigned int num_unlines = 1;
	int squash = 0;

	for (; (line = memchr (line, '\n', hunk + hlen - line)); prev = line) {
		/* Go to the character _after_ the newline character */
		line++;

		/* Stop when there's nothing left */
		if (line == hunk + hlen)
			break;

		/* Move the line pointer to the last unline in the chunk of up
		 * to max_context*2 unlines so the loop in split_patch_hunks()
		 * skips over it and thus skips over the entire unline chunk. */
		if (strncmp (line + 1, unline, unline_len)) {
			squash = 1;
			break;
		}

		if (++num_unlines > max_context * 2)
			break;
	}

	/* Always advance the line pointer even without squashing */
	*line_ptr = prev;
	return squash;
}

static void
write_xctx (struct xtra_context *xctx, FILE *out)
{
	if (xctx->s) {
		fwrite (xctx->s, xctx->len, 1, out);
		free (xctx->s);
	}
}

/* Regenerate a patch with the hunks split up to ensure more of the patch gets
 * applied successfully. Outputs a `hunk_offs` array (if requested) to map each
 * hunk's post-split offset from the original hunk's new line number.
 *
 * When the unline is provided, that is a hint to strip unlines from context and
 * perform splits at unlines in the middle of a hunk. */
static FILE *
split_patch_hunks (FILE *patch, size_t len, char *file,
		   unsigned long **hunk_offs, const char *unline)
{
	char *fbuf, *hunk, *next_hunk;
	unsigned long hnum = 0;
	int has_output = 0;
	size_t unline_len;
	FILE *out;

	/* Read the patch into a NUL-terminated buffer */
	if (len) {
		fbuf = xmalloc (len + 1);
		if (fread (fbuf, 1, len, patch) != len)
			error (EXIT_FAILURE, errno, "fread() of patch failed");
	} else {
		/* The patch is a pipe; we can't seek it, so read until EOF */
		fbuf = NULL;
		for (int ch; (ch = fgetc (patch)) != EOF;) {
			fbuf = xrealloc (fbuf, ++len + 1);
			fbuf[len - 1] = ch;
		}
	}
	fbuf[len] = '\0';

	/* Find the first hunk. `fbuf` is positioned at the start of a line. */
	if (!strncmp (fbuf, "@@ ", 3)) {
		hunk = fbuf;
	} else {
		hunk = strstr (fbuf, "\n@@ ");
		if (!hunk)
			error (EXIT_FAILURE, 0, "patch file malformed: %s", fbuf);
	}

	if (unline) {
		/* Create a temporary file for the unline-cleansed output */
		out = xtmpfile ();

		/* Find the length of the unline now to use it in the loop */
		unline_len = strlen (unline);
	} else {
		char *out_file;

		/* Create the output file */
		if (asprintf (&out_file, "%s.patch", file) < 0)
			error (EXIT_FAILURE, errno, "asprintf failed");
		out = xopen (out_file, "w+");
		free (out_file);
	}

	do {
		/* nctx[0] = pre-context lines, nctx[1] = post-context lines
		 * ndelta[0] = deleted lines, ndelta[1] = added lines */
		unsigned long nctx[2] = {}, ndelta[2] = {};
		unsigned long ostart, nstart, orig_nstart, start_line_idx = 0;
		struct xtra_context xctx_pre = {};
		struct line_info *lines = NULL;
		unsigned long num_lines = 0;
		int skipped_lines = 0;
		char *line;
		size_t hlen;

		if (read_atatline (hunk, &ostart, NULL, &nstart, NULL))
			error (EXIT_FAILURE, 0, "line not understood: %s",
			       strsep (&hunk, "\n"));

		/* Save the original hunk's new line number */
		orig_nstart = nstart;

		/* Find the next hunk now to tell where the current hunk ends */
		next_hunk = strstr (hunk, "\n@@ ");
		if (next_hunk)
			hlen = ++next_hunk - hunk;
		else
			hlen = strlen (hunk);

		/* Split this hunk into multiple smaller hunks, if possible.
		 * This is done by looking for deltas (+/- lines) that aren't
		 * contiguous and thus have context lines in between them. Note
		 * that the first line is intentionally skipped because the
		 * first line is the @@ line. When no splitting occurs, this
		 * still has the effect of trimming context lines for the hunk
		 * to ensure the number of pre-context lines and post-context
		 * lines are equal. */
		for (line = hunk; (line = memchr (line, '\n', hunk + hlen - line));) {
			unsigned long start_off = 0, onum, nnum;
			struct line_info *start_line, *end_line;
			struct xtra_context xctx_post = {};
			size_t hlen_rem;

			/* Go to the character _after_ the newline character */
			line++;

			/* Set the length of the previous line (if any). Only do
			 * this once because when doing unline splitting, the
			 * unlines aren't recorded into the lines array. */
			if (lines && !lines[num_lines - 1].len)
				lines[num_lines - 1].len =
					line - lines[num_lines - 1].s;

			/* Count the number of characters left to parse */
			hlen_rem = hunk + hlen - line;

			/* Check if this is an unline that we need to remove, or
			 * if this is a bogus hunk. A bogus hunk may not have an
			 * unline as its final line, hence we need to consider
			 * this when there are no more lines left to parse. */
			if (unline && (!hlen_rem || !strncmp (line + 1, unline,
							      unline_len))) {
				/* Split the hunk now if there's a delta, unless
				 * this is a bogus hunk from a rejected patch
				 * hunk. Bogus hunks stem from one side of the
				 * diff operation consisting only of unlines.
				 * Such diffs have only unlines in their context
				 * and only one delta type: either additions or
				 * subtractions, _not_ both. Discard bogus hunks
				 * by skipping over them here, which is fine
				 * since the corresponding rejected patch hunk
				 * is emitted later.
				 *
				 * Sometimes a hunk may appear bogus when it is
				 * not; this can be identified by checking if
				 * there are no more than max_context*2 unlines
				 * until the next hunk. Squash the unlines away
				 * in that case, which alters the line numbers
				 * of the hunk as a side effect. The assumption
				 * is that these two hunks are related to each
				 * other but are just slightly offset in the two
				 * diffed files due to small bits of missing
				 * context that were filled in with unlines. */
				if (ndelta[0] || ndelta[1]) {
					if (nctx[0] || nctx[1] ||
					    (ndelta[0] && ndelta[1]))
						goto split_hunk_incl_latest;

					if (squash_unline_gap (&line, hlen_rem,
							       unline,
							       unline_len)) {
						skipped_lines = 1;
						continue;
					}

					/* Bogus hunk, reset the delta counts */
					ndelta[0] = ndelta[1] = 0;
				}

				/* Stop now when nothing remains, since all that
				 * we've got here is a bogus hunk to discard. */
				if (!hlen_rem)
					break;

				/* Move forward the starting line offset,
				 * discarding any pre-context lines seen. The
				 * starting line index is set to the _next_
				 * (non-unline) line, which may not exist. */
				start_line_idx = num_lines;
				start_off += nctx[0] + 1;
				nctx[0] = 0;
				continue;
			}

			/* Check if this is the end. If so, terminate the hunk
			 * now because there isn't any new line to parse. */
			if (!hlen_rem)
				goto split_hunk_incl_latest;

			/* Skip '\ No newline at end of file' markers for
			 * counting purposes but include them in the output
			 * by extending the previous line's length to cover
			 * the marker. */
			if (*line == '\\') {
				if (lines) {
					char *eol = memchr (line, '\n',
							    hunk + hlen -
							    line);
					lines[num_lines - 1].len =
						(eol ? eol + 1 : hunk + hlen)
						- lines[num_lines - 1].s;
				}
				continue;
			}

			/* Record the current line, setting `len` to zero */
			lines = xrealloc (lines, ++num_lines * sizeof (*lines));
			lines[num_lines - 1] = (typeof (*lines)){ line };

			/* Track +/- lines as well as pre-context and post-
			 * context lines. Split the hunk upon encountering a +/-
			 * line after post-context lines, unless we're splitting
			 * at unlines instead. */
			if (*line == '+' || *line == '-') {
				if (!unline && nctx[1]) {
					/* The current line belongs to the
					 * _next_ split hunk. Exclude it. */
					end_line = &lines[num_lines - 2];
					goto split_hunk;
				}

				ndelta[*line == '+']++;
			} else {
				nctx[ndelta[0] || ndelta[1]]++;
			}

			/* Keep parsing until there's a need to do a split */
			continue;

split_hunk_incl_latest:
			/* Split the hunk including the latest recorded line */
			end_line = &lines[num_lines - 1];
split_hunk:
			/* Stop now if there are no lines left to make a hunk */
			if (start_line_idx == num_lines)
				break;

			/* Check that there's an actual delta recorded */
			if (!ndelta[0] && !ndelta[1])
				error (EXIT_FAILURE, 0, "hunk without +/- lines?");

			/* Split the current hunk by terminating it and starting
			 * a new hunk. When generating a patch to apply, there
			 * must be the same number of pre-context lines as post-
			 * context lines, otherwise patch will need to fuzz the
			 * extra context lines. An exception is when the context
			 * is at either the beginning or end of the file. Target
			 * having the same number of pre-context and post-
			 * context lines as the original hunk itself, so the
			 * user-provided fuzz factor behaves as expected. Note
			 * that this adjustment impacts ostart and nstart either
			 * for the current split hunk or the next split hunk. */
			start_line = &lines[start_line_idx];
			if (unline) {
				/* Add the start offset to the old/new lines */
				ostart += start_off;
				nstart += start_off;
			} else if (hlen_rem && nctx[1] < nctx[0] + xctx_pre.num) {
				/* Ensure post-context is at least as large as
				 * pre-context to avoid a negative suffix_fuzz
				 * in the patch utility, which would restrict
				 * matching to end-of-file. */
				xctx_post.num = nctx[0] + xctx_pre.num - nctx[1];
				ctx_lookahead (line, hlen_rem, &xctx_post);
			}

			/* Cap post-context at the pre-context count so the fuzz
			 * budget is evenly split. The patch utility distributes
			 * fuzz as prefix_fuzz = fuzz + prefix - context, so when
			 * suffix > prefix, all the fuzz goes to the suffix and
			 * prefix mismatches can't be fuzzed at all. */
			if (!unline) {
				unsigned long prefix = nctx[0] + xctx_pre.num;
				unsigned long suffix = nctx[1] + xctx_post.num;
				int fuzz = get_fuzz ();

				if (suffix > prefix) {
					unsigned long trim = suffix - prefix;

					/* Keep at least 1 context line so
					 * the patch utility can anchor the
					 * hunk to the correct position. */
					if (trim >= nctx[1])
						trim = nctx[1] ? nctx[1] - 1
							       : 0;
					end_line -= trim;
					nctx[1] -= trim;
				} else if (prefix > suffix + fuzz &&
					   nctx[0]) {
					/* Trim excess pre-context when the
					 * suffix_fuzz would go negative in
					 * the patch utility. This handles
					 * the last sub-hunk at end-of-file
					 * where ctx_lookahead cannot help. */
					unsigned long trim =
						prefix - suffix - fuzz;
					if (trim > nctx[0])
						trim = nctx[0];
					start_line += trim;
					start_line_idx += trim;
					ostart += trim;
					nstart += trim;
					nctx[0] -= trim;
				}
			}

			/* Calculate the old and new line counts */
			onum = nnum = xctx_pre.num + /* Extra pre-context */
				      end_line + 1 - start_line + /* Hunk */
				      xctx_post.num; /* Extra post-context */
			onum -= ndelta[1];
			nnum -= ndelta[0];

			/* Emit the hunk to the output file */
			fprintf (out, "@@ -%lu,%lu +%lu,%lu @@\n",
				 ostart, onum, nstart, nnum);
			write_xctx (&xctx_pre, out);
			/* If lines were skipped, then the output needs to be
			 * written one line at a time. */
			if (skipped_lines) {
				skipped_lines = 0;
				for (unsigned long i = start_line_idx;
				     &lines[i] <= end_line; i++)
					fwrite (lines[i].s, lines[i].len, 1, out);
			} else {
				fwrite (start_line->s,
					end_line->s + end_line->len - start_line->s,
					1, out);
			}
			write_xctx (&xctx_post, out);
			has_output = 1;

			/* Save the offset from this hunk's original new line */
			if (hunk_offs) {
				*hunk_offs = xrealloc (*hunk_offs, ++hnum *
						       sizeof (*hunk_offs));
				(*hunk_offs)[hnum - 1] = nstart - orig_nstart;
			}

			/* Stop when there's nothing left */
			if (!hlen_rem)
				break;

			/* Start the next hunk */
			start_line_idx = num_lines;
			ostart += onum;
			nstart += nnum;
			if (unline) {
				/* The current line is not included in the next
				 * hunk when splitting at unlines. */
				nctx[0] = nctx[1] = ndelta[0] = ndelta[1] = 0;
			} else {
				/* Find extra pre-context if extra post-context
				 * was used for this split hunk, since it means
				 * that there isn't enough normal post-context
				 * to be the next split hunk's pre-context.
				 * Clamp both nctx[0] and xctx_pre so the next
				 * hunk's prefix never causes a negative
				 * suffix_fuzz in the patch utility. */
				nctx[0] = MIN (nctx[1], max_context);

				/* If the overlap would push ostart below 1,
				 * skip it — there are no lines before the
				 * start of the file to overlap with. */
				if (xctx_post.num + nctx[0] >= ostart) {
					nctx[0] = 0;
					start_line_idx -= 1;
				} else {
					start_line_idx -= 1 + nctx[0];
					xctx_pre = (typeof (xctx_pre))
						{ MIN (xctx_post.num,
						       get_fuzz ()) };
					if (xctx_pre.num)
						ctx_lookbehind (lines,
							start_line_idx,
							&xctx_pre);
					ostart -= xctx_pre.num +
						  xctx_post.num + nctx[0];
					nstart -= xctx_pre.num +
						  xctx_post.num + nctx[0];
				}
				nctx[1] = 0;
				ndelta[1] = *line == '+';
				ndelta[0] = !ndelta[1];
			}
		}
		free (lines);
	} while ((hunk = next_hunk));
	free (fbuf);

	/* No output, no party. Can happen if the hunks were only unlines. */
	if (!has_output) {
		fclose (out);
		return NULL;
	}

	/* Reposition the output file back to the beginning */
	rewind (out);
	return out;
}

static int
hunk_info_cmp (const void *lhs_ptr, const void *rhs_ptr)
{
	const struct hunk_info *lhs = lhs_ptr, *rhs = rhs_ptr;

	return lhs->nstart - rhs->nstart;
}

static int
hunk_reloc_cmp (const void *lhs_ptr, const void *rhs_ptr)
{
	const struct hunk_reloc *lhs = lhs_ptr, *rhs = rhs_ptr;

	return lhs->new - rhs->new;
}

static void
parse_fuzzed_hunks (FILE *patch_out, const unsigned long *hunk_offs,
		    struct hunk_reloc **relocs, unsigned long *num_relocs)
{
	char *line = NULL;
	size_t linelen;

	/* Parse out each fuzzed hunk's line offset */
	while (getline (&line, &linelen, patch_out) > 0) {
		struct hunk_reloc *prev = &(*relocs)[*num_relocs - 1];
		unsigned long fuzz = 0, hnum, lnum, split_off;
		long off;

		if (sscanf (line, "Hunk #%lu succeeded at %lu (offset %ld",
			    &hnum, &lnum, &off) != 3 &&
		    sscanf (line, "Hunk #%lu succeeded at %lu with fuzz %lu (offset %ld",
			    &hnum, &lnum, &fuzz, &off) != 4)
			continue;

		/* Get the split offset for this hunk - the difference between
		 * the split hunk's new line number and the original hunk's. */
		split_off = hunk_offs[hnum - 1];

		/* Skip if this hunk belongs to the same original hunk as the
		 * previous relocation. Split hunks are contiguous. Compare
		 * original line numbers (applied position - total offset). */
		if (*relocs && lnum - off - split_off == prev->new - prev->off)
			continue;

		/* Store the actual applied position in the patched file as `new`,
		 * and the total offset (patch offset + split offset) needed to
		 * relocate the hunk back to its original intended position. */
		*relocs = xrealloc (*relocs, ++*num_relocs * sizeof (**relocs));
		(*relocs)[*num_relocs - 1] =
			(typeof (**relocs)){ lnum, off + split_off, fuzz };
	}
	free (line);
}

static void
fuzzy_relocate_hunks (const char *file, const char *unline, FILE *patch_out,
		      const unsigned long *hunk_offs)
{
	struct hunk_info *hunks = NULL;
	struct hunk_reloc *relocs = NULL;
	unsigned long num_hunks = 0, num_relocs = 0;
	unsigned long i, j, num_unlines = 0;
	char *end, *endl, *fbuf, *start;
	int new_hunk = 1;
	size_t unlinelen;
	struct stat st;
	FILE *fp;

	/* Parse the fuzzed hunks when relocating for line offset differences */
	if (patch_out)
		parse_fuzzed_hunks (patch_out, hunk_offs, &relocs, &num_relocs);

	/* Open the patched file and copy it into a buffer */
	if (stat (file, &st) < 0)
		error (EXIT_FAILURE, errno, "stat() fail");
	fbuf = xmalloc (st.st_size);
	fp = xopen (file, "r");
	if (fread (fbuf, 1, st.st_size, fp) != st.st_size)
		error (EXIT_FAILURE, errno, "fread() fail");
	fclose (fp);

	/* Sort the relocations array by ascending order of new line number. A
	 * relocation may indicate that a contiguous block of code should
	 * actually be split into two or more hunks to better align with the
	 * other file, since they are split up in the other file. Sorting the
	 * relocations is needed for tracking this during hunk enumeration. */
	if (relocs)
		qsort (relocs, num_relocs, sizeof (*relocs), hunk_reloc_cmp);

	/* Enumerate every hunk in the file */
	start = fbuf; /* Start of the line */
	end = fbuf + st.st_size; /* End of the file */
	unlinelen = strlen(unline); /* Unline length (includes newline char) */
	for (endl = fbuf, i = 1, j = 0;
	     (endl = memchr (endl, '\n', end - endl));
	     start = ++endl, i++) {
		size_t len = endl - start + 1;

		/* Cut a new hunk if a relocated hunk starts at this line. This
		 * is important because a relocated hunk may start in the middle
		 * of a larger hunk, which is a hint to split the hunk. Note
		 * that a relocation may occur on an unline, which is corrected
		 * later on in a different loop. When that is the case, we still
		 * need to iterate past the relocation at that line in order to
		 * continue through the relocations array. */
		if (j < num_relocs && i == relocs[j].new) {
			j++;
			new_hunk = 1;
		}

		/* Skip over unlines */
		if (len == unlinelen && !memcmp (start, unline, len)) {
			num_unlines++;
			new_hunk = 1;
			continue;
		}

		/* Keep expanding the current detected hunk */
		if (!new_hunk) {
			hunks[num_hunks - 1].len += len;
			hunks[num_hunks - 1].nend++;
			num_unlines = 0;
			continue;
		}
		new_hunk = 0;

		/* Start a new hunk */
		hunks = xrealloc (hunks, ++num_hunks * sizeof (*hunks));
		hunks[num_hunks - 1] = (typeof (*hunks)){ start, len, i, i };

		/* Check the number of unlines between the end of the previous
		 * hunk (if any) and the start of the current hunk. If there are
		 * no more than max_context*2 unlines between the two, then eat
		 * the unlines and combine the hunks together. Note that we must
		 * also ignore the relocation for this hunk, if any, while
		 * accounting for the relocation new line possibly being up to
		 * `fuzz` lines _before_ the actual line (see more below). */
		if (num_hunks > 1 && num_unlines <= max_context * 2) {
			struct hunk_info *hcurr = &hunks[num_hunks - 1];
			struct hunk_info *hprev = hcurr - 1;

			for (int k = num_relocs - 1; k >= 0; k--) {
				struct hunk_reloc *rcurr = &relocs[k];
				unsigned long delta;

				if (rcurr->new <= hcurr->nstart) {
					delta = hcurr->nstart - rcurr->new;
					if (delta <= rcurr->fuzz)
						rcurr->ignored = 1;
					break;
				}
			}

			hcurr->nstart = hcurr->nend = hprev->nend + 1;
		}
		num_unlines = 0;
	}

	/* Check and possibly correct the new line number in the case of fuzzed
	 * hunks. Patch can screw this up and emit a line number up to `fuzz`
	 * lines _before_ the actual line. */
	for (i = 0; i < num_relocs; i++) {
		struct hunk_reloc *rcurr = &relocs[i];

		/* Skip ignored relocations and relocations without fuzz */
		if (rcurr->ignored || !rcurr->fuzz)
			continue;

		for (j = 0; j < num_hunks; j++) {
			struct hunk_info *hcurr = &hunks[j];
			unsigned long delta;

			/* Find a hunk that starts within `fuzz` lines after
			 * this relocation. If it does, correct the new line
			 * number and the offset to use this hunk. */
			if (hcurr->nstart >= rcurr->new) {
				delta = hcurr->nstart - rcurr->new;
				if (delta <= rcurr->fuzz) {
					rcurr->new += delta;
					rcurr->off += delta;
				}
				break;
			}
		}
	}

	/* Apply relocations */
	for (i = 0; i < num_relocs; i++) {
		struct hunk_reloc *rcurr = &relocs[i];
		int found = 0;

		if (rcurr->ignored)
			continue;

		for (j = 0; j < num_hunks; j++) {
			struct hunk_info *hcurr = &hunks[j], *hprev = hcurr - 1;

			/* Make sure we don't relocate a hunk more than once */
			if (hcurr->relocated)
				continue;

			/* Look for the hunk that starts at the new line number,
			 * subtracting the offset to get the hunk's _original_
			 * new line number. And relocate succeeding hunks that
			 * had their unlines squelched between this hunk. */
			if (hcurr->nstart == rcurr->new ||
			    (found && hcurr->nstart ==
			     hprev->nend + rcurr->off + 1)) {
				hcurr->nstart -= rcurr->off;
				hcurr->nend -= rcurr->off;
				hcurr->relocated = 1;
				found = 1;
			} else if (found) {
				break;
			}
		}

		/* Fail if we couldn't find the hunk in question */
		if (!found)
			error (EXIT_FAILURE, 0, "failed to relocate hunk");
	}

	/* Now that all hunks' final positions are determined, discard hunks
	 * that overlap with a relocated hunk's new position. Such hunks will
	 * have generated rejects on the other orig file, which will be emitted
	 * separately and thus removing the conflicting hunk here won't result
	 * in any loss of information from the diff. */
	for (i = 0; i < num_hunks; i++) {
		/* Find the next relocated hunk */
		if (!hunks[i].relocated)
			continue;

		/* Check all non-relocated hunks for conflicts to discard. It is
		 * possible for there to be more than one conflicting hunk. */
		for (j = 0; j < num_hunks; j++) {
			if (hunks[j].relocated || hunks[j].discard)
				continue;

			/* Check if hunks[j] starts or ends in hunks[i] */
			if ((hunks[j].nstart >= hunks[i].nstart &&
			     hunks[j].nstart <= hunks[i].nend) ||
			    (hunks[j].nend >= hunks[i].nstart &&
			     hunks[j].nend <= hunks[i].nend))
				hunks[j].discard = 1;
		}
	}

	/* Sort the hunks by ascending order of starting line number */
	qsort (hunks, num_hunks, sizeof (*hunks), hunk_info_cmp);

	/* Write the final result to the patched file, maintaining the same
	 * unline. The result (in bytes, not lines) may be smaller than before
	 * due to some hunks getting discarded and thus replaced by unlines, so
	 * truncate the entire file before writing. */
	fp = xopen (file, "w+");
	for (i = 0, j = 1; i < num_hunks; i++) {
		if (hunks[i].discard)
			continue;

		/* Write out unlines between the previous and current hunks */
		for (; j < hunks[i].nstart; j++)
			fwrite (unline, unlinelen, 1, fp);
		j = hunks[i].nend + 1;

		/* Write out the hunk itself */
		fwrite (hunks[i].s, hunks[i].len, 1, fp);
	}

	/* All done, clean everything up */
	fclose (fp);
	free (fbuf);
	free (hunks);
	free (relocs);
}

static void
fuzzy_cleanup (const char *file, int rej)
{
	size_t len = strlen (file);
	char *tmp = xmalloc (len + sizeof (".patch"));
	char *end = &tmp[len];

	memcpy (tmp, file, len);

	/* Remove the .rej file if one was generated */
	if (rej) {
		strcpy (end, ".rej");
		unlink (tmp);
	}

	/* Remove the .patch file generated from splitting up the hunks */
	strcpy (end, ".patch");
	unlink (tmp);

	free (tmp);
}

/* Run diff on two files and return a FILE* positioned at the first @@ line.
 * The --- and +++ header lines are consumed.
 *
 * Returns NULL if diff is empty. The caller must waitpid() on the returned pid
 * when return is non-NULL. */
static FILE *
run_diff (const char *options, const char *file1, const char *file2,
	  pid_t *child_out)
{
	pid_t child;
	FILE *in;

	fflush (NULL);

	char *argv[2 + num_diff_opts + 2 + 1];
	memcpy (argv, ((const char *[]) { DIFF, options }), 2 * sizeof (char *));
	memcpy (argv + 2, diff_opts, num_diff_opts * sizeof (char *));
	memcpy (argv + 2 + num_diff_opts,
		((char *[]) { (char *)file1, (char *)file2, NULL }),
		(2 + 1) * sizeof (char *));
	in = xpipe (DIFF, &child, "r", argv);

	*child_out = child;

	/* Skip past the --- / +++ lines output by diff */
	if (skip_header_lines (in)) {
		fclose (in);
		waitpid (child, NULL, 0);
		return NULL;
	}

	return in;
}

static int
line_info_eq (const struct line_info *a, const struct line_info *b)
{
	return a->len == b->len && !memcmp (a->s, b->s, a->len);
}

static void
strarray_push (struct strarray *sa, const char *str, size_t len)
{
	sa->lines = xrealloc (sa->lines, (sa->len + 1) * sizeof (*sa->lines));
	sa->lines[sa->len].s = xmalloc (len);
	memcpy (sa->lines[sa->len].s, str, len);
	sa->lines[sa->len].len = len;
	sa->len++;
}

static void
strarray_free (struct strarray *sa)
{
	for (size_t i = 0; i < sa->len; i++)
		free (sa->lines[i].s);
	free (sa->lines);
}

/* Parse hunks from a FILE* into an array of hunk_lines. For delta hunks, also
 * captures raw hunk content into raw_hunks[] for later output.
 *
 * Pass NULL for raw_hunks if not needed. */
static size_t
parse_hunks (FILE *f, struct hunk_lines **out, struct line_info **raw_hunks)
{
	struct hunk_lines *hunks = NULL;
	size_t nhunks = 0, linelen;
	char *line = NULL;
	ssize_t got;

	if (raw_hunks)
		*raw_hunks = NULL;

	rewind (f);
	while ((got = getline (&line, &linelen, f)) > 0) {
		unsigned char *is_delta = NULL;
		size_t nlines = 0, ctx_idx;
		struct hunk_lines *h;
		unsigned int dist;

		if (strncmp (line, "@@ ", 3))
			continue;

		nhunks++;
		hunks = xrealloc (hunks, nhunks * sizeof (*hunks));
		h = &hunks[nhunks - 1];
		*h = (typeof (*h)){};

		/* Save the @@ line as the start of the raw hunk content */
		if (raw_hunks) {
			*raw_hunks = xrealloc (*raw_hunks,
					       nhunks * sizeof (**raw_hunks));
			(*raw_hunks)[nhunks - 1].s = xmalloc (got);
			memcpy ((*raw_hunks)[nhunks - 1].s, line, got);
			(*raw_hunks)[nhunks - 1].len = got;
		}

		/* Read body lines until the next @@ or EOF, classifying each
		 * line as context or delta (+/-) and recording whether each
		 * line is a delta for distance computation. */
		while ((got = getline (&line, &linelen, f)) > 0) {
			struct strarray *sa;

			if (!strncmp (line, "@@ ", 3)) {
				fseek (f, -got, SEEK_CUR);
				break;
			}

			/* Append the raw line to the current hunk's buffer */
			if (raw_hunks) {
				struct line_info *r = &(*raw_hunks)[nhunks - 1];

				r->s = xrealloc (r->s, r->len + got);
				memcpy (r->s + r->len, line, got);
				r->len += got;
			}

			is_delta = xrealloc (is_delta, nlines + 1);
			if (line[0] == ' ') {
				is_delta[nlines] = 0;
				sa = &h->ctx;
			} else {
				is_delta[nlines] = 1;
				sa = line[0] == '+' ? &h->add : &h->del;
			}
			strarray_push (sa, line + 1, got - 2);
			nlines++;
		}

		/* Forward pass: compute the distance of each context line from
		 * the nearest preceding delta line. Lines closest to deltas are
		 * most valuable for disambiguation. */
		h->ctx_dist = xmalloc (h->ctx.len * sizeof (*h->ctx_dist));
		dist = UINT_MAX;
		ctx_idx = 0;
		for (size_t k = 0; k < nlines; k++) {
			if (is_delta[k]) {
				dist = 0;
			} else {
				if (dist < UINT_MAX)
					dist++;
				h->ctx_dist[ctx_idx++] = dist;
			}
		}

		/* Backward pass: take the min of forward and backward distances
		 * so each context line reflects its distance from the nearest
		 * delta in either direction. */
		dist = UINT_MAX;
		ctx_idx = h->ctx.len;
		for (size_t k = nlines; k > 0; k--) {
			if (is_delta[k - 1]) {
				dist = 0;
			} else {
				ctx_idx--;
				if (dist < UINT_MAX)
					dist++;
				if (dist < h->ctx_dist[ctx_idx])
					h->ctx_dist[ctx_idx] = dist;
			}
		}

		free (is_delta);
	}

	free (line);
	*out = hunks;
	return nhunks;
}

static void
free_hunk_lines (struct hunk_lines *hunks, size_t nhunks)
{
	for (size_t i = 0; i < nhunks; i++) {
		strarray_free (&hunks[i].add);
		strarray_free (&hunks[i].del);
		strarray_free (&hunks[i].ctx);
		free (hunks[i].ctx_dist);
	}
	free (hunks);
}

/* Score how well the context lines from a rejected hunk match a delta hunk.
 * Context lines closer to +/- changes are weighted more heavily, mirroring how
 * the patch utility prioritizes inner context for fuzzy matching. The score is
 * line_length / distance_from_nearest_change for each matching line. */
static int
context_score (const struct hunk_lines *rej, const struct hunk_lines *delta)
{
	int score = 0;

	for (size_t i = 0; i < rej->ctx.len; i++) {
		for (size_t j = 0; j < delta->ctx.len; j++) {
			if (line_info_eq (&rej->ctx.lines[i],
					  &delta->ctx.lines[j])) {
				unsigned int dist = rej->ctx_dist[i];

				if (dist < delta->ctx_dist[j])
					dist = delta->ctx_dist[j];
				score += rej->ctx.lines[i].len /
					 (dist ? dist : 1);
				break;
			}
		}
	}

	return score;
}

/* Check if a rejected hunk matches a delta hunk at the given positions. Since
 * the delta is the inverse of the rejection, the rejected hunk's "+" lines are
 * compared against the delta's "-" lines and vice versa. On match, *pos_del and
 * *pos_add are advanced past the matched lines. */
static int
rej_matches_delta_at (const struct hunk_lines *rej,
		      const struct hunk_lines *delta,
		      size_t *pos_del, size_t *pos_add)
{
	if (!rej->add.len && !rej->del.len)
		return 0;

	if (rej->add.len) {
		if (*pos_del + rej->add.len > delta->del.len)
			return 0;

		for (size_t i = 0; i < rej->add.len; i++) {
			if (!line_info_eq (&delta->del.lines[*pos_del + i],
					   &rej->add.lines[i]))
				return 0;
		}
	}

	if (rej->del.len) {
		if (*pos_add + rej->del.len > delta->add.len)
			return 0;

		for (size_t i = 0; i < rej->del.len; i++) {
			if (!line_info_eq (&delta->add.lines[*pos_add + i],
					   &rej->del.lines[i]))
				return 0;
		}
	}

	*pos_del += rej->add.len;
	*pos_add += rej->del.len;
	return 1;
}

/* Check if a rejected hunk's change lines appear anywhere in a delta hunk's
 * change lines. Unlike rej_matches_delta_at(), this searches all positions. */
static int
rej_matches_delta (const struct hunk_lines *rej, const struct hunk_lines *delta)
{
	if (rej->add.len > delta->del.len || rej->del.len > delta->add.len)
		return 0;

	for (size_t pd = 0; pd <= delta->del.len - rej->add.len; pd++) {
		for (size_t pa = 0; pa <= delta->add.len - rej->del.len; pa++) {
			size_t tmp_pd = pd, tmp_pa = pa;

			if (rej_matches_delta_at (rej, delta, &tmp_pd, &tmp_pa))
				return 1;
		}
	}

	return 0;
}

/* Filter delta diff hunks that are just the inverse of rejected hunks. When
 * patch2 has a hunk that was rejected on patch1_orig, and patch1 makes the same
 * change, the delta diff will show a bogus difference. This function removes
 * those bogus hunks by comparing each delta hunk's change lines against the
 * rejected hunks' change lines in reverse.
 *
 * Each rejected hunk is used at most once. When a rejected hunk matches
 * multiple delta hunks, the one with the most matching context lines wins.
 * A single delta hunk may span multiple rejected hunks that diff merged.
 *
 * Returns a new FILE* with the filtered output, or NULL if all hunks were
 * filtered out. */
static FILE *
filter_inverted_rejects (FILE *delta, FILE *rej)
{
	struct hunk_lines *rej_hunks, *delta_hunks;
	struct line_info *raw_hunks;
	size_t nrej, ndelta;
	long *rej_assigned;
	FILE *out = NULL;

	if (!(nrej = parse_hunks (rej, &rej_hunks, NULL)) ||
	    !(ndelta = parse_hunks (delta, &delta_hunks, &raw_hunks)))
		error (EXIT_FAILURE, 0,
		       "filter_inverted_rejects: no hunks parsed");

	rej_assigned = xmalloc (nrej * sizeof (*rej_assigned));

	/* For each rejected hunk, find the delta hunk whose change lines match
	 * (inverted) and which has the best context overlap. */
	for (size_t r = 0; r < nrej; r++) {
		int best_score = -1;

		rej_assigned[r] = -1;
		for (size_t d = 0; d < ndelta; d++) {
			int score;

			if (!rej_matches_delta (&rej_hunks[r], &delta_hunks[d]))
				continue;

			score = context_score (&rej_hunks[r], &delta_hunks[d]);
			if (score > best_score) {
				best_score = score;
				rej_assigned[r] = d;
			}
		}
	}

	/* For each delta hunk, check if all of its change lines are fully
	 * covered by the rejected hunks assigned to it (in order). */
	for (size_t d = 0; d < ndelta; d++) {
		size_t pos_del = 0, pos_add = 0;

		for (size_t r = 0; r < nrej; r++) {
			if (rej_assigned[r] == d &&
			    !rej_matches_delta_at (&rej_hunks[r],
						   &delta_hunks[d],
						   &pos_del, &pos_add))
				break;
		}

		/* Emit the delta hunk if it shouldn't be filtered */
		if (pos_del != delta_hunks[d].del.len ||
		    pos_add != delta_hunks[d].add.len) {
			if (!out)
				out = xtmpfile ();
			fwrite (raw_hunks[d].s, raw_hunks[d].len, 1, out);
		}
		free (raw_hunks[d].s);
	}

	free (rej_assigned);
	free (raw_hunks);
	free_hunk_lines (delta_hunks, ndelta);
	free_hunk_lines (rej_hunks, nrej);
	return out;
}

/* Filter out spurious edge lines from context diff hunks. Changes that are
 * exclusively additions or exclusively deletions at the top or bottom edge of a
 * hunk are artifacts of one patch capturing more context lines than the other.
 * Hunks composed entirely of such edges are dropped. Hunks with real changes in
 * the middle have their additions-only or deletions-only edges trimmed and the
 * @@ header line counts adjusted accordingly.
 *
 * Closes the input file. Returns NULL if nothing remains. */
static FILE *
filter_edge_hunks (FILE *in)
{
	struct line_info atat, *lines = NULL;
	char *fbuf, *end, *line;
	size_t nlines = 0, fsz;
	FILE *out = NULL;

	/* Read entire input into a buffer */
	fseek (in, 0, SEEK_END);
	fsz = ftell (in);
	fbuf = xmalloc (fsz);
	rewind (in);
	if (fread (fbuf, 1, fsz, in) != fsz)
		error (EXIT_FAILURE, errno, "fread() fail");
	fclose (in);

	end = fbuf + fsz;
	atat = (typeof (atat)){ fbuf }; /* The first line is the @@ line */
	for (line = fbuf; (line = memchr (line, '\n', end - line));) {
		/* ntop/nbot[0] = deleted, [1] = added edge lines */
		int ntop[2] = {}, nbot[2] = {};
		size_t first_ctx, last_ctx = 0, from = 0, to;

		/* Set the previous line length, advancing `line` past '\n' */
		if (atat.len)
			lines[nlines - 1].len = ++line - lines[nlines - 1].s;
		else
			atat.len = ++line - atat.s;

		/* Accumulate non-@@ lines into the current hunk. At EOF,
		 * line == end so we fall through to process the last hunk. */
		if (line < end && strncmp (line, "@@ ", 3)) {
			lines = xrealloc (lines, (nlines + 1) * sizeof (*lines));
			lines[nlines++].s = line;
			continue;
		}

		first_ctx = to = nlines;

		/* Process accumulated hunk on new @@ or final line (no-op when
		 * nlines == 0 since all loop ranges are empty). Find first and
		 * last context lines. */
		for (size_t i = 0; i < nlines; i++) {
			if (lines[i].s[0] == ' ') {
				if (first_ctx == nlines)
					first_ctx = i;
				last_ctx = i;
			}
		}

		/* Count top edge +/- lines (before first context) */
		for (size_t i = 0; i < first_ctx; i++)
			ntop[lines[i].s[0] == '+']++;

		/* Count bottom edge +/- lines (after last context) */
		for (size_t i = last_ctx + 1; i < nlines; i++)
			nbot[lines[i].s[0] == '+']++;

		/* Trim one-sided edges; reset counts for two-sided edges */
		if (ntop[0] && ntop[1])
			ntop[0] = ntop[1] = 0;
		else if (ntop[0] || ntop[1])
			from = first_ctx;
		if (nbot[0] && nbot[1])
			nbot[0] = nbot[1] = 0;
		else if (nbot[0] || nbot[1])
			to = last_ctx + 1;

		/* Write hunk if remaining lines have changes */
		for (size_t i = from; i < to; i++) {
			if (lines[i].s[0] == ' ')
				continue;

			if (!out)
				out = xtmpfile ();

			if (from || to < nlines) {
				/* Edges were trimmed; regenerate the @@ header
				 * with adjusted line counts. sscanf is fine
				 * instead of read_atatline because the input
				 * comes directly from diff and is always
				 * uniformly formatted. */
				int ostart, ocount, nstart, ncount;
				sscanf (atat.s, "@@ -%d,%d +%d,%d @@",
					&ostart, &ocount, &nstart, &ncount);
				fprintf (out, "@@ -%d,%d +%d,%d @@\n",
					 ostart + ntop[0],
					 ocount - ntop[0] - nbot[0],
					 nstart + ntop[1],
					 ncount - ntop[1] - nbot[1]);
			} else {
				fwrite (atat.s, atat.len, 1, out);
			}
			for (size_t j = from; j < to; j++)
				fwrite (lines[j].s, lines[j].len, 1, out);
			break;
		}

		/* Reset for the next hunk */
		nlines = 0;
		atat = (typeof (atat)){ line };
	}
	free (lines);
	free (fbuf);

	if (out)
		rewind (out);
	return out;
}

/* Run diff and filter out bogus hunks containing unlines.
 *
 * Returns NULL if the resulting diff is empty. */
static FILE *
run_and_clean_diff (const char *options, const char *file1, const char *file2,
		    const char *unline)
{
	pid_t child;
	FILE *diff;

	diff = run_diff (options, file1, file2, &child);
	if (diff) {
		FILE *sp;

		sp = split_patch_hunks (diff, 0, NULL, NULL, unline);
		fclose (diff);
		diff = sp;
	}
	waitpid (child, NULL, 0);

	return diff;
}

/* Write a lines_info struct to a new temp file derived from the given template
 * path (replaces the last 6 chars with XXXXXX for mkstemp).
 *
 * Returns the allocated filename which must be freed by the caller. */
static char *
write_to_tmpfile (const char *tmpl, struct lines_info *info)
{
	char *file = xstrdup (tmpl);
	int fd;

	strcpy (file + strlen (file) - 6, "XXXXXX");
	fd = mkstemp (file);
	if (fd < 0)
		error (EXIT_FAILURE, errno, "mkstemp failed");

	write_file (info, fd);
	close (fd);
	return file;
}

static int
output_delta (FILE *p1, FILE *p2, FILE *out)
{
	const char *tmpdir = getenv ("TMPDIR");
	unsigned int tmplen;
	const char tail1[] = "/interdiff-1.XXXXXX";
	const char tail2[] = "/interdiff-2.XXXXXX";
	char *tmpp1, *tmpp2;
	char *unline = NULL;
	int tmpp1fd, tmpp2fd;
	struct lines_info file = { NULL, 0, 0, NULL, NULL };
	struct lines_info file2 = { NULL, 0, 0, NULL, NULL };
	struct rej_file rej;
	int patch_ret = 0, ctx_ret = 0;
	char *oldname = NULL, *newname = NULL;
	pid_t child;
	FILE *in;
	size_t namelen;
	long pos1 = ftell (p1), pos2 = ftell (p2);
	long pristine1, pristine2;
	long start1, start2;
	char options[100];

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
	create_orig (p2, &file, 0, NULL);
	create_orig (p1, &file2, mode == mode_combine, NULL);
	pos1 = ftell (p1);
	pos2 = ftell (p2);
	fseek (p1, start1, SEEK_SET);
	fseek (p2, start2, SEEK_SET);

	/* Skip fuzzy processing for file creations/deletions. One
	 * side is /dev/null, so there's no content to diff. */
	if (fuzzy &&
	    (!strcmp (oldname + 4, "/dev/null") ||
	     !strcmp (newname + 4, "/dev/null")))
		goto fuzzy_skip;

	if (fuzzy) {
		unsigned long *hunk_offs = NULL, *ctx_hunk_offs = NULL;
		char *patch1_new_file, *ctx_patch1_orig_file;
		struct lines_info patch1_new_info = {};
		FILE *sp, *delta_diff, *ctx_diff;
		FILE *ctx_patch_out = NULL;
		int delta_empty, ctx_empty;

		/* Ensure the same unline is used for both files.
		 * file = patch2_orig, file2 = patch1_orig */
		write_file (&file, tmpp2fd);
		unline = file.unline;
		file2.unline = unline;
		write_file (&file2, tmpp1fd);

		/*
		 * DELTA DIFFING:
		 * 1. Construct patch1_new from patch1 (reverted=1 -> new side)
		 * 2. Split patch2 and apply to tmpp1 (patch1_orig)
		 * 3. delta_diff = diff(patch1_new, tmpp1)
		 * 4. Filter delta hunks that match rejected patch2 hunks
		 *
		 * CONTEXT DIFFING:
		 * 1. Make a fresh copy of patch1_orig (tmpp1 is modified above)
		 * 2. Apply delta_diff in reverse to tmpp2 (patch2_orig) to
		 *    remove delta differences
		 * 3. Relocate hunks using fuzz offsets
		 * 4. ctx_diff = diff(patch1_orig, patch2_orig)
		 */

		/* Create patch1_new (reverted=1 gives the new side of patch1) */
		fseek (p1, start1, SEEK_SET);
		create_orig (p1, &patch1_new_info, 1, NULL);
		patch1_new_info.unline = unline;
		patch1_new_file = write_to_tmpfile (tmpp1, &patch1_new_info);
		free_lines (patch1_new_info.head);

		/* Split patch2 and apply to tmpp1 (patch1_orig) */
		sp = split_patch_hunks (p2, pos2 - start2, tmpp1, &hunk_offs, NULL);
		patch_ret = apply_patch (sp, tmpp1, 0, &rej, NULL);
		fclose (sp);
		free (hunk_offs);

		/* Delta diff: diff(patch1_new, patch1_orig + patch2) */
		delta_diff = run_and_clean_diff (options, patch1_new_file,
						 tmpp1, unline);
		delta_empty = !delta_diff;

		/* Filter bogus delta hunks that are just the inverse of rejected
		 * hunks (both patches make the same change but patch2's was
		 * rejected due to context mismatch) */
		if (patch_ret && rej.fp) {
			/* rej.fp ownership transfers to the list;
			 * fuzzy_free_list() will fclose it. */
			fuzzy_add_file (&fuzzy_delta_rej_files, oldname + 4,
					newname + 4, rej.fp);

			if (!delta_empty) {
				FILE *filtered;

				rewind (rej.fp);
				filtered = filter_inverted_rejects (delta_diff,
								    rej.fp);
				fclose (delta_diff);
				delta_diff = filtered;
				delta_empty = !delta_diff;
			}
		}

		/* Apply delta_diff in reverse to tmpp2 (patch2_orig) to
		 * remove delta differences and isolate context diffs */
		if (!delta_empty) {
			FILE *sp2;

			rewind (delta_diff);
			sp2 = split_patch_hunks (delta_diff, 0, tmpp2,
						 &ctx_hunk_offs, NULL);
			if (sp2) {
				ctx_ret = apply_patch (sp2, tmpp2, 1, NULL,
						       &ctx_patch_out);
				fclose (sp2);
			}

			if (ctx_patch_out && ctx_hunk_offs)
				fuzzy_relocate_hunks (tmpp2, unline,
						      ctx_patch_out,
						      ctx_hunk_offs);
			if (ctx_patch_out)
				fclose (ctx_patch_out);
			free (ctx_hunk_offs);
		}

		/* Fresh copy of patch1_orig for context comparison
		 * since tmpp1 was modified by delta diffing above. */
		ctx_patch1_orig_file = write_to_tmpfile (tmpp1, &file2);

		ctx_diff = run_and_clean_diff (options, ctx_patch1_orig_file,
					       tmpp2, unline);
		if (ctx_diff)
			ctx_diff = filter_edge_hunks (ctx_diff);
		ctx_empty = !ctx_diff;

		unlink (ctx_patch1_orig_file);
		free (ctx_patch1_orig_file);
		unlink (patch1_new_file);
		free (patch1_new_file);

		if (!delta_empty)
			fuzzy_add_file (&fuzzy_delta_files, oldname + 4,
					newname + 4, delta_diff);

		if (!ctx_empty)
			fuzzy_add_file (&fuzzy_ctx_files, oldname + 4,
					newname + 4, ctx_diff);
	} else {
		/* Write it out. */
		merge_lines (&file, &file2);
		write_file (&file, tmpp1fd);
		write_file (&file, tmpp2fd);

		if (apply_patch (p1, tmpp1, mode == mode_combine, NULL, NULL))
			error (EXIT_FAILURE, 0,
			       "Error applying patch1 to reconstructed file");

		if (apply_patch (p2, tmpp2, 0, NULL, NULL))
			error (EXIT_FAILURE, 0,
			       "Error applying patch2 to reconstructed file");
	}

	if (!fuzzy && (in = run_diff (options, tmpp1, tmpp2, &child))) {
		/* ANOTHER temporary file!  This is to catch the case
		 * where we just don't have enough context to generate
		 * a proper interdiff. */
		FILE *tmpdiff = xtmpfile ();
		int exit_err = 0;
		char *line = NULL;
		size_t linelen;
		for (;;) {
			ssize_t got = getline (&line, &linelen, in);
			if (got < 0)
				break;
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
					exit_err = 1;
					break;
				}
				fwrite (line, (size_t) got, 1, tmpdiff);
			}
		}
		free (line);
		fclose (in);
		waitpid (child, NULL, 0);
		if (exit_err)
			goto evasive_action;

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

	/* Restore file positions for the caller's iteration loop */
	fseek (p1, pos1, SEEK_SET);
	fseek (p2, pos2, SEEK_SET);

	if (debug)
		printf ("reconstructed orig1=%s orig2=%s\n", tmpp1, tmpp2);
	else {
		unlink (tmpp1);
		unlink (tmpp2);
		if (fuzzy) {
			fuzzy_cleanup (tmpp1, patch_ret);
			fuzzy_cleanup (tmpp2, ctx_ret);
		}
	}
 fuzzy_skip:
	free (oldname);
	free (newname);
	if (fuzzy) {
		free_lines (file.head);
		free_lines (file2.head);
		free (unline);
	} else {
		clear_lines_info (&file);
		/* In non-fuzzy mode, merge_lines() transfers file2's nodes
		 * into file, so they're already freed above. */
	}
	return 0;

 evasive_action:
	if (debug)
		printf ("reconstructed orig1=%s orig2=%s\n", tmpp1, tmpp2);
	else {
		unlink (tmpp1);
		unlink (tmpp2);
		if (fuzzy) {
			fuzzy_cleanup (tmpp1, patch_ret);
			fuzzy_cleanup (tmpp2, 0);
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
		FILE *p2out;

		if (file_in_list (files_done, at->file) != -1)
			continue;

		/* check if we need to process it and init context */
		if (!check_filename(at->file))
			continue;

		fseek (p2, at->pos, SEEK_SET);

		if (fuzzy) {
			if (!fuzzy_only_in_patch2)
				fuzzy_only_in_patch2 = xtmpfile ();
			p2out = fuzzy_only_in_patch2;
		} else {
			if (human_readable && mode != mode_flip)
				fprintf (out, "only in patch2:\n");
			p2out = out;
		}

		output_patch1_only (p2, p2out, 1);
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
			int found = 0;

			while (!found &&
			       getline (&line, &linelen, patch_file) > 0) {
				if (strncmp (line, "@@ ", 3))
					continue;

				p = strchr (line + 3, '+');
				if (!p)
					continue;

				p = strchr (p, ',');
				if (p) {
					/* Like '@@ -1,3 +1,3 @@' */
					p++;
					skip = strtoul (p, &end, 10);
					if (p == end)
						continue;
				} else
					/* Like '@@ -1 +1 @@' */
					skip = 1;
				found = 1;
			}

			if (!found) {
				free (names[0]);
				free (names[1]);
				break;
			}

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

		free (names[0]);
		free (names[1]);
	}

	if (line)
		free (line);

	/* Return success if the file is empty, has indexed files, or
	 * has no --- lines at all (merge commits, mode-only changes,
	 * notes-only commits, binary-only patches, etc.). */
	if (file_is_empty || *file_list)
		return 0;

	/* Check if the patch has any file-header --- lines (as
	 * opposed to commit-message separators or b4 tracking).
	 * If not, it simply has no diffable content. */
	line = NULL;
	linelen = 0;
	rewind (patch_file);
	while (getline (&line, &linelen, patch_file) != -1)
		if (!strncmp (line, "--- a/", 6) ||
		    !strncmp (line, "--- /dev/null", 13)) {
			free (line);
			return 1;
		}
	free (line);
	return 0;
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
	if (apply_patch (p1, tmpp1, 1, NULL, NULL))
		error (EXIT_FAILURE, 0,
		       "Error reconstructing original file");

	/* Write it out again and apply patch2, so we end up with the
	 * file as it should look after both patches. */
	tmpfd = xmkstemp (tmpp3);
	write_file (&intermediate, tmpfd);
	fsetpos (p2, &at2);
	if (apply_patch (p2, tmpp3, 0, NULL, NULL))
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

		if (getline (&line, &linelen, p1) == -1) {
			free (names[0]);
			break;
		}

		if (strncmp (line, "+++ ", 4)) {
			free (names[0]);
			continue;
		}

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
			FILE *p1out;

			if (fuzzy && mode == mode_inter) {
				if (!fuzzy_only_in_patch1)
					fuzzy_only_in_patch1 = xtmpfile ();
				p1out = fuzzy_only_in_patch1;
			} else {
				p1out = mode == mode_flip ? flip2 : stdout;
			}
			output_patch1_only (p1, p1out, mode != mode_inter);
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

	if (!file_is_empty && !patch_found) {
		/* Don't warn for patches with no file-header --- lines
		 * (merge commits, mode-only, binary-only, etc.). */
		int has_diff_content = 0;

		rewind (p1);
		while (getline (&line, &linelen, p1) != -1)
			if (!strncmp (line, "--- a/", 6) ||
			    !strncmp (line, "--- /dev/null", 13)) {
				has_diff_content = 1;
				break;
			}
		if (has_diff_content)
			no_patch (patch1);
	}

	copy_residue (p2, mode == mode_flip ? flip1 : stdout);

	/* Output the fuzzy sections after all files have been processed */
	if (fuzzy && (fuzzy_delta_files.head || fuzzy_ctx_files.head ||
		      fuzzy_delta_rej_files.head || fuzzy_only_in_patch1 ||
		      fuzzy_only_in_patch2)) {
		int printed = 0;

		if (fuzzy_delta_files.head) {
			fputs (DELTA_DIFF_HEADER, stdout);
			fuzzy_output_list (&fuzzy_delta_files, 0, stdout);
			printed = 1;
		}

		if (fuzzy_delta_rej_files.head) {
			if (printed)
				fputc ('\n', stdout);
			fputs (DELTA_REJ_HEADER, stdout);
			fuzzy_output_list (&fuzzy_delta_rej_files, 1, stdout);
			printed = 1;
		}

		if (fuzzy_ctx_files.head) {
			if (printed)
				fputc ('\n', stdout);
			fputs (CONTEXT_DIFF_HEADER, stdout);
			fuzzy_output_list (&fuzzy_ctx_files, 0, stdout);
			printed = 1;
		}

		if (fuzzy_only_in_patch1) {
			if (printed)
				fputc ('\n', stdout);
			fputs (ONLY_IN_PATCH1_HEADER, stdout);
			colorize_diff (fuzzy_only_in_patch1, stdout);
			fclose (fuzzy_only_in_patch1);
			printed = 1;
		}

		if (fuzzy_only_in_patch2) {
			if (printed)
				fputc ('\n', stdout);
			fputs (ONLY_IN_PATCH2_HEADER, stdout);
			colorize_diff (fuzzy_only_in_patch2, stdout);
			fclose (fuzzy_only_in_patch2);
		}

		fuzzy_free_list (&fuzzy_delta_files);
		fuzzy_free_list (&fuzzy_ctx_files);
		fuzzy_free_list (&fuzzy_delta_rej_files);
	}

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
"  --fuzzy[=N]\n"
"                  (interdiff) Perform a fuzzy comparison, showing the minimal\n"
"                  set of differences including those in context lines.\n"
"                  Optionally set N to the maximum number of context lines\n"
"                  to fuzz (which passes '--fuzz=N' to the patch utility).\n";

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
			{"fuzzy", 2, 0, 1000 + 'f' },
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
			if (optarg) {
				max_fuzz_user = strtoul (optarg, &end, 0);
				if (optarg == end)
					syntax (1);
			}
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
