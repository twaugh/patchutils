/*
 * diff.c - diff specific util functions
 * Copyright (C) 2001, 2002, 2003, 2004, 2005, 2009, 2011, 2013 Tim Waugh <twaugh@redhat.com>
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
# include "config.h"
#endif

#include <errno.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif /* HAVE_ERROR_H */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include "diff.h"
#include "util.h"

int num_pathname_components (const char *x)
{
        int num = 0;
        while ((x = strchr (x, '/')) != NULL) {
                while (*x == '/')
                        x++;
                num++;
        }
        return num;
}

/*
 * Find the best name from a list.
 *
 * Of the names with the fewest path name components, select the
 * one with the shortest base name.  Of any remaining candidates,
 * select the one with the shortest name.
 *
 */
char *best_name (int n, char **names)
{
        int *pathname_components;
        int *basename_length;
	int *is_dev_null;
        int best_pn, best_bn, best_n, best = 0; /* shut gcc up */
        int i;

        pathname_components = xmalloc (sizeof (int) * n);
        basename_length = xmalloc (sizeof (int) * n);
        is_dev_null = xmalloc (sizeof (int) * n);

        best_pn = -1;
        for (i = 0; i < n; i++) {
		is_dev_null[i] = !strcmp (names[i], "/dev/null");
		if (is_dev_null[i])
			continue;

                pathname_components[i] = num_pathname_components (names[i]);
                if ((best_pn == -1) ||
                    (pathname_components[i] < best_pn))
                        best_pn = pathname_components[i];
        }

        best_bn = -1;
        for (i = 0; i < n; i++) {
                char *p;

		if (is_dev_null[i])
			continue;

                if (pathname_components[i] != best_pn)
                        continue;

                p = strrchr (names[i], '/');
                if (p)
                        p++;
                else
                        p = names[i];

                basename_length[i] = strlen (p);
                if ((best_bn == -1) ||
                    (basename_length[i] < best_bn))
                        best_bn = basename_length[i];
        }

	best_n = -1;
	for (i = 0; i < n; i++) {
		int len;

		if (is_dev_null[i])
			continue;

                if (basename_length[i] != best_bn)
                        continue;

		len = strlen (names[i]);
		if ((best_n == -1) ||
		    (len < best_n)) {
			best_n = len;
			best = i;
		}
	}

        free (pathname_components);
        free (basename_length);
        free (is_dev_null);
        return names[best];
}

const char *stripped (const char *name, int num_components)
{
	const char *basename;
	int i = 0;

	if (!strcmp (name, "/dev/null"))
		return name;

	basename = strrchr (name, '/');
	if (!basename)
		basename = name;
	else basename++;
	while (i < num_components &&
	       (name = strchr (name, '/')) != NULL) {
		while (*name == '/')
			name++;
		i++;
	}
	return name ? name : basename;
}

unsigned long calculate_num_lines (const char *atatline, char which)
{
        char *p = strchr (atatline, which);
        if (!p)
                return 1;
        while (*p && *p != ',' && *p != ' ') p++;
        if (!*p || *p == ' ')
                return 1;
        return strtoul (p + 1, NULL, 10);
}

unsigned long orig_num_lines (const char *atatline)
{
        return calculate_num_lines (atatline, '-');
}

unsigned long new_num_lines (const char *atatline)
{
        return calculate_num_lines (atatline, '+');
}

/* Parse an @@ line. */
int read_atatline (const char *atatline,
		   unsigned long *orig_offset,
		   unsigned long *orig_count,
		   unsigned long *new_offset,
		   unsigned long *new_count)
{
	char *endptr;
	unsigned long res;
	char *p;

	if (orig_offset) {
		p = strchr (atatline, '-');
		if (!p)
			return 1;
		p++;
		res = strtoul (p, &endptr, 10);
		if (p == endptr)
			return 1;
		*orig_offset = res;
	}

	if (orig_count)
		*orig_count = orig_num_lines (atatline);

	if (new_offset) {
		p = strchr (atatline, '+');
		if (!p)
			return 1;
		p++;
		res = strtoul (p, &endptr, 10);
		if (p == endptr)
			return 1;
		*new_offset = res;
	}

	if (new_count)
		*new_count = new_num_lines (atatline);

	return 0;
}

