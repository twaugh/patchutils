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

#include "patchfilter.h"

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

/* Path prefix options */
static char *add_prefix = NULL;         /* --addprefix */
static char *add_old_prefix = NULL;     /* --addoldprefix */
static char *add_new_prefix = NULL;     /* --addnewprefix */

/* Pattern matching */
static struct patlist *pat_include = NULL;  /* -i, --include */
static struct patlist *pat_exclude = NULL;  /* -x, --exclude */
static struct range *files = NULL;          /* -F, --files */
static int files_exclude = 0;               /* -F with x prefix */
static struct range *lines = NULL;          /* --lines */
static int lines_exclude = 0;               /* --lines with x prefix */
static struct range *hunks = NULL;          /* --hunks */
static int hunks_exclude = 0;               /* --hunks with x prefix */

/* File counter for -N option */
static int file_number = 0;
static unsigned long filecount = 0;

/* Structure to hold pending file information */
struct pending_file {
    char *best_filename;
    const char *patchname;
    char initial_status;
    unsigned long header_line;
    int old_is_empty;
    int new_is_empty;
    int should_display;
    int is_context_diff;       /* Flag for context diff format */
    int has_matching_lines;    /* Flag for --lines filtering (include mode) */
    int has_excluded_lines;    /* Flag for --lines filtering (exclude mode) */
    int has_matching_hunks;    /* Flag for --hunks filtering (include mode) */
    int has_excluded_hunks;    /* Flag for --hunks filtering (exclude mode) */
};

/* Forward declarations */
static void syntax(int err) __attribute__((noreturn));
static void process_patch_file(FILE *fp, const char *filename);
static void display_filename(const char *filename, const char *patchname, char status, unsigned long linenum);
/* determine_file_status and get_best_filename are declared in patchfilter.h */
static const char *strip_path_components(const char *filename, int components);
static int should_display_file(const char *filename);
static int lines_in_range(unsigned long orig_offset, unsigned long orig_count);
static int hunk_in_range(unsigned long hunknum);
static void parse_range(struct range **r, const char *rstr);
static void process_pending_file(struct pending_file *pending);
static void add_filename_candidate(char **stripped_candidates, const char **candidates,
                                  int *count, const char *filename);

static void syntax(int err)
{
    FILE *f = err ? stderr : stdout;

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
    fprintf(f, "  --addprefix=PREFIX           add PREFIX to each filename\n");
    fprintf(f, "  --addoldprefix=PREFIX        add PREFIX to old filenames\n");
    fprintf(f, "  --addnewprefix=PREFIX        add PREFIX to new filenames\n");
    fprintf(f, "  --git-prefixes=strip|keep    handle a/ and b/ prefixes in Git diffs (default: keep)\n");
    fprintf(f, "  -i PAT, --include=PAT        include only files matching PAT\n");
    fprintf(f, "  -x PAT, --exclude=PAT        exclude files matching PAT\n");
    fprintf(f, "  -I FILE, --include-from-file=FILE  include only files matching patterns in FILE\n");
    fprintf(f, "  -X FILE, --exclude-from-file=FILE  exclude files matching patterns in FILE\n");
    fprintf(f, "  -F RANGE, --files=RANGE      include only files in range RANGE\n");
    fprintf(f, "  --lines=RANGE                include only files with hunks affecting lines in RANGE\n");
    fprintf(f, "  --hunks=RANGE                include only files with hunks in RANGE\n");
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
 */
static void add_filename_candidate(char **stripped_candidates, const char **candidates,
                                  int *count, const char *filename)
{
    if (!filename) {
        return;
    }

    stripped_candidates[*count] = strip_git_prefix_from_filename(filename, git_prefix_mode);
    candidates[*count] = stripped_candidates[*count];
    (*count)++;
}

char *get_best_filename(const struct patch_headers *headers)
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
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name);
                } else {
                    /* Deleted or modified file: prefer old names (git_old_name, old_name) */
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name);
                    add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name);
                }
            } else if (headers->rename_from || headers->rename_to) {
                /* Pure rename (no hunks): use git diff line filenames (source first for tie-breaking) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name);
            } else if (headers->copy_from || headers->copy_to) {
                /* Pure copy (no hunks): use git diff line filenames (source first for tie-breaking) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name);
            } else {
                /* Git diff without hunks - prefer git_old_name (traditional behavior) */
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_old_name);
                add_filename_candidate(stripped_candidates, candidates, &count, headers->git_new_name);
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
            add_filename_candidate(stripped_candidates, candidates, &count, headers->old_name);
            add_filename_candidate(stripped_candidates, candidates, &count, headers->new_name);

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

