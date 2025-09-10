/*
 * patch_scanner.c - unified patch parsing implementation
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "patch_scanner.h"
#include "util.h"

/* Forward declarations for header parsing functions */
static void scanner_parse_git_diff_line(patch_scanner_t *scanner, const char *line);
static void scanner_parse_old_file_line(patch_scanner_t *scanner, const char *line);
static void scanner_parse_new_file_line(patch_scanner_t *scanner, const char *line);
static void scanner_parse_index_line(patch_scanner_t *scanner, const char *line);
static void scanner_parse_mode_line(patch_scanner_t *scanner, const char *line, int *mode_field);
static void scanner_parse_similarity_line(patch_scanner_t *scanner, const char *line);
static void scanner_parse_dissimilarity_line(patch_scanner_t *scanner, const char *line);
static void scanner_determine_git_diff_type(patch_scanner_t *scanner);

/* Helper functions for common parsing patterns */
static char *scanner_extract_filename(const char *line, int prefix_len);
static const char *scanner_find_timestamp_start(const char *filename);
static void scanner_parse_index_percentage(const char *line, const char *prefix, int *target_field);
static void scanner_parse_filename_field(const char *line, int prefix_len, char **target_field);

/* Forward declarations for header order validation functions */
static int scanner_validate_git_header_order(patch_scanner_t *scanner);
static int scanner_validate_context_header_order(patch_scanner_t *scanner);
static int scanner_validate_unified_header_order(patch_scanner_t *scanner);
static int scanner_is_git_extended_header(const char *line);

/* Scanner internal state */
enum scanner_state {
    STATE_SEEKING_PATCH,         /* Looking for start of patch */
    STATE_ACCUMULATING_HEADERS,  /* Collecting potential headers */
    STATE_IN_PATCH,             /* Processing patch content */
    STATE_IN_HUNK,              /* Processing hunk lines */
    STATE_BINARY_READY,         /* Ready to emit binary content */
    STATE_ERROR                 /* Error state */
};

/* Internal scanner structure */
struct patch_scanner {
    FILE *file;                 /* Input stream */

    /* Line reading state */
    char *line_buffer;          /* Reusable line buffer */
    size_t line_buffer_size;    /* Buffer size */
    unsigned long line_number;  /* Current line number (1-based) */
    long current_position;      /* Current file position */

    /* Parser state */
    enum scanner_state state;   /* Current parsing state */

    /* Header accumulation */
    struct patch_headers *pending_headers; /* Headers being accumulated */
    char **header_lines;        /* Raw header lines */
    unsigned int num_header_lines; /* Number of accumulated headers */
    unsigned int header_lines_allocated; /* Allocated header slots */
    unsigned long header_start_line; /* Line number where current headers started */

    /* Current content being emitted */
    struct patch_content current_content; /* Content structure for emission */
    struct patch_headers current_headers; /* Current patch headers */
    struct patch_hunk current_hunk;       /* Current hunk */
    struct patch_hunk_line current_line;  /* Current hunk line */

    /* Hunk processing state */
    unsigned long hunk_orig_remaining; /* Remaining original lines in hunk */
    unsigned long hunk_new_remaining;  /* Remaining new lines in hunk */
    int in_hunk;                       /* Are we currently in a hunk? */

    /* Simple one-line buffer for stdin-compatible peek-ahead */
    char *next_line;                   /* Next line buffered for peek-ahead */
    unsigned long next_line_number;    /* Line number of buffered line */
    int has_next_line;                 /* Flag: next_line contains valid data */
};

/* Forward declarations */
static int scanner_read_line(patch_scanner_t *scanner);
static int scanner_is_potential_patch_start(const char *line);
static int scanner_is_header_continuation(patch_scanner_t *scanner, const char *line);
static int scanner_validate_headers(patch_scanner_t *scanner);
static int scanner_parse_headers(patch_scanner_t *scanner);
static void scanner_init_content(patch_scanner_t *scanner, enum patch_content_type type);
static int scanner_emit_non_patch(patch_scanner_t *scanner, const char *line, size_t length);
static int scanner_emit_headers(patch_scanner_t *scanner);
static int scanner_emit_hunk_header(patch_scanner_t *scanner, const char *line);
static int scanner_emit_context_hunk_header(patch_scanner_t *scanner, const char *line);
static int scanner_emit_context_new_hunk_header(patch_scanner_t *scanner, const char *line);
static int scanner_emit_hunk_line(patch_scanner_t *scanner, const char *line);
static int scanner_emit_no_newline(patch_scanner_t *scanner, const char *line);
static int scanner_emit_binary(patch_scanner_t *scanner, const char *line);
static void scanner_free_headers(patch_scanner_t *scanner);
static void scanner_reset_for_next_patch(patch_scanner_t *scanner);

/* Stdin-compatible header completion logic */
static int scanner_should_wait_for_unified_headers(patch_scanner_t *scanner);

/* Public API implementation */

patch_scanner_t* patch_scanner_create(FILE *file)
{
    patch_scanner_t *scanner;

    if (!file) {
        return NULL;
    }

    scanner = xmalloc(sizeof(patch_scanner_t));
    memset(scanner, 0, sizeof(patch_scanner_t));

    scanner->file = file;
    scanner->line_buffer_size = 1024;
    scanner->line_buffer = xmalloc(scanner->line_buffer_size);
    scanner->line_number = 0;
    scanner->current_position = ftell(file);
    scanner->state = STATE_SEEKING_PATCH;

    /* Initialize header accumulation */
    scanner->header_lines_allocated = 8;
    scanner->header_lines = xmalloc(sizeof(char*) * scanner->header_lines_allocated);

    /* Initialize simple peek-ahead buffer */
    scanner->next_line = NULL;
    scanner->next_line_number = 0;
    scanner->has_next_line = 0;

    return scanner;
}