static void copy_context_hunks (char **line, size_t *linelen, ssize_t *got,
				unsigned long *linenum)
{
	for (;;) {
		unsigned long unchanged = 0;
		unsigned long line_start, line_end, line_count;
		char *n, *end;
		int i;

		for (i = 0; i < 2; i++) {
			int first = 1;
			*got = getline (line, linelen, stdin);
			if (*got == -1)
				return;
			++*linenum;

			if (!i && !strncmp (*line, "***************", 15)) {
				/* Some diffs seem to have this for
				 * every set of changes.  SuSV2 says
				 * not to, but the GNU diff info page
				 * disagrees. */
				i--;
				continue;
			}

			if (strncmp (*line, i ? "--- " : "*** ", 4))
				return;

		do_line_counts:
			n = *line + 4;
			line_start = strtoul (n, &end, 10);
			if (n == end)
				return;

			if (*end == ',') {
				n = end + 1;
				line_end = strtoul (n, &end, 10);
				if (n == end)
					return;

				if (line_start > line_end)
					return;

				line_count = line_end - line_start + 1;
			} else {
				line_end = line_start;
				line_count = line_start ? 1 : 0;
			}

			fwrite (*line, (size_t) *got, 1, stdout);

			if (i && line_count == unchanged)
				break;

			while (line_count--) {
				*got = getline (line, linelen, stdin);
				if (*got == -1)
					return;
				++*linenum;

				if (!i && first) {
					first = 0;
					if (!strncmp (*line, "--- ", 4)) {
						/* From lines were omitted. */
						i++;
						goto do_line_counts;
					}
				}

				fwrite (*line, (size_t) *got, 1, stdout);
				if (**line == ' ')
					unchanged++;
			}
		}
	}
}

