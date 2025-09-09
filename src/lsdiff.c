/*
 * lsdiff - list files modified by a patch
 * Copyright (C) 2024 Tim Waugh <twaugh@redhat.com>
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
 * This is a scanner-based implementation of lsdiff using the unified patch scanner API.
 *
 * TODO: CRITICAL COMPATIBILITY ISSUES (30 test failures)
 * ======================================================
 * URGENT FIXES NEEDED (causing test failures):
 *   1. Line number tracking (-n): Option parsed but linenum always 0
 *   2. Filename selection: Scanner prefers new_name, tests expect old_name logic
 *   3. Empty files as absent (-E): Option parsed but logic not implemented
 *   4. Git status detection: Files without hunks not handled properly
 *
 * ADVANCED MISSING FEATURES (for full filterdiff.c parity):
 *   --strip=N             Strip N leading path components (different from -p)
 *   --git-prefixes=MODE   Handle a/ and b/ prefixes (strip|keep)
 *   --addprefix=PREFIX    Add prefix to all pathnames
 *   --addoldprefix=PREFIX Add prefix to old file pathnames
 *   --addnewprefix=PREFIX Add prefix to new file pathnames
 *
 * RANGE PARSING IMPROVEMENTS:
 *   Full range syntax: "1,3-5,8", "3-", "-", "x1,3" (exclusion)
 *   Currently only supports single numbers
 *
 * See filterdiff.c for reference implementations of missing features.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <locale.h>
#include <fnmatch.h>
#include <errno.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif

#include "patch_scanner.h"
#include "util.h"

/* Range structure (for option parsing) */
struct range {
    struct range *next;
    unsigned long start;
    unsigned long end;
};

/* Global options */
static int show_status = 0;           /* -s, --status */
static int show_line_numbers = 0;     /* -n, --line-number */
static int number_files = 0;          /* -N, --number-files */
static int show_patch_names = -1;     /* -H/-h, --with-filename/--no-filename */
static int empty_files_as_absent = 0; /* -E, --empty-files-as-absent */
static int strip_components = 0;      /* -p, --strip-match */
static int strip_output_components = 0; /* --strip */
static int verbose = 0;               /* -v, --verbose */
static int unzip = 0;                 /* -z, --decompress */
static enum git_prefix_mode git_prefix_mode = GIT_PREFIX_KEEP; /* --git-prefixes */

/* TODO: Missing options from original lsdiff:
 * --addprefix=PREFIX  - add prefix to pathnames
 * --addoldprefix=PREFIX - add prefix to old file pathnames
 * --addnewprefix=PREFIX - add prefix to new file pathnames
 */

/* Pattern matching */
static struct patlist *pat_include = NULL;  /* -i, --include */
static struct patlist *pat_exclude = NULL;  /* -x, --exclude */
static struct range *files = NULL;          /* -F, --files */

/* File counter for -N option */
static int file_number = 0;

/* Forward declarations */
static void syntax(int err) __attribute__((noreturn));
static void process_patch_file(FILE *fp, const char *filename);
static void display_filename(const char *filename, const char *patchname, char status, unsigned long linenum);
static char determine_file_status(const struct patch_headers *headers);
static const char *get_best_filename(const struct patch_headers *headers);
static char *strip_git_prefix_from_filename(const char *filename);
static const char *strip_path_components(const char *filename, int components);
static int should_display_file(const char *filename);
static void parse_range(struct range **r, const char *rstr);

