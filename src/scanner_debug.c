/*
 * scanner_debug.c - patch scanner debugging utility
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
 * This utility shows exactly what events the patch scanner API emits
 * for a given patch file, making it easy to debug scanner behaviour.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include "patch_scanner.h"
#include "util.h"

/* Global options */
static int show_positions = 0;    /* -p, --positions */
static int show_content = 0;      /* -c, --content */
static int show_extra = 0;        /* -x, --extra */
static int color_output = 0;      /* --color */
static int verbose_output = 0;    /* -v, --verbose */

/* ANSI color codes for pretty output */
#define COLOR_RESET     "\033[0m"
#define COLOR_BOLD      "\033[1m"
#define COLOR_RED       "\033[31m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_BLUE      "\033[34m"
#define COLOR_MAGENTA   "\033[35m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_GRAY      "\033[90m"

/* Color helpers */
#define C(color) (color_output ? color : "")

/* Forward declarations */
static void usage(int exit_code);
static void print_event_header(const char *event_name, const char *color,
                               unsigned long line_num, long position);
static void print_compact_event(const char *event_name, const char *color,
                               unsigned long line_num, const char *content);
static void print_headers_info(const struct patch_headers *headers);
static void print_hunk_info(const struct patch_hunk *hunk);
static void print_hunk_line_info(const struct patch_hunk_line *line);
static void print_content_sample(const char *content, size_t length);
static const char *patch_type_name(enum patch_type type);
static const char *git_diff_type_name(enum git_diff_type type);
static const char *hunk_line_type_name(enum patch_hunk_line_type type);

