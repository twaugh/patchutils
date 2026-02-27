/*
 * lsdiff - list files modified by a patch
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
#include "patch_common.h"

/* Global options (lsdiff-specific) */
static int show_status = 0;           /* -s, --status */
static int empty_files_as_absent = 0; /* -E, --empty-files-as-absent */

/* Pattern matching (lsdiff-specific) */
static struct range *files = NULL;          /* -F, --files */
static int files_exclude = 0;               /* -F with x prefix */
static struct range *lines = NULL;          /* --lines */
static int lines_exclude = 0;               /* --lines with x prefix */
static struct range *hunks = NULL;          /* --hunks */
static int hunks_exclude = 0;               /* --hunks with x prefix */


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
/* determine_file_status, get_best_filename, parse_range, and other shared functions are declared in patchfilter.h */
static int file_range_filter(const char *filename);
static int lines_in_range(unsigned long orig_offset, unsigned long orig_count);
static int hunk_in_range(unsigned long hunknum);
static void process_pending_file(struct pending_file *pending);

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
    fprintf(f, "  --git-extended-diffs=exclude|include\n");
    fprintf(f, "            process Git diffs without hunks: renames, copies, mode-only\n");
    fprintf(f, "            changes, binary files; default is include\n");
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

/* File range filter callback for ls-specific functionality */
static int file_range_filter(const char *filename)
{
    (void)filename; /* Unused - we use global file_number instead */

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
            /* Check if we should skip git extended diffs in exclude mode */
            if (git_extended_diffs_mode == GIT_EXTENDED_DIFFS_EXCLUDE) {
                enum git_diff_type git_type = content->data.headers->git_type;
                /* In exclude mode, skip all extended/special types (only keep GIT_DIFF_NORMAL) */
                if (git_type != GIT_DIFF_NORMAL) {
                    continue;
                }
            }

            filecount++;

            /* If we have a pending file, display it now */
            if ((empty_files_as_absent || lines || hunks) && pending.best_filename) {
                process_pending_file(&pending);
            }

            char *best_filename = get_best_filename(content->data.headers, git_prefix_mode,
                                                    strip_output_components, add_prefix,
                                                    add_old_prefix, add_new_prefix);
            char status = determine_file_status(content->data.headers, empty_files_as_absent);

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
                pending.should_display = should_display_file_extended(best_filename, file_range_filter);
                pending.is_context_diff = (content->data.headers->type == PATCH_TYPE_CONTEXT);
                pending.has_matching_lines = 0;  /* Reset line matching flag */
                pending.has_excluded_lines = 0;  /* Reset line exclusion flag */
                pending.has_matching_hunks = 0;  /* Reset hunk matching flag */
                pending.has_excluded_hunks = 0;  /* Reset hunk exclusion flag */
                current_file = pending.should_display ? best_filename : NULL;
                best_filename = NULL;  /* Transfer ownership, don't free */
            } else {
                /* Normal processing - display immediately */
                if (should_display_file_extended(best_filename, file_range_filter)) {
                    display_filename_extended(best_filename, filename, header_line, status, show_status);
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

    /* Initialize common options */
    init_common_options();

    setlocale(LC_TIME, "C");

    while (1) {
        static struct option long_options[MAX_TOTAL_OPTIONS];
        int next_idx = 0;

        /* Add common long options */
        add_common_long_options(long_options, &next_idx);

        /* Add tool-specific long options */
        long_options[next_idx++] = (struct option){"help", 0, 0, 1000 + 'H'};
        long_options[next_idx++] = (struct option){"version", 0, 0, 1000 + 'V'};
        long_options[next_idx++] = (struct option){"status", 0, 0, 's'};
        long_options[next_idx++] = (struct option){"empty-files-as-absent", 0, 0, 'E'};
        long_options[next_idx++] = (struct option){"files", 1, 0, 'F'};
        long_options[next_idx++] = (struct option){"lines", 1, 0, 1000 + 'L'};
        long_options[next_idx++] = (struct option){"hunks", 1, 0, '#'};
        /* Mode options (handled by patchfilter, but need to be recognized) */
        long_options[next_idx++] = (struct option){"list", 0, 0, 1000 + 'l'};
        long_options[next_idx++] = (struct option){"filter", 0, 0, 1000 + 'f'};
        long_options[next_idx++] = (struct option){"grep", 0, 0, 1000 + 'g'};
        long_options[next_idx++] = (struct option){0, 0, 0, 0};

        /* Safety check: ensure we haven't exceeded MAX_TOTAL_OPTIONS */
        if (next_idx > MAX_TOTAL_OPTIONS) {
            error(EXIT_FAILURE, 0, "Internal error: too many total options (%d > %d). "
                  "Increase MAX_TOTAL_OPTIONS in patch_common.h", next_idx, MAX_TOTAL_OPTIONS);
        }

        /* Combine common and tool-specific short options */
        char short_options[64];
        snprintf(short_options, sizeof(short_options), "%ssEF:#:", get_common_short_options());

        int c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1)
            break;

        /* Try common option parsing first */
        if (parse_common_option(c, optarg)) {
            continue;
        }

        /* Handle tool-specific options */
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
        case 'E':
            empty_files_as_absent = 1;
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
    cleanup_common_options();
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
        display_filename_extended(pending->best_filename, pending->patchname, pending->header_line, final_status, show_status);
    }

    free(pending->best_filename);
    pending->best_filename = NULL;
}