int patch_scanner_next(patch_scanner_t *scanner, const patch_content_t **content)
{
    char *line;
    size_t line_length;
    int result;

    if (!scanner || !content) {
        return PATCH_SCAN_ERROR;
    }

    if (scanner->state == STATE_ERROR) {
        return PATCH_SCAN_ERROR;
    }

    /* Main parsing loop - prevents recursion */
    for (;;) {
        /* Handle states that don't require reading a new line */
        if (scanner->state == STATE_BINARY_READY) {
            /* Emit binary content for binary-only patches */
            scanner_emit_binary(scanner, "Binary patch");
            scanner->state = STATE_SEEKING_PATCH; /* Reset for next patch */
            *content = &scanner->current_content;
            return PATCH_SCAN_OK;
        }

        /* Read next line */
        result = scanner_read_line(scanner);
        if (result == PATCH_SCAN_EOF) {
            /* Handle EOF - if we were accumulating headers, emit them as non-patch */
            if (scanner->state == STATE_ACCUMULATING_HEADERS && scanner->num_header_lines > 0) {
                /* TODO: Emit accumulated headers as non-patch content */
            }
            return PATCH_SCAN_EOF;
        } else if (result != PATCH_SCAN_OK) {
            scanner->state = STATE_ERROR;
            return result;
        }

        line = scanner->line_buffer;
        line_length = strlen(line);

        /* State machine for parsing */
        switch (scanner->state) {
        case STATE_SEEKING_PATCH:
            if (scanner_is_potential_patch_start(line)) {
                /* Start accumulating headers */
                scanner->state = STATE_ACCUMULATING_HEADERS;
                scanner->num_header_lines = 0;
                scanner->header_start_line = scanner->line_number;

                /* Store first header line */
                if (scanner->num_header_lines >= scanner->header_lines_allocated) {
                    /* Prevent integer overflow and limit maximum headers */
                    if (scanner->header_lines_allocated > 1024) {
                        scanner->state = STATE_ERROR;
                        return PATCH_SCAN_ERROR;
                    }
                    unsigned int new_size = scanner->header_lines_allocated * 2;
                    if (new_size < scanner->header_lines_allocated) {
                        /* Overflow detected */
                        scanner->state = STATE_ERROR;
                        return PATCH_SCAN_ERROR;
                    }
                    scanner->header_lines_allocated = new_size;
                    scanner->header_lines = xrealloc(scanner->header_lines,
                        sizeof(char*) * scanner->header_lines_allocated);
                }
                scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);

                /* Don't emit yet, continue accumulating */
                continue;
            } else {
                /* Emit as non-patch content */
                scanner_emit_non_patch(scanner, line, line_length);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            }

        case STATE_ACCUMULATING_HEADERS:
            if (scanner_is_header_continuation(scanner, line)) {
                /* Add to accumulated headers */
                if (scanner->num_header_lines >= scanner->header_lines_allocated) {
                    /* Prevent integer overflow and limit maximum headers */
                    if (scanner->header_lines_allocated > 1024) {
                        scanner->state = STATE_ERROR;
                        return PATCH_SCAN_ERROR;
                    }
                    unsigned int new_size = scanner->header_lines_allocated * 2;
                    if (new_size < scanner->header_lines_allocated) {
                        /* Overflow detected */
                        scanner->state = STATE_ERROR;
                        return PATCH_SCAN_ERROR;
                    }
                    scanner->header_lines_allocated = new_size;
                    scanner->header_lines = xrealloc(scanner->header_lines,
                        sizeof(char*) * scanner->header_lines_allocated);
                }
                scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);

                /* Check if we have complete headers */
                if (scanner_validate_headers(scanner)) {
                    /* We have valid headers - parse and emit them */
                    scanner_parse_headers(scanner);
                    scanner->state = STATE_IN_PATCH;

                    /* Check if this is a binary-only patch (no hunks expected) */
                    if (scanner->current_headers.is_binary &&
                        (scanner->current_headers.git_type == GIT_DIFF_NEW_FILE ||
                         scanner->current_headers.git_type == GIT_DIFF_DELETED_FILE ||
                         scanner->current_headers.git_type == GIT_DIFF_BINARY)) {
                        /* For binary patches, we need to emit both headers and binary content */
                        scanner->state = STATE_BINARY_READY;
                    }

                    scanner_emit_headers(scanner);
                    *content = &scanner->current_content;
                    return PATCH_SCAN_OK;
                }

                /* Continue accumulating */
                continue;
            } else {
                /* This line doesn't continue headers - accumulated lines weren't a patch */
                /* TODO: Emit accumulated lines as non-patch content */
                /* Reset and process current line */
                scanner_free_headers(scanner);
                scanner->state = STATE_SEEKING_PATCH;

                /* Process current line in SEEKING state */
                if (scanner_is_potential_patch_start(line)) {
                    scanner->state = STATE_ACCUMULATING_HEADERS;
                    scanner->num_header_lines = 0;
                    scanner->header_start_line = scanner->line_number;
                    scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);
                    continue;
                } else {
                    scanner_emit_non_patch(scanner, line, line_length);
                    *content = &scanner->current_content;
                    return PATCH_SCAN_OK;
                }
            }

        case STATE_IN_PATCH:
            if (!strncmp(line, "@@ ", sizeof("@@ ") - 1)) {
                /* Unified diff hunk header */
                scanner->state = STATE_IN_HUNK;
                scanner_emit_hunk_header(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "*** ", sizeof("*** ") - 1) && strstr(line, " ****")) {
                /* Context diff old hunk header: *** 1,3 **** */
                scanner->state = STATE_IN_HUNK;
                scanner_emit_context_hunk_header(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "***************", sizeof("***************") - 1)) {
                /* Context diff separator - skip it */
                continue;
            } else if (!strncmp(line, "Binary files ", sizeof("Binary files ") - 1) ||
                       !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1)) {
                /* Binary content */
                scanner_emit_binary(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (scanner_is_potential_patch_start(line)) {
                /* Start of next patch */
                scanner_reset_for_next_patch(scanner);
                scanner->state = STATE_ACCUMULATING_HEADERS;
                scanner->num_header_lines = 0;
                scanner->header_start_line = scanner->line_number;
                scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);
                continue;
            } else {
                /* Non-patch content between patches */
                scanner_emit_non_patch(scanner, line, line_length);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            }

        case STATE_IN_HUNK:

            if (line[0] == ' ' || line[0] == '+' || line[0] == '!' ||
                (line[0] == '-' && !(strncmp(line, "--- ", 4) == 0 && strstr(line, " ----")))) {
                /* Hunk line - but exclude context diff "--- N ----" headers */
                int result = scanner_emit_hunk_line(scanner, line);
                if (result != PATCH_SCAN_OK) {
                    scanner->state = STATE_ERROR;
                    return result;
                }

                /* Check if hunk is complete */
                if (scanner->hunk_orig_remaining == 0 && scanner->hunk_new_remaining == 0) {
                    /* For context diffs, make sure we've actually processed the new section */
                    /* If new_count is 0 but new_remaining was never set (still 0 from init), */
                    /* it means we haven't seen the "--- N ----" line yet */
                    if (scanner->current_headers.type == PATCH_TYPE_CONTEXT &&
                        scanner->current_hunk.new_count == 0 && scanner->hunk_new_remaining == 0) {
                        /* Context diff: old section complete, but new section not started yet */
                        /* Don't transition out of hunk state yet */
                    } else {
                        scanner->state = STATE_IN_PATCH;
                        scanner->in_hunk = 0;
                    }
                }

                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (line[0] == '\\') {
                /* No newline marker */
                scanner_emit_no_newline(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "@@ ", sizeof("@@ ") - 1)) {
                /* Next unified diff hunk */
                int result = scanner_emit_hunk_header(scanner, line);
                if (result != PATCH_SCAN_OK) {
                    scanner->state = STATE_ERROR;
                    return result;
                }
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "--- ", sizeof("--- ") - 1) && strstr(line, " ----")) {
                /* Context diff new hunk header: --- 1,3 ---- */
                int result = scanner_emit_context_new_hunk_header(scanner, line);
                if (result != PATCH_SCAN_OK) {
                    scanner->state = STATE_ERROR;
                    return result;
                }
                /* Continue to next line - this just updates hunk info */
                continue;
            } else if (!strncmp(line, "*** ", sizeof("*** ") - 1) && strstr(line, " ****")) {
                /* Next context diff hunk */
                int result = scanner_emit_context_hunk_header(scanner, line);
                if (result != PATCH_SCAN_OK) {
                    scanner->state = STATE_ERROR;
                    return result;
                }
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else {
                /* End of patch */
                scanner->state = STATE_SEEKING_PATCH;
                scanner->in_hunk = 0;

                /* Process current line in seeking state */
                if (scanner_is_potential_patch_start(line)) {
                    scanner->state = STATE_ACCUMULATING_HEADERS;
                    scanner->num_header_lines = 0;
                    scanner->header_start_line = scanner->line_number;
                    scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);
                    continue;
                } else {
                    scanner_emit_non_patch(scanner, line, line_length);
                    *content = &scanner->current_content;
                    return PATCH_SCAN_OK;
                }
            }

        case STATE_ERROR:
            return PATCH_SCAN_ERROR;

        default:
            scanner->state = STATE_ERROR;
            return PATCH_SCAN_ERROR;
    }

    /* Should never reach here due to loop structure */
    } /* end of for(;;) loop */
}

