/*
 * patch_common.c - shared functionality for patch processing tools
 * Copyright (C) 2025 Tim Waugh <twaugh@redhat.com>
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif

#include "patch_common.h"

/* Shared global options */
int show_line_numbers = 0;     /* -n, --line-number */
int number_files = 0;          /* -N, --number-files */
int show_patch_names = -1;     /* -H/-h, --with-filename/--no-filename */
int strip_components = 0;      /* -p, --strip-match */
int strip_output_components = 0; /* --strip */
int verbose = 0;               /* -v, --verbose */
int unzip = 0;                 /* -z, --decompress */
enum git_prefix_mode git_prefix_mode = GIT_PREFIX_KEEP; /* --git-prefixes */
enum git_extended_diffs_mode git_extended_diffs_mode = GIT_EXTENDED_DIFFS_INCLUDE; /* --git-extended-diffs */

/* Path prefix options */
char *add_prefix = NULL;         /* --addprefix */
char *add_old_prefix = NULL;     /* --addoldprefix */
char *add_new_prefix = NULL;     /* --addnewprefix */

/* Pattern matching */
struct patlist *pat_include = NULL;  /* -i, --include */
struct patlist *pat_exclude = NULL;  /* -x, --exclude */

/* File counter for -N option */
int file_number = 0;
unsigned long filecount = 0;

/* Global line offset tracking */
unsigned long global_line_offset = 0;

int should_display_file(const char *filename)
{
	/* Apply include/exclude patterns */
	if (pat_exclude && patlist_match(pat_exclude, filename))
		return 0;
	if (pat_include && !patlist_match(pat_include, filename))
		return 0;

	return 1;
}

void display_filename(const char *filename, const char *patchname, unsigned long linenum)
{
	display_filename_extended(filename, patchname, linenum, '\0', 0);
}

int should_display_file_extended(const char *filename, file_filter_callback_t extra_filter)
{
	/* Apply include/exclude patterns */
	if (pat_exclude && patlist_match(pat_exclude, filename))
		return 0;
	if (pat_include && !patlist_match(pat_include, filename))
		return 0;

	/* Apply additional filter if provided */
	if (extra_filter && !extra_filter(filename))
		return 0;

	return 1;
}

void display_filename_extended(const char *filename, const char *patchname, unsigned long linenum,
                              char status, int show_status_flag)
{
	if (show_patch_names > 0)
		printf("%s:", patchname);

	if (show_line_numbers)
		printf("%lu\t", linenum);

	if (number_files)
		printf("File #%-3lu\t", filecount);

	if (show_status_flag && status != '\0')
		printf("%c ", status);

	printf("%s\n", filename);
}

int parse_common_option(int c, char *optarg)
{
	char *end;

	switch (c) {
	case 'n':
		show_line_numbers = 1;
		return 1;
	case 'N':
		number_files = 1;
		return 1;
	case 'H':
		show_patch_names = 1;
		return 1;
	case 'h':
		show_patch_names = 0;
		return 1;
	case 'p':
		strip_components = strtoul(optarg, &end, 0);
		if (optarg == end) {
			error(EXIT_FAILURE, 0, "invalid argument to -p: %s", optarg);
		}
		return 1;
	case 'i':
		patlist_add(&pat_include, optarg);
		return 1;
	case 'x':
		patlist_add(&pat_exclude, optarg);
		return 1;
	case 'I':
		patlist_add_file(&pat_include, optarg);
		return 1;
	case 'X':
		patlist_add_file(&pat_exclude, optarg);
		return 1;
	case 'v':
		verbose++;
		if (show_line_numbers && verbose > 1)
			number_files = 1;
		return 1;
	case 'z':
		unzip = 1;
		return 1;
	case 1000 + 'G':
		if (!strcmp(optarg, "strip")) {
			git_prefix_mode = GIT_PREFIX_STRIP;
		} else if (!strcmp(optarg, "keep")) {
			git_prefix_mode = GIT_PREFIX_KEEP;
		} else {
			error(EXIT_FAILURE, 0, "invalid argument to --git-prefixes: %s (expected 'strip' or 'keep')", optarg);
		}
		return 1;
	case 1000 + 'S':
		strip_output_components = strtoul(optarg, &end, 0);
		if (optarg == end) {
			error(EXIT_FAILURE, 0, "invalid argument to --strip: %s", optarg);
		}
		return 1;
	case 1000 + 'A':
		add_prefix = optarg;
		return 1;
	case 1000 + 'O':
		add_old_prefix = optarg;
		return 1;
	case 1000 + 'N':
		add_new_prefix = optarg;
		return 1;
	case 1000 + 'D':
		if (!strcmp(optarg, "exclude")) {
			git_extended_diffs_mode = GIT_EXTENDED_DIFFS_EXCLUDE;
		} else if (!strcmp(optarg, "include")) {
			git_extended_diffs_mode = GIT_EXTENDED_DIFFS_INCLUDE;
		} else {
			error(EXIT_FAILURE, 0, "invalid argument to --git-extended-diffs: %s (expected 'exclude' or 'include')", optarg);
		}
		return 1;
	}

	return 0; /* Not handled */
}

