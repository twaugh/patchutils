/*
 * rediff - fix offset and counts of a hand-edited diff
 * Copyright (C) 2001, 2002, 2004, 2009, 2011 Tim Waugh <twaugh@redhat.com>
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

#include <assert.h>
#include <errno.h>
#ifdef HAVE_ERROR_H
# include <error.h>
#endif /* HAVE_ERROR_H */
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h> // for ssize_t
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h> // for access
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */

#include "diff.h"
#include "util.h"

#ifndef DIFF
#define DIFF "diff"
#endif /* DIFF */

struct file_info
{
	const char *orig_file;
	const char *new_file;
	int info_written;
	int info_pending;
};

struct hunk 
{
	fpos_t filepos;
	struct file_info *info;
	unsigned long line_in_diff;
	unsigned long num_lines;
	unsigned long orig_offset;
	unsigned long orig_count;
	unsigned long new_offset;
	unsigned long new_count;
	struct hunk *next; /* must be ordered by line_in_diff */
	int discard_offset;
};

/* Copy hunk from in to out with no modifications.
 * @@ line has already been read.
 * orig_lines: original line count
 * new_lines: new line count */
static unsigned long copy_hunk (FILE *in, FILE *out,
				unsigned long orig_lines,
				unsigned long new_lines)
{
	int newline = 1;
	fpos_t pos;
	char *line = NULL;
	size_t linelen = 0;
	unsigned long count = 0;

	while (orig_lines || new_lines || newline) {
		fgetpos (in, &pos);
		if (getline (&line, &linelen, in) == -1)
			break;

		if (!orig_lines && !new_lines &&
		    line[0] != '\\')
			break;

		count++;

		fputs (line, out);
		switch (line[0]) {
		case ' ':
			if (new_lines) new_lines--;
		case '-':
			if (orig_lines) orig_lines--;
			break;
		case '+':
			if (new_lines) new_lines--;
			break;
		case '\\':
			newline = 0;
			break;
		}
	}

	if (newline)
		/* Back up a line. */
		fsetpos (in, &pos);

	return count;
}

/* Copy hunk from in to out, adjusting offsets by line_offset. */
static unsigned long adjust_offsets_and_copy (long *offset, FILE *in,
					      FILE *out)
{
	char *line = NULL;
	size_t linelen = 0;
	unsigned long orig_offset, orig_lines, new_offset, new_lines;
	unsigned long count = 0;
	char *trailing;

	if (getline (&line, &linelen, in) == -1)
		goto out;
	count++;

	if (!strncmp (line, "--- ", 4)) {
		/* This is the first hunk of a group.  Copy the
		 * file info. */
		fputs (line, out);
		if (getline (&line, &linelen, in) == -1)
			goto out;
		count++;
		fputs (line, out);
		if (getline (&line, &linelen, in) == -1)
			goto out;
		count++;

		/* The line offsets no longer apply. */
		*offset = 0;
	}

	if (read_atatline (line, &orig_offset, &orig_lines,
			   &new_offset, &new_lines)) {
		line[strlen (line) - 1] = '\0';
		error (EXIT_FAILURE, 0, "Line not understood: %s", line);
	}

	/* Find the part after "@@...@@". */
	trailing = strchr (line, '+');
	trailing += strcspn (trailing, " \n");
	if (*trailing == ' ')
		trailing++;
	trailing += strspn (trailing, "@");

	/* Adjust offsets */
	fprintf (out, "@@ -%lu", orig_offset);
	if (orig_lines != 1)
		fprintf (out, ",%lu", orig_lines);
	fprintf (out, " +%lu", new_offset + *offset);
	if (new_lines != 1)
		fprintf (out, ",%lu", new_lines);
	fprintf (out, " @@%s", trailing);
	free (line);

	/* Copy remaining lines of hunk */
	count += copy_hunk (in, out, orig_lines, new_lines);

out:
	return count;
}

/* Copy n lines from in to out. */
static unsigned long copy_lines (FILE *in, FILE *out, unsigned long n)
{
	char *line = NULL;
	size_t linelen = 0;
	unsigned long count = 0;
	while (n--) {
		if (getline (&line, &linelen, in) == -1)
			break;
		count++;
		fputs (line, out);
	}
	if (line)
		free (line);
	return count;
}