long patch_scanner_position(patch_scanner_t *scanner)
{
    if (!scanner) {
        return -1;
    }
    return scanner->current_position;
}

unsigned long patch_scanner_line_number(patch_scanner_t *scanner)
{
    if (!scanner) {
        return 0;
    }
    return scanner->line_number;
}

void patch_scanner_destroy(patch_scanner_t *scanner)
{
    if (!scanner) {
        return;
    }

    scanner_free_headers(scanner);

    if (scanner->header_lines) {
        free(scanner->header_lines);
    }

    if (scanner->line_buffer) {
        free(scanner->line_buffer);
    }

    /* Free simple peek-ahead buffer */
    if (scanner->next_line) {
        free(scanner->next_line);
    }

    /* Free any allocated strings in current content structures */
    if (scanner->current_headers.old_name) {
        free(scanner->current_headers.old_name);
    }
    if (scanner->current_headers.new_name) {
        free(scanner->current_headers.new_name);
    }
    if (scanner->current_headers.old_hash) {
        free(scanner->current_headers.old_hash);
    }
    if (scanner->current_headers.new_hash) {
        free(scanner->current_headers.new_hash);
    }
    if (scanner->current_hunk.context) {
        free(scanner->current_hunk.context);
    }

    free(scanner);
}

int patch_scanner_skip_current_patch(patch_scanner_t *scanner)
{
    const patch_content_t *content;
    int result;

    if (!scanner) {
        return PATCH_SCAN_ERROR;
    }

    /* Skip until we're no longer in a patch */
    while (scanner->state == STATE_IN_PATCH || scanner->state == STATE_IN_HUNK) {
        result = patch_scanner_next(scanner, &content);
        if (result != PATCH_SCAN_OK) {
            return result;
        }
    }

    return PATCH_SCAN_OK;
}

int patch_scanner_at_patch_start(patch_scanner_t *scanner)
{
    if (!scanner) {
        return 0;
    }

    return (scanner->state == STATE_ACCUMULATING_HEADERS ||
            scanner->state == STATE_IN_PATCH);
}

/* Internal helper functions */

static int scanner_read_line(patch_scanner_t *scanner)
{
    ssize_t result;

    /* Check if we have a buffered line from peek-ahead */
    if (scanner->has_next_line) {
        /* Use the buffered line */
        size_t len = strlen(scanner->next_line) + 1; /* +1 for null terminator */

        /* Ensure line_buffer is large enough */
        if (scanner->line_buffer_size < len) {
            scanner->line_buffer = xrealloc(scanner->line_buffer, len);
            scanner->line_buffer_size = len;
        }

        /* Copy buffered line to line_buffer */
        strcpy(scanner->line_buffer, scanner->next_line);

        /* Update line number */
        scanner->line_number = scanner->next_line_number;

        /* Clear the buffer */
        free(scanner->next_line);
        scanner->next_line = NULL;
        scanner->has_next_line = 0;

        /* Set current position (approximate) */
        scanner->current_position = ftell(scanner->file);

        return PATCH_SCAN_OK;
    }

    /* Normal line reading */
    scanner->current_position = ftell(scanner->file);
    result = getline(&scanner->line_buffer, &scanner->line_buffer_size, scanner->file);

    if (result == -1) {
        if (feof(scanner->file)) {
            return PATCH_SCAN_EOF;
        } else {
            return PATCH_SCAN_IO_ERROR;
        }
    }

    scanner->line_number++;
    return PATCH_SCAN_OK;
}

static int scanner_is_potential_patch_start(const char *line)
{
    /* Check for diff command */
    if (!strncmp(line, "diff ", sizeof("diff ") - 1)) {
        return 1;
    }

    /* Check for unified diff old file line */
    if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
        /* Exclude context diff hunk headers like "--- 1,3 ----" */
        if (strstr(line, " ----")) {
            return 0;
        }
        return 1;
    }

    /* Check for context diff old file line */
    if (!strncmp(line, "*** ", sizeof("*** ") - 1)) {
        /* Exclude context diff hunk headers like "*** 1,3 ****" */
        if (strstr(line, " ****")) {
            return 0;
        }
        return 1;
    }

    return 0;
}

static int scanner_is_header_continuation(patch_scanner_t *scanner, const char *line)
{
    /* Check if line is a valid patch header line */
    (void)scanner; /* unused parameter */
    return (!strncmp(line, "diff --git ", sizeof("diff --git ") - 1) ||
            !strncmp(line, "+++ ", sizeof("+++ ") - 1) ||
            !strncmp(line, "--- ", sizeof("--- ") - 1) ||
            !strncmp(line, "index ", sizeof("index ") - 1) ||
            !strncmp(line, "new file mode ", sizeof("new file mode ") - 1) ||
            !strncmp(line, "deleted file mode ", sizeof("deleted file mode ") - 1) ||
            !strncmp(line, "old mode ", sizeof("old mode ") - 1) ||
            !strncmp(line, "new mode ", sizeof("new mode ") - 1) ||
            !strncmp(line, "similarity index ", sizeof("similarity index ") - 1) ||
            !strncmp(line, "dissimilarity index ", sizeof("dissimilarity index ") - 1) ||
            !strncmp(line, "rename from ", sizeof("rename from ") - 1) ||
            !strncmp(line, "rename to ", sizeof("rename to ") - 1) ||
            !strncmp(line, "copy from ", sizeof("copy from ") - 1) ||
            !strncmp(line, "copy to ", sizeof("copy to ") - 1) ||
            strstr(line, "Binary files ") ||
            !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1));
}