static void convert_unified_hunks_to_context (char **line, size_t *linelen,
					      unsigned long *linenum)
{
	unsigned long orig_offset, orig_count = 0, new_offset, new_count = 0;
	char **orig_line = NULL, **new_line = NULL;
	char **orig_what = NULL, **new_what = NULL;
	char **whats = NULL;
	unsigned int n_whats = 0;
	unsigned long orig_linenum, new_linenum;
	const char *no_newline_str = "\\ No newline at end of file\n";
	char *misc = NULL;

	if (feof (stdin))
		goto eof;

	if (getline (line, linelen, stdin) == -1)
		goto eof;
	++*linenum;

	for (;;) {
		char *last_orig = NULL;
		char *last_new = NULL;
		char *what = NULL;
		int newline = 1;
		int can_omit_from = 1, can_omit_to = 1;
		unsigned long i;

		misc = NULL;

		if (read_atatline (*line, &orig_offset, &orig_count,
				   &new_offset, &new_count))
			return;

		misc = strchr (*line + 2, '@');
		misc += 2;
		misc = xstrdup (misc);

		/* Read in the change lines. */
		orig_line = xmalloc (sizeof (char *) * orig_count);
		new_line = xmalloc (sizeof (char *) * new_count);
		orig_what = xmalloc (sizeof (char *) * orig_count);
		new_what = xmalloc (sizeof (char *) * new_count);
		whats = xmalloc (sizeof (char *) * (orig_count + new_count));
		orig_linenum = new_linenum = 0;
		while ((orig_linenum < orig_count) ||
		       (new_linenum < new_count) || newline) {
			int get_out = 0;

			if (getline (line, linelen, stdin) == -1)
				/* Should write out everything to date? */
				break;
			++*linenum;

			if (orig_linenum >= orig_count &&
			    new_linenum >= new_count &&
			    **line != '\\')
				break;

			switch (**line) {
			case ' ':
				if (orig_linenum == orig_count)
					/* We've already seen all the orig
					 * lines we were expecting. */
					error (EXIT_FAILURE, 0,
					       "Garbled input at line %lu",
					       *linenum);

				if (new_linenum == new_count)
					/* We've already seen all the new
					 * lines we were expecting. */
					error (EXIT_FAILURE, 0,
					       "Garbled input at line %lu",
					       *linenum);

				what = NULL;
				orig_what[orig_linenum] = " ";
				new_what[new_linenum] = " ";
				orig_line[orig_linenum] = xstrdup (*line + 1);
				new_line[new_linenum] = xstrdup (*line + 1);
				last_orig = orig_line[orig_linenum++];
				last_new = new_line[new_linenum++];
				break;

			case '-':
				if (orig_linenum == orig_count)
					/* We've already seen all the orig
					 * lines we were expecting. */
					error (EXIT_FAILURE, 0,
					       "Garbled input at line %lu",
					       *linenum);

				if (what) {
					if (*what != '-')
						*what = '!';
				} else {
					what = xmalloc (sizeof (char));
					*what = '-';
					whats[n_whats++] = what;
				}
				orig_what[orig_linenum] = what;
				orig_line[orig_linenum] = xstrdup (*line + 1);
				last_orig = orig_line[orig_linenum++];
				last_new = NULL;
				can_omit_from = 0;
				break;

			case '+':
				if (new_linenum == new_count)
					/* We've already seen all the new
					 * lines we were expecting. */
					error (EXIT_FAILURE, 0,
					       "Garbled input at line %lu",
					       *linenum);

				if (what) {
					if (*what != '+')
						*what = '!';
				} else {
					what = xmalloc (sizeof (char));
					*what = '+';
					whats[n_whats++] = what;
				}
				new_what[new_linenum] = what;
				new_line[new_linenum] = xstrdup (*line + 1);
				last_orig = NULL;
				last_new = new_line[new_linenum++];
				can_omit_to = 0;
				break;

			case '\\':
				if (last_orig)
					last_orig[strlen(last_orig)-1] = '\0';
				if (last_new)
					last_new[strlen(last_new)-1] = '\0';
				last_orig = last_new = NULL;
				newline = 0;
				break;

			default:
				get_out = 1;
			}

			if (get_out)
				break;
		}

		if ((orig_linenum < orig_count) ||
		    (new_linenum < new_count))
			error (EXIT_FAILURE, 0, "Garbled input at line %lu",
			       *linenum);

		printf ("*** %lu", orig_offset);
		if (orig_count)
			printf (",%lu", orig_offset + orig_count - 1);

		printf (" ****%s", misc);
		if (!can_omit_from)
			for (i = 0; i < orig_count; i++) {
				char *l = orig_line[i];
				printf ("%c %s", *orig_what[i], l);
				if (l[strlen (l) - 1] != '\n')
					printf ("\n%s", no_newline_str);
			}

		printf ("--- %lu", new_offset);
		if (new_count)
			printf (",%lu", new_offset + new_count - 1);

		puts (" ----");
		if (!can_omit_to)
			for (i = 0; i < new_count; i++) {
				char *l = new_line[i];
				printf ("%c %s", *new_what[i], l);
				if (l[strlen (l) - 1] != '\n')
					printf ("\n%s", no_newline_str);
			}

	eof:
		for (i = 0; i < orig_count; i++)
			free (orig_line[i]);

		for (i = 0; i < new_count; i++)
			free (new_line[i]);

		for (i = 0; i < n_whats; i++)
			free (whats[i]);

		free (orig_line);
		free (new_line);
		free (orig_what);
		free (new_what);
		free (whats);
		free (misc);
		orig_count = new_count = n_whats = 0;
		orig_line = new_line = NULL;
		orig_what = new_what = NULL;
		whats = NULL;
		misc = NULL;

		if (feof (stdin))
			return;
	}
}