static void syntax(int err)
{
    FILE *f = err ? stderr : stdout;

    /* TODO: Update help text to include missing options when implemented */

    fprintf(f, "Usage: %s [OPTION]... [FILE]...\n", "lsdiff");
    fprintf(f, "List files modified by patches.\n\n");
    fprintf(f, "Options:\n");
    fprintf(f, "  -s, --status                 show file additions (+), removals (-), and modifications\n");
    fprintf(f, "  -n, --line-number            show line numbers\n");
    fprintf(f, "  -N, --number-files           show file numbers (for use with filterdiff --files)\n");
    fprintf(f, "  -H, --with-filename          show patch file names\n");
    fprintf(f, "  -h, --no-filename            suppress patch file names\n");
    fprintf(f, "  -E, --empty-files-as-absent  treat empty files as absent\n");
    fprintf(f, "  -p N, --strip-match=N        strip N leading path components\n");
    fprintf(f, "  --strip=N                    strip N leading path components from output\n");
    fprintf(f, "  --git-prefixes=strip|keep    handle a/ and b/ prefixes in Git diffs (default: keep)\n");
    fprintf(f, "  -i PAT, --include=PAT        include only files matching PAT\n");
    fprintf(f, "  -x PAT, --exclude=PAT        exclude files matching PAT\n");
    fprintf(f, "  -I FILE, --include-from-file=FILE  include only files matching patterns in FILE\n");
    fprintf(f, "  -X FILE, --exclude-from-file=FILE  exclude files matching patterns in FILE\n");
    fprintf(f, "  -F RANGE, --files=RANGE      include only files in range RANGE\n");
    fprintf(f, "  -v, --verbose                verbose output\n");
    fprintf(f, "  -z, --decompress             decompress .gz and .bz2 files\n");
    fprintf(f, "      --help                   display this help and exit\n");
    fprintf(f, "      --version                output version information and exit\n");
    fprintf(f, "\nReport bugs to <twaugh@redhat.com>.\n");

    exit(err);
}

static const char *strip_path_components(const char *filename, int components)
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
static int count_pathname_components(const char *name)
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
static const char *choose_best_name(const char **names, int count)
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

    /* Among remaining candidates, find shortest total name */
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
        if (best_n == -1 || n < best_n) {
            best_n = n;
            best_idx = i;
        }
    }

    return names[best_idx];
}

/* Helper function to strip Git a/ or b/ prefixes from a filename */
static char *strip_git_prefix_from_filename(const char *filename)
{
    if (git_prefix_mode == GIT_PREFIX_STRIP && filename &&
        ((filename[0] == 'a' && filename[1] == '/') ||
         (filename[0] == 'b' && filename[1] == '/'))) {
        return xstrdup(filename + 2);
    }
    return filename ? xstrdup(filename) : NULL;
}

static const char *get_best_filename(const struct patch_headers *headers)
{
    const char *filename = NULL;

    /* Use best_name algorithm to choose filename with Git prefix handling */
    switch (headers->type) {
    case PATCH_TYPE_GIT_EXTENDED:
        {
            char *stripped_candidates[4];
            const char *candidates[4];
            int count = 0;
            int i;

            /* Apply Git prefix stripping if requested */
            /* Prefer git_old_name (a/) over git_new_name (b/) for Git diffs without hunks */
            if (headers->git_old_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->git_old_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }
            if (headers->git_new_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->git_new_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }
            if (headers->new_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->new_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }
            if (headers->old_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->old_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }

            filename = choose_best_name(candidates, count);

            /* Create a persistent copy since we'll free the stripped candidates */
            static char *cached_filename = NULL;
            if (cached_filename) free(cached_filename);
            cached_filename = xstrdup(filename);
            filename = cached_filename;

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

            /* Apply Git prefix stripping if requested */
            if (headers->new_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->new_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }
            if (headers->old_name) {
                stripped_candidates[count] = strip_git_prefix_from_filename(headers->old_name);
                candidates[count] = stripped_candidates[count];
                count++;
            }

            filename = choose_best_name(candidates, count);

            /* Create a persistent copy since we'll free the stripped candidates */
            static char *cached_filename2 = NULL;
            if (cached_filename2) free(cached_filename2);
            cached_filename2 = xstrdup(filename);
            filename = cached_filename2;

            /* Free the stripped candidates */
            for (i = 0; i < count; i++) {
                free(stripped_candidates[i]);
            }
        }
        break;
    }

    if (!filename)
        filename = "(unknown)";

    /* TODO: Apply --addprefix, --addoldprefix, --addnewprefix options here */

    return strip_path_components(filename, strip_output_components);
}

static char determine_file_status(const struct patch_headers *headers)
{
    /* Use the shared utility function for file status determination */
    return patch_determine_file_status(headers, empty_files_as_absent);
}

