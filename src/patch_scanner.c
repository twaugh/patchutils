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

#include "patch_scanner.h"
#include "util.h"

/* Scanner internal state */
enum scanner_state {
    STATE_SEEKING_PATCH,         /* Looking for start of patch */
    STATE_ACCUMULATING_HEADERS,  /* Collecting potential headers */
    STATE_IN_PATCH,             /* Processing patch content */
    STATE_IN_HUNK,              /* Processing hunk lines */
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

    /* Current content being emitted */
    struct patch_content current_content; /* Content structure for emission */
    struct patch_headers current_headers; /* Current patch headers */
    struct patch_hunk current_hunk;       /* Current hunk */
    struct patch_hunk_line current_line;  /* Current hunk line */

    /* Hunk processing state */
    unsigned long hunk_orig_remaining; /* Remaining original lines in hunk */
    unsigned long hunk_new_remaining;  /* Remaining new lines in hunk */
    int in_hunk;                       /* Are we currently in a hunk? */
};

/* Forward declarations */
static int scanner_read_line(patch_scanner_t *scanner);
static int scanner_is_potential_patch_start(const char *line);
static int scanner_is_header_continuation(patch_scanner_t *scanner, const char *line);
static int scanner_validate_headers(patch_scanner_t *scanner);
static int scanner_parse_headers(patch_scanner_t *scanner);
static int scanner_emit_non_patch(patch_scanner_t *scanner, const char *line, size_t length);
static int scanner_emit_headers(patch_scanner_t *scanner);
static int scanner_emit_hunk_header(patch_scanner_t *scanner, const char *line);
static int scanner_emit_hunk_line(patch_scanner_t *scanner, const char *line);
static int scanner_emit_no_newline(patch_scanner_t *scanner, const char *line);
static int scanner_emit_binary(patch_scanner_t *scanner, const char *line);
static void scanner_free_headers(patch_scanner_t *scanner);
static void scanner_reset_for_next_patch(patch_scanner_t *scanner);

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
                    scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);
                    continue;
                } else {
                    scanner_emit_non_patch(scanner, line, line_length);
                    *content = &scanner->current_content;
                    return PATCH_SCAN_OK;
                }
            }

        case STATE_IN_PATCH:
            if (!strncmp(line, "@@ ", 3)) {
                /* Hunk header */
                scanner->state = STATE_IN_HUNK;
                scanner_emit_hunk_header(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "Binary files ", 13) ||
                       !strncmp(line, "GIT binary patch", 16)) {
                /* Binary content */
                scanner_emit_binary(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (scanner_is_potential_patch_start(line)) {
                /* Start of next patch */
                scanner_reset_for_next_patch(scanner);
                scanner->state = STATE_ACCUMULATING_HEADERS;
                scanner->num_header_lines = 0;
                scanner->header_lines[scanner->num_header_lines++] = xstrdup(line);
                continue;
            } else {
                /* Non-patch content between patches */
                scanner_emit_non_patch(scanner, line, line_length);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            }

        case STATE_IN_HUNK:
            if (line[0] == ' ' || line[0] == '+' || line[0] == '-') {
                /* Hunk line */
                scanner_emit_hunk_line(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (line[0] == '\\') {
                /* No newline marker */
                scanner_emit_no_newline(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else if (!strncmp(line, "@@ ", 3)) {
                /* Next hunk */
                scanner_emit_hunk_header(scanner, line);
                *content = &scanner->current_content;
                return PATCH_SCAN_OK;
            } else {
                /* End of patch */
                scanner->state = STATE_SEEKING_PATCH;

                /* Process current line in seeking state */
                if (scanner_is_potential_patch_start(line)) {
                    scanner->state = STATE_ACCUMULATING_HEADERS;
                    scanner->num_header_lines = 0;
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
    return (!strncmp(line, "diff ", 5) ||
            !strncmp(line, "--- ", 4) ||
            !strncmp(line, "*** ", 4));
}

static int scanner_is_header_continuation(patch_scanner_t *scanner, const char *line)
{
    /* TODO: Implement proper header continuation logic */
    /* For now, simple heuristics */
    (void)scanner; /* unused parameter */
    return (!strncmp(line, "+++ ", 4) ||
            !strncmp(line, "--- ", 4) ||
            !strncmp(line, "index ", 6) ||
            !strncmp(line, "new file mode ", 14) ||
            !strncmp(line, "deleted file mode ", 18) ||
            !strncmp(line, "old mode ", 9) ||
            !strncmp(line, "new mode ", 9) ||
            !strncmp(line, "similarity index ", 17) ||
            !strncmp(line, "dissimilarity index ", 20) ||
            !strncmp(line, "rename from ", 12) ||
            !strncmp(line, "rename to ", 10) ||
            !strncmp(line, "copy from ", 10) ||
            !strncmp(line, "copy to ", 8));
}

static int scanner_validate_headers(patch_scanner_t *scanner)
{
    /* TODO: Implement proper header validation */
    /* For now, just check if we have old and new file lines */
    int has_old = 0, has_new = 0;

    for (unsigned int i = 0; i < scanner->num_header_lines; i++) {
        if (!strncmp(scanner->header_lines[i], "--- ", 4)) {
            has_old = 1;
        } else if (!strncmp(scanner->header_lines[i], "+++ ", 4)) {
            has_new = 1;
        }
    }

    return has_old && has_new;
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
    scanner->current_headers.start_position = scanner->current_position;

    /* Copy header lines */
    scanner->current_headers.header_lines = scanner->header_lines;
    scanner->current_headers.num_headers = scanner->num_header_lines;

    /* Extract filenames - simplified for now */
    for (unsigned int i = 0; i < scanner->num_header_lines; i++) {
        if (!strncmp(scanner->header_lines[i], "--- ", 4)) {
            /* TODO: Proper filename parsing */
            scanner->current_headers.old_name = xstrdup("old_file");
        } else if (!strncmp(scanner->header_lines[i], "+++ ", 4)) {
            /* TODO: Proper filename parsing */
            scanner->current_headers.new_name = xstrdup("new_file");
        }
    }

    return PATCH_SCAN_OK;
}

static int scanner_emit_non_patch(patch_scanner_t *scanner, const char *line, size_t length)
{
    scanner->current_content.type = PATCH_CONTENT_NON_PATCH;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
    scanner->current_content.data.non_patch.line = line;
    scanner->current_content.data.non_patch.length = length;

    return PATCH_SCAN_OK;
}

static int scanner_emit_headers(patch_scanner_t *scanner)
{
    scanner->current_content.type = PATCH_CONTENT_HEADERS;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_headers.start_position;
    scanner->current_content.data.headers = &scanner->current_headers;

    return PATCH_SCAN_OK;
}

static int scanner_emit_hunk_header(patch_scanner_t *scanner, const char *line)
{
    /* TODO: Parse hunk header properly */
    (void)line; /* unused parameter - TODO: parse actual hunk header */
    scanner->current_hunk.orig_offset = 1;
    scanner->current_hunk.orig_count = 1;
    scanner->current_hunk.new_offset = 1;
    scanner->current_hunk.new_count = 1;
    scanner->current_hunk.context = NULL;
    scanner->current_hunk.position = scanner->current_position;

    scanner->current_content.type = PATCH_CONTENT_HUNK_HEADER;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
    scanner->current_content.data.hunk = &scanner->current_hunk;

    return PATCH_SCAN_OK;
}

static int scanner_emit_hunk_line(patch_scanner_t *scanner, const char *line)
{
    scanner->current_line.type = (enum patch_hunk_line_type)line[0];
    scanner->current_line.content = line + 1;
    scanner->current_line.length = strlen(line) - 1;
    scanner->current_line.position = scanner->current_position;

    scanner->current_content.type = PATCH_CONTENT_HUNK_LINE;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
    scanner->current_content.data.line = &scanner->current_line;

    return PATCH_SCAN_OK;
}

static int scanner_emit_no_newline(patch_scanner_t *scanner, const char *line)
{
    scanner->current_content.type = PATCH_CONTENT_NO_NEWLINE;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
    scanner->current_content.data.no_newline.line = line;
    scanner->current_content.data.no_newline.length = strlen(line);

    return PATCH_SCAN_OK;
}

static int scanner_emit_binary(patch_scanner_t *scanner, const char *line)
{
    scanner->current_content.type = PATCH_CONTENT_BINARY;
    scanner->current_content.line_number = scanner->line_number;
    scanner->current_content.position = scanner->current_position;
    scanner->current_content.data.binary.line = line;
    scanner->current_content.data.binary.length = strlen(line);
    scanner->current_content.data.binary.is_git_binary = !strncmp(line, "GIT binary patch", 16);

    return PATCH_SCAN_OK;
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