/* Read diff on stdin, write context format version on stdout.
 * Note: stdin may already be in context format. */
static void do_convert_to_context (void)
{
	char *line = NULL;
	size_t linelen = 0;
	unsigned long linenum = 0;
	ssize_t got = getline (&line, &linelen, stdin);

	if (got == -1)
		return;
	linenum++;

	for (;;) {
		int is_context = 0;

		for (;;) {
			if (feof (stdin))
				goto eof;

			if (!strncmp (line, "--- ", 4)) {
				is_context = 0;
				break;
			}

			if (!strncmp (line, "*** ", 4)) {
				is_context = 1;
				break;
			}

			fwrite (line, (size_t) got, 1, stdout);

			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;
		}

		if (is_context) {
			fwrite (line, (size_t) got, 1, stdout);
			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;

			if (strncmp (line, "--- ", 4))
				continue;

			fwrite (line, (size_t) got, 1, stdout);
			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;

			if (strncmp (line, "***************", 15))
				continue;

			fwrite (line, (size_t) got, 1, stdout);
			copy_context_hunks (&line, &linelen, &got, &linenum);
		} else {
			printf ("*** %s", line + 4);
			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;

			if (strncmp (line, "+++ ", 4))
				continue;

			printf ("--- ");
			fwrite (line + 4, (size_t) got - 4, 1, stdout);
			puts ("***************");
			convert_unified_hunks_to_context (&line, &linelen,
							  &linenum);
		}
	}

 eof:
	if (line)
		free (line);

	return;
}

static void copy_unified_hunks (char **line, size_t *linelen, ssize_t *got,
				unsigned long *linenum)
{
	unsigned long orig_count = 0, new_count = 0;
	unsigned long orig_offset, new_offset;

	for (;;) {
		if (feof (stdin))
			return;

		*got = getline (line, linelen, stdin);
		if (*got == -1)
			return;
		++*linenum;

		if (!orig_count && !new_count && **line != '\\') {
			if (strncmp (*line, "@@ ", 3))
				break;

			if (read_atatline (*line, &orig_offset, &orig_count,
					   &new_offset, &new_count))
				error (EXIT_FAILURE, 0,
				       "line %lu not understood: %s", *linenum,
				       *line);

			fwrite (*line, (size_t) *got, 1, stdout);
			continue;
		}

		if (orig_count && **line != '+')
			orig_count--;
		if (new_count && **line != '-')
			new_count--;

		fwrite (*line, (size_t) *got, 1, stdout);
	}
}