static int scanner_validate_headers(patch_scanner_t *scanner)
{
    /* Validate header presence, order, and structure */
    unsigned int i;
    int has_old_file = 0;
    int has_new_file = 0;
    int has_git_diff = 0;
    int has_context_old = 0;
    int has_context_new = 0;
    (void)has_git_diff; /* used in validation logic */

    /* Reset header info */
    memset(&scanner->current_headers, 0, sizeof(scanner->current_headers));
    scanner->current_headers.type = PATCH_TYPE_UNIFIED;
    scanner->current_headers.git_type = GIT_DIFF_NORMAL;

    /* First pass: identify patch type and basic structure */
    for (i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];

        if (!strncmp(line, "diff --git ", sizeof("diff --git ") - 1)) {
            has_git_diff = 1;
            scanner->current_headers.type = PATCH_TYPE_GIT_EXTENDED;
        }
        else if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
            if (has_context_old) {
                /* This is the new file line in context diff */
                has_context_new = 1;
            } else {
                has_old_file = 1;
            }
        }
        else if (!strncmp(line, "+++ ", sizeof("+++ ") - 1)) {
            has_new_file = 1;
        }
        else if (!strncmp(line, "*** ", sizeof("*** ") - 1)) {
            has_context_old = 1;
            scanner->current_headers.type = PATCH_TYPE_CONTEXT;
        }
    }

    /* Validate header order based on patch type */
    if (scanner->current_headers.type == PATCH_TYPE_GIT_EXTENDED) {
        if (!scanner_validate_git_header_order(scanner)) {
            return 0;
        }
    } else if (scanner->current_headers.type == PATCH_TYPE_CONTEXT) {
        if (!scanner_validate_context_header_order(scanner)) {
            return 0;
        }
    } else {
        if (!scanner_validate_unified_header_order(scanner)) {
            return 0;
        }
    }

    /* Determine if we have a valid patch header structure */
    if (scanner->current_headers.type == PATCH_TYPE_CONTEXT) {
        return has_context_old && has_context_new;
    } else if (scanner->current_headers.type == PATCH_TYPE_GIT_EXTENDED) {
        /* Git extended headers are complete if:
         * 1. Git validation passed (already done above), AND
         * 2. Either no unified diff headers present, OR both --- and +++ are present
         */
        if (has_old_file || has_new_file) {
            /* If we have any unified diff headers, we need both */
            return has_old_file && has_new_file;
        } else {
            /* Pure Git metadata diff (no hunks) - complete */
            return 1;
        }
    } else {
        return has_old_file && has_new_file;
    }
}

static int scanner_parse_headers(patch_scanner_t *scanner)
{
    /* TODO: Implement proper header parsing */
    /* For now, just extract basic filenames */

    memset(&scanner->current_headers, 0, sizeof(scanner->current_headers));
    scanner->current_headers.type = PATCH_TYPE_UNIFIED;
    scanner->current_headers.git_type = GIT_DIFF_NORMAL;
    scanner->current_headers.old_mode = -1;
    scanner->current_headers.new_mode = -1;
    scanner->current_headers.similarity_index = -1;
    scanner->current_headers.dissimilarity_index = -1;
    scanner->current_headers.start_position = scanner->current_position;
    scanner->current_headers.start_line = scanner->header_start_line;

    /* Copy header lines */
    scanner->current_headers.header_lines = scanner->header_lines;
    scanner->current_headers.num_headers = scanner->num_header_lines;

    /* Parse specific header types */
    for (unsigned int i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];

        if (!strncmp(line, "diff --git ", sizeof("diff --git ") - 1)) {
            scanner->current_headers.type = PATCH_TYPE_GIT_EXTENDED;
            scanner_parse_git_diff_line(scanner, line);
        }
        else if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
            /* Check if this is a context diff by looking for a previous *** line */
            int is_context_diff = 0;
            for (unsigned int j = 0; j < scanner->num_header_lines; j++) {
                if (!strncmp(scanner->header_lines[j], "*** ", sizeof("*** ") - 1)) {
                    is_context_diff = 1;
                    break;
                }
            }

            if (is_context_diff) {
                /* In context diff, --- line is the new file */
                scanner_parse_new_file_line(scanner, line);
            } else {
                /* In unified diff, --- line is the old file */
                scanner_parse_old_file_line(scanner, line);
            }
        }
        else if (!strncmp(line, "+++ ", sizeof("+++ ") - 1)) {
            scanner_parse_new_file_line(scanner, line);
        }
        else if (!strncmp(line, "*** ", sizeof("*** ") - 1)) {
            scanner->current_headers.type = PATCH_TYPE_CONTEXT;
            /* Parse context diff old file line: *** filename */
            scanner->current_headers.old_name = scanner_extract_filename(line, sizeof("*** ") - 1);
        }
        else if (!strncmp(line, "index ", sizeof("index ") - 1)) {
            scanner_parse_index_line(scanner, line);
        }
        else if (!strncmp(line, "new file mode ", sizeof("new file mode ") - 1)) {
            scanner->current_headers.git_type = GIT_DIFF_NEW_FILE;
            scanner_parse_mode_line(scanner, line, &scanner->current_headers.new_mode);
        }
        else if (!strncmp(line, "deleted file mode ", sizeof("deleted file mode ") - 1)) {
            scanner->current_headers.git_type = GIT_DIFF_DELETED_FILE;
            scanner_parse_mode_line(scanner, line, &scanner->current_headers.old_mode);
        }
        else if (!strncmp(line, "old mode ", sizeof("old mode ") - 1)) {
            scanner_parse_mode_line(scanner, line, &scanner->current_headers.old_mode);
        }
        else if (!strncmp(line, "new mode ", sizeof("new mode ") - 1)) {
            scanner_parse_mode_line(scanner, line, &scanner->current_headers.new_mode);
        }
        else if (!strncmp(line, "similarity index ", sizeof("similarity index ") - 1)) {
            scanner_parse_similarity_line(scanner, line);
        }
        else if (!strncmp(line, "dissimilarity index ", sizeof("dissimilarity index ") - 1)) {
            scanner_parse_dissimilarity_line(scanner, line);
        }
        else if (!strncmp(line, "rename from ", sizeof("rename from ") - 1)) {
            scanner->current_headers.git_type = GIT_DIFF_RENAME;
            scanner_parse_filename_field(line, sizeof("rename from ") - 1, &scanner->current_headers.rename_from);
        }
        else if (!strncmp(line, "rename to ", sizeof("rename to ") - 1)) {
            scanner_parse_filename_field(line, sizeof("rename to ") - 1, &scanner->current_headers.rename_to);
        }
        else if (!strncmp(line, "copy from ", sizeof("copy from ") - 1)) {
            scanner->current_headers.git_type = GIT_DIFF_COPY;
            scanner_parse_filename_field(line, sizeof("copy from ") - 1, &scanner->current_headers.copy_from);
        }
        else if (!strncmp(line, "copy to ", sizeof("copy to ") - 1)) {
            scanner_parse_filename_field(line, sizeof("copy to ") - 1, &scanner->current_headers.copy_to);
        }
        else if (strstr(line, "Binary files ") || !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1)) {
            scanner->current_headers.is_binary = 1;
        }
    }

    /* Determine final git diff type based on parsed information */
    scanner_determine_git_diff_type(scanner);

    return PATCH_SCAN_OK;
}