char determine_file_status(const struct patch_headers *headers)
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

        /* Check if file number matches any range (-1UL is wildcard) */
        for (r = files; r; r = r->next) {
            if ((r->start == -1UL || r->start <= file_number) &&
                (r->end == -1UL || file_number <= r->end)) {
                file_matches = 1;
                break;
            }
        }

        /* Handle exclusion logic */
        if (files && !file_matches && !files_exclude)
            return 0;  /* File doesn't match and we're including */
        if (files && file_matches && files_exclude)
            return 0;  /* File matches and we're excluding */
    }

    return 1;
}

static void display_filename(const char *filename, const char *patchname, char status, unsigned long linenum)
{
    if (show_patch_names > 0)
        printf("%s:", patchname);

    if (show_line_numbers)
        printf("%lu\t", linenum);

    if (number_files)
        printf("File #%-3lu\t", filecount);

    if (show_status)
        printf("%c ", status);

    printf("%s\n", filename);
}


/* Global cumulative line counter for tracking across multiple files */
static unsigned long global_line_offset = 0;

static void process_patch_file(FILE *fp, const char *filename)
{
    patch_scanner_t *scanner;
    const patch_content_t *content;
    enum patch_scanner_result result;
    unsigned long header_line = 1;
    const char *current_file = NULL;
    int hunk_number = 0;
    struct pending_file pending = {0};

    scanner = patch_scanner_create(fp);
    if (!scanner) {
        error(EXIT_FAILURE, 0, "Failed to create patch scanner");
        return;
    }

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        if (content->type == PATCH_CONTENT_HEADERS) {
            filecount++;

            /* If we have a pending file, display it now */
            if ((empty_files_as_absent || lines || hunks) && pending.best_filename) {
                process_pending_file(&pending);
            }

            char *best_filename = get_best_filename(content->data.headers);
            char status = determine_file_status(content->data.headers);

            /* Use the line number where the headers started, adjusted for global offset */
            header_line = global_line_offset + content->data.headers->start_line;

            file_number++;
            hunk_number = 0;  /* Reset hunk counter for new file */

            if (empty_files_as_absent || lines || hunks) {
                /* Store pending file info for -E processing, --lines filtering, or --hunks filtering */
                pending.best_filename = best_filename;  /* Transfer ownership to pending */
                pending.patchname = filename;
                pending.initial_status = status;
                pending.header_line = header_line;
                pending.old_is_empty = 1;  /* Assume empty until proven otherwise */
                pending.new_is_empty = 1;  /* Assume empty until proven otherwise */
                pending.should_display = should_display_file(best_filename);
                pending.is_context_diff = (content->data.headers->type == PATCH_TYPE_CONTEXT);
                pending.has_matching_lines = 0;  /* Reset line matching flag */
                pending.has_excluded_lines = 0;  /* Reset line exclusion flag */
                pending.has_matching_hunks = 0;  /* Reset hunk matching flag */
                pending.has_excluded_hunks = 0;  /* Reset hunk exclusion flag */
                current_file = pending.should_display ? best_filename : NULL;
                best_filename = NULL;  /* Transfer ownership, don't free */
            } else {
                /* Normal processing - display immediately */
                if (should_display_file(best_filename)) {
                    display_filename(best_filename, filename, status, header_line);
                    current_file = best_filename;  /* Track current file for verbose output */
                } else {
                    current_file = NULL;  /* Don't show hunks for filtered files */
                }
                free(best_filename);  /* Free immediately after use */
            }
        } else if (content->type == PATCH_CONTENT_HUNK_HEADER) {
            const struct patch_hunk *hunk = content->data.hunk;

            hunk_number++;  /* Increment hunk counter */

            /* Check if this hunk's lines are in the specified ranges */
            if (lines && (empty_files_as_absent || lines || hunks) && pending.best_filename) {
                if (lines_in_range(hunk->orig_offset, hunk->orig_count)) {
                    if (!lines_exclude) {
                        /* Include mode: this hunk causes file to be included */
                        pending.has_matching_lines = 1;
                    } else {
                        /* Exclude mode: this hunk causes file to be excluded */
                        pending.has_excluded_lines = 1;
                    }
                } else {
                    if (lines_exclude) {
                        /* Exclude mode: this hunk doesn't match exclusion, so it supports inclusion */
                        pending.has_matching_lines = 1;
                    }
                }
            }

            /* Check if this hunk is in the specified ranges */
            if (hunks && (empty_files_as_absent || lines || hunks) && pending.best_filename) {
                if (hunk_in_range(hunk_number)) {
                    if (!hunks_exclude) {
                        /* Include mode: this hunk causes file to be included */
                        pending.has_matching_hunks = 1;
                    } else {
                        /* Exclude mode: this hunk causes file to be excluded */
                        pending.has_excluded_hunks = 1;
                    }
                } else {
                    if (hunks_exclude) {
                        /* Exclude mode: this hunk doesn't match exclusion, so it supports inclusion */
                        pending.has_matching_hunks = 1;
                    }
                }
            }

            if (empty_files_as_absent && pending.best_filename) {
                /* Analyze hunk to determine if files are empty */

                if (pending.is_context_diff) {
                    /* For context diffs, we'll track emptiness via hunk lines instead */
                    /* The hunk header approach doesn't work because new_count isn't set yet */
                    /* So we defer this and track via actual hunk content */
                    if (hunk->orig_count > 0) {
                        pending.old_is_empty = 0;
                    }
                    /* Don't check new_count here for context diffs - it's not reliable */
                } else {
                    /* For unified diffs, both counts are available immediately */
                    if (hunk->orig_count > 0) {
                        pending.old_is_empty = 0;
                    }
                    if (hunk->new_count > 0) {
                        pending.new_is_empty = 0;
                    }
                }
            }

            if (verbose > 0 && show_line_numbers && current_file) {
                /* In numbered verbose mode, show hunk information */

                /* Show patch name prefix if enabled, with '-' suffix for hunk lines */
                if (show_patch_names > 0)
                    printf("%s-", filename);

                if (show_line_numbers) {
                    printf("\t%lu\tHunk #%d", global_line_offset + content->line_number, hunk_number);
                    if (verbose > 1 && hunk->context && hunk->context[0]) {
                        printf("\t%s", hunk->context);
                    }
                    printf("\n");
                } else {
                    printf("\tHunk #%d", hunk_number);
                    if (verbose > 1 && hunk->context && hunk->context[0]) {
                        printf("\t%s", hunk->context);
                    }
                    printf("\n");
                }
            }
        } else if (content->type == PATCH_CONTENT_HUNK_LINE) {
            if (empty_files_as_absent && pending.best_filename && pending.is_context_diff) {
                /* For context diffs, determine emptiness from hunk line content */
                const struct patch_hunk_line *hunk_line = content->data.line;


                switch (hunk_line->type) {
                case ' ':  /* Context line - both files have content */
                case '!':  /* Changed line - both files have content */
                    pending.old_is_empty = 0;
                    pending.new_is_empty = 0;
                    break;
                case '-':  /* Removed line - old file has content */
                    pending.old_is_empty = 0;
                    break;
                case '+':  /* Added line - new file has content */
                    pending.new_is_empty = 0;
                    break;
                case '\\': /* No newline marker - doesn't affect emptiness */
                    break;
                }
            }
        }
    }

    /* Handle final pending file */
    if ((empty_files_as_absent || lines || hunks) && pending.best_filename) {
        process_pending_file(&pending);
    }

    if (result == PATCH_SCAN_ERROR) {
        if (verbose)
            fprintf(stderr, "Warning: Error parsing patch in %s\n", filename);
    }

    /* Update global line offset for next file (subtract 1 to avoid double-counting) */
    global_line_offset += patch_scanner_line_number(scanner) - 1;

    patch_scanner_destroy(scanner);
}

