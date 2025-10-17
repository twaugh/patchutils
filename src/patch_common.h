/*
 * patch_common.h - shared functionality for patch processing tools
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

#ifndef PATCH_COMMON_H
#define PATCH_COMMON_H

#include "patchfilter.h"

/* Shared global options */
extern int show_line_numbers;     /* -n, --line-number */
extern int number_files;          /* -N, --number-files */
extern int show_patch_names;      /* -H/-h, --with-filename/--no-filename */
extern int strip_components;      /* -p, --strip-match */
extern int strip_output_components; /* --strip */
extern int verbose;               /* -v, --verbose */
extern int unzip;                 /* -z, --decompress */
extern enum git_prefix_mode git_prefix_mode; /* --git-prefixes */

/* Path prefix options */
extern char *add_prefix;         /* --addprefix */
extern char *add_old_prefix;     /* --addoldprefix */
extern char *add_new_prefix;     /* --addnewprefix */

/* Pattern matching */
extern struct patlist *pat_include;  /* -i, --include */
extern struct patlist *pat_exclude;  /* -x, --exclude */

/* File counter for -N option */
extern int file_number;
extern unsigned long filecount;

/* Global line offset tracking */
extern unsigned long global_line_offset;

/* Common functions */
int should_display_file(const char *filename);
void display_filename(const char *filename, const char *patchname, unsigned long linenum);

/* Extended functions with optional parameters */
typedef int (*file_filter_callback_t)(const char *filename);
int should_display_file_extended(const char *filename, file_filter_callback_t extra_filter);
void display_filename_extended(const char *filename, const char *patchname, unsigned long linenum,
                              char status, int show_status_flag);
int parse_common_option(int c, char *optarg);
void init_common_options(void);
void cleanup_common_options(void);

/* Common option parsing helpers */
#define MAX_COMMON_OPTIONS 16
#define MAX_TOOL_OPTIONS 16  /* Generous space for tool-specific options */
#define MAX_TOTAL_OPTIONS (MAX_COMMON_OPTIONS + MAX_TOOL_OPTIONS)

void add_common_long_options(struct option *options, int *next_index);
const char *get_common_short_options(void);

#endif /* PATCH_COMMON_H */