/* Helper function to initialize common content fields */
static void scanner_init_content(patch_scanner_t *scanner, enum patch_content_type type)
{
    scanner->current_content.type = type;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
}

static int scanner_emit_non_patch(patch_scanner_t *scanner, const char *line, size_t length)
{
    scanner_init_content(scanner, PATCH_CONTENT_NON_PATCH);
    scanner->current_content.data.non_patch.line = line;
    scanner->current_content.data.non_patch.length = length;

    return PATCH_SCAN_OK;
}

static int scanner_emit_headers(patch_scanner_t *scanner)
{
    scanner_init_content(scanner, PATCH_CONTENT_HEADERS);
    scanner->current_content.position = scanner->current_headers.start_position; /* Override with header position */
    scanner->current_content.data.headers = &scanner->current_headers;

    return PATCH_SCAN_OK;
}

static int scanner_emit_hunk_header(patch_scanner_t *scanner, const char *line)
{
    char *endptr;
    unsigned long res;
    char *p;
    const char *context_start;

    /* Parse @@ -<orig_offset>[,<orig_count>] +<new_offset>[,<new_count>] @@[<context>] */

    /* Find original offset after '-' */
    p = strchr(line, '-');
    if (!p) {
        return PATCH_SCAN_ERROR;
    }
    p++;
    res = strtoul(p, &endptr, 10);
    if (p == endptr) {
        return PATCH_SCAN_ERROR;
    }
    scanner->current_hunk.orig_offset = res;

    /* Parse original count after ',' if present */
    if (*endptr == ',') {
        p = endptr + 1;
        res = strtoul(p, &endptr, 10);
        if (p == endptr) {
            return PATCH_SCAN_ERROR;
        }
        scanner->current_hunk.orig_count = res;
    } else {
        scanner->current_hunk.orig_count = 1;
    }

    /* Find new offset after '+' */
    p = strchr(endptr, '+');
    if (!p) {
        return PATCH_SCAN_ERROR;
    }
    p++;
    res = strtoul(p, &endptr, 10);
    if (p == endptr) {
        return PATCH_SCAN_ERROR;
    }
    scanner->current_hunk.new_offset = res;

    /* Parse new count after ',' if present */
    if (*endptr == ',') {
        p = endptr + 1;
        res = strtoul(p, &endptr, 10);
        if (p == endptr) {
            return PATCH_SCAN_ERROR;
        }
        scanner->current_hunk.new_count = res;
    } else {
        scanner->current_hunk.new_count = 1;
    }

    /* Find context after the closing @@ */
    context_start = strstr(endptr, "@@");
    if (context_start) {
        context_start += 2;
        if (*context_start == ' ') {
            context_start++;
        }
        if (*context_start != '\0' && *context_start != '\n') {
            /* Copy context, removing trailing newline if present */
            size_t context_len = strlen(context_start);
            if (context_len > 0 && context_start[context_len - 1] == '\n') {
                context_len--;
            }
            scanner->current_hunk.context = xstrndup(context_start, context_len);
        } else {
            scanner->current_hunk.context = NULL;
        }
    } else {
        scanner->current_hunk.context = NULL;
    }

    scanner->current_hunk.position = scanner->current_position;

    /* Initialize hunk line tracking */
    scanner->hunk_orig_remaining = scanner->current_hunk.orig_count;
    scanner->hunk_new_remaining = scanner->current_hunk.new_count;
    scanner->in_hunk = 1;

    scanner_init_content(scanner, PATCH_CONTENT_HUNK_HEADER);
    scanner->current_content.data.hunk = &scanner->current_hunk;

    return PATCH_SCAN_OK;
}

static int scanner_emit_context_hunk_header(patch_scanner_t *scanner, const char *line)
{
    char *endptr;
    unsigned long res;
    char *p;

    /* Parse *** <orig_offset>[,<orig_count>] **** */

    /* Find original offset after '*** ' */
    p = (char *)line + sizeof("*** ") - 1;

    /* Parse original offset */
    res = strtoul(p, &endptr, 10);
    if (endptr == p) {
        return PATCH_SCAN_ERROR;
    }
    scanner->current_hunk.orig_offset = res;

    /* Check for comma and count */
    if (*endptr == ',') {
        p = endptr + 1;
        res = strtoul(p, &endptr, 10);
        if (endptr == p) {
            return PATCH_SCAN_ERROR;
        }
        scanner->current_hunk.orig_count = res;
    } else {
        scanner->current_hunk.orig_count = 1;
    }

    /* For context diffs, we need to wait for the --- line to get new file info */
    scanner->current_hunk.new_offset = 0;
    scanner->current_hunk.new_count = 0;

    /* No context string in context diff hunk headers */
    scanner->current_hunk.context = NULL;
    scanner->current_hunk.position = scanner->current_position;

    /* Don't initialize hunk line tracking yet - wait for --- line */
    scanner->hunk_orig_remaining = scanner->current_hunk.orig_count;
    scanner->hunk_new_remaining = 0; /* Will be set when we see --- line */
    scanner->in_hunk = 1;

    scanner_init_content(scanner, PATCH_CONTENT_HUNK_HEADER);
    scanner->current_content.data.hunk = &scanner->current_hunk;

    return PATCH_SCAN_OK;
}

static int scanner_emit_context_new_hunk_header(patch_scanner_t *scanner, const char *line)
{
    char *endptr;
    unsigned long res;
    char *p;

    /* Parse --- <new_offset>[,<new_count>] ---- */

    /* Find new offset after '--- ' */
    p = (char *)line + sizeof("--- ") - 1;

    /* Parse new offset */
    res = strtoul(p, &endptr, 10);
    if (endptr == p) {
        return PATCH_SCAN_ERROR;
    }
    scanner->current_hunk.new_offset = res;

    /* Check for comma and count */
    if (*endptr == ',') {
        p = endptr + 1;
        res = strtoul(p, &endptr, 10);
        if (endptr == p) {
            return PATCH_SCAN_ERROR;
        }
        scanner->current_hunk.new_count = res;
    } else {
        scanner->current_hunk.new_count = 1;
    }

    /* Now we have complete hunk info, initialize line tracking */
    scanner->hunk_new_remaining = scanner->current_hunk.new_count;

    /* This is not a new hunk header emission, it completes the previous one */
    /* So we don't emit PATCH_CONTENT_HUNK_HEADER again, just continue processing lines */
    return PATCH_SCAN_OK;
}

