/*
 * patch_scanner.h - patch parsing API
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
#include "diff.h"

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

/**
 * Content types emitted by scanner in sequential order for each patch.
 *
 * TYPICAL PATCH CONTENT SEQUENCE:
 * 1. PATCH_CONTENT_NON_PATCH (optional, for comments/junk before patch)
 * 2. PATCH_CONTENT_HEADERS (once per patch, contains complete validated headers)
 * 3. For each hunk in the patch:
 *    a. PATCH_CONTENT_HUNK_HEADER (hunk @@ line or context diff ranges)
 *    b. PATCH_CONTENT_HUNK_LINE (multiple, for each +/- line in hunk)
 *    c. PATCH_CONTENT_NO_NEWLINE (optional, if "\ No newline" follows)
 * 4. PATCH_CONTENT_BINARY (instead of hunks, for binary patches)
 * 5. PATCH_CONTENT_NON_PATCH (optional, for content between patches)
 *
 * MEMORY MANAGEMENT:
 * - All content pointers are valid until next patch_scanner_next() call
 * - Scanner owns all memory - consumers should copy data if needed beyond next call
 * - Content lifetime ends when scanner is destroyed
 */
enum patch_content_type {
    PATCH_CONTENT_NON_PATCH = 0, /* Comments, unrecognized lines, content between patches */
    PATCH_CONTENT_HEADERS,       /* Complete validated patch headers (filenames, modes, etc.) */
    PATCH_CONTENT_HUNK_HEADER,   /* Hunk start: @@ lines or context diff *** N,M **** / --- N,M ---- */
    PATCH_CONTENT_HUNK_LINE,     /* Individual patch lines: ' ' (context), '+' (add), '-' (remove), '!' (change) */
    PATCH_CONTENT_NO_NEWLINE,    /* "\ No newline at end of file" marker following hunk lines */
    PATCH_CONTENT_BINARY         /* "Binary files differ" or "GIT binary patch" content */
};

/* Patch format types */
enum patch_type {
    PATCH_TYPE_UNIFIED = 0,      /* Unified diff format */
    PATCH_TYPE_CONTEXT,          /* Context diff format */
    PATCH_TYPE_GIT_EXTENDED      /* Git extended diff format */
};
/* Hunk line types */
enum patch_hunk_line_type {
    PATCH_LINE_CONTEXT = ' ',    /* Context line */
    PATCH_LINE_ADDED = '+',      /* Added line */
    PATCH_LINE_REMOVED = '-',    /* Removed line */
    PATCH_LINE_CHANGED = '!',    /* Changed line (context diff) */
    PATCH_LINE_NO_NEWLINE = '\\' /* No newline marker */
};

/* Context for patch lines (especially important for context diff changed lines) */
enum patch_line_context {
    PATCH_CONTEXT_BOTH = 0,      /* Normal lines (space, +, -, \) - applies to both old and new */
    PATCH_CONTEXT_OLD,           /* This represents the "old" version of a changed line (!) */
    PATCH_CONTEXT_NEW            /* This represents the "new" version of a changed line (!) */
};

/**
 * Complete patch headers information.
 *
 * FIELD POPULATION BY PATCH TYPE:
 *
 * UNIFIED DIFFS (diff -u):
 *   - type = PATCH_TYPE_UNIFIED
 *   - old_name, new_name: from "--- file" and "+++ file" lines
 *   - Git fields: all NULL/-1 (not applicable)
 *
 * CONTEXT DIFFS (diff -c):
 *   - type = PATCH_TYPE_CONTEXT
 *   - old_name, new_name: from "*** file" and "--- file" lines
 *   - Git fields: all NULL/-1 (not applicable)
 *
 * GIT EXTENDED DIFFS:
 *   - type = PATCH_TYPE_GIT_EXTENDED
 *   - old_name, new_name: best names after Git processing (prefer --- +++ over git names)
 *   - git_old_name, git_new_name: raw names from "diff --git a/old b/new" line
 *   - Git fields: populated based on presence of corresponding header lines
 *
 * FILENAME RESOLUTION PRIORITY (for old_name/new_name):
 *   1. "--- filename" / "+++ filename" lines (if present)
 *   2. Git rename_to/copy_to (for new_name)
 *   3. Git rename_from/copy_from (for old_name)
 *   4. git_old_name/git_new_name (fallback)
 *   5. "/dev/null" for new/deleted files
 */
struct patch_headers {
    enum patch_type type;        /* Patch format: unified, context, or Git extended */
    enum git_diff_type git_type; /* Git operation type (normal, new, delete, rename, etc.) */

