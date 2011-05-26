/*
 * filterdiff - extract (or exclude) a diff from a diff file
 * lsdiff - show which files are modified by a patch
 * grepdiff - show files modified by a patch containing a regexp
 * Copyright (C) 2001, 2002, 2003, 2004, 2008, 2009, 2011 Tim Waugh <twaugh@redhat.com>
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
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h> // for ssize_t
#endif /* HAVE_SYS_TYPES_H */
#include <fnmatch.h>
#include <getopt.h>
#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"
#include "diff.h"

struct range {
	struct range *next;
	unsigned long start;
	unsigned long end;
};

enum line_numbering {
	None = 0,
	Before,
	After
};

enum {
	output_none = 0,
	output_hunk,
	output_file
} output_matching = output_none;

static struct patlist *pat_include = NULL;
static struct patlist *pat_exclude = NULL;
static struct range *hunks = NULL;
static struct range *lines = NULL;
static struct range *files = NULL;
static enum line_numbering number_lines = None;
static int number_files = 0;

static int unzip = 0;
static enum {
	mode_filter,
	mode_list,
	mode_grep,
} mode;
static regex_t *regex = NULL;
static size_t num_regex = 0;
static int clean_comments = 0;
static int numbering = 0;
static int annotating = 0;
static int ignore_components = 0;
static int strip_components = 0;
static const char *prefix_to_add = NULL;
static const char *old_prefix_to_add = NULL;
static const char *new_prefix_to_add = NULL;
static int show_status = 0;
static int verbose = 0;
static int removing_timestamp = 0;
static int egrepping = 0;
static int print_patchnames = -1;
static int empty_files_as_absent = 0;
static unsigned long filecount=0;

static int
regexecs (regex_t *regex, size_t num_regex, const char *string,
	  size_t nmatch, regmatch_t pmatch[], int eflags)
{
	size_t i;
	int ret = REG_NOMATCH;
	for (i = 0; i < num_regex; i++)
		if (!(ret = regexec (&regex[i], string, nmatch,
				     pmatch, eflags)))
			break;
	return ret;
}

