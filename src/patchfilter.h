/*
 * patchfilter.h - common definitions for scanner-based patch tools
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

#ifndef PATCHFILTER_H
#define PATCHFILTER_H

#include "patch_scanner.h"
#include "util.h"
#include "diff.h"

/* Tool modes */
enum tool_mode {
    MODE_FILTER,    /* filterdiff, patchview */
    MODE_LIST,      /* lsdiff */
    MODE_GREP       /* grepdiff */
};

/* Common functionality */
enum tool_mode determine_mode(int argc, char *argv[]);

/* Mode-specific entry points */
int run_ls_mode(int argc, char *argv[]);
int run_grep_mode(int argc, char *argv[]);
int run_filter_mode(int argc, char *argv[]);

/* Shared utilities for scanner-based processing
 * Note: Each mode can override these with more specialized implementations */
int filename_matches_patterns(const patch_headers_t *headers,
                             struct patlist *pat_include,
                             struct patlist *pat_exclude,
                             int strip_components);
char patchfilter_determine_file_status(const patch_headers_t *headers);  /* Basic version */
char *patchfilter_get_best_filename(const patch_headers_t *headers);     /* Basic version */

#endif /* PATCHFILTER_H */
