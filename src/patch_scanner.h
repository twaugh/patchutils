/*
 * patch_scanner.h - unified patch parsing API
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

#ifndef PATCH_SCANNER_H
#define PATCH_SCANNER_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct patch_scanner patch_scanner_t;
typedef struct patch_content patch_content_t;
typedef struct patch_headers patch_headers_t;
typedef struct patch_hunk patch_hunk_t;
typedef struct patch_hunk_line patch_hunk_line_t;

/* Scanner result codes */
enum patch_scanner_result {
    PATCH_SCAN_OK = 0,           /* Content available */
    PATCH_SCAN_EOF = 1,          /* End of input reached */
    PATCH_SCAN_ERROR = -1,       /* Generic error */
    PATCH_SCAN_MEMORY_ERROR = -2, /* Memory allocation failed */
    PATCH_SCAN_IO_ERROR = -3     /* I/O error reading input */
};

/* Content types emitted by scanner */
enum patch_content_type {
    PATCH_CONTENT_NON_PATCH = 0, /* Comments, unrecognized lines */
    PATCH_CONTENT_HEADERS,       /* Complete validated patch headers */
    PATCH_CONTENT_HUNK_HEADER,   /* @@ lines */
    PATCH_CONTENT_HUNK_LINE,     /* +/- lines */
    PATCH_CONTENT_NO_NEWLINE,    /* \ No newline at end of file */
    PATCH_CONTENT_BINARY         /* Binary files differ / GIT binary patch */
};

/* Patch format types */
enum patch_type {
    PATCH_TYPE_UNIFIED = 0,      /* Unified diff format */
    PATCH_TYPE_CONTEXT,          /* Context diff format */
    PATCH_TYPE_GIT_EXTENDED      /* Git extended diff format */
};

/* Git-specific diff types */
enum git_diff_type {
    GIT_DIFF_NORMAL = 0,         /* Regular diff with hunks */
    GIT_DIFF_NEW_FILE,           /* New file creation */
    GIT_DIFF_DELETED_FILE,       /* File deletion */
    GIT_DIFF_RENAME,             /* File rename */
    GIT_DIFF_PURE_RENAME,        /* Pure rename (100% similarity) */
    GIT_DIFF_COPY,               /* File copy */
    GIT_DIFF_MODE_ONLY,          /* Mode change only */
    GIT_DIFF_MODE_CHANGE,        /* Mode change with content changes */
    GIT_DIFF_BINARY              /* Binary file diff */
};

/* Hunk line types */
enum patch_hunk_line_type {
    PATCH_LINE_CONTEXT = ' ',    /* Context line */
    PATCH_LINE_ADDED = '+',      /* Added line */
    PATCH_LINE_REMOVED = '-',    /* Removed line */
    PATCH_LINE_NO_NEWLINE = '\\' /* No newline marker */
};

/* Complete patch headers information */
struct patch_headers {
    enum patch_type type;        /* Format type */
    enum git_diff_type git_type; /* Git-specific type */

    /* Raw header lines */
    char **header_lines;         /* All header lines in order */
    unsigned int num_headers;    /* Number of header lines */

    /* Parsed file information */
    char *old_name;              /* Old filename (best name after Git processing) */
    char *new_name;              /* New filename (best name after Git processing) */

    /* Git-specific information (valid when type == PATCH_TYPE_GIT_EXTENDED) */
    char *git_old_name;          /* Original filename from diff --git line */
    char *git_new_name;          /* New filename from diff --git line */
    int old_mode;                /* Old file mode (-1 if not specified) */
    int new_mode;                /* New file mode (-1 if not specified) */
    char *old_hash;              /* Old file hash (NULL if not specified) */
    char *new_hash;              /* New file hash (NULL if not specified) */
    int similarity_index;        /* Similarity index for renames/copies (-1 if not specified) */
    int dissimilarity_index;     /* Dissimilarity index (-1 if not specified) */
    char *rename_from;           /* Source filename for renames */
    char *rename_to;             /* Target filename for renames */
    char *copy_from;             /* Source filename for copies */
    char *copy_to;               /* Target filename for copies */
    int is_binary;               /* 1 if binary patch, 0 otherwise */