static int file_exists (const char *name, const char *timestamp)
{
	struct tm t;
	long zone = -1;

	if (!strcmp (name, "/dev/null"))
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

static int output_header_line (const char *line)
{
	char *fn;
	int h = strcspn (line + 4, "\t\n");

	fwrite (line, 1, 4, stdout);

	if (prefix_to_add)
		fputs (prefix_to_add, stdout);
	else {
		if (old_prefix_to_add && strncmp (line, "---", 3) == 0)
			fputs (old_prefix_to_add, stdout);
		if (new_prefix_to_add && strncmp (line, "+++", 3) == 0)
			fputs (new_prefix_to_add, stdout);
	}

	fn = xstrndup (line + 4, h);
	fputs (stripped (fn, strip_components), stdout);
	if (removing_timestamp)
		putchar ('\n');
	else
		fputs (line + 4 + h, stdout);

	free (fn);
	return 0;
}

static int
file_matches (void)
{
	int f = 0;
	struct range *r;
	
	// See if the file range list includes this file.  -1UL is a
	// wildcard.
	for (r = files; r; r = r->next)
		if ((r->start == -1UL ||
		     r->start <= filecount) &&
		    (r->end == -1UL ||
		     filecount <= r->end)) {
			f = 1;
			break;
		}

	if (files && !f)
		return 0;

	return 1;
}

static void display_filename (unsigned long linenum, char status,
			      const char *filename, const char *patchname)
{
	if (mode == mode_list && !file_matches ())
		/* This is lsdiff --files=... and this file is not to be
		 * listed. */
		return;

	if (print_patchnames)
		printf ("%s:", patchname);
	if (numbering)
		printf ("%lu\t", linenum);
	if (number_files)
		printf ("File #%-3lu\t", filecount);
	if (show_status)
		printf ("%c ", status);
	if (prefix_to_add)
		fputs (prefix_to_add, stdout);
	puts (stripped (filename, strip_components));
}

static int
hunk_matches (unsigned long orig_offset, unsigned long orig_count,
	      unsigned long hunknum)
{
	int h = 0, l = 0;
	struct range *r;

	/* The hunk can't match if the containing file doesn't. */
	if (!file_matches ())
		return 0;

	// For the purposes of matching, zero lines at offset n counts
	// as line n.
	if (!orig_count)
		orig_count = 1;

	// See if the hunk range list includes this hunk.  -1UL is a
	// wildcard.
	for (r = hunks; r; r = r->next)
		if ((r->start == -1UL ||
		     r->start <= hunknum) &&
		    (r->end == -1UL ||
		     hunknum <= r->end)) {
			h = 1;
			break;
		}

	// See if the lines range list includes this hunk.  -1UL is a
	// wildcard.
	for (r = lines; r; r = r->next)
		if ((r->start == -1UL ||
		     r->start < (orig_offset + orig_count)) &&
		    (r->end == -1UL ||
		     r->end >= orig_offset)) {
			l = 1;
			break;
		}

	if (hunks && !h)
		return 0;

	if (lines && !l)
		return 0;

	return 1;
}

static int
do_unified (FILE *f, char *header[2], int match, char **line,
	    size_t *linelen, unsigned long *linenum,
	    unsigned long start_linenum, char status,
	    const char *bestname, const char *patchname,
	    int *orig_file_exists, int *new_file_exists)
{
	/* Skip hunk. */
	unsigned long orig_count = 0, new_count = 0;
	unsigned long orig_offset, new_offset;
	unsigned long hunknum = 0;
	unsigned long track_linenum = 0;
	int header_displayed = 0;
	int hunk_match = match;
	long munge_offset = 0;
	int displayed_filename = 0;
	unsigned long last_hunkmatch = 0;
	unsigned long hunk_linenum = *linenum;
	FILE *match_tmpf = NULL;
	int grepmatch = 0;
	long delayed_munge = 0;
	int ret = 0;
	int orig_is_empty = 1, new_is_empty = 1; /* assume until otherwise */

	if (output_matching == output_file)
		match_tmpf = xtmpfile ();

	for (;;) {
		ssize_t got = getline (line, linelen, f);
		if (got == -1) {
			ret = EOF;
			goto out;
		}
		++*linenum;

		if (!orig_count && !new_count && **line != '\\') {
			char *trailing;

			if (strncmp (*line, "@@ ", 3))
				break;

			/* Next chunk. */
			hunknum++;
			hunk_linenum = *linenum;

			if (output_matching == output_hunk && !grepmatch)
				// We are missing this hunk out, but
				// we need to remember how many lines
				// it would have added or removed.
				munge_offset += delayed_munge;

			if (output_matching != output_file)
				grepmatch = 0;
			if (output_matching == output_hunk) {
				if (match_tmpf)
					fclose (match_tmpf);
				match_tmpf = xtmpfile ();
			}

			if (read_atatline (*line, &orig_offset, &orig_count,
					   &new_offset, &new_count))
				error (EXIT_FAILURE, 0,
				      "line not understood: %s", *line);

			if (orig_count)
				orig_is_empty = 0;
			if (new_count)
				new_is_empty = 0;

			// Decide if this hunk matches.
			if (match)
				hunk_match = hunk_matches (orig_offset,
							   orig_count,
							   hunknum);
			else hunk_match = 0;

			trailing = strchr (*line, '+');
			trailing += strcspn (trailing, " \n");
			if (*trailing == ' ')
				trailing++;
			trailing += strspn (trailing, "@");

			if (hunk_match && numbering && verbose &&
			    mode != mode_grep) {
				if (print_patchnames)
					printf ("%s-", patchname);
				printf ("\t%lu\tHunk #%lu",
					hunk_linenum, hunknum);
				if (verbose > 1) {
					char *p = trailing;
					if (*p != '\n')
						p++;
					printf ("\t%s", p);
				} else
					putchar ('\n');
			}

			if (hunk_match &&
			    (mode == mode_filter ||
			     output_matching != output_none)) {
				int first_hunk = !header_displayed;
				FILE *output_to = stdout;

				if (mode == mode_grep) {
					delayed_munge = orig_count - new_count;
					if (!grepmatch)
						output_to = match_tmpf;
				}

				if (!header_displayed &&
				    mode != mode_grep) {
					// Display the header.
					if (number_lines != After)
						output_header_line (header[0]);
					if (number_lines != Before)
						output_header_line (header[1]);
					header_displayed = 1;
				}
				switch (number_lines) {
				case None:
					// Display the offsets and
					// counts, adjusting for any
					// hunks we've previously
					// missed out.
					fprintf (output_to,
						 "@@ -%lu", orig_offset);
					if (orig_count != 1)
						fprintf (output_to,
							 ",%lu", orig_count);
					fprintf (output_to, " +%lu",
						 new_offset + munge_offset);
					if (new_count != 1)
						fprintf (output_to,
							 ",%lu", new_count);
					fprintf (output_to, " @@");

					if (annotating)
						fprintf (output_to,
							 " Hunk #%lu, %s",
							 hunknum, bestname);

					fputs (trailing, output_to);
					break;
				case Before:
					// Note the initial line number
					track_linenum = orig_offset;
					if (!first_hunk ||
					    (output_matching == output_file &&
					     hunknum > 1))
						fputs ("...\n", output_to);
					break;
				case After:
					// Note the initial line number
					track_linenum = (new_offset +
							 munge_offset);
					if (!first_hunk ||
					    (output_matching == output_file &&
					     hunknum > 1))
						fputs ("...\n", output_to);
					break;
				}
			} else if (mode == mode_filter)
				// We are missing this hunk out, but
				// we need to remember how many lines
				// it would have added or removed.
				munge_offset += orig_count - new_count;

			continue;
		}

		if (**line != '\\') {
			if (orig_count && **line != '+')
				orig_count--;
			if (new_count && **line != '-')
				new_count--;
		}

		if (hunk_match && mode == mode_grep &&
		    !regexecs (regex, num_regex, *line + 1, 0, NULL, 0)) {
			if (output_matching == output_none) {
				if (!displayed_filename) {
					displayed_filename = 1;
					display_filename (start_linenum,
							  status, bestname,
							  patchname);
				}

				if (numbering && verbose &&
				    hunknum > last_hunkmatch) {
					last_hunkmatch = hunknum;
					if (print_patchnames)
						printf ("%s-", patchname);
					printf ("\t%lu\tHunk #%lu\n",
						hunk_linenum, hunknum);
				}
			} else {
				if (match_tmpf) {
					if (!header_displayed &&
					    number_lines != After)
						output_header_line (header[0]);

					if (!header_displayed &&
					    number_lines != Before)
						output_header_line (header[1]);

					if (!header_displayed)
						header_displayed = 1;

					rewind (match_tmpf);
					while (!feof (match_tmpf)) {
						int ch = fgetc (match_tmpf);
						if (ch == EOF)
							break;
						putchar (ch);
					}
					fclose (match_tmpf);
					match_tmpf = NULL;
				}
				grepmatch = 1;
			}
		}

		if (hunk_match &&
		    (mode == mode_filter ||
		     output_matching != output_none)) {
			FILE *output_to = stdout;
			if (mode == mode_grep && !grepmatch)
				output_to = match_tmpf;
			if (number_lines == None)
				// Just display each line.
				fwrite (*line, (size_t) got, 1, output_to);
			else if ((number_lines == Before && **line != '+') ||
				 (number_lines == After && **line != '-'))
				// Numbered line.
				fprintf (output_to, "%lu\t:%s",
					 track_linenum++, 1 + *line);
		}
	}

 out:
	if (match_tmpf)
		fclose (match_tmpf);

	if (empty_files_as_absent) {
		if (orig_file_exists != NULL && orig_is_empty)
			*orig_file_exists = 0;
		if (new_file_exists != NULL && new_is_empty)
			*new_file_exists = 0;
	}

	return ret;
}

static int
do_context (FILE *f, char *header[2], int match, char **line,
	    size_t *linelen, unsigned long *linenum,
	    unsigned long start_linenum, char status,
	    const char *bestname, const char *patchname,
	    int *orig_file_exists, int *new_file_exists)
{
	/* Skip before and after segments. */
	unsigned long line_start, line_end, line_count;
	unsigned long hunknum = 0;
	unsigned long track_linenum = 0;
	unsigned long changed[2];
	long munge_offset = 0;
	int header_displayed = 0;
	char *n, *end;
	int i;
	int hunk_match = 0;
	int displayed_filename = 0;
	unsigned long last_hunkmatch = 0;
	unsigned long hunk_linenum = *linenum;
	FILE *match_tmpf = NULL;
	int grepmatch = 0;
	int ret = 0;
	unsigned long unchanged;
	int first_hunk = 0;
	int orig_is_empty = 1, new_is_empty = 1; /* assume until otherwise */
	size_t got = 0;

	/* Context diff hunks are like this:
	 *
	 * *************** [GNU diff can put stuff here]
	 * *** start[,end] **** [we sometimes put stuff here]
	 *   from lines... (omitted if there are only insertions)
	 *[*************** [GNU diff can put stuff here]]
	 * --- start[,end] ----
	 *   to lines... (omitted if there are only deletions)
	 *[*** start[,end] ****
	 * ...]
	 *
	 * Both from and to lines may end with:
	 * \ No newline at end of file
	 */

	if (getline (line, linelen, f) == -1)
		return EOF;
	++*linenum;

	if (strncmp (*line, "***************", 15))
		return 1;

	if (getline (line, linelen, f) == -1)
		return EOF;
	++*linenum;

	if (output_matching == output_file)
		match_tmpf = xtmpfile ();

 next_hunk:
	unchanged = 0;
	changed[0] = changed[1] = 0; // for munge calculation

	for (i = 0; i < 2; i++) {
		int first = 1;

		if (i == 0)
			first_hunk = !header_displayed;

		if (!i && !strncmp (*line, "***************", 15)) {
			/* Some diffs seem to have this for every
			 * set of changes.  SuSV2 says not to,
			 * but the GNU diff info page disagrees. */
			i--;

			if (getline (line, linelen, f) == -1) {
			    ret = EOF;
			    goto out;
			}

			++*linenum;
			continue;
		}

		if (strncmp (*line, i ? "--- " : "*** ", 4)) {
			ret = 1;
			goto out;
		}

		if (!i) {
			hunknum++;
			hunk_linenum = *linenum;
			if (output_matching != output_file)
				grepmatch = 0;
			if (output_matching == output_hunk) {
				if (match_tmpf)
					fclose (match_tmpf);
				match_tmpf = xtmpfile ();
			}
		}

	do_line_counts:
		n = *line + 4;
		line_start = strtoul (n, &end, 10);
		if (n == end) {
			ret = 1;
			goto out;
		}

		if (*end == ',') {
			n = end + 1;
			line_end = strtoul (n, &end, 10);
			if (n == end) {
				ret = 1;
				goto out;
			}

			if (line_start > line_end) {
				ret = 1;
				goto out;
			}

			line_count = line_end - line_start + 1;
		} else {
			line_end = line_start;
			line_count = line_start ? 1 : 0;
		}

		n = strchr (n, '*');
		if (n)
			n += 4;

		if (!i) {
			if (match)
				hunk_match = hunk_matches (line_start,
							   line_count,
							   hunknum);
			else hunk_match = 0;

			if (hunk_match && numbering && verbose &&
			    mode != mode_grep) {
				if (print_patchnames)
					printf ("%s-", patchname);
				printf ("\t%lu\tHunk #%lu\n",
					hunk_linenum, hunknum);
			}
		}

		if (hunk_match &&
		    (mode == mode_filter || output_matching != output_none)) {
			FILE *output_to= stdout;

			if (mode == mode_grep && !grepmatch)
				output_to = match_tmpf;

			// Display the line counts.
			if (!header_displayed && mode == mode_filter) {
				if (number_lines != After)
					output_header_line (header[0]);
				if (number_lines != Before)
					output_header_line (header[1]);
				header_displayed = 1;
			}

			if (number_lines == None) switch (i) {
			case 0:
				fputs ("***************\n", output_to);
				fprintf (output_to, "*** %lu", line_start);
				if (line_end != line_start)
					fprintf (output_to, ",%lu", line_end);
				fputs (" ****", output_to);

				if (annotating)
					fprintf (output_to,
						 " Hunk #%lu, %s\n",
						 hunknum, bestname);
				else if (n)
					fputs (n, output_to);
				else
					fputc ('\n', output_to);

				break;
			case 1:
				fprintf (output_to, "--- %lu",
					 line_start + munge_offset);
				if (line_end != line_start)
					fprintf (output_to, ",%lu",
						 line_end + munge_offset);
				fputs (" ----\n", output_to);
				break;
			}

			switch (number_lines) {
			case None:
				break;

			case Before:
				if (i != 0)
					break;

				// Note the initial line number.
				track_linenum = line_start;
				if (!first_hunk ||
				    (output_matching == output_file &&
				     hunknum > 1))
					fputs ("...\n", output_to);
				break;

			case After:
				if (i != 1)
					break;

				track_linenum = line_start + munge_offset;
				if (!first_hunk ||
				    (output_matching == output_file &&
				     hunknum > 1))
					fputs ("...\n", output_to);
				break;
			}
		}

		if (i && line_count == unchanged)
			break;

		got = getline (line, linelen, f);
		if (got == -1) {
			ret = EOF;
			goto out;
		}

		++*linenum;

		while ((line_count == 0 && **line == '\\') ||
		       line_count--) {
			if (hunk_match && mode == mode_grep &&
			    !regexecs (regex, num_regex, *line + 2,
				       0, NULL, 0)) {
				if (output_matching == output_none) {
					if (!displayed_filename) {
						displayed_filename = 1;
						display_filename(start_linenum,
								 status,
								 bestname,
								 patchname);
					}

					if (numbering && verbose &&
					    hunknum > last_hunkmatch) {
						last_hunkmatch = hunknum;
						if (print_patchnames)
							printf ("%s-",
								patchname);
						printf ("\t%lu\tHunk #%lu\n",
							hunk_linenum, hunknum);
					}
				} else {
					if (!header_displayed) {
						if (number_lines != After)
							output_header_line (header[0]);
						if (number_lines != Before)
							output_header_line (header[1]);
						header_displayed = 1;
					}

					if (match_tmpf) {
						rewind (match_tmpf);
						while (!feof (match_tmpf)) {
							int ch;
							ch = fgetc(match_tmpf);
							if (ch == EOF)
								break;
							putchar (ch);
						}
						fclose (match_tmpf);
						match_tmpf = NULL;
					}

					grepmatch = 1;
				}
			}

			if (!i && first) {
				first = 0;
				if (!strncmp (*line, "--- ", 4)) {
					/* From lines were
					 * omitted. */
					i++;
					goto do_line_counts;
				}
			}

			if (**line == ' ')
				unchanged++;

			if (empty_files_as_absent) switch (**line) {
			case ' ':
			case '!':
				new_is_empty = orig_is_empty = 0;
				break;
			case '+':
				new_is_empty = 0;
				break;
			case '-':
				orig_is_empty = 0;
				break;
			}

			if (hunk_match &&
			    (mode == mode_filter ||
			     output_matching != output_none)) {
				FILE *output_to = stdout;
				if (mode == mode_grep && !grepmatch)
					output_to = match_tmpf;

				if (number_lines == None)
					fwrite (*line, (size_t) got,
						1, output_to);
				else if ((number_lines == Before && !i) ||
					 (number_lines == After && i)) {
					fprintf (output_to, "%lu\t:",
						 track_linenum++);
					fwrite (2 + *line, (size_t) got - 2,
						1, output_to);
				}
			}

			if ((mode == mode_filter && !hunk_match) ||
			    output_matching == output_hunk)
				switch (**line) {
				case '!':
				case '\\':
					changed[i]++;
					break;
				case '+':
					changed[1]++;
					break;
				case '-':
					changed[0]++;
					break;
				}

			got = getline (line, linelen, f);
			if (got == -1) {
				ret = EOF;
				goto out;
			}

			++*linenum;
		}
	}

	if (output_matching != output_hunk || !grepmatch)
		munge_offset += changed[0] - changed[1];
	goto next_hunk;

out:
	if (match_tmpf)
		fclose (match_tmpf);

	if (empty_files_as_absent) {
		if (orig_file_exists != NULL && orig_is_empty)
			*orig_file_exists = 0;
		if (new_file_exists != NULL && new_is_empty)
			*new_file_exists = 0;
	}

	return ret;
}

static int filterdiff (FILE *f, const char *patchname)
{
	static unsigned long linenum = 1;
	char *names[2];
	char *header[2] = { NULL, NULL };
	char *line = NULL;
	size_t linelen = 0;
	char *p;
	const char *p_stripped;
	int match;
	int i;

	if (getline (&line, &linelen, f) == -1)
		return 0;

	for (;;) {
		char status = '!';
		unsigned long start_linenum;
		int orig_file_exists, new_file_exists;
		int is_context = 0;
		int result;
		int (*do_diff) (FILE *, char *[2], int, char **, size_t *,
				unsigned long *, unsigned long,
				char, const char *, const char *,
				int *, int *);

		orig_file_exists = 0; // shut gcc up

		// Search for start of patch ("--- " for unified diff,
		// "*** " for context).
		for (;;) {
			if (!strncmp (line, "--- ", 4)) {
				is_context = 0;
				break;
			}

			if (!strncmp (line, "*** ", 4)) {
				is_context = 1;
				break;
			}

			/* Show non-diff lines if excluding, or if
			 * in verbose mode, and if --clean isn't specified. */
			if (mode == mode_filter && (pat_exclude || verbose)
				&& !clean_comments)
				fputs (line, stdout);

			if (getline (&line, &linelen, f) == -1)
				goto eof;
			linenum++;
		}

		start_linenum = linenum;
		header[0] = xstrdup (line);
		names[0] = filename_from_header (line + 4);
		if (mode != mode_filter && show_status)
			orig_file_exists = file_exists (names[0], line + 4 +
							strlen (names[0]));

		if (getline (&line, &linelen, f) == -1) {
			/* Show non-diff lines if excluding, or if
			 * in verbose mode, and if --clean isn't specified. */
			if (mode == mode_filter && (pat_exclude || verbose)
				&& !clean_comments)
				fputs (header[0], stdout);
			free (names[0]);
			goto eof;
		}
		linenum++;

		if (strncmp (line, is_context ? "--- " : "+++ ", 4)) {
			/* Show non-diff lines if excluding, or if
			 * in verbose mode, and if --clean isn't specified. */
			if (mode == mode_filter && (pat_exclude || verbose)
				&& !clean_comments)
				fputs (header[0], stdout);
			free (names[0]);
			free (header[0]);
			header[0] = NULL;
			continue;
		}

		filecount++;
		header[1] = xstrdup (line);
		names[1] = filename_from_header (line + 4);

		if (mode != mode_filter && show_status)
			new_file_exists = file_exists (names[1], line + 4 +
						       strlen (names[1]));

		// Decide whether this matches this pattern.
		p = best_name (2, names);
		p_stripped = stripped (p, ignore_components);

		match = !patlist_match(pat_exclude, p_stripped);
		if (match && pat_include != NULL)
			match = patlist_match(pat_include, p_stripped);

		// print if it matches.
		if (match && !show_status && mode == mode_list)
			display_filename (start_linenum, status,
					  p, patchname);

		if (is_context)
			do_diff = do_context;
		else
			do_diff = do_unified;

		result = do_diff (f, header, match, &line,
				  &linelen, &linenum,
				  start_linenum, status, p, patchname,
				  &orig_file_exists, &new_file_exists);

		// print if it matches.
		if (match && show_status && mode == mode_list) {
			if (!orig_file_exists)
				status = '+';
			else if (!new_file_exists)
				status = '-';

			display_filename (start_linenum, status,
					  p, patchname);
		}

		switch (result) {
		case EOF:
			free (names[0]);
			free (names[1]);
			goto eof;
		case 1:
			goto next_diff;
		}

	next_diff:
		for (i = 0; i < 2; i++) {
			free (names[i]);
			free (header[i]);
			header[i] = NULL;
		}
	}

 eof:
	for (i = 0; i < 2; i++)
		if (header[i])
			free (header[i]);

	if (line)
		free (line);

	return 0;
}

const char * syntax_str =
"Options:\n"
"  -x PAT, --exclude=PAT\n"
"            exclude files matching PAT\n"
"  -X FILE, --exclude-from-file=FILE\n"
"            exclude files that match any pattern in FILE\n"
"  -i PAT, --include=PAT\n"
"            include only files matching PAT\n"
"  -I FILE, --include-from-file=FILE\n"
"            include only files that match any pattern in FILE\n"
"  --hunks=H, -# H\n"
"            include only hunks in range H\n"
"  --lines=L include only hunks with (original) lines in range L\n"
"  --files=F include only files in range F\n"
"  --annotate (filterdiff, grepdiff)\n"
"            annotate each hunk with the filename and hunk number (filterdiff, grepdiff)\n"
"  --as-numbered-lines=before|after (filterdiff, grepdiff)\n"
"            display lines as they would look before, or after, the (filterdiff, grepdiff)\n"
"            patch is applied (filterdiff, grepdiff)\n"
"  --format=context|unified (filterdiff, grepdiff)\n"
"            set output format (filterdiff, grepdiff)\n"
"  --output-matching=hunk|file (grepdiff)\n"
"            show matching hunks or file-level diffs (grepdiff)\n"
"  --remove-timestamps (filterdiff, grepdiff)\n"
"            don't show timestamps from output (filterdiff, grepdiff)\n"
"  --clean (filterdiff)\n"
"            remove all comments (non-diff lines) from output (filterdiff)\n"
"  -z, --decompress\n"
"            decompress .gz and .bz2 files\n"
"  -n, --line-number\n"
"            show line numbers (lsdiff, grepdiff)\n"
"  --number-files (lsdiff, grepdiff)\n"
"            show file numbers, for use with filterdiff's --files option (lsdiff, grepdiff)\n"
"  -H, --with-filename (lsdiff, grepdiff)\n"
"            show patch file names (lsdiff, grepdiff)\n"
"  -h, --no-filename (lsdiff, grepdiff)\n"
"            suppress patch file names (lsdiff, grepdiff)\n"
"  -p N, --strip-match=N\n"
"            initial pathname components to ignore\n"
"  --strip=N initial pathname components to strip\n"
"  --addprefix=PREFIX\n"
"            prefix pathnames with PREFIX\n"
"  --addoldprefix=PREFIX\n"
"            prefix pathnames in old files with PREFIX\n"
"  --addnewprefix=PREFIX\n"
"            prefix pathnames in new files with PREFIX\n"
"  -s, --status\n"
"            show file additions and removals (lsdiff)\n"
"  -v, --verbose\n"
"            verbose output -- use more than once for extra verbosity\n"
"  -E, --extended-regexp\n"
"            use extended regexps, like egrep (grepdiff)\n"
"  -E, --empty-files-as-absent (lsdiff)\n"
"            treat empty files as absent (lsdiff)\n"
"  -f FILE, --file=FILE\n"
"            read regular expressions from FILE (grepdiff)\n"
"  --filter  run as 'filterdiff' (grepdiff, lsdiff)\n"
"  --list    run as 'lsdiff' (filterdiff, grepdiff)\n"
"  --grep    run as 'grepdiff' (filterdiff, lsdiff)\n"
;

NORETURN
static void syntax (int err)
{
	char *s = xstrdup (syntax_str);
	const char *usage = "usage: %s [OPTION]... [files ...]\n";
	char *p, *next;
	if (mode == mode_grep)
		usage = "usage: %s [OPTION]... REGEX [files ...]\n";
	fprintf (err ? stderr : stdout, usage, progname);
	for (p = s; p && *p; p = next) {
		char *endp;
		next = strchr (p, '\n');
		if (!next)
			break;
		endp = next;
		*next++ = '\0';
		if (*--endp == ')') {
			char *begp = strrchr (p, '(');
			char *comma;
			if (!begp)
				break;
			*begp++ = '\0';
			*endp = '\0';
			do {
				comma = strchr (begp, ',');
				if (comma)
					*comma = '\0';
				if (!strcmp (progname, begp)) {
					puts (p);
					break;
				}
				if (comma)
					begp = (comma + 1 +
						strspn (comma + 1, " "));
			} while (comma);
		}
		else puts (p);
	}
	exit (err);
}

static void
parse_range (struct range **r, const char *rstr)
{
	unsigned long n;
	char *end;

	if (*rstr == '-')
		n = -1UL;
	else {
		n = strtoul (rstr, &end, 0);
		if (rstr == end) {
			if (*end)
				error (EXIT_FAILURE, 0,
				       "not understood: '%s'", end);
			else
				error (EXIT_FAILURE, 0,
				       "missing number in range list");

			*r = NULL;
			return;
		}

		rstr = end;
	}

	*r = xmalloc (sizeof **r);
	(*r)->start = (*r)->end = n;
	(*r)->next = NULL;
	if (*rstr == '-') {
		rstr++;
		n = strtoul (rstr, &end, 0);
		if (rstr == end)
			n = -1UL;

		(*r)->end = n;
		rstr = end;

		if ((*r)->start != -1UL && (*r)->start > (*r)->end)
			error (EXIT_FAILURE, 0, "invalid range: %lu-%lu",
			       (*r)->start, (*r)->end);
	}

	if (*rstr == ',')
		parse_range (&(*r)->next, rstr + 1);
	else if (*rstr != '\0')
		error (EXIT_FAILURE, 0, "not understood: '%s'", rstr);
}

static void set_list (void)
{
	/* This is lsdiff. */
	set_progname ("lsdiff");
	mode = mode_list;
}

static void set_filter (void)
{
	/* This is filterdiff. */
	set_progname ("filterdiff");
	mode = mode_filter;
}

static void set_grep (void)
{
	/* This is grepdiff. */
	set_progname ("grepdiff");
	mode = mode_grep;
}

static void determine_mode_from_name (const char *argv0)
{
	/* This is filterdiff, unless it is named 'lsdiff' or 'grepdiff'. */
	const char *p = strrchr (argv0, '/');
	if (!p++)
		p = argv0;
	if (strstr (p, "lsdiff"))
		set_list ();
	else if (strstr (p, "grepdiff"))
		set_grep ();
	else
		set_filter ();
}

static FILE *convert_format (FILE *f, char format)
{
	switch (format) {
	default:
		break;
	case 'c':
		f = convert_to_context (f, "rb", 0);
		break;
	case 'u':
		f = convert_to_unified (f, "rb", 0);
		break;
	}

	return f;
}

static int
read_regex_file (const char *file)
{
	FILE *f = fopen (file, "r");
	char *line = NULL;
	size_t linelen = 0;
	ssize_t got;
	int err;

	if (!f)
		error (EXIT_FAILURE, errno, "cannot open %s", file);

	while ((got = getline (&line, &linelen, f)) > 0) {
		if (line[--got] == '\n')
			line[got] = '\0';

		regex = xrealloc (regex, ++num_regex * sizeof (regex[0]));
		err = regcomp (&regex[num_regex - 1], line,
			       REG_NOSUB | egrepping);
		if (err) {
			char errstr[300];
			regerror (err, &regex[num_regex - 1], errstr,
				  sizeof (errstr));
			error (EXIT_FAILURE, 0, errstr);
			exit (1);
		}
	}

	free (line);
	return fclose (f);
}

int main (int argc, char *argv[])
{
	int i;
	FILE *f = stdin;
	char format = '\0';
	int regex_file_specified = 0;

	setlocale (LC_TIME, "C");
	determine_mode_from_name (argv[0]);
	while (1) {
		static struct option long_options[] = {
	       		{"help", 0, 0, 1000 + 'H'},
			{"version", 0, 0, 1000 + 'V'},
			{"verbose", 0, 0, 'v'},
			{"list", 0, 0, 'l'},
			{"filter", 0, 0, 1000 + 'f'},
			{"grep", 0, 0, 'g'},
			{"strip", 1, 0, 1000 + 'S'},
			{"addprefix", 1, 0, 1000 + 'A'},
			{"addoldprefix", 1, 0, 1000 + 'O'},
			{"addnewprefix", 1, 0, 1000 + 'N'},
			{"hunks", 1, 0, '#'},
			{"lines", 1, 0, 1000 + ':'},
			{"files", 1, 0, 1000 + 'w'},
			{"as-numbered-lines", 1, 0, 1000 + 'L'},
			{"annotate", 0, 0, 1000 + 'a'},
			{"format", 1, 0, 1000 + 'F'},
			{"output-matching", 1, 0, 1000 + 'o'},
			{"remove-timestamps", 0, 0, 1000 + 'r'},
			{"with-filename", 0, 0, 'H'},
			{"no-filename", 0, 0, 'h'},
			{"empty-files-as-absent", 0, 0, 'E'},
			{"number-files", 0, 0, 1000 + 'n'},
			{"clean", 0, 0, 1000 + 'c'},
			{"strip-match", 1, 0, 'p'},
			{"include", 1, 0, 'i'},
			{"exclude", 1, 0, 'x'},
			{"include-from-file", 1, 0, 'I'},
			{"exclude-from-file", 1, 0, 'X'},
			{"decompress", 0, 0, 'z'},
			{"line-number", 0, 0, 'n'},
			{"strip-match", 1, 0, 'p'},
			{"status", 0, 0, 's'},
			{"extended-regexp", 0, 0, 'E'},
			{"empty-files-as-removed", 0, 0, 'E'},
			{"file", 1, 0, 'f'},
			{0, 0, 0, 0}
		};
		char *end;
		int c = getopt_long (argc, argv, "vp:i:I:x:X:zns#:Ef:Hh",
				     long_options, NULL);
		if (c == -1)
			break;
		
		switch (c) {
		case 'g':
			set_grep ();
			break;
		case 1000 + 'f':
			set_filter ();
			break;
		case 'l':
			set_list ();
			break;
		case 'E':
			if (mode == mode_grep)
				egrepping = REG_EXTENDED;
			else if (mode == mode_list)
				empty_files_as_absent = 1;
			else syntax (1);
			break;
		case 'f':
			if (mode == mode_grep) {
				regex_file_specified = 1;
				read_regex_file (optarg);
			} else syntax (1);
			break;
		case 1000 + 'V':
			printf("%s - patchutils version %s\n", progname,
			       VERSION);
			exit(0);
		case 1000 + 'H':
			syntax (0);
			break;
		case 1000 + 'S':
			strip_components = strtoul (optarg, &end, 0);
			if (optarg == end)
				syntax (1);
			break;
		case 1000 + 'A':
			prefix_to_add = optarg;
			break;
		case 1000 + 'O':
			old_prefix_to_add = optarg;
			break;
		case 1000 + 'N':
			new_prefix_to_add = optarg;
			break;
		case 'p':
			ignore_components = strtoul (optarg, &end, 0);
			if (optarg == end)
				syntax (1);
			break;
		case 'x':
			patlist_add (&pat_exclude, optarg);
			break;
		case 'X':
			patlist_add_file (&pat_exclude, optarg);
			break;
		case 'i':
			patlist_add (&pat_include, optarg);
			break;
		case 'I':
			patlist_add_file (&pat_include, optarg);
			break;
		case 'z':
			unzip = 1;
			break;
		case 'n':
			numbering = 1;
			break;
		case 1000 + 'n':
			number_files = 1;
			break;
		case 's':
			show_status = 1;
			break;
		case 'v':
			verbose++;
			if (numbering && verbose > 1)
				number_files = 1;
			break;
		case '#':
			if (hunks)
				syntax (1);
			parse_range (&hunks, optarg);
			break;
		case 'H':
			if (mode == mode_list || mode == mode_grep)
				print_patchnames = 1;
			else syntax (1);
			break;
		case 'h':
			if (mode == mode_list || mode == mode_grep)
				print_patchnames = 0;
			else syntax (1);
			break;
		case 1000 + ':':
			if (lines)
				syntax (1);
			parse_range (&lines, optarg);
			break;
		case 1000 + 'w':
			if (files)
				syntax (1);
			parse_range (&files, optarg);
			break;
		case 1000 + 'L':
			if (!strcmp (optarg, "before"))
				number_lines = Before;
			else if (!strcmp (optarg, "after"))
				number_lines = After;
			else syntax (1);
			break;
		case 1000 + 'a':
			if (mode == mode_list)
				syntax (1);
			annotating = 1;
			break;
		case 1000 + 'F':
			if (!strcmp (optarg, "context") && !format)
				format = 'c';
			else if (!strcmp (optarg, "unified") && !format)
				format = 'u';
			else syntax (1);
			break;
		case 1000 + 'o':
			if (!strncmp (optarg, "hunk", 4))
				output_matching = output_hunk;
			else if (!strncmp (optarg, "file", 4))
				output_matching = output_file;
			else syntax (1);
			break;
		case 1000 + 'r':
			removing_timestamp = 1;
			break;
		case 1000 + 'c':
			clean_comments = 1;
			break;
		default:
			syntax(1);
		}
	}

	/* Preserve the old semantics of -p. */
	if (mode != mode_filter && ignore_components && !strip_components &&
	    !pat_include && !pat_exclude) {
		fprintf (stderr,
			 "-p given without -i or -x; guessing that you "
			 "meant --strip instead.\n");
		strip_components = ignore_components;
		ignore_components = 0;
	}

	if (mode != mode_grep && output_matching != output_none)
		error (EXIT_FAILURE, 0, "--output-matching only applies to "
		       "grep mode");

	if (numbering &&
	    !(mode == mode_list ||
	      (mode == mode_grep && output_matching == output_none)))
		error (EXIT_FAILURE, 0, "-n only applies to list mode");

	if (mode != mode_filter &&
	    output_matching == output_none &&
	    number_lines != None)
		error (EXIT_FAILURE, 0, "--as-numbered-lines is "
		       "inappropriate in this context");

	if (mode == mode_filter &&
	    verbose && clean_comments)
		error (EXIT_FAILURE, 0, "can't use --verbose and "
		       "--clean options simultaneously");

	if (mode == mode_grep && !regex_file_specified) {
		int err;

		if (optind == argc)
			syntax (1);

		regex = xrealloc (regex, ++num_regex * sizeof (regex[0]));
		err = regcomp (&regex[num_regex - 1], argv[optind++],
			       REG_NOSUB | egrepping);
		if (err) {
			char errstr[300];
			regerror (err, &regex[num_regex - 1], errstr,
				  sizeof (errstr));
			error (EXIT_FAILURE, 0, errstr);
			exit (1);
		}
	}

	if (number_lines != None ||
	    output_matching != output_none) {
		if (print_patchnames == 1)
			error (EXIT_FAILURE, 0,
			       "-H is inappropriate in this context");
	} else if (print_patchnames == -1) {
		if ((mode == mode_list || mode == mode_grep) &&
		    optind + 1 < argc)
			print_patchnames = 1;
		else
			print_patchnames = 0;
	}

	if (optind == argc) {
		f = convert_format (stdin, format);
		filterdiff (f, "(standard input)");
		fclose (f);
	} else {
		for (i = optind; i < argc; i++) {
			if (unzip) {
				f = xopen_unzip (argv[i], "rb");
			} else {
				f = xopen(argv[i], "rbm");
			}

			f = convert_format (f, format);
			filterdiff (f, argv[i]);
			fclose (f);
		}
	}

	return 0;
}