static int scanner_emit_hunk_line(patch_scanner_t *scanner, const char *line)
{
    char line_type = line[0];

    /* Validate line type */
    if (line_type != ' ' && line_type != '+' && line_type != '-' && line_type != '!') {
        return PATCH_SCAN_ERROR;
    }

    /* Update remaining line counts based on line type */
    switch (line_type) {
    case ' ':
        /* Context line - counts against both original and new */
        if (scanner->hunk_orig_remaining > 0) {
            scanner->hunk_orig_remaining--;
        }
        if (scanner->hunk_new_remaining > 0) {
            scanner->hunk_new_remaining--;
        }
        break;
    case '-':
        /* Deletion - counts against original only */
        if (scanner->hunk_orig_remaining > 0) {
            scanner->hunk_orig_remaining--;
        }
        break;
    case '+':
        /* Addition - counts against new only */
        if (scanner->hunk_new_remaining > 0) {
            scanner->hunk_new_remaining--;
        }
        break;
    case '!':
        /* Changed line in context diff - counts against both */
        if (scanner->hunk_orig_remaining > 0) {
            scanner->hunk_orig_remaining--;
        }
        if (scanner->hunk_new_remaining > 0) {
            scanner->hunk_new_remaining--;
        }
        break;
    }

    scanner->current_line.type = (enum patch_hunk_line_type)line_type;
    scanner->current_line.content = line + 1;
    scanner->current_line.length = strlen(line) - 1;
    scanner->current_line.position = scanner->current_position;

    scanner_init_content(scanner, PATCH_CONTENT_HUNK_LINE);
    scanner->current_content.data.line = &scanner->current_line;

    return PATCH_SCAN_OK;
}

static int scanner_emit_no_newline(patch_scanner_t *scanner, const char *line)
{
    scanner_init_content(scanner, PATCH_CONTENT_NO_NEWLINE);
    scanner->current_content.data.no_newline.line = line;
    scanner->current_content.data.no_newline.length = strlen(line);

    return PATCH_SCAN_OK;
}

static int scanner_emit_binary(patch_scanner_t *scanner, const char *line)
{
    scanner_init_content(scanner, PATCH_CONTENT_BINARY);
    scanner->current_content.data.binary.line = line;
    scanner->current_content.data.binary.length = strlen(line);
    scanner->current_content.data.binary.is_git_binary = !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1);

    return PATCH_SCAN_OK;
}

/* Helper functions for common parsing patterns */
static char *scanner_extract_filename(const char *line, int prefix_len)
{
    /* Extract filename from header line, handling whitespace and timestamps */
    const char *filename = line + prefix_len;

    /* Skip whitespace */
    while (*filename == ' ' || *filename == '\t') filename++;

    /* Find end of filename (before timestamp if present) */
    const char *end = filename;

    /* Find timestamp using simple heuristics */
    const char *timestamp_pos = scanner_find_timestamp_start(filename);

    if (timestamp_pos) {
        end = timestamp_pos;
    } else {
        /* No timestamp found - look for tab separator */
        const char *tab_pos = strchr(filename, '\t');
        if (tab_pos) {
            end = tab_pos;
        } else {
            /* No timestamp or tab found - go to end of line */
            while (*end && *end != '\n' && *end != '\r') {
                end++;
            }
        }
    }

    /* Trim trailing whitespace from filename */
    while (end > filename && (*(end-1) == ' ' || *(end-1) == '\t')) {
        end--;
    }

    return xstrndup(filename, end - filename);
}

/* Helper function to find the start of a timestamp in a filename line
 * Returns pointer to the beginning of the timestamp, or NULL if not found
 *
 * This uses simple heuristics to detect common timestamp patterns:
 * - 4-digit years (19xx, 20xx)
 * - Month names (Jan, Feb, etc.)
 * - Day names (Mon, Tue, etc.) followed by comma or space
 * - Time patterns (HH:MM)
 */
static const char *scanner_find_timestamp_start(const char *filename)
{
    const char *pos = filename;
    const char *best_match = NULL;

    /* Common timestamp markers to look for */
    static const char *month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
    };
    static const char *day_names[] = {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", NULL
    };

    while (*pos) {
        /* Skip to next potential timestamp boundary (whitespace) */
        if (*pos != ' ' && *pos != '\t') {
            pos++;
            continue;
        }

        /* Found whitespace - check what follows */
        const char *after_space = pos;
        while (*after_space == ' ' || *after_space == '\t') after_space++;

        if (!*after_space) break;

        /* Check for 4-digit year */
        if ((after_space[0] == '1' && after_space[1] == '9') ||
            (after_space[0] == '2' && after_space[1] == '0')) {
            if (isdigit(after_space[2]) && isdigit(after_space[3])) {
                best_match = pos;
                break;
            }
        }

        /* Check for month names */
        for (int i = 0; month_names[i]; i++) {
            if (strncmp(after_space, month_names[i], 3) == 0 &&
                (after_space[3] == ' ' || after_space[3] == '\t')) {
                best_match = pos;
                break;
            }
        }
        if (best_match) break;

        /* Check for day names */
        for (int i = 0; day_names[i]; i++) {
            if (strncmp(after_space, day_names[i], 3) == 0 &&
                (after_space[3] == ',' || after_space[3] == ' ' || after_space[3] == '\t')) {
                best_match = pos;
                break;
            }
        }
        if (best_match) break;

        /* Check for time pattern (HH:MM) */
        if (isdigit(after_space[0]) && isdigit(after_space[1]) && after_space[2] == ':' &&
            isdigit(after_space[3]) && isdigit(after_space[4])) {
            best_match = pos;
            break;
        }

        pos++;
    }

    /* Trim leading whitespace from timestamp position */
    if (best_match) {
        while (best_match > filename &&
               (*(best_match-1) == ' ' || *(best_match-1) == '\t')) {
            best_match--;
        }
    }

    return best_match;
}

static void scanner_parse_index_percentage(const char *line, const char *prefix, int *target_field)
{
    /* Parse "prefix NN%" format safely */
    const char *percent = strchr(line, '%');
    int prefix_len = strlen(prefix);

    if (percent && strlen(line) > (size_t)prefix_len) {
        const char *start = line + prefix_len;
        /* Ensure we have a number before the % */
        if (start < percent) {
            *target_field = (int)strtol(start, NULL, 10);
        }
    }
}