    /* Position information */
    long start_position;         /* File position where this patch starts */
};

/* Hunk header information */
struct patch_hunk {
    unsigned long orig_offset;   /* Original file line offset */
    unsigned long orig_count;    /* Number of lines in original file */
    unsigned long new_offset;    /* New file line offset */
    unsigned long new_count;     /* Number of lines in new file */
    char *context;               /* Optional context string from @@ line */
    long position;               /* File position of this hunk header */
};

/* Individual hunk line */
struct patch_hunk_line {
    enum patch_hunk_line_type type; /* Line type */
    const char *content;         /* Line content (without prefix) */
    size_t length;               /* Content length */
    long position;               /* File position of this line */
};

/* Content structure passed to consumers */
struct patch_content {
    enum patch_content_type type; /* Content type */
    unsigned long line_number;   /* Line number in input */
    long position;               /* File position of this content */

    union {
        struct {                 /* For PATCH_CONTENT_NON_PATCH */
            const char *line;    /* Raw line content */
            size_t length;       /* Line length */
        } non_patch;

        const struct patch_headers *headers; /* For PATCH_CONTENT_HEADERS */
        const struct patch_hunk *hunk;       /* For PATCH_CONTENT_HUNK_HEADER */
        const struct patch_hunk_line *line;  /* For PATCH_CONTENT_HUNK_LINE */

        struct {                 /* For PATCH_CONTENT_NO_NEWLINE */
            const char *line;    /* Raw line content */
            size_t length;       /* Line length */
        } no_newline;

        struct {                 /* For PATCH_CONTENT_BINARY */
            const char *line;    /* Raw line content */
            size_t length;       /* Line length */
            int is_git_binary;   /* 1 if GIT binary patch, 0 if "Binary files differ" */
        } binary;
    } data;
};

/* Core scanner API */

/**
 * Create a new patch scanner for the given input stream.
 *
 * @param file Input stream to read from (must remain valid for scanner lifetime)
 * @return New scanner instance, or NULL on error
 */
patch_scanner_t* patch_scanner_create(FILE *file);

/**
 * Get the next piece of content from the scanner.
 *
 * @param scanner Scanner instance
 * @param content Output parameter for content (valid until next call or scanner destruction)
 * @return PATCH_SCAN_OK if content available, PATCH_SCAN_EOF if done, or error code
 */
int patch_scanner_next(patch_scanner_t *scanner, const patch_content_t **content);

/**
 * Get the current file position of the scanner.
 *
 * @param scanner Scanner instance
 * @return Current file position, or -1 on error
 */
long patch_scanner_position(patch_scanner_t *scanner);

/**
 * Get the current line number being processed.
 *
 * @param scanner Scanner instance
 * @return Current line number (1-based), or 0 on error
 */
unsigned long patch_scanner_line_number(patch_scanner_t *scanner);

/**
 * Destroy a patch scanner and free all associated resources.
 *
 * @param scanner Scanner instance (may be NULL)
 */
void patch_scanner_destroy(patch_scanner_t *scanner);

/* Convenience functions */

/**
 * Skip all content for the current patch (if we're in the middle of one).
 * Useful for indexing scenarios where you just want patch locations.
 *
 * @param scanner Scanner instance
 * @return PATCH_SCAN_OK on success, error code on failure
 */
int patch_scanner_skip_current_patch(patch_scanner_t *scanner);

/**
 * Check if the scanner is currently positioned at the start of a new patch.
 *
 * @param scanner Scanner instance
 * @return 1 if at patch start, 0 otherwise
 */
int patch_scanner_at_patch_start(patch_scanner_t *scanner);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_SCANNER_H */