    /* Raw header lines (for tools that need original text) */
    char **header_lines;         /* All header lines in order as they appeared */
    unsigned int num_headers;    /* Number of header lines */

    /* Primary file information (always populated, best available names) */
    char *old_name;              /* Old filename - resolved using priority rules above */
    char *new_name;              /* New filename - resolved using priority rules above */

    /* Git-specific information (only valid when type == PATCH_TYPE_GIT_EXTENDED) */
    char *git_old_name;          /* Raw "a/filename" from diff --git line (NULL if not Git) */
    char *git_new_name;          /* Raw "b/filename" from diff --git line (NULL if not Git) */
    int old_mode;                /* Old file mode in octal (-1 if not specified) */
    int new_mode;                /* New file mode in octal (-1 if not specified) */
    char *old_hash;              /* Old file SHA hash from index line (NULL if not specified) */
    char *new_hash;              /* New file SHA hash from index line (NULL if not specified) */
    int similarity_index;        /* Rename/copy similarity 0-100% (-1 if not specified) */
    int dissimilarity_index;     /* Dissimilarity percentage 0-100% (-1 if not specified) */
    char *rename_from;           /* Source filename for renames (NULL if not rename) */
    char *rename_to;             /* Target filename for renames (NULL if not rename) */
    char *copy_from;             /* Source filename for copies (NULL if not copy) */
    char *copy_to;               /* Target filename for copies (NULL if not copy) */
    int is_binary;               /* 1 if binary patch detected, 0 for text patches */

    /* Position tracking (for tools that need to locate patches in input) */
    long start_position;         /* Byte offset in input where this patch starts */
    unsigned long start_line;    /* Line number where this patch starts (1-based) */
};

/**
 * Hunk header information.
 *
 * UNIFIED DIFF FORMAT: "@@ -orig_offset,orig_count +new_offset,new_count @@ context"
 * CONTEXT DIFF FORMAT: "*** orig_offset,orig_count ****" + "--- new_offset,new_count ----"
 *
 * LINE COUNTING:
 * - orig_count: number of lines from original file in this hunk (context + removed)
 * - new_count: number of lines in new file for this hunk (context + added)
 * - Context lines count toward both orig_count and new_count
 * - If count is omitted in diff, defaults to 1 (unless offset is 0, then count is 0)
 */
struct patch_hunk {
    unsigned long orig_offset;   /* Starting line number in original file (1-based, 0 = empty file) */
    unsigned long orig_count;    /* Number of lines from original file in this hunk */
    unsigned long new_offset;    /* Starting line number in new file (1-based, 0 = empty file) */
    unsigned long new_count;     /* Number of lines in new file for this hunk */
    char *context;               /* Context string after @@ in unified diffs (NULL if none) */
    long position;               /* Byte offset in input where this hunk header appears */
};

/**
 * Individual hunk line (content within a hunk).
 *
 * LINE TYPES:
 * - PATCH_LINE_CONTEXT (' '): Line exists in both old and new file
 * - PATCH_LINE_ADDED ('+'): Line exists only in new file
 * - PATCH_LINE_REMOVED ('-'): Line exists only in old file
 * - PATCH_LINE_CHANGED ('!'): Line changed between files (context diffs only)
 * - PATCH_LINE_NO_NEWLINE ('\\'): Not a real line, indicates previous line has no newline
 *
 * CONTEXT HANDLING:
 * - context indicates which version of the file this line represents
 * - PATCH_CONTEXT_BOTH: Normal lines (applies to both old and new file versions)
 * - PATCH_CONTEXT_OLD: For PATCH_LINE_CHANGED, this is the "old" version of the line
 * - PATCH_CONTEXT_NEW: For PATCH_LINE_CHANGED, this is the "new" version of the line
 *
 * CONTEXT DIFF DUAL EMISSION:
 * - For context diffs, changed lines (!) are emitted twice with identical content:
 *   1. First emission: during old section parsing (context = PATCH_CONTEXT_OLD)
 *   2. Second emission: during new section parsing (context = PATCH_CONTEXT_NEW)
 * - This allows consumers to easily filter for "before" vs "after" views
 * - Unified diffs don't have this behavior (changed lines appear as separate - and + lines)
 *
 * CONTENT HANDLING:
 * - line points to the FULL original line INCLUDING the prefix character
 * - length is the byte length of the full line (includes prefix, excludes newline)
 * - content points to clean content WITHOUT prefix or format-specific spaces
 * - content_length is the byte length of the clean content
 * - Neither line nor content are null-terminated (use length fields for bounds)
 * - The type field indicates what the prefix character is
 */