/* Copy trailing non-diff lines in hunk from in to out.
 * done: number of lines of hunk already copied. */
static void copy_trailing (struct hunk *hunk, FILE *in, FILE *out,
			   unsigned long done)
{
	if (hunk->next) {
		/* Copy trailing non-diff text. */
		unsigned long todo;
		if (done < hunk->next->line_in_diff - hunk->line_in_diff) {
			todo = (hunk->next->line_in_diff -
				hunk->line_in_diff - done);
			/* Take one off to account for the difference. */
			if (todo > 1)
				copy_lines (in, out, todo - 1);
		}
	} else {
		/* Copy the rest of the file. */
		while (copy_lines (in, out, 100))
			;
	}
}

/* Copy hunks from in to out.
 * from: starting hunk.
 * upto: hunk to stop before, or NULL.
 * line_offset: offset adjustment to apply.
 * is_first: zero unless this is the first hunk in the file. */
static void copy_to (struct hunk *from, struct hunk *upto,
		     long *line_offset, FILE *in, FILE *out, int is_first)
{
	if (!is_first && from && from->info && !from->info->info_written &&
	    from->info->info_pending) {
		fputs (from->info->orig_file, out);
		fputs (from->info->new_file, out);
		from->info->info_written = 1;
	}

	if (is_first && from) {
		/* Copy leading non-diff text. */
		fseek (in, 0, SEEK_SET);
		copy_lines (in, out, from->line_in_diff - 1);
	}

	for (; from && from != upto; from = from->next) {
		unsigned long count;
		fsetpos (in, &from->filepos);
		count = adjust_offsets_and_copy (line_offset, in, out);
		copy_trailing (from, in, out, count);
	}
}

/* Deal with an added hunk. */
static long added_hunk (const char *meta, long offset, FILE *modify, FILE *t,
			unsigned long morig_count, unsigned long mnew_count)
{
	long this_offset = 0;
	char *line = NULL;
	size_t linelen = 0;
	char *p = strchr (meta, '-'), *q;
	unsigned long orig_offset = 0, new_offset;
	unsigned long orig_count = 0, new_count = 0;
	FILE *newhunk = xtmpfile ();

	if (!newhunk)
		error (EXIT_FAILURE, errno, "Couldn't create temporary file");

	if (p) {
		p++;
		orig_offset = strtoul (p, &q, 10);
	}

	if (!p || p == q) {
		if (p)
			p[strlen (p) - 1] = '\0';
		error (EXIT_FAILURE, 0,
		       "Hunk addition requires original line: %s", meta);
	}

	while (morig_count || mnew_count) {
		if (getline (&line, &linelen, modify) == -1)
			break;

		if (line[0] != '+')
			error (EXIT_FAILURE, 0,
			       "Only whole hunks may be added");

		mnew_count--;

		switch (line[1]) {
		case ' ':
			orig_count++;
			new_count++;
			break;
		case '+':
			new_count++;
			this_offset++;
			break;
		case '-':
			orig_count++;
			this_offset--;
			break;
		default:
			error (EXIT_FAILURE, 0,
			       "Multiple added hunks not supported");
		}

		fputs (line + 1, newhunk);
	}

	new_offset = orig_offset + offset;

	if (!new_count)
		new_offset--;

	fprintf (t, "@@ -%lu", orig_offset);
	if (orig_count != 1)
		fprintf (t, ",%lu", orig_count);
	fprintf (t, " +%lu", new_offset);
	if (new_count != 1)
		fprintf (t, ",%lu", new_count);
	fprintf (t, " @@\n");

	rewind (newhunk);
	while (copy_lines (newhunk, t, 100));
	fclose (newhunk);

	if (line)
		free (line);

	return this_offset;
}

