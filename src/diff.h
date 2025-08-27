/*
 * diff.h - diff specific util functions - header
 * Copyright (C) 2001, 2002, 2003 Tim Waugh <twaugh@redhat.com>
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

#include <time.h>

int num_pathname_components (const char *x);

/*
 * Find the best name from a list.
 *
 * Of the names with the fewest path name components, select the
 * one with the shortest base name.
 *
 */
char *best_name (int n, char **names);

/*
 * Strip a pathname by a specified number of components.
 */
const char *stripped (const char *name, int num_components);

unsigned long calculate_num_lines (const char *atatline, char which);
unsigned long orig_num_lines (const char *atatline);
unsigned long new_num_lines (const char *atatline);
int read_atatline (const char *atatline,
		   unsigned long *orig_offset,
		   unsigned long *orig_count,
		   unsigned long *new_offset,
		   unsigned long *new_count);

/* Conversion between formats. */
FILE *convert_to_context (FILE *destination, const char *mode, int seekable);
FILE *convert_to_unified (FILE *destination, const char *mode, int seekable);

/* Filename/timestamp separation. */
int read_timestamp (const char *timestamp,
		    struct tm *result /* may be NULL */,
		    long *zone        /* may be NULL */);

/* Git diff support */
enum git_diff_type {
	GIT_DIFF_NORMAL = 0,	/* Regular diff with hunks */
	GIT_DIFF_RENAME,	/* Pure rename (similarity index 100%) */
	GIT_DIFF_BINARY,	/* Binary file diff */
	GIT_DIFF_MODE_ONLY,	/* Mode change only */
	GIT_DIFF_NEW_FILE,	/* New file creation */
	GIT_DIFF_DELETED_FILE	/* File deletion */
};

enum git_prefix_mode {
	GIT_PREFIX_KEEP,   /* default for compatibility */
	GIT_PREFIX_STRIP
};

char *filename_from_header (const char *header);
char *filename_from_header_with_git_prefix_mode (const char *header, enum git_prefix_mode prefix_mode);

enum git_diff_type detect_git_diff_type (char **headers, unsigned int num_headers);
int extract_git_filenames (char **headers, unsigned int num_headers,
			   char **old_name, char **new_name, enum git_prefix_mode prefix_mode);