static void convert_context_hunks_to_unified (char **line, size_t *linelen,
					      unsigned long *linenum)
{
	int happy = 1;

	while (happy) {
		size_t i;
		char *misc = NULL;
		unsigned long unchanged = 0;
		unsigned long line_start[2], line_end, line_count[2];
		char *n, *end;
		char **lines[2];
		size_t *linelengths[2];
		size_t n_lines[2];
		size_t at[2];
		ssize_t got;

		n_lines[0] = n_lines[1] = 0;
		linelengths[0] = linelengths[1] = NULL;
		lines[0] = lines[1] = NULL;

		for (i = 0; i < 2; i++) {
			int first = 1;
			unsigned long lnum;

			if (feof (stdin))
				goto eof;

			if (getline (line, linelen, stdin) == -1)
				goto eof;
			++*linenum;

			if (!i && !misc &&
			    !strncmp (*line, "***************", 15)) {
				char *m = *line + 15;
				if (strcmp (m, "\n"))
					misc = xstrdup (m);
				i--;
				continue;
			}

			if (strncmp (*line, i ? "--- " : "*** ", 4))
				goto unhappy;

		do_line_counts:
			n = *line + 4;
			line_start[i] = strtoul (n, &end, 10);
			if (n == end)
				goto unhappy;

			if (*end == ',') {
				n = end + 1;
				line_end = strtoul (n, &end, 10);
				if (n == end)
					goto unhappy;

				if (line_start[i] > line_end)
					goto unhappy;

				line_count[i] = line_end - line_start[i] + 1;
			} else {
				line_end = line_start[i];
				line_count[i] = line_start[i] ? 1 : 0;
			}

			n = strstr (n, i ? "----" : "****");
			if (!misc)
				misc = xstrdup (n + 4);

			if (i && line_count[i] == unchanged)
				break;

			n_lines[i] = line_count[i];
			lines[i] = xmalloc (sizeof (char *) * line_count[i]);
			linelengths[i] = xmalloc (sizeof (size_t) *
						  line_count[i]);
			memset (lines[i], 0, sizeof (char *) * line_count[i]);

			for (lnum = 0; lnum < line_count[i]; lnum++) {
				got = getline (line, linelen, stdin);
				if (got == -1)
					goto eof;
				++*linenum;

				if (!i && first) {
					first = 0;
					if (!strncmp (*line, "--- ", 4)) {
						/* From lines were omitted. */
						n_lines[i] = 0;
						i++;
						goto do_line_counts;
					}
				}

				lines[i][lnum] = xmalloc ((size_t) got + 1);
				memcpy (lines[i][lnum], *line, (size_t) got);
				lines[i][lnum][got] = '\0';
				linelengths[i][lnum] = (size_t) got;
				if (**line == ' ')
					unchanged++;
			}
		}

		printf ("@@ -%lu", line_start[0]);
		if (line_count[0] != 1)
			printf (",%lu", line_count[0]);

		printf (" +%lu", line_start[1]);
		if (line_count[1] != 1)
			printf (",%lu", line_count[1]);

		printf (" @@%s", misc);

		/* There MUST be an easier way than this!! */
		at[0] = at[1] = 0;
		while (at[0] < n_lines[0] || at[1] < n_lines[1]) {
			char *l[2] = { NULL, NULL };
			size_t llen[2];
			if (lines[0] && at[0] < n_lines[0]) {
				l[0] = lines[0][at[0]];
				llen[0] = linelengths[0][at[0]];
			}
			if (lines[1] && at[1] < n_lines[1]) {
				l[1] = lines[1][at[1]];
				llen[1] = linelengths[1][at[1]];
			}

			if (l[0] && *l[0] == ' ' && l[1] && *l[1] == ' ') {
				fwrite (l[0] + 1, llen[0] - 1, 1, stdout);
				at[0]++;
				at[1]++;
			} else if (l[0] && *l[0] == ' ' && !l[1]) {
				fwrite (l[0] + 1, llen[0] - 1, 1, stdout);
				at[0]++;
			} else if (l[0] && *l[0] == '-') {
				putchar ('-');
				fwrite (l[0] + 2, llen[0] - 2, 1, stdout);
				at[0]++;
			} else if (l[1] && *l[1] == '+') {
				putchar ('+');
				fwrite (l[1] + 2, llen[1] - 2, 1, stdout);
				at[1]++;
			} else if (l[0] && *l[0] == '!' &&
				   l[1] && *l[1] == '!') {
				while (at[0] < n_lines[0] &&
				       *lines[0][at[0]] == '!') {
					putchar ('-');
					fwrite (lines[0][at[0]] + 2,
						linelengths[0][at[0]] - 2,
						1, stdout);
					at[0]++;
				}
				while (at[1] < n_lines[1] &&
				       *lines[1][at[1]] == '!') {
					putchar ('+');
					fwrite (lines[1][at[1]] + 2,
						linelengths[1][at[1]] - 2,
						1, stdout);
					at[1]++;
				}
			} else if (l[0] && *l[0] == '!') {
				putchar ('-');
				fwrite (l[0] + 2, llen[0] - 2, 1, stdout);
				at[0]++;
			} else if (l[1] && *l[1] == '!') {
				putchar ('+');
				fwrite (l[1] + 2, llen[1] - 2, 1, stdout);
				at[1]++;
			} else if (l[0] && *l[0] == '\\') {
				fwrite (l[0], llen[0], 1, stdout);
				putchar ('\n');
				at[0]++;
			} else if (l[1] && *l[1] == '\\') {
				fwrite (l[1], llen[1], 1, stdout);
				putchar ('\n');
				at[1]++;
			} else if (!l[0]) {
				putchar (*l[1]);
				fwrite (l[1] + 2, llen[1] - 2, 1, stdout);
				at[1]++;
			} else {
				printf ("Don't know how to handle this:\n"
					"1: %s2: %s", l[0], l[1]);
				exit (1);
			}
		}

	eof:
		free (misc);
		for (i = 0; i < n_lines[0]; i++)
			free (lines[0][i]);
		for (i = 0; i < n_lines[1]; i++)
			free (lines[1][i]);
		free (lines[0]);
		free (linelengths[0]);
		free (lines[1]);
		free (linelengths[1]);

		if (feof (stdin))
			return;

		continue;

	unhappy:
		happy = 0;
		goto eof;
	}
}