/* Deal with a removed hunk. */
static long removed_hunk (const char *meta, FILE *modify, FILE *t,
			  struct hunk **hunkp, unsigned long morig_count,
			  unsigned long mnew_count, unsigned long *replaced)
{
	struct hunk *hunk = *hunkp;
	long this_offset = 0;
	char *line = NULL;
	size_t linelen = 0;
	unsigned long orig_offset, new_offset;
	unsigned long orig_count, new_count;
	int r;

	*replaced = 0;
	if (read_atatline (meta, &orig_offset, &orig_count,
			   &new_offset, &new_count))
		goto out;

	if (getline (&line, &linelen, modify) == -1)
		goto out;

	if (line[0] == '+' && line[1] == '@') {
		/* Minimally correct modified @@ banners. */
		unsigned long oo, no;
		char *trailing;
		if (read_atatline (line + 1, &oo, NULL, &no, NULL))
			goto out;

		/* Display a file name banner. */
		if (hunk->info && !hunk->info->info_written) {
			fputs (hunk->info->orig_file, t);
			fputs (hunk->info->new_file, t);
			hunk->info->info_written = 1;
		}

		trailing = strchr (line + 1, '+');
		trailing += strcspn (trailing, " \n");
		if (*trailing == ' ')
			trailing++;
		trailing += strspn (trailing, "@");

		if (oo != orig_offset)
			no = new_offset + oo - orig_offset;
		else
			oo = orig_offset + no - new_offset;
		fprintf (t, "@@ -%lu", oo);
		if (orig_count != 1)
			fprintf (t, ",%lu", orig_count);
		fprintf (t, " +%lu", no);
		if (new_count != 1)
			fprintf (t, ",%lu", new_count);
		fprintf (t, " @@%s", trailing);
		goto out;
	}

	if (mnew_count)
		goto only_whole;

	*replaced = --morig_count;

	while (morig_count) {
		while (orig_count || new_count) {
			if (line[0] != '-')
				goto only_whole;

			switch (line[1]) {
			case ' ':
				orig_count--;
				new_count--;
				break;
			case '+':
				new_count--;
				this_offset--;
				break;
			case '-':
				orig_count--;
				this_offset++;
				break;
			default:
				error (EXIT_FAILURE, 0,
				       "Garbled input: %s", line + 1);
			}

			if (!--morig_count)
				break;

			r = getline (&line, &linelen, modify);
			assert (r != -1);
		}

		if (morig_count) {
			if (line[0] != '-')
				goto only_whole;

			if (read_atatline (line + 1, &orig_offset, &orig_count,
					   &new_offset, &new_count))
				goto out;

			hunk = hunk->next;
			hunk->info = (*hunkp)->info;
			if (hunk->info)
				hunk->info->info_pending = 1;
			*hunkp = hunk;

			r = getline (&line, &linelen, modify);
			assert (r != -1);
			morig_count--;
		}
	}

 out:
	if (line)
		free (line);

	return this_offset;

 only_whole:
	error (EXIT_FAILURE, 0,
	       "Only whole hunks may be added");
	exit (EXIT_FAILURE);
}

/* Output a modified hunk to out.
 * hunk: original hunk
 * line_offset: offset modification to apply
 * modify: diff output that applies to this hunk
 * original: original diff */