static void scanner_parse_filename_field(const char *line, int prefix_len, char **target_field)
{
    /* Parse filename field and strip newlines */
    const char *filename = line + prefix_len;
    size_t len = strcspn(filename, "\n\r");
    *target_field = xstrndup(filename, len);
}

/* Helper functions for parsing specific header types */
static void scanner_parse_git_diff_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "diff --git a/old.txt b/new.txt" */
    const char *a_start = strstr(line, " a/");
    const char *b_start = strstr(line, " b/");

    if (a_start && b_start && a_start < b_start) {
        a_start += 1; /* Skip " " but keep "a/" */
        const char *a_end = strchr(a_start, ' ');
        if (a_end && a_end <= b_start) {
            scanner->current_headers.git_old_name = xstrndup(a_start, a_end - a_start);
        }

        b_start += 1; /* Skip " " but keep "b/" */
        size_t len = strcspn(b_start, "\n\r");
        scanner->current_headers.git_new_name = xstrndup(b_start, len);
    }
}

static void scanner_parse_old_file_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "--- filename" - extract filename, handle /dev/null */
    scanner->current_headers.old_name = scanner_extract_filename(line, sizeof("--- ") - 1);
}

static void scanner_parse_new_file_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "+++ filename" - extract filename, handle /dev/null */
    scanner->current_headers.new_name = scanner_extract_filename(line, sizeof("+++ ") - 1);
}

static void scanner_parse_index_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "index abc123..def456 100644" */
    const char *start = line + sizeof("index ") - 1;
    const char *dots = strstr(start, "..");
    if (dots) {
        scanner->current_headers.old_hash = xstrndup(start, dots - start);

        const char *new_start = dots + 2;
        const char *space = strchr(new_start, ' ');
        if (space) {
            scanner->current_headers.new_hash = xstrndup(new_start, space - new_start);
        } else {
            size_t len = strcspn(new_start, "\n\r");
            scanner->current_headers.new_hash = xstrndup(new_start, len);
        }
    }
}

static void scanner_parse_mode_line(patch_scanner_t *scanner, const char *line, int *mode_field)
{
    /* Parse mode from lines like "new file mode 100644" or "old mode 100755" */
    (void)scanner; /* unused parameter */
    const char *mode_str = strrchr(line, ' ');
    if (mode_str) {
        *mode_field = (int)strtol(mode_str + 1, NULL, 8); /* Octal mode */
    }
}

static void scanner_parse_similarity_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "similarity index 85%" */
    scanner_parse_index_percentage(line, "similarity index ", &scanner->current_headers.similarity_index);
}

static void scanner_parse_dissimilarity_line(patch_scanner_t *scanner, const char *line)
{
    /* Parse "dissimilarity index 98%" */
    scanner_parse_index_percentage(line, "dissimilarity index ", &scanner->current_headers.dissimilarity_index);
}

static void scanner_determine_git_diff_type(patch_scanner_t *scanner)
{
    /* Determine final git diff type based on parsed information */
    if (scanner->current_headers.similarity_index == 100 &&
        scanner->current_headers.rename_from && scanner->current_headers.rename_to) {
        scanner->current_headers.git_type = GIT_DIFF_PURE_RENAME;
    }
    else if (scanner->current_headers.rename_from && scanner->current_headers.rename_to) {
        scanner->current_headers.git_type = GIT_DIFF_RENAME;
    }
    else if (scanner->current_headers.copy_from && scanner->current_headers.copy_to) {
        scanner->current_headers.git_type = GIT_DIFF_COPY;
    }
    else if (scanner->current_headers.old_mode != -1 && scanner->current_headers.new_mode != -1 &&
             scanner->current_headers.old_mode != scanner->current_headers.new_mode) {
        scanner->current_headers.git_type = GIT_DIFF_MODE_CHANGE;
    }
    else if (scanner->current_headers.is_binary &&
             scanner->current_headers.git_type != GIT_DIFF_NEW_FILE &&
             scanner->current_headers.git_type != GIT_DIFF_DELETED_FILE) {
        /* Only set as binary if it's not already a new file or deleted file */
        scanner->current_headers.git_type = GIT_DIFF_BINARY;
    }
    /* GIT_DIFF_NEW_FILE and GIT_DIFF_DELETED_FILE are set during parsing and take precedence */
}

/* Header order validation functions */
static int scanner_validate_unified_header_order(patch_scanner_t *scanner)
{
    /* Unified diff order: [diff command], ---, +++ */
    unsigned int i;
    int seen_old_file = 0;
    int seen_new_file = 0;

    for (i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];

        if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
            if (seen_new_file) {
                /* --- after +++ is invalid */
                return 0;
            }
            seen_old_file = 1;
        }
        else if (!strncmp(line, "+++ ", sizeof("+++ ") - 1)) {
            if (!seen_old_file) {
                /* +++ without preceding --- is invalid */
                return 0;
            }
            seen_new_file = 1;
        }
    }

    return seen_old_file && seen_new_file;
}

static int scanner_validate_context_header_order(patch_scanner_t *scanner)
{
    /* Context diff order: [diff command], ***, --- */
    unsigned int i;
    int seen_context_old = 0;
    int seen_context_new = 0;

    for (i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];

        if (!strncmp(line, "*** ", sizeof("*** ") - 1)) {
            if (seen_context_new) {
                /* *** after --- is invalid in context diff */
                return 0;
            }
            seen_context_old = 1;
        }
        else if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
            if (!seen_context_old) {
                /* --- without preceding *** is invalid in context diff */
                return 0;
            }
            seen_context_new = 1;
        }
    }

    return seen_context_old && seen_context_new;
}