/* Read diff on stdin, write unified format version on stdout.
 * Note: stdin may already be in unified format. */
static void do_convert_to_unified (void)
{
	char *line = NULL;
	size_t linelen = 0;
	unsigned long linenum = 0;
	ssize_t got = getline (&line, &linelen, stdin);

	if (got == -1)
		return;
	linenum++;

	for (;;) {
		int is_context = 0;

		for (;;) {
			if (feof (stdin))
				goto eof;

			if (!strncmp (line, "--- ", 4)) {
				is_context = 0;
				break;
			}

			if (!strncmp (line, "*** ", 4)) {
				is_context = 1;
				break;
			}

			fwrite (line, (size_t) got, 1, stdout);

			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;
		}

		if (is_context) {
			printf ("--- %s", line + 4);
			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;

			if (strncmp (line, "--- ", 4))
				continue;

			printf ("+++ ");
			fwrite (line + 4, (size_t) got - 4, 1, stdout);
			convert_context_hunks_to_unified (&line, &linelen,
							  &linenum);
		} else {
			fwrite (line, (size_t) got, 1, stdout);
			got = getline (&line, &linelen, stdin);
			if (got == -1)
				goto eof;
			linenum++;

			if (strncmp (line, "+++ ", 4))
				continue;

			fwrite (line, (size_t) got, 1, stdout);
			copy_unified_hunks (&line, &linelen, &got, &linenum);
		}
	}

 eof:
	if (line)
		free (line);

	return;
}