static long show_modified_hunk (struct hunk **hunkp, long line_offset,
				FILE *modify, FILE *original, FILE *out)
{
	struct hunk *hunk = *hunkp;
	long this_offset = 0;
	unsigned long calc_orig_count;
	unsigned long calc_new_offset, calc_new_count;
	unsigned long orig_offset, orig_count, new_offset, new_count;
	unsigned long morig_offset, morig_count, mnew_offset, mnew_count;
	char *line = NULL;
	size_t linelen = 0;
	FILE *t = xtmpfile ();
	int t_written_to = 0;
	unsigned long i, at = 1;
	unsigned long replaced, unaltered;
	char *trailing;
	int r;

	if (!t)
		error (EXIT_FAILURE, errno, "Couldn't open temporary file");

	rewind (modify);

	fsetpos (original, &hunk->filepos);
	r = getline (&line, &linelen, original);
	assert (r != -1);
	if (hunk->info) {
		r = getline (&line, &linelen, original);
		assert (r != -1);
		r = getline (&line, &linelen, original);
		assert (r != -1);
		at += 2;
	}
	r = read_atatline (line, &orig_offset, &orig_count,
			   &new_offset, &new_count);
	assert (!r);
	calc_orig_count = orig_count;
	calc_new_offset = new_offset;
	calc_new_count = new_count;

	trailing = strchr (line, '+');
	trailing += strcspn (trailing, " \n");
	if (*trailing == ' ')
		trailing++;
	trailing += strspn (trailing, "@");
	trailing = xstrdup (trailing);

	r = getline (&line, &linelen, modify);
	assert (r != -1);
	r = read_atatline (line, &morig_offset, &morig_count,
			   &mnew_offset, &mnew_count);
	assert (!r);
	replaced = morig_count;

	do {
		/* Lines before the modification are unaltered. */
		int trim = 0;
		if (morig_offset < hunk->line_in_diff)
			error (EXIT_FAILURE, errno, "Invalid changes made");

		unaltered = morig_offset - hunk->line_in_diff;
		if (!morig_count)
			unaltered++;
		if (unaltered > at)
			unaltered -= at;
		else unaltered = 0;

		if (!unaltered)
			trim = 1;
#ifdef DEBUG
		fprintf (stderr, "First %lu lines unaltered\n", unaltered);
#endif /* DEBUG */
		for (i = unaltered; i; i--) {
			r = getline (&line, &linelen, original);
			assert (r != -1);
			fputs (line, t);
			at++;
			t_written_to = 1;

			switch (line[0]) {
			case ' ':
				if (new_count) new_count--;
			case '-':
				if (orig_count) orig_count--;
				break;
			case '+':
				if (new_count) new_count--;
				break;
			}
		}

		while (morig_count || mnew_count) {
			if (getline (&line, &linelen, modify) == -1)
				break;

			if (line[0] == '\\' || line[1] == '\\')
				error (EXIT_FAILURE, 0,
				       "Don't know how to handle newline "
				       "issues yet.");

#ifdef DEBUG
			fprintf (stderr, "Modify using: %s", line);
#endif /* DEBUG */
			switch (line[0]) {
			case '-':
				switch (line[1]) {
				case '+':
					this_offset--;
					calc_new_count--;
					trim = 0;
					break;
				case '-':
					this_offset++;
					calc_orig_count--;
					break;
				case ' ':
					calc_new_count--;
					calc_orig_count--;
					break;
				case '@':
					goto hunk_end;
				default:
					error (EXIT_FAILURE, 0,
					       "Not supported: %c%c",
					       line[0], line[1]);
				}

				if (trim) {
					orig_offset++;
					calc_new_offset++;
				}

				if (morig_count)
					morig_count--;

				break;
			case '+':
				switch (line[1]) {
				case '+':
					this_offset++;
					calc_new_count++;
					fputs (line + 1, t);
					t_written_to = 1;
					break;
				case '-':
					this_offset--;
					calc_orig_count++;
					trim = 0;
					fputs (line + 1, t);
					t_written_to = 1;
					break;
				case ' ':
					calc_orig_count++;
					calc_new_count++;
					fputs (line + 1, t);
					t_written_to = 1;
					break;
				case '@':
					goto hunk_end;
				default:
					error (EXIT_FAILURE, 0,
					       "Not supported: %c%c",
					       line[0], line[1]);
				}

				if (trim) {
					orig_offset--;
					calc_new_offset--;
				}

				if (mnew_count)
					mnew_count--;

				break;
			}
		}

		/* Skip replaced lines in original hunk. */
#ifdef DEBUG
		fprintf (stderr, "Skip %lu replaced lines\n", replaced);
#endif /* DEBUG */
		while (replaced) {
			replaced--;
			r = getline (&line, &linelen, original);
			assert (r != -1);
			at++;
			switch (line[0]) {
			case ' ':
				if (new_count) new_count--;
			case '-':
				if (orig_count) orig_count--;
				break;
			case '+':
				if (new_count) new_count--;
				break;
			}
		}

	hunk_end:
		if (line[0] && line[1] == '@') {
			if (line[0] == '+') {
				long loff;
				FILE *write_to;
				write_to = t_written_to ? t : out;
				if (hunk->info && !hunk->info->info_written) {
					fputs (hunk->info->orig_file, out);
					fputs (hunk->info->new_file, out);
					hunk->info->info_written = 1;
				}
				loff = added_hunk (line + 1,
						   this_offset,
						   modify, write_to,
						   morig_count,
						   mnew_count);

				if (!hunk->discard_offset)
					line_offset += loff;
#ifdef DEBUG
				else
					fprintf (stderr,
						 "Discarding offset: %ld\n",
						 loff);
#endif /* DEBUG */
			} else if (line[0] == '-') {
				FILE *write_to;
				write_to = t_written_to ? t : out;
				this_offset += removed_hunk (line + 1,
							     modify, write_to,
							     hunkp, morig_count,
							     mnew_count,
							     &replaced);
				hunk = *hunkp;

#ifdef DEBUG
				fprintf (stderr, "Skip %lu replaced lines\n",
					 replaced);
#endif /* DEBUG */

				/* Prevent an offset banner for this
				 * hunk being generated. */
				calc_orig_count = 0;
				calc_new_count = 0;

				while (replaced) {
					replaced--;
					r = getline (&line, &linelen,
						     original);
					assert (r != -1);
					at++;
					switch (line[0]) {
					case ' ':
						if (new_count) new_count--;
					case '-':
						if (orig_count) orig_count--;
						break;
					case '+':
						if (new_count) new_count--;
						break;
					}
				}
			} else error (EXIT_FAILURE, 0,
				    "diff output not understood");
		}

		if (getline (&line, &linelen, modify) == -1)
			break;

		r = read_atatline (line, &morig_offset, &morig_count,
				   &mnew_offset, &mnew_count);
		assert (!r);
		replaced = morig_count;
	} while (!feof (modify));

#ifdef DEBUG
	fprintf (stderr, "Copy remaining lines of original hunk (%lu,%lu)\n",
		 orig_count, new_count);
#endif /* DEBUG */
	while (orig_count || new_count) {
		if (getline (&line, &linelen, original) == -1)
			break;
		fputs (line, t);
#ifdef DEBUG
		fputs (line, stderr);
#endif /* DEBUG */
		at++;
		switch (line[0]) {
		case ' ':
			if (new_count) new_count--;
		case '-':
			if (orig_count) orig_count--;
			break;
		case '+':
			if (new_count) new_count--;
			break;
		}
		assert (line[0] != '@');
	}

#ifdef DEBUG
	fprintf (stderr, "Result:\n");
#endif /* DEBUG */

	if (hunk->info && !hunk->info->info_written) {
		fputs (hunk->info->orig_file, out);
		fputs (hunk->info->new_file, out);
		hunk->info->info_written = 1;
	}
	rewind (t);

	if (calc_orig_count || calc_new_count) {
		fprintf (out, "@@ -%lu", orig_offset);
		if (calc_orig_count != 1)
			fprintf (out, ",%lu", calc_orig_count);
		fprintf (out, " +%lu", calc_new_offset + line_offset);
		if (calc_new_count != 1)
			fprintf (out, ",%lu", calc_new_count);
		fprintf (out, " @@%s", trailing);
	}

	free (trailing);
	while (getline (&line, &linelen, t) != -1)
		fputs (line, out);

	if (line)
		free (line);
	fclose (modify);
	fclose (t);

#ifdef DEBUG
	fprintf (stderr, "Trailing:\n");
#endif /* DEBUG */
	copy_trailing (hunk, original, out, at - 1);
#ifdef DEBUG
	fprintf (stderr, "(End, offset %ld)\n", this_offset);
#endif /* DEBUG */
	return this_offset;
}