int main(int argc, char *argv[])
{
    int opt;
    FILE *input = stdin;
    const char *filename = "(stdin)";

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"content", no_argument, 0, 'c'},
        {"positions", no_argument, 0, 'p'},
        {"extra", no_argument, 0, 'x'},
        {"color", no_argument, 0, 1000},
        {0, 0, 0, 0}
    };

    /* Parse command line options */
    while ((opt = getopt_long(argc, argv, "hvcpx", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(0);
            break;
        case 'v':
            verbose_output = 1;
            break;
        case 'c':
            show_content = 1;
            break;
        case 'p':
            show_positions = 1;
            break;
        case 'x':
            show_extra = 1;
            break;
        case 1000: /* --color */
            color_output = 1;
            break;
        default:
            usage(1);
        }
    }

    /* Handle input file */
    if (optind < argc) {
        filename = argv[optind];
        input = fopen(filename, "r");
        if (!input) {
            fprintf(stderr, "Error: Cannot open file '%s': %s\n",
                    filename, strerror(errno));
            return 1;
        }
    }

    printf("%sScanner Debug Output for: %s%s%s\n",
           C(COLOR_BOLD), C(COLOR_CYAN), filename, C(COLOR_RESET));
    printf("%s%s%s\n", C(COLOR_GRAY),
           "================================================================",
           C(COLOR_RESET));

    /* Create scanner */
    patch_scanner_t *scanner = patch_scanner_create(input);
    if (!scanner) {
        fprintf(stderr, "Error: Failed to create patch scanner\n");
        if (input != stdin) fclose(input);
        return 1;
    }

    /* Process all events */
    const patch_content_t *content;
    enum patch_scanner_result result;
    int event_count = 0;

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        event_count++;

        if (!verbose_output) {
            /* Compact columnar output (default) */
            switch (content->type) {
            case PATCH_CONTENT_NON_PATCH:
                print_compact_event("NON-PATCH", COLOR_GRAY, content->line_number,
                                   content->data.non_patch.line);
                break;
            case PATCH_CONTENT_HEADERS:
                {
                    char header_desc[256];
                    snprintf(header_desc, sizeof(header_desc), "%s: %s → %s",
                            patch_type_name(content->data.headers->type),
                            content->data.headers->old_name ? content->data.headers->old_name : "?",
                            content->data.headers->new_name ? content->data.headers->new_name : "?");
                    print_compact_event("HEADERS", COLOR_GREEN, content->line_number, header_desc);
                }
                break;
            case PATCH_CONTENT_HUNK_HEADER:
                {
                    char hunk_desc[128];
                    snprintf(hunk_desc, sizeof(hunk_desc), "-%lu,%lu +%lu,%lu",
                            content->data.hunk->orig_offset, content->data.hunk->orig_count,
                            content->data.hunk->new_offset, content->data.hunk->new_count);
                    print_compact_event("HUNK_HEADER", COLOR_YELLOW, content->line_number, hunk_desc);
                }
                break;
            case PATCH_CONTENT_HUNK_LINE:
                {
                    char line_desc[128];
                    const char *type_str = "";
                    switch (content->data.line->type) {
                    case PATCH_LINE_CONTEXT: type_str = " "; break;
                    case PATCH_LINE_ADDED: type_str = "+"; break;
                    case PATCH_LINE_REMOVED: type_str = "-"; break;
                    case PATCH_LINE_CHANGED: type_str = "!"; break;
                    case PATCH_LINE_NO_NEWLINE: type_str = "\\"; break;
                    default: type_str = "?"; break;
                    }
                    snprintf(line_desc, sizeof(line_desc), "%s%.*s",
                            type_str,
                            (int)(content->data.line->length > 60 ? 60 : content->data.line->length),
                            content->data.line->content ? content->data.line->content : "");
                    /* Remove newline for cleaner display */
                    char *nl = strchr(line_desc, '\n');
                    if (nl) *nl = '\0';
                    print_compact_event("HUNK_LINE", COLOR_BLUE, content->line_number, line_desc);
                }
                break;
            case PATCH_CONTENT_NO_NEWLINE:
                print_compact_event("NO_NEWLINE", COLOR_MAGENTA, content->line_number,
                                   content->data.no_newline.line);
                break;
            case PATCH_CONTENT_BINARY:
                print_compact_event("BINARY", COLOR_RED, content->line_number,
                                   content->data.binary.is_git_binary ? "Git binary patch" : "Binary files differ");
                break;
            default:
                {
                    char unknown_desc[64];
                    snprintf(unknown_desc, sizeof(unknown_desc), "Unknown type: %d", content->type);
                    print_compact_event("UNKNOWN", COLOR_RED, content->line_number, unknown_desc);
                }
                break;
            }
        } else {
            /* Verbose output (-v/--verbose) */
            switch (content->type) {
            case PATCH_CONTENT_NON_PATCH:
                print_event_header("NON-PATCH", COLOR_GRAY,
                                 content->line_number, content->position);
                if (show_content) {
                    print_content_sample(content->data.non_patch.line,
                                        content->data.non_patch.length);
                }
                break;

            case PATCH_CONTENT_HEADERS:
                print_event_header("HEADERS", COLOR_GREEN,
                                 content->line_number, content->position);
                print_headers_info(content->data.headers);
                break;

            case PATCH_CONTENT_HUNK_HEADER:
                print_event_header("HUNK_HEADER", COLOR_YELLOW,
                                 content->line_number, content->position);
                print_hunk_info(content->data.hunk);
                break;

            case PATCH_CONTENT_HUNK_LINE:
                print_event_header("HUNK_LINE", COLOR_BLUE,
                                 content->line_number, content->position);
                print_hunk_line_info(content->data.line);
                break;

            case PATCH_CONTENT_NO_NEWLINE:
                print_event_header("NO_NEWLINE", COLOR_MAGENTA,
                                 content->line_number, content->position);
                if (show_content) {
                    print_content_sample(content->data.no_newline.line,
                                        content->data.no_newline.length);
                }
                break;

            case PATCH_CONTENT_BINARY:
                print_event_header("BINARY", COLOR_RED,
                                 content->line_number, content->position);
                printf("  %sType:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
                       content->data.binary.is_git_binary ? "Git binary patch" : "Binary files differ");
                if (show_content) {
                    print_content_sample(content->data.binary.line,
                                        content->data.binary.length);
                }
                break;

            default:
                print_event_header("UNKNOWN", COLOR_RED,
                                 content->line_number, content->position);
                printf("  %sUnknown content type: %d%s\n",
                       C(COLOR_RED), content->type, C(COLOR_RESET));
                break;
            }

            printf("\n");  /* Blank line between events in verbose mode */
        }
    }

    /* Print final summary */
    printf("%s%s%s\n", C(COLOR_GRAY),
           "================================================================",
           C(COLOR_RESET));

    if (result == PATCH_SCAN_EOF) {
        printf("%sSummary:%s Processed %s%d%s events, scanner finished normally\n",
               C(COLOR_BOLD), C(COLOR_RESET), C(COLOR_GREEN), event_count, C(COLOR_RESET));
    } else {
        printf("%sError:%s Scanner failed with code %d after %d events\n",
               C(COLOR_RED), C(COLOR_RESET), result, event_count);
    }

    if (show_extra) {
        printf("%sFinal position:%s %ld, line: %lu\n",
               C(COLOR_BOLD), C(COLOR_RESET),
               patch_scanner_position(scanner),
               patch_scanner_line_number(scanner));
    }

    /* Cleanup */
    patch_scanner_destroy(scanner);
    if (input != stdin) fclose(input);

    return (result == PATCH_SCAN_EOF) ? 0 : 1;
}