static FILE *do_convert (FILE *f, const char *mode, int seekable,
			 void (*fn) (void))
{
	int fildes[2];
	int fd = fileno (f);
	FILE *ret;

	fflush (NULL);
	if (strchr (mode, 'r')) {
		if (strchr (mode, 'w') || strchr (mode, '+'))
			/* Can't do bidirectional conversions. */
			return NULL;

		/* Read from f (which may be in unified format), and
		 * return a FILE* that gives context format when
		 * read. */

		if (pipe (fildes))
			error (EXIT_FAILURE, errno, "pipe failed");

		switch (fork ()) {
		case -1:
			error (EXIT_FAILURE, errno, "fork failed");

		default:
			/* Parent. */
			close (fildes[1]);
			ret = fdopen (fildes[0], mode);
			if (!ret)
			    error (EXIT_FAILURE, errno, "fdopen failed");

			if (seekable) {
				FILE *tmp = xtmpfile ();	
				while (!feof (ret)) {
					int c = fgetc (ret);

					if (c == EOF)
						break;

					fputc (c, tmp);
				}

				fclose (ret);
				rewind (tmp);
				return tmp;
			}

			return ret;

		case 0:
			/* Child. */
#ifdef PROFILE
			{
				extern void _start (void), etext (void);
				monstartup ((u_long) &_start, (u_long) &etext);
			}
#endif
#ifdef DEBUG
			sleep (10);
#endif /* DEBUG */
			close (fildes[0]);

			if (fd != STDIN_FILENO) {
				dup2 (fd, STDIN_FILENO);
				fclose (f);
			}

			if (fildes[1] != STDOUT_FILENO)
				dup2 (fildes[1], STDOUT_FILENO);

			(*fn) ();
			exit (0);
		}
	}
	else if (strchr (mode, 'w')) {
		if (strchr (mode, 'r') || strchr (mode, '+'))
			/* Can't do bidirectional conversions. */
			return NULL;

		/* Return a FILE* that, when written in unified
		 * format, sends cnotext format to f. */

		if (pipe (fildes))
			error (EXIT_FAILURE, errno, "pipe failed");

		switch (fork ()) {
		case -1:
			error (EXIT_FAILURE, errno, "fork failed");

		default:
			/* Parent. */
			close (fildes[1]);
			return fdopen (fildes[1], mode);

		case 0:
			/* Child. */
#ifdef PROFILE
			{
				extern void _start (void), etext (void);
				monstartup ((u_long) &_start, (u_long) &etext);
			}
#endif
			close (fildes[0]);

			if (fildes[0] != STDIN_FILENO)
				dup2 (fildes[0], STDIN_FILENO);

			if (fd != STDOUT_FILENO) {
				dup2 (fd, STDOUT_FILENO);
				fclose (f);
			}

			(*fn) ();
			exit (0);
		}
	}

	return NULL;
}

FILE *convert_to_context (FILE *f, const char *mode, int seekable)
{
	return do_convert (f, mode, seekable, do_convert_to_context);
}

FILE *convert_to_unified (FILE *f, const char *mode, int seekable)
{
	return do_convert (f, mode, seekable, do_convert_to_unified);
}

static int
read_timezone (const char *tz)
{
	int zone = -1;
	char *endptr;

	tz += strspn (tz, " ");
	zone = strtol (tz, &endptr, 10);
	if (tz == endptr)
		zone = -1;

	return zone;
}

int
read_timestamp (const char *timestamp, struct tm *result, long *zone)
{
	char *end;
	struct tm tm_t;
	long zone_t;

	if (!result)
		result = &tm_t;
	if (!zone)
		zone = &zone_t;

	timestamp += strspn (timestamp, " \t");

	/* First try ISO 8601-style timestamp */
	end = strptime (timestamp, "%Y-%m-%d %H:%M:%S", result);
	if (end) {
		/* Skip nanoseconds. */
		if (*end == '.') {
			end++;
			end += strspn (end, "0123456789");
		}

		*zone = read_timezone (end);
	} else {
		/* If that fails try a traditional format */
		if ((end = strptime (timestamp, "%a %b %e %T %Y", result)))
			*zone = read_timezone (end);
		else if ((end = strptime (timestamp, "%b %Y %H:%M:%S",
					  result)))
			*zone = read_timezone (end);
		else
			return 1;
	}

	return 0;
}

char *
filename_from_header (const char *header)
{
	int first_space = strcspn (header, " \t\n");
	int h = first_space;
	while (header[h] == ' ') {
		int i;
		i = strspn (header + h, " \t");
		if (!header[h + i])
			break;
		if (!read_timestamp (header + h + i, NULL, NULL))
			break;
		h += i + 1;
		h += strcspn (header + h, " \t\n");
	}

	if (header[h] == '\n' && h > first_space)
		/* If we couldn't see a date we recognized, but did see
		   at least one space, split at the first. */
		h = first_space;

	return xstrndup (header, h);
}