void init_common_options(void)
{
	/* Initialize global variables to default values */
	show_line_numbers = 0;
	number_files = 0;
	show_patch_names = -1;
	strip_components = 0;
	strip_output_components = 0;
	verbose = 0;
	unzip = 0;
	git_prefix_mode = GIT_PREFIX_KEEP;
	git_extended_diffs_mode = GIT_EXTENDED_DIFFS_INCLUDE;
	add_prefix = NULL;
	add_old_prefix = NULL;
	add_new_prefix = NULL;
	pat_include = NULL;
	pat_exclude = NULL;
	file_number = 0;
	filecount = 0;
	global_line_offset = 0;
}

void cleanup_common_options(void)
{
	/* Free allocated memory */
	if (pat_include) {
		patlist_free(&pat_include);
	}
	if (pat_exclude) {
		patlist_free(&pat_exclude);
	}
}

const char *get_common_short_options(void)
{
	return "nNHhp:i:x:I:X:vz";
}

void add_common_long_options(struct option *options, int *next_index)
{
	int idx = *next_index;
	int start_idx = idx;

	options[idx++] = (struct option){"line-number", 0, 0, 'n'};
	options[idx++] = (struct option){"number-files", 0, 0, 'N'};
	options[idx++] = (struct option){"with-filename", 0, 0, 'H'};
	options[idx++] = (struct option){"no-filename", 0, 0, 'h'};
	options[idx++] = (struct option){"strip-match", 1, 0, 'p'};
	options[idx++] = (struct option){"include", 1, 0, 'i'};
	options[idx++] = (struct option){"exclude", 1, 0, 'x'};
	options[idx++] = (struct option){"include-from-file", 1, 0, 'I'};
	options[idx++] = (struct option){"exclude-from-file", 1, 0, 'X'};
	options[idx++] = (struct option){"verbose", 0, 0, 'v'};
	options[idx++] = (struct option){"decompress", 0, 0, 'z'};
	options[idx++] = (struct option){"git-prefixes", 1, 0, 1000 + 'G'};
	options[idx++] = (struct option){"git-extended-diffs", 1, 0, 1000 + 'D'};
	options[idx++] = (struct option){"strip", 1, 0, 1000 + 'S'};
	options[idx++] = (struct option){"addprefix", 1, 0, 1000 + 'A'};
	options[idx++] = (struct option){"addoldprefix", 1, 0, 1000 + 'O'};
	options[idx++] = (struct option){"addnewprefix", 1, 0, 1000 + 'N'};

	/* Safety check: ensure we haven't exceeded MAX_COMMON_OPTIONS */
	if (idx - start_idx > MAX_COMMON_OPTIONS) {
		error(EXIT_FAILURE, 0, "Internal error: too many common options (%d > %d). "
		      "Increase MAX_COMMON_OPTIONS in patch_common.h",
		      idx - start_idx, MAX_COMMON_OPTIONS);
	}

	*next_index = idx;
}