/* Write a corrected version of the edited diff to standard output.
 *
 * This works by comparing the modified lines in the edited diff with
 * the hunks in the original diff. */
static int rediff (const char *original, const char *edited, FILE *out)
{
	pid_t child;
	FILE *o;
	FILE *m;
	FILE *t = NULL;
	char *line = NULL;
	size_t linelen = 0;
	unsigned long linenum = 0;
	struct hunk *hunks = NULL, **p = &hunks, *last = NULL;
	struct hunk *current_hunk = NULL;
	long line_offset = 0;

	/* Let's take a look at what hunks are in the original diff. */
	o = xopen (original, "rbm");
	while (!feof (o)) {
		unsigned long o_count, n_count;
		struct hunk *newhunk;
		fpos_t pos;

		/* Search for start of hunk (or file info). */
		do {
			fgetpos (o, &pos);
			if (getline (&line, &linelen, o) == -1)
				break;
			linenum++;

			if (!strncmp (line, "*** ", 4))
				error (EXIT_FAILURE, errno,
				       "Don't know how to handle context "
				       "format yet.");
		} while (strncmp (line, "@@ ", 3) &&
			 strncmp (line, "--- ", 4));

		if (feof (o))
			break;

		if (last)
			last->num_lines = linenum - last->line_in_diff + 1;

		newhunk = xmalloc (sizeof *newhunk);
		newhunk->filepos = pos;
		newhunk->line_in_diff = linenum;
		newhunk->num_lines = 0;

		if (!strncmp (line, "--- ", 4)) {
			struct file_info *info = xmalloc (sizeof *info);
			info->info_written = info->info_pending = 0;
			info->orig_file = xstrdup (line);
			if (getline (&line, &linelen, o) == -1)
				error (EXIT_FAILURE, errno,
				       "Premature end of file");
			info->new_file = xstrdup (line);
			newhunk->info = info;
			if (getline (&line, &linelen, o) == -1)
				error (EXIT_FAILURE, errno,
				       "Premature end of file");
			linenum += 2;
		} else newhunk->info = NULL;

		read_atatline (line, &newhunk->orig_offset,
			       &newhunk->orig_count,
			       &newhunk->new_offset,
			       &newhunk->new_count);
		o_count = newhunk->orig_count;
		n_count = newhunk->new_count;

		newhunk->next = NULL;
		if (*p)
			(*p)->next = newhunk;
		*p = last = newhunk;
		p = &newhunk->next;

#ifdef DEBUG
		if (newhunk->info)
			fprintf (stderr, "This is the first of a group\n");
		fprintf (stderr, "Original hunk at line %lu: "
			 "-%lu,%lu +%lu,%lu\n", newhunk->line_in_diff,
			 newhunk->orig_offset, newhunk->orig_count,
			 newhunk->new_offset, newhunk->new_count);
#endif /* DEBUG */

		/* Skip to next hunk. */
		while (o_count || n_count) {
			if (getline (&line, &linelen, o) == -1)
				break;

			linenum++;
			switch (line[0]) {
			case ' ':
				if (n_count) n_count--;
			case '-':
				if (o_count) o_count--;
				break;
			case '+':
				if (n_count) n_count--;
				break;
			}
		}
	}

	if (!hunks)
		error (EXIT_FAILURE, 0, "Original patch seems empty");

	last->num_lines = linenum - last->line_in_diff + 1;

	/* Run diff between original and edited. */
	t = xpipe (DIFF, &child, "r", DIFF, "-U0",
		   original, edited, NULL);
	m = xtmpfile ();
	if (m) {
		size_t buffer_size = 10000;
		char *buffer = xmalloc (buffer_size);
		while (!feof (t)) {
			size_t got = fread (buffer, 1, buffer_size, t);
			fwrite (buffer, 1, got, m);
		}
		fclose (t);
		waitpid (child, NULL, 0);
		rewind (m);
		free (buffer);
	} else error (EXIT_FAILURE, errno, "Couldn't create temporary file");

	/* For each hunk in m, identify which hunk in o has been
	 * touched.  Display unmodified hunks before that one
	 * (adjusting offsets), then step through the touched hunk
	 * applying changes as necessary. */
	*line = '\0';
	while (!feof (m)) {
		unsigned long orig_line;
		unsigned long orig_count;
		struct hunk *which;
		fpos_t pos;

		while (strncmp (line, "@@ ", 3)) {
			fgetpos (m, &pos);
			if (getline (&line, &linelen, m) == -1)
				break;
		}
	
		if (feof (m))
			break;

		read_atatline (line, &orig_line, &orig_count, NULL, NULL);
		if (!orig_count)
			orig_line++;

		/* Find out which hunk that is. */
		for (which = hunks; which; which = which->next) {
			int header;

			if (!which->next) {
				/* Last one; this must be it. */
				break;
			}

			header = which->next->info ? 2 : 0;
			if (which->next->line_in_diff + header > orig_line)
				/* Next one is past that point. */
				break;
		}
		assert (which);

		if (which->line_in_diff + which->num_lines <= orig_line)
			which->discard_offset = 1;

#ifdef DEBUG
		fprintf (stderr, "Modified hunk starts on line %lu\n",
			 which->line_in_diff);
		if (which->discard_offset)
			fprintf (stderr, "(But discarding offset)\n");
#endif /* DEBUG */

		/* If this is modifying a new hunk, we need to write
		 * out what we had and all the intervening hunks,
		 * adjusting offsets as we go. */
		if (current_hunk != which) {
			if (current_hunk) {
				line_offset += show_modified_hunk
					(&current_hunk, line_offset,
					 t, o, out);
				current_hunk = current_hunk->next;
			}

			/* Copy hunks, adjusting offsets. */
			copy_to (current_hunk ? current_hunk : hunks,
				 which, &line_offset, o, out,
				 current_hunk == NULL);

			/* This meta hunk is the first pertaining to
			 * the hunk in the original. */
			t = xtmpfile ();
		}

		current_hunk = which;

		/* Append the meta hunk to a temporary file. */
		fputs (line, t);
		while (!feof (m)) {
			if (getline (&line, &linelen, m) == -1)
				break;
			if (!strncmp (line, "@@ ", 3))
				break;
			fputs (line, t);
		}
	}

	/* Now display the remaining hunks, adjusting offsets. */
	if (current_hunk) {
		line_offset += show_modified_hunk (&current_hunk, line_offset,
						   t, o, out);
		current_hunk = current_hunk->next;
		if (current_hunk)
			copy_to (current_hunk, NULL, &line_offset, o, out, 0);
	} else
		copy_to (hunks, NULL, &line_offset, o, out, 1);

	fclose (o);
	fclose (m);
	if (line)
		free (line);

	return 0;
}
static char * syntax_str = "usage: %s ORIGINAL EDITED\n"
                           "       %s EDITED\n";