static void usage(int exit_code)
{
    printf("Usage: scanner_debug [OPTIONS] [FILE]\n");
    printf("Debug utility to show patch scanner API events\n\n");
    printf("Options:\n");
    printf("  -h, --help       Show this help message\n");
    printf("  -v, --verbose    Use  multi-line output instead of compact\n");
    printf("  -c, --content    Show content samples for events (verbose mode)\n");
    printf("  -p, --positions  Show file positions for all events (verbose mode)\n");
    printf("  -x, --extra      Show extra details like Git metadata (verbose mode)\n");
    printf("      --color      Use colored output\n\n");
    printf("By default, uses compact columnar output. Use -v/--verbose for more detail.\n\n");
    printf("If no FILE is specified, reads from stdin.\n\n");
    printf("Examples:\n");
    printf("  scanner_debug --color patch.diff\n");
    printf("  scanner_debug -v --color --content patch.diff\n");
    printf("  diff -u old new | scanner_debug -v\n");
    printf("  scanner_debug --color < complex.patch\n");
    exit(exit_code);
}

static void print_event_header(const char *event_name, const char *color,
                               unsigned long line_num, long position)
{
    printf("%s[%s]%s",
           C(color), event_name, C(COLOR_RESET));

    if (show_positions || show_extra) {
        printf(" %s(line %lu, pos %ld)%s",
               C(COLOR_GRAY), line_num, position, C(COLOR_RESET));
    }
    printf("\n");
}

static void print_compact_event(const char *event_name, const char *color,
                               unsigned long line_num, const char *content)
{
    printf("%s%3lu%s %s%-12s%s ",
           C(COLOR_GRAY), line_num, C(COLOR_RESET),
           C(color), event_name, C(COLOR_RESET));

    if (content) {
        /* Print content but strip trailing newlines for compact display */
        const char *p = content;
        while (*p) {
            if (*p == '\n') {
                /* Skip newlines - they cause blank lines in compact mode */
                p++;
                continue;
            } else if (*p == '\r') {
                /* Skip carriage returns too */
                p++;
                continue;
            }
            putchar(*p);
            p++;
        }
    }
    printf("\n");
}

static void print_headers_info(const struct patch_headers *headers)
{
    printf("  %sType:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
           patch_type_name(headers->type));

    if (headers->type == PATCH_TYPE_GIT_EXTENDED) {
        printf("  %sGit Type:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
               git_diff_type_name(headers->git_type));
    }

    if (headers->old_name) {
        printf("  %sOld:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
               headers->old_name);
    }

    if (headers->new_name) {
        printf("  %sNew:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
               headers->new_name);
    }

    if (show_extra) {
        if (headers->git_old_name) {
            printf("  %sGit Old:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
                   headers->git_old_name);
        }
        if (headers->git_new_name) {
            printf("  %sGit New:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
                   headers->git_new_name);
        }
        if (headers->old_mode != -1) {
            printf("  %sOld Mode:%s %06o\n", C(COLOR_BOLD), C(COLOR_RESET),
                   headers->old_mode);
        }
        if (headers->new_mode != -1) {
            printf("  %sNew Mode:%s %06o\n", C(COLOR_BOLD), C(COLOR_RESET),
                   headers->new_mode);
        }
        if (headers->is_binary) {
            printf("  %sBinary:%s yes\n", C(COLOR_BOLD), C(COLOR_RESET));
        }
        printf("  %sHeaders:%s %u lines\n", C(COLOR_BOLD), C(COLOR_RESET),
               headers->num_headers);
    }
}

