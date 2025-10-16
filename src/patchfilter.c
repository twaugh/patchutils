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

/* ============================================================================
 * Shared utility functions for filename resolution and path manipulation
 * These functions are used by both lsdiff and filterdiff implementations
 * ============================================================================ */

const char *strip_path_components(const char *filename, int components)
{
    const char *p = filename;
    int i;

    if (!filename || components <= 0)
        return filename;

    for (i = 0; i < components && p; i++) {
        p = strchr(p, '/');
        if (p)
            p++; /* Skip the '/' */
    }

    return p ? p : filename;
}

/* Helper function to count pathname components */
int count_pathname_components(const char *name)
{
    int count = 0;
    const char *p = name;

    if (!name || !*name)
        return 0;

    /* Count directory separators */
    while ((p = strchr(p, '/')) != NULL) {
        count++;
        p++;
    }

    /* Add 1 for the basename */
    return count + 1;
}

/* Choose best filename using the same algorithm as filterdiff's best_name() */
const char *choose_best_name(const char **names, int count)
{
    int best_pn = -1, best_bn = -1, best_n = -1;
    int best_idx = 0;
    int i;

    if (count == 0)
        return NULL;

    /* Skip /dev/null entries and find fewest path components */
    for (i = 0; i < count; i++) {
        if (!names[i] || strcmp(names[i], "/dev/null") == 0)
            continue;

        int pn = count_pathname_components(names[i]);
        if (best_pn == -1 || pn < best_pn) {
            best_pn = pn;
        }
    }

    if (best_pn == -1) /* All names were /dev/null */
        return names[0];

    /* Among names with fewest path components, find shortest basename */
    for (i = 0; i < count; i++) {
        if (!names[i] || strcmp(names[i], "/dev/null") == 0)
            continue;

        if (count_pathname_components(names[i]) != best_pn)
            continue;

        const char *basename = strrchr(names[i], '/');
        basename = basename ? basename + 1 : names[i];
        int bn = strlen(basename);

        if (best_bn == -1 || bn < best_bn) {
            best_bn = bn;
        }
    }

    /* Among remaining candidates, find shortest total name.
     * In case of tie, prefer source name (index 0). */
    for (i = 0; i < count; i++) {
        if (!names[i] || strcmp(names[i], "/dev/null") == 0)
            continue;

        if (count_pathname_components(names[i]) != best_pn)
            continue;

        const char *basename = strrchr(names[i], '/');
        basename = basename ? basename + 1 : names[i];
        if (strlen(basename) != best_bn)
            continue;

        int n = strlen(names[i]);
        if (best_n == -1 || n < best_n || (n == best_n && i == 0)) {
            best_n = n;
            best_idx = i;
        }
    }

    return names[best_idx];
}


/*
 * Helper function to add a filename candidate to the candidate arrays.
 *
 * @param stripped_candidates Array to store stripped filename copies
 * @param candidates Array of candidate pointers
 * @param count Pointer to current candidate count (will be incremented)
 * @param filename Filename to add (may be NULL, in which case nothing is added)
 * @param git_prefix_mode How to handle Git a/ and b/ prefixes
 */
void add_filename_candidate(char **stripped_candidates, const char **candidates,
                           int *count, const char *filename, enum git_prefix_mode git_prefix_mode)
{
    if (!filename) {
        return;
    }

    stripped_candidates[*count] = strip_git_prefix_from_filename(filename, git_prefix_mode);
    candidates[*count] = stripped_candidates[*count];
    (*count)++;
}

