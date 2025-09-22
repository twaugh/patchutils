/*
 * patchfilter.c - unified scanner-based patch filtering tool
 * Provides: filterdiff, lsdiff, grepdiff, patchview functionality
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
#include <locale.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif

#include "patchfilter.h"

/* Determine tool mode based on program name */
static enum tool_mode determine_mode_from_name(const char *argv0)
{
    const char *p = strrchr(argv0, '/');
    if (!p++)
        p = argv0;

    if (strstr(p, "lsdiff"))
        return MODE_LIST;
    else if (strstr(p, "grepdiff"))
        return MODE_GREP;
    else if (strstr(p, "patchview"))
        return MODE_FILTER;  /* patchview is a filter variant */
    else
        return MODE_FILTER;  /* default to filterdiff mode */
}

/* Parse command line to determine if mode is overridden */
static enum tool_mode determine_mode_from_options(int argc, char *argv[], enum tool_mode default_mode)
{
    int i;
    enum tool_mode mode = default_mode;

    /* Scan arguments for mode options without consuming them */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--filter") == 0) {
            mode = MODE_FILTER;
        } else if (strcmp(argv[i], "--list") == 0) {
            mode = MODE_LIST;
        } else if (strcmp(argv[i], "--grep") == 0) {
            mode = MODE_GREP;
        }
        /* Note: We don't break here because later options override earlier ones */
    }

    return mode;
}

/* Main mode determination function */
enum tool_mode determine_mode(int argc, char *argv[])
{
    enum tool_mode mode;

    /* First, determine mode from program name */
    mode = determine_mode_from_name(argv[0]);

    /* Then allow command-line options to override */
    mode = determine_mode_from_options(argc, argv, mode);

    return mode;
}

/* Shared utilities for scanner-based processing */

int filename_matches_patterns(const patch_headers_t *headers,
                             struct patlist *pat_include,
                             struct patlist *pat_exclude,
                             int strip_components)
{
    const char *filename;
    const char *stripped_filename;
    char *best_name;
    int match;

    /* Get the best filename from headers */
    best_name = patchfilter_get_best_filename(headers);
    if (!best_name) {
        return 0;
    }

    filename = best_name;

    /* Apply path stripping */
    stripped_filename = filename;
    if (strip_components > 0) {
        int components_to_strip = strip_components;
        while (components_to_strip > 0 && *stripped_filename) {
            /* Find next path separator */
            const char *next_sep = strchr(stripped_filename, '/');
            if (!next_sep) {
                break; /* No more separators */
            }
            stripped_filename = next_sep + 1;
            components_to_strip--;
        }
    }

    /* Apply pattern matching */
    match = !patlist_match(pat_exclude, stripped_filename);
    if (match && pat_include != NULL) {
        match = patlist_match(pat_include, stripped_filename);
    }

    free(best_name);
    return match;
}

/* Basic filename matching utility - each mode can override with more specific logic */
char *patchfilter_get_best_filename(const patch_headers_t *headers)
{
    const char *filename = NULL;
    char *result = NULL;

    /* Simple algorithm: prefer new name over old name, handle /dev/null */
    if (headers->new_name && strcmp(headers->new_name, "/dev/null") != 0) {
        filename = headers->new_name;
    } else if (headers->old_name && strcmp(headers->old_name, "/dev/null") != 0) {
        filename = headers->old_name;
    } else if (headers->git_new_name) {
        filename = headers->git_new_name;
    } else if (headers->git_old_name) {
        filename = headers->git_old_name;
    }

    if (filename) {
        result = xstrdup(filename);
    }

    return result;
}

/* Basic file status determination - each mode can override with more specific logic */
char patchfilter_determine_file_status(const patch_headers_t *headers)
{
    /* Use the existing utility function from util.c for basic status determination */
    return patch_determine_file_status(headers, 0);
}

/* Main entry point */
int main(int argc, char *argv[])
{
    enum tool_mode mode;

    setlocale(LC_TIME, "C");

    /* Determine which mode to run in */
    mode = determine_mode(argc, argv);

    /* Dispatch to appropriate mode implementation */
    switch (mode) {
    case MODE_LIST:
        return run_ls_mode(argc, argv);
    case MODE_GREP:
        return run_grep_mode(argc, argv);
    case MODE_FILTER:
        return run_filter_mode(argc, argv);
    default:
        error(EXIT_FAILURE, 0, "Unknown mode");
    }
}