static int scanner_validate_git_header_order(patch_scanner_t *scanner)
{
    /* Git diff order:
     * 1. diff --git a/old b/new
     * 2. Git extended headers (mode, similarity, rename/copy, index)
     * 3. --- a/old (or /dev/null)
     * 4. +++ b/new (or /dev/null)
     */
    unsigned int i;
    int seen_git_diff = 0;
    int seen_old_file = 0;
    int seen_new_file = 0;
    int in_extended_headers = 0;

    for (i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];

        if (!strncmp(line, "diff --git ", sizeof("diff --git ") - 1)) {
            if (seen_git_diff || seen_old_file || seen_new_file) {
                /* Multiple diff --git lines or diff --git after file lines */
                return 0;
            }
            seen_git_diff = 1;
            in_extended_headers = 1;
        }
        else if (!strncmp(line, "--- ", sizeof("--- ") - 1)) {
            if (!seen_git_diff) {
                /* --- without preceding diff --git */
                return 0;
            }
            if (seen_new_file) {
                /* --- after +++ is invalid */
                return 0;
            }
            seen_old_file = 1;
            in_extended_headers = 0;
        }
        else if (!strncmp(line, "+++ ", sizeof("+++ ") - 1)) {
            if (!seen_old_file) {
                /* +++ without preceding --- */
                return 0;
            }
            seen_new_file = 1;
        }
        else if (in_extended_headers) {
            /* Validate that this is a recognized Git extended header */
            if (!scanner_is_git_extended_header(line)) {
                /* Unknown header in extended section */
                return 0;
            }
        }
        else if (seen_new_file) {
            /* No headers should appear after +++ */
            return 0;
        }
    }

    /* Check if this is a binary patch that doesn't need --- and +++ lines */
    int has_binary_marker = 0;
    for (i = 0; i < scanner->num_header_lines; i++) {
        const char *line = scanner->header_lines[i];
        if (strstr(line, "Binary files ") || !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1)) {
            has_binary_marker = 1;
            break;
        }
    }

    if (has_binary_marker) {
        /* Binary patches only require diff --git line and binary marker */
        return seen_git_diff;
    }

    /* Check if this is a Git diff without hunks (e.g., new file, deleted file, mode change) */
    if (seen_git_diff && !seen_old_file && !seen_new_file) {
        /* Git diff with no --- and +++ lines - use look-ahead to determine if complete */
        int has_new_file = 0, has_deleted_file = 0, has_mode_change = 0, has_index = 0;

        for (i = 0; i < scanner->num_header_lines; i++) {
            const char *line = scanner->header_lines[i];
            if (!strncmp(line, "new file mode ", sizeof("new file mode ") - 1)) {
                has_new_file = 1;
            } else if (!strncmp(line, "deleted file mode ", sizeof("deleted file mode ") - 1)) {
                has_deleted_file = 1;
            } else if (!strncmp(line, "old mode ", sizeof("old mode ") - 1) ||
                       !strncmp(line, "new mode ", sizeof("new mode ") - 1)) {
                has_mode_change = 1;
            } else if (!strncmp(line, "index ", sizeof("index ") - 1)) {
                has_index = 1;
            }
        }

        /* For pure mode changes, complete immediately */
        if (has_mode_change && has_index) {
            return 1;
        }

        /* For new/deleted files, use look-ahead to check if --- and +++ lines are coming */
        if ((has_new_file || has_deleted_file) && has_index) {
            /* First check if we already have a binary marker in current headers */
            int has_current_binary = 0;
            for (i = 0; i < scanner->num_header_lines; i++) {
                const char *line = scanner->header_lines[i];
                if (strstr(line, "Binary files ")) {
                    has_current_binary = 1;
                    break;
                }
            }

            /* If we already have binary content, complete immediately */
            if (has_current_binary) {
                return 1;
            }
            /* For new/deleted files with index, check if unified diff headers are coming */
            return scanner_should_wait_for_unified_headers(scanner);
        }
    }

    /* Regular patches (including Git diffs with --- and +++ lines) need all three lines */
    return seen_git_diff && seen_old_file && seen_new_file;
}

static int scanner_is_git_extended_header(const char *line)
{
    /* Check if line is a valid Git extended header */
    return (!strncmp(line, "old mode ", sizeof("old mode ") - 1) ||
            !strncmp(line, "new mode ", sizeof("new mode ") - 1) ||
            !strncmp(line, "deleted file mode ", sizeof("deleted file mode ") - 1) ||
            !strncmp(line, "new file mode ", sizeof("new file mode ") - 1) ||
            !strncmp(line, "similarity index ", sizeof("similarity index ") - 1) ||
            !strncmp(line, "dissimilarity index ", sizeof("dissimilarity index ") - 1) ||
            !strncmp(line, "rename from ", sizeof("rename from ") - 1) ||
            !strncmp(line, "rename to ", sizeof("rename to ") - 1) ||
            !strncmp(line, "copy from ", sizeof("copy from ") - 1) ||
            !strncmp(line, "copy to ", sizeof("copy to ") - 1) ||
            !strncmp(line, "index ", sizeof("index ") - 1) ||
            strstr(line, "Binary files ") ||
            !strncmp(line, "GIT binary patch", sizeof("GIT binary patch") - 1));
}

static void scanner_free_headers(patch_scanner_t *scanner)
{
    if (scanner->header_lines) {
        for (unsigned int i = 0; i < scanner->num_header_lines; i++) {
            if (scanner->header_lines[i]) {
                free(scanner->header_lines[i]);
                scanner->header_lines[i] = NULL;
            }
        }
    }
    scanner->num_header_lines = 0;
}

static void scanner_reset_for_next_patch(patch_scanner_t *scanner)
{
    /* Free previous patch data */
    if (scanner->current_headers.old_name) {
        free(scanner->current_headers.old_name);
        scanner->current_headers.old_name = NULL;
    }
    if (scanner->current_headers.new_name) {
        free(scanner->current_headers.new_name);
        scanner->current_headers.new_name = NULL;
    }
    if (scanner->current_headers.old_hash) {
        free(scanner->current_headers.old_hash);
        scanner->current_headers.old_hash = NULL;
    }
    if (scanner->current_headers.new_hash) {
        free(scanner->current_headers.new_hash);
        scanner->current_headers.new_hash = NULL;
    }
    if (scanner->current_hunk.context) {
        free(scanner->current_hunk.context);
        scanner->current_hunk.context = NULL;
    }

    scanner_free_headers(scanner);
    scanner->in_hunk = 0;
}

/* Look-ahead implementation */

/* Stdin-compatible peek-ahead for Git header completion */

static int scanner_should_wait_for_unified_headers(patch_scanner_t *scanner)
{
    /* If we already have a buffered line, use it */
    if (scanner->has_next_line) {
        const char *next_line = scanner->next_line;

        /* Check if the next line is a unified diff header */
        if (!strncmp(next_line, "--- ", 4) || !strncmp(next_line, "+++ ", 4)) {
            return 0; /* Don't complete yet - wait for unified headers */
        } else if (strstr(next_line, "Binary files ")) {
            return 0; /* Don't complete yet - wait for binary content */
        } else {
            return 1; /* Complete as Git metadata-only */
        }
    }

    /* Read the next line and buffer it */
    char *line = NULL;
    size_t len = 0;
    ssize_t read = getline(&line, &len, scanner->file);

    if (read == -1) {
        /* EOF - complete as metadata-only */
        free(line);
        return 1;
    }

    /* Remove trailing newline */
    if (read > 0 && line[read - 1] == '\n') {
        line[read - 1] = '\0';
    }

    /* Store in buffer for later consumption */
    scanner->next_line = line;
    scanner->next_line_number = scanner->line_number + 1;
    scanner->has_next_line = 1;

    /* Check what type of line this is */
    if (!strncmp(line, "--- ", 4) || !strncmp(line, "+++ ", 4)) {
        return 0; /* Don't complete yet - wait for unified headers */
    } else if (strstr(line, "Binary files ")) {
        return 0; /* Don't complete yet - wait for binary content */
    } else {
        return 1; /* Complete as Git metadata-only */
    }
}
