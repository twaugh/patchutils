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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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

char *filename_from_header (const char *header);