NORETURN
static void syntax (int err)
{
	fprintf (err ? stderr : stdout, syntax_str, progname, progname);
	exit (err);
}

int main (int argc, char *argv[])
{
	/* name to use in error messages */
	set_progname ("rediff");
	
	while (1) {
		static struct option long_options[] = {
	       		{"help", 0, 0, 'h'},
			{"version", 0, 0, 'v'},
			{0, 0, 0, 0}
		};
		int c = getopt_long (argc, argv, "vh",
				long_options, NULL);
		if (c == -1)
			break;
		
		switch (c) {
		case 'v':
			printf("rediff - patchutils version %s\n", VERSION);
			exit(0);
		case 'h':
			syntax (0);
			break;
		default:
			syntax(1);
		}
				
	}
	
	if (argc - optind < 1)
		syntax (1);

	if (argc - optind == 1) {
		char *p = xmalloc (strlen (argv[0]) +
				   strlen ("recountdiff") + 1);
		char *f;
		char **const new_argv = xmalloc (sizeof (char *) * argc);
		memcpy (new_argv, argv, sizeof (char *) * argc);
		new_argv[0] = p;
		strcpy (p, argv[0]);
		f = strrchr (p, '/');
		if (!f)
			f = p;
		else f++;
		strcpy (f, "recountdiff");
		execvp (new_argv[0], new_argv);
		p = xstrdup (new_argv[0]);
		f = strstr (p, "src/");
		if (f) {
			while (*(f + 4)) {
				*f = *(f + 4);
				f++;
			}
			*f = '\0';
			new_argv[0] = p;
			execv (new_argv[0], new_argv);
		}
		error (EXIT_FAILURE, 0, "couldn't execute recountdiff");
	}

	if (access (argv[optind + 1], R_OK))
		error (EXIT_FAILURE, errno, "can't read edited file");

	return rediff (argv[optind], argv[optind + 1], stdout);
}