static int should_display_file(const char *filename)
{
    /* TODO: Apply pattern matching to the filename AFTER prefix handling and stripping */

    /* Apply include/exclude patterns */
    if (pat_exclude && patlist_match(pat_exclude, filename))
        return 0;
    if (pat_include && !patlist_match(pat_include, filename))
        return 0;

    /* Apply file range filter */
    if (files) {
        struct range *r;
        int file_matches = 0;

        /* TODO: Handle files_exclude flag and range exclusion (x prefix) */

        for (r = files; r; r = r->next) {
            if (file_number >= r->start && file_number <= r->end) {
                file_matches = 1;
                break;
            }
        }

        if (!file_matches)
            return 0;
    }

    return 1;
}

static void display_filename(const char *filename, const char *patchname, char status, unsigned long linenum)
{
    if (show_patch_names > 0)
        printf("%s:", patchname);

    if (number_files)
        printf("%d\t", file_number);

    if (show_line_numbers)
        printf("%lu\t", linenum);

    if (show_status)
        printf("%c ", status);

    printf("%s\n", filename);
}

static void process_patch_file(FILE *fp, const char *filename)
{
    patch_scanner_t *scanner;
    const patch_content_t *content;
    enum patch_scanner_result result;
    unsigned long header_line = 1;
    const char *current_file = NULL;
    int hunk_number = 0;

    scanner = patch_scanner_create(fp);
    if (!scanner) {
        error(EXIT_FAILURE, 0, "Failed to create patch scanner");
        return;
    }

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        if (content->type == PATCH_CONTENT_HEADERS) {
            const char *best_filename = get_best_filename(content->data.headers);
            char status = determine_file_status(content->data.headers);

            /* Use the line number where the headers started */
            header_line = content->data.headers->start_line;

            file_number++;
            hunk_number = 0;  /* Reset hunk counter for new file */

            if (should_display_file(best_filename)) {
                display_filename(best_filename, filename, status, header_line);
                current_file = best_filename;  /* Track current file for verbose output */
            } else {
                current_file = NULL;  /* Don't show hunks for filtered files */
            }
        } else if (content->type == PATCH_CONTENT_HUNK_HEADER && verbose && current_file) {
            /* In verbose mode, show hunk information */
            hunk_number++;

            if (show_line_numbers) {
                printf("\t%lu\tHunk #%d\n", content->line_number, hunk_number);
            } else {
                printf("\tHunk #%d\n", hunk_number);
            }
        }
    }

    if (result == PATCH_SCAN_ERROR) {
        if (verbose)
            fprintf(stderr, "Warning: Error parsing patch in %s\n", filename);
    }

    patch_scanner_destroy(scanner);
}