char *get_best_filename(const struct patch_headers *headers, enum git_prefix_mode git_prefix_mode,
                       int strip_output_components, const char *add_prefix,
                       const char *add_old_prefix, const char *add_new_prefix)
{
    const char *filename = NULL;
    char *result = NULL;

    /* Use best_name algorithm to choose filename with Git prefix handling */
    switch (headers->type) {
    case PATCH_TYPE_GIT_EXTENDED:
        {
            char *stripped_candidates[4];
            const char *candidates[4];
            int count = 0;
            int i;

            /* Apply Git prefix stripping and choose candidate order based on patch type */

            /* For Git diffs with unified diff headers (hunks), prefer unified diff headers */
            if (headers->new_name || headers->old_name) {
                /* Git diff with hunks - choose based on whether it's new, deleted, or modified */
                if (headers->git_type == GIT_DIFF_NEW_FILE) {
                    /* New file: prefer new names (new_name, git_new_name) */
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name, git_prefix_mode);
                } else {
                    /* Deleted or modified file: prefer old names (git_old_name, old_name) */
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name, git_prefix_mode);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name, git_prefix_mode);
                }
            } else if (headers->rename_from || headers->rename_to) {
                /* Pure rename (no hunks): use git diff line filenames (source first for tie-breaking) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name, git_prefix_mode);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name, git_prefix_mode);
            } else if (headers->copy_from || headers->copy_to) {
                /* Pure copy (no hunks): use git diff line filenames (source first for tie-breaking) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name, git_prefix_mode);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name, git_prefix_mode);
            } else {
                /* Git diff without hunks - prefer git_old_name (traditional behavior) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name, git_prefix_mode);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name, git_prefix_mode);
            }

            filename = choose_best_name(candidates, count);

            /* Create a copy since we'll free the stripped candidates */
            if (filename) {
                result = xstrdup(filename);
            }

            /* Free the stripped candidates */
            for (i = 0; i < count; i++) {
                free(stripped_candidates[i]);
            }
        }
        break;

    case PATCH_TYPE_UNIFIED:
    case PATCH_TYPE_CONTEXT:
        {
            char *stripped_candidates[2];
            const char *candidates[2];
            int count = 0;
            int i;

            /* Apply Git prefix stripping if requested - add source (old) first for tie-breaking */
            add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name, git_prefix_mode);
            add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name, git_prefix_mode);

            filename = choose_best_name(candidates, count);

            /* Create a copy since we'll free the stripped candidates */
            if (filename) {
                result = xstrdup(filename);
            }

            /* Free the stripped candidates */
            for (i = 0; i < count; i++) {
                free(stripped_candidates[i]);
            }
        }
        break;
    }

    if (!result) {
        result = xstrdup("(unknown)");
    }

    /* Apply path prefixes */
    const char *stripped_filename = strip_path_components(result, strip_output_components);

    if (add_prefix) {
        /* Concatenate prefix with filename */
        size_t prefix_len = strlen(add_prefix);
        size_t filename_len = strlen(stripped_filename);
        char *prefixed_filename = xmalloc(prefix_len + filename_len + 1);
        strcpy(prefixed_filename, add_prefix);
        strcat(prefixed_filename, stripped_filename);

        free(result);  /* Free the original result */
        return prefixed_filename;
    }

    /* TODO: Apply --addoldprefix, --addnewprefix options here */

    /* If we used strip_path_components, we need to create a new string */
    if (stripped_filename != result) {
        char *final_result = xstrdup(stripped_filename);
        free(result);
        return final_result;
    }

    return result;
}

char determine_file_status(const struct patch_headers *headers, int empty_files_as_absent)
{
    /* Use the shared utility function for file status determination */
    return patch_determine_file_status(headers, empty_files_as_absent);
}

/*
 * Parse a range specification for the -F/--files, --lines, and --hunks options.
 *
 * Range formats supported:
 *   "3"     - single number 3
 *   "3-5"   - range 3 through 5 (inclusive)
 *   "3-"    - 3 through end
 *   "-"     - all (wildcard)
 *   "1,3-5,8" - comma-separated list of ranges
 */
void parse_range(struct range **r, const char *rstr)
{
    unsigned long n;
    char *end;

    if (*rstr == '-')
        n = -1UL;
    else {
        n = strtoul(rstr, &end, 0);
        if (rstr == end) {
            if (*end)
                error(EXIT_FAILURE, 0,
                      "not understood: '%s'", end);
            else
                error(EXIT_FAILURE, 0,
                      "missing number in range list");

            *r = NULL;
            return;
        }

        rstr = end;
    }

    *r = xmalloc(sizeof **r);
    (*r)->start = (*r)->end = n;
    (*r)->next = NULL;
    if (*rstr == '-') {
        rstr++;
        n = strtoul(rstr, &end, 0);
        if (rstr == end)
            n = -1UL;

        (*r)->end = n;
        rstr = end;

        if ((*r)->start != -1UL && (*r)->start > (*r)->end)
            error(EXIT_FAILURE, 0, "invalid range: %lu-%lu",
                  (*r)->start, (*r)->end);
    }

    if (*rstr == ',')
        parse_range(&(*r)->next, rstr + 1);
    else if (*rstr != '\0')
        error(EXIT_FAILURE, 0, "not understood: '%s'", rstr);
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