int run_ls_mode(int argc, char *argv[])
{
    int i;
    FILE *fp;

    /* Reset global line offset for each invocation */
    global_line_offset = 0;

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
            {"addprefix", 1, 0, 1000 + 'A'},
            {"addoldprefix", 1, 0, 1000 + 'O'},
            {"addnewprefix", 1, 0, 1000 + 'N'},
            {"lines", 1, 0, 1000 + 'L'},
            {"hunks", 1, 0, '#'},
            /* Mode options (handled by patchfilter, but need to be recognized) */
            {"list", 0, 0, 1000 + 'l'},
            {"filter", 0, 0, 1000 + 'f'},
            {"grep", 0, 0, 1000 + 'g'},
            {0, 0, 0, 0}
        };

        char *end;
        int c = getopt_long(argc, argv, "snNHhEp:i:x:I:X:F:vz#:", long_options, NULL);
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
            if (files)
                syntax(1);
            if (*optarg == 'x') {
                files_exclude = 1;
                optarg = optarg + 1;
            }
            parse_range(&files, optarg);
            break;
        case 'v':
            verbose++;
            if (show_line_numbers && verbose > 1)
                number_files = 1;
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
        case 1000 + 'A':
            add_prefix = optarg;
            break;
        case 1000 + 'O':
            add_old_prefix = optarg;
            break;
        case 1000 + 'N':
            add_new_prefix = optarg;
            break;
        case 1000 + 'L':
            if (lines)
                syntax(1);
            if (*optarg == 'x') {
                lines_exclude = 1;
                optarg = optarg + 1;
            }
            parse_range(&lines, optarg);
            break;
        case '#':
            if (hunks)
                syntax(1);
            if (*optarg == 'x') {
                hunks_exclude = 1;
                optarg = optarg + 1;
            }
            parse_range(&hunks, optarg);
            break;
        case 1000 + 'l':
        case 1000 + 'f':
        case 1000 + 'g':
            /* Mode options - handled by patchfilter, ignore here */
            break;
        default:
            syntax(1);
        }
    }

    /* Determine show_patch_names default */
    if (show_patch_names == -1) {
        show_patch_names = (optind + 1 < argc) ? 1 : 0;
    }

    /* Handle -p without -i/-x: print warning and use as --strip */
    if (strip_components > 0 && strip_output_components == 0 && !pat_include && !pat_exclude) {
        fprintf(stderr, "-p given without -i or -x; guessing that you meant --strip instead.\n");
        strip_output_components = strip_components;
    }

    /* Process input files */
    if (optind >= argc) {
        /* Read from stdin */
        process_patch_file(stdin, "(standard input)");
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
    if (lines) {
        struct range *r, *next;
        for (r = lines; r; r = next) {
            next = r->next;
            free(r);
        }
    }
    if (hunks) {
        struct range *r, *next;
        for (r = hunks; r; r = next) {
            next = r->next;
            free(r);
        }
    }

    return 0;
}