int main(int argc, char *argv[])
{
    int i;
    FILE *fp;

    setlocale(LC_TIME, "C");

    while (1) {
        static struct option long_options[] = {
            {"help", 0, 0, 1000 + 'H'},
            {"version", 0, 0, 1000 + 'V'},
            {"status", 0, 0, 's'},
            {"line-number", 0, 0, 'n'},
            {"number-files", 0, 0, 'N'},
            {"with-filename", 0, 0, 'H'},
            {"no-filename", 0, 0, 'h'},
            {"empty-files-as-absent", 0, 0, 'E'},
            {"strip-match", 1, 0, 'p'},
            {"include", 1, 0, 'i'},
            {"exclude", 1, 0, 'x'},
            {"include-from-file", 1, 0, 'I'},
            {"exclude-from-file", 1, 0, 'X'},
            {"files", 1, 0, 'F'},
            {"verbose", 0, 0, 'v'},
            {"decompress", 0, 0, 'z'},
            {"git-prefixes", 1, 0, 1000 + 'G'},
            {"strip", 1, 0, 1000 + 'S'},
            /* TODO: Add missing long options:
             * {"addprefix", 1, 0, 1000 + 'A'},
             * {"addoldprefix", 1, 0, 1000 + 'O'},
             * {"addnewprefix", 1, 0, 1000 + 'N'},
             */
            {0, 0, 0, 0}
        };

        char *end;
        int c = getopt_long(argc, argv, "snNHhEp:i:x:I:X:F:vz", long_options, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 1000 + 'H':
            syntax(0);
            break;
        case 1000 + 'V':
            printf("lsdiff - patchutils version %s\n", VERSION);
            exit(0);
        case 's':
            show_status = 1;
            break;
        case 'n':
            show_line_numbers = 1;
            break;
        case 'N':
            number_files = 1;
            break;
        case 'H':
            show_patch_names = 1;
            break;
        case 'h':
            show_patch_names = 0;
            break;
        case 'E':
            empty_files_as_absent = 1;
            break;
        case 'p':
            strip_components = strtoul(optarg, &end, 0);
            if (optarg == end)
                syntax(1);
            break;
        case 'i':
            patlist_add(&pat_include, optarg);
            break;
        case 'x':
            patlist_add(&pat_exclude, optarg);
            break;
        case 'I':
            patlist_add_file(&pat_include, optarg);
            break;
        case 'X':
            patlist_add_file(&pat_exclude, optarg);
            break;
        case 'F':
            parse_range(&files, optarg);
            break;
        case 'v':
            verbose++;
            break;
        case 'z':
            unzip = 1;
            break;
        case 1000 + 'G':
            if (!strcmp(optarg, "strip")) {
                git_prefix_mode = GIT_PREFIX_STRIP;
            } else if (!strcmp(optarg, "keep")) {
                git_prefix_mode = GIT_PREFIX_KEEP;
            } else {
                error(EXIT_FAILURE, 0, "invalid argument to --git-prefixes: %s (expected 'strip' or 'keep')", optarg);
            }
            break;
        case 1000 + 'S':
            {
                char *end;
                strip_output_components = strtoul(optarg, &end, 0);
                if (optarg == end) {
                    error(EXIT_FAILURE, 0, "invalid argument to --strip: %s", optarg);
                }
            }
            break;
        /* TODO: Add missing option cases:
         * case 1000 + 'A': // --addprefix=PREFIX
         * case 1000 + 'O': // --addoldprefix=PREFIX
         * case 1000 + 'N': // --addnewprefix=PREFIX
         */
        default:
            syntax(1);
        }
    }

    /* Determine show_patch_names default */
    if (show_patch_names == -1) {
        show_patch_names = (optind + 1 < argc) ? 1 : 0;
    }

    /* Process input files */
    if (optind >= argc) {
        /* Read from stdin */
        process_patch_file(stdin, "(stdin)");
    } else {
        /* Process each file */
        for (i = optind; i < argc; i++) {
            if (unzip) {
                fp = xopen_unzip(argv[i], "rb");
            } else {
                fp = xopen(argv[i], "r");
            }

            process_patch_file(fp, argv[i]);
            fclose(fp);
        }
    }

    /* Clean up */
    if (pat_include)
        patlist_free(&pat_include);
    if (pat_exclude)
        patlist_free(&pat_exclude);
    if (files) {
        struct range *r, *next;
        for (r = files; r; r = next) {
            next = r->next;
            free(r);
        }
    }

    return 0;
}

/*
 * Parse a range specification for the -F/--files option.
 *
 * Range formats supported:
 *   "3"     - single file number 3
 *   "3-5"   - files 3 through 5 (inclusive)
 *   "3-"    - files 3 through end
 *   "-"     - all files (wildcard)
 *   "1,3-5,8" - comma-separated list of ranges
 *
 * Used with -F option to select specific files from a patch by their
 * position (file number), which can then be used with filterdiff's
 * --files option for further processing.
 *
 * This is a simplified implementation that only supports single numbers.
 * The full implementation in filterdiff.c supports all range formats above.
 *
 * TODO: Implement full range parsing functionality:
 *   - Support ranges: "3-5", "3-", "-"
 *   - Support comma-separated lists: "1,3-5,8"
 *   - Support exclusion ranges with 'x' prefix
 *   - Add proper error handling for invalid ranges
 */
static void parse_range(struct range **r, const char *rstr)
{
    unsigned long n;
    char *end;
    struct range *new_range;

    n = strtoul(rstr, &end, 0);
    if (rstr == end)
        return; /* Invalid number */

    new_range = malloc(sizeof(struct range));
    if (!new_range)
        return;

    new_range->start = n;
    new_range->end = n;
    new_range->next = *r;
    *r = new_range;
}