struct patch_hunk_line {
    enum patch_hunk_line_type type; /* Line operation type (space, +, -, !, \) */
    enum patch_line_context context; /* Which file version this line represents */
    const char *line;            /* Full original line INCLUDING prefix (NOT null-terminated) */
    size_t length;               /* Length of full line in bytes (includes prefix, excludes newline) */
    const char *content;         /* Clean content WITHOUT prefix/spaces (NOT null-terminated) */
    size_t content_length;       /* Length of clean content in bytes */
    long position;               /* Byte offset in input where this line appears */
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
 * SUPPORTED INPUT FORMATS:
 * - Unified diffs (diff -u, git diff)
 * - Context diffs (diff -c)
 * - Git extended diffs (git format-patch, git show)
 * - Mixed content (patches with interspersed comments/junk)
 * - Binary patches (both Git binary and "Binary files differ")
 *
 * @param file Input stream to read from (must remain valid for scanner lifetime)
 * @return New scanner instance, or NULL on memory allocation error
 */
patch_scanner_t* patch_scanner_create(FILE *file);

/**
 * Get the next piece of content from the scanner.
 *
 * USAGE PATTERN:
 *   const patch_content_t *content;
 *   int result;
 *   while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
 *       switch (content->type) {
 *           case PATCH_CONTENT_HEADERS:
 *               // Process patch header
 *               break;
 *           case PATCH_CONTENT_HUNK_LINE:
 *               // Process individual line
 *               break;
 *           // ... handle other types
 *       }
 *   }
 *   if (result != PATCH_SCAN_EOF) {
 *       // Handle error
 *   }
 *
 * MEMORY LIFETIME:
 * - Returned content pointer is valid until next patch_scanner_next() call
 * - All pointers within content structure have same lifetime
 * - Consumer must copy data if needed beyond next call
 *
 * @param scanner Scanner instance (must not be NULL)
 * @param content Output parameter for content pointer (must not be NULL)
 * @return PATCH_SCAN_OK if content available, PATCH_SCAN_EOF if done, or error code
 */
int patch_scanner_next(patch_scanner_t *scanner, const patch_content_t **content);

/**
 * Get the current file position of the scanner.
 *
 * Useful for implementing patch indexing or seeking to specific patches.
 * Position corresponds to the start of the most recently returned content.
 *
 * @param scanner Scanner instance (must not be NULL)
 * @return Current byte offset in input stream, or -1 on error
 */
long patch_scanner_position(patch_scanner_t *scanner);

/**
 * Get the current line number being processed.
 *
 * Line numbers are 1-based and correspond to the input stream.
 * Useful for error reporting and debugging.
 *
 * @param scanner Scanner instance (must not be NULL)
 * @return Current line number (1-based), or 0 on error
 */
unsigned long patch_scanner_line_number(patch_scanner_t *scanner);

/**
 * Destroy a patch scanner and free all associated resources.
 *
 * After calling this function:
 * - Scanner pointer becomes invalid
 * - All content pointers previously returned become invalid
 * - Input file stream is NOT closed (caller responsibility)
 *
 * @param scanner Scanner instance (NULL is safe to pass)
 */
void patch_scanner_destroy(patch_scanner_t *scanner);

/* Convenience functions */

/**
 * Skip all content for the current patch (if we're in the middle of one).
 *
 * USAGE SCENARIOS:
 * - Patch indexing: record patch locations without processing content
 * - Selective processing: skip patches that don't match criteria
 * - Error recovery: skip malformed patches and continue
 *
 * BEHAVIOR:
 * - If not currently in a patch, returns immediately with PATCH_SCAN_OK
 * - If in a patch, consumes all remaining content until next patch or EOF
 * - After successful skip, next patch_scanner_next() will return next patch or non-patch content
 *
 * @param scanner Scanner instance (must not be NULL)
 * @return PATCH_SCAN_OK on success, PATCH_SCAN_EOF if no more content, or error code
 */
int patch_scanner_skip_current_patch(patch_scanner_t *scanner);

/**
 * Check if the scanner is currently positioned at the start of a new patch.
 *
 * USAGE:
 * - Determine patch boundaries without consuming content
 * - Implement patch counting or indexing
 * - Coordinate with other processing logic
 *
 * DEFINITION OF "PATCH START":
 * - Just returned PATCH_CONTENT_HEADERS, or
 * - About to return PATCH_CONTENT_HEADERS on next call, or
 * - Currently accumulating/validating potential patch headers
 *
 * @param scanner Scanner instance (must not be NULL)
 * @return 1 if at patch start, 0 otherwise (including error conditions)
 */
int patch_scanner_at_patch_start(patch_scanner_t *scanner);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_SCANNER_H */