/*
 * Check if lines are in the specified line ranges.
 * Returns 1 if the lines are in the range, 0 otherwise.
 */
static int lines_in_range(unsigned long orig_offset, unsigned long orig_count)
{
    struct range *r;

    if (!lines)
        return 0; /* No line filter specified */

    /* For the purposes of matching, zero lines at offset n counts as line n */
    if (!orig_count)
        orig_count = 1;

    /* See if the line range list includes this hunk's lines.  -1UL is a wildcard. */
    for (r = lines; r; r = r->next) {
        if ((r->start == -1UL ||
             r->start < (orig_offset + orig_count)) &&
            (r->end == -1UL ||
             r->end >= orig_offset)) {
            return 1;
        }
    }

    return 0;
}

/*
 * Check if a hunk number is in the specified hunk ranges.
 * Returns 1 if the hunk number is in the range, 0 otherwise.
 */
static int hunk_in_range(unsigned long hunknum)
{
    struct range *r;

    if (!hunks)
        return 0; /* No hunk filter specified */

    /* See if the hunk range list includes this hunk.  -1UL is a wildcard. */
    for (r = hunks; r; r = r->next) {
        if ((r->start == -1UL || r->start <= hunknum) &&
            (r->end == -1UL || hunknum <= r->end)) {
            return 1;
        }
    }

    return 0;
}

/*
 * Process a pending file: apply filtering logic and display if it matches.
 * This function handles the complete logic for determining whether a pending
 * file should be displayed, including empty-as-absent processing and all
 * filtering criteria (lines, hunks, patterns).
 */
static void process_pending_file(struct pending_file *pending)
{
    if (!pending || !pending->best_filename) {
        return;
    }

    char final_status = pending->initial_status;

    /* Apply empty-as-absent logic if -E is specified */
    if (empty_files_as_absent) {
        if (pending->old_is_empty && !pending->new_is_empty) {
            final_status = '+'; /* Treat as new file */
        } else if (!pending->old_is_empty && pending->new_is_empty) {
            final_status = '-'; /* Treat as deleted file */
        }
    }

    /* Check if we should display this file based on filtering criteria */
    int should_display = pending->should_display;

    /* Apply line filtering first */
    if (lines && should_display) {
        /* If --lines is specified, apply line filtering logic */
        if (!lines_exclude) {
            /* Include mode: only display if file has matching lines */
            should_display = pending->has_matching_lines;
        } else {
            /* Exclude mode: only display if file has NO excluded lines */
            should_display = !pending->has_excluded_lines;
        }
    }

    /* Apply hunk filtering (both filters must pass if both are specified) */
    if (hunks && should_display) {
        /* If --hunks is specified, apply hunk filtering logic */
        if (!hunks_exclude) {
            /* Include mode: only display if file has matching hunks */
            should_display = pending->has_matching_hunks;
        } else {
            /* Exclude mode: only display if file has NO excluded hunks */
            should_display = !pending->has_excluded_hunks;
        }
    }

    if (should_display) {
        display_filename(pending->best_filename, pending->patchname, final_status, pending->header_line);
    }

    free(pending->best_filename);
    pending->best_filename = NULL;
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
 */
static void parse_range(struct range **r, const char *rstr)
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