static void print_hunk_info(const struct patch_hunk *hunk)
{
    printf("  %sRange:%s -%lu,%lu +%lu,%lu\n",
           C(COLOR_BOLD), C(COLOR_RESET),
           hunk->orig_offset, hunk->orig_count,
           hunk->new_offset, hunk->new_count);

    if (hunk->context && show_content) {
        printf("  %sContext:%s %s\n", C(COLOR_BOLD), C(COLOR_RESET),
               hunk->context);
    }
}

static void print_hunk_line_info(const struct patch_hunk_line *line)
{
    printf("  %sType:%s %s", C(COLOR_BOLD), C(COLOR_RESET),
           hunk_line_type_name(line->type));

    if (show_content && line->content) {
        printf(" %sContent:%s ", C(COLOR_BOLD), C(COLOR_RESET));
        print_content_sample(line->content, line->length);
    } else {
        printf("\n");
    }
}

static void print_content_sample(const char *content, size_t length)
{
    if (!content) {
        printf("(null)\n");
        return;
    }

    /* Limit sample length and handle newlines */
    size_t sample_len = length > 60 ? 60 : length;

    printf("\"");
    for (size_t i = 0; i < sample_len; i++) {
        switch (content[i]) {
        case '\n':
            printf("\\n");
            break;
        case '\t':
            printf("\\t");
            break;
        case '\r':
            printf("\\r");
            break;
        case '\\':
            printf("\\\\");
            break;
        case '"':
            printf("\\\"");
            break;
        default:
            if (content[i] >= 32 && content[i] <= 126) {
                putchar(content[i]);
            } else {
                printf("\\x%02x", (unsigned char)content[i]);
            }
            break;
        }
    }

    if (length > sample_len) {
        printf("...");
    }
    printf("\"\n");
}

static const char *patch_type_name(enum patch_type type)
{
    switch (type) {
    case PATCH_TYPE_UNIFIED:
        return "Unified";
    case PATCH_TYPE_CONTEXT:
        return "Context";
    case PATCH_TYPE_GIT_EXTENDED:
        return "Git Extended";
    default:
        return "Unknown";
    }
}

static const char *git_diff_type_name(enum git_diff_type type)
{
    switch (type) {
    case GIT_DIFF_NORMAL:
        return "Normal";
    case GIT_DIFF_NEW_FILE:
        return "New File";
    case GIT_DIFF_DELETED_FILE:
        return "Deleted File";
    case GIT_DIFF_RENAME:
        return "Rename";
    case GIT_DIFF_PURE_RENAME:
        return "Pure Rename";
    case GIT_DIFF_COPY:
        return "Copy";
    case GIT_DIFF_MODE_ONLY:
        return "Mode Only";
    case GIT_DIFF_MODE_CHANGE:
        return "Mode Change";
    case GIT_DIFF_BINARY:
        return "Binary";
    default:
        return "Unknown";
    }
}

static const char *hunk_line_type_name(enum patch_hunk_line_type type)
{
    switch (type) {
    case PATCH_LINE_CONTEXT:
        return "Context (' ')";
    case PATCH_LINE_ADDED:
        return "Added ('+')";
    case PATCH_LINE_REMOVED:
        return "Removed ('-')";
    case PATCH_LINE_CHANGED:
        return "Changed ('!')";
    case PATCH_LINE_NO_NEWLINE:
        return "No Newline ('\\')";
    default:
        return "Unknown";
    }
}
