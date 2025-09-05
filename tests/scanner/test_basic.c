/*
 * test_basic.c - basic patch scanner tests
 * Copyright (C) 2024 Tim Waugh <twaugh@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../src/patch_scanner.h"

/* Test data */
static const char *simple_unified_diff =
    "--- old.txt\t2024-01-01 12:00:00.000000000 +0000\n"
    "+++ new.txt\t2024-01-01 12:00:01.000000000 +0000\n"
    "@@ -1,3 +1,3 @@\n"
    " line1\n"
    "-old line\n"
    "+new line\n"
    " line3\n";

static const char *non_patch_content =
    "This is not a patch\n"
    "Just some random text\n"
    "Nothing to see here\n";

static const char *mixed_content =
    "Some header comment\n"
    "--- old.txt\t2024-01-01 12:00:00.000000000 +0000\n"
    "+++ new.txt\t2024-01-01 12:00:01.000000000 +0000\n"
    "@@ -1,1 +1,1 @@\n"
    "-old\n"
    "+new\n"
    "Some footer comment\n";

/* Helper function to create FILE* from string */
static FILE* string_to_file(const char *str)
{
    FILE *f = tmpfile();
    if (!f) {
        return NULL;
    }

    fwrite(str, strlen(str), 1, f);
    rewind(f);
    return f;
}

/* Test scanner creation and destruction */
static void test_scanner_lifecycle(void)
{
    FILE *f = string_to_file(simple_unified_diff);
    assert(f != NULL);

    patch_scanner_t *scanner = patch_scanner_create(f);
    assert(scanner != NULL);

    /* Test position and line number functions */
    assert(patch_scanner_position(scanner) == 0);
    assert(patch_scanner_line_number(scanner) == 0);

    patch_scanner_destroy(scanner);
    fclose(f);

    printf("✓ Scanner lifecycle test passed\n");
}

/* Test scanning non-patch content */
static void test_non_patch_content(void)
{
    FILE *f = string_to_file(non_patch_content);
    assert(f != NULL);

    patch_scanner_t *scanner = patch_scanner_create(f);
    assert(scanner != NULL);

    const patch_content_t *content;
    int result;
    int line_count = 0;

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        assert(content->type == PATCH_CONTENT_NON_PATCH);
        assert(content->data.non_patch.line != NULL);
        assert(content->data.non_patch.length > 0);
        line_count++;
    }

    assert(result == PATCH_SCAN_EOF);
    assert(line_count == 3);  /* Three lines in non_patch_content */

    patch_scanner_destroy(scanner);
    fclose(f);

    printf("✓ Non-patch content test passed\n");
}

/* Test scanning simple unified diff */
static void test_simple_unified_diff(void)
{
    FILE *f = string_to_file(simple_unified_diff);
    assert(f != NULL);

    patch_scanner_t *scanner = patch_scanner_create(f);
    assert(scanner != NULL);

    const patch_content_t *content;
    int result;
    int found_headers = 0;
    int found_hunk_header = 0;
    int found_hunk_lines = 0;

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        switch (content->type) {
        case PATCH_CONTENT_HEADERS:
            found_headers++;
            assert(content->data.headers != NULL);
            /* TODO: Add more header validation once parsing is implemented */
            break;

        case PATCH_CONTENT_HUNK_HEADER:
            found_hunk_header++;
            assert(content->data.hunk != NULL);
            break;

        case PATCH_CONTENT_HUNK_LINE:
            found_hunk_lines++;
            assert(content->data.line != NULL);
            assert(content->data.line->content != NULL);
            break;

        case PATCH_CONTENT_NON_PATCH:
            /* Shouldn't have any non-patch content in this test */
            assert(0);
            break;

        default:
            break;
        }
    }

    assert(result == PATCH_SCAN_EOF);
    assert(found_headers == 1);
    assert(found_hunk_header == 1);
    assert(found_hunk_lines == 4);  /* 1 context + 1 removed + 1 added + 1 context */

    patch_scanner_destroy(scanner);
    fclose(f);

    printf("✓ Simple unified diff test passed\n");
}

/* Test scanning mixed content */
static void test_mixed_content(void)
{
    FILE *f = string_to_file(mixed_content);
    assert(f != NULL);

    patch_scanner_t *scanner = patch_scanner_create(f);
    assert(scanner != NULL);

    const patch_content_t *content;
    int result;
    int found_non_patch = 0;
    int found_headers = 0;
    int found_hunk_content = 0;

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        switch (content->type) {
        case PATCH_CONTENT_NON_PATCH:
            found_non_patch++;
            break;

        case PATCH_CONTENT_HEADERS:
            found_headers++;
            break;

        case PATCH_CONTENT_HUNK_HEADER:
        case PATCH_CONTENT_HUNK_LINE:
            found_hunk_content++;
            break;

        default:
            break;
        }
    }

    assert(result == PATCH_SCAN_EOF);
    assert(found_non_patch == 2);  /* Header and footer comments */
    assert(found_headers == 1);
    assert(found_hunk_content > 0);

    patch_scanner_destroy(scanner);
    fclose(f);

    printf("✓ Mixed content test passed\n");
}

/* Test error conditions */
static void test_error_conditions(void)
{
    /* Test NULL parameters */
    assert(patch_scanner_create(NULL) == NULL);

    patch_scanner_t *scanner = patch_scanner_create(tmpfile());
    assert(scanner != NULL);

    const patch_content_t *content;
    assert(patch_scanner_next(NULL, &content) == PATCH_SCAN_ERROR);
    assert(patch_scanner_next(scanner, NULL) == PATCH_SCAN_ERROR);

    assert(patch_scanner_position(NULL) == -1);
    assert(patch_scanner_line_number(NULL) == 0);

    /* Test that destroy handles NULL gracefully */
    patch_scanner_destroy(NULL);

    patch_scanner_destroy(scanner);

    printf("✓ Error conditions test passed\n");
}

int main(void)
{
    printf("Running patch scanner basic tests...\n\n");

    test_scanner_lifecycle();
    test_non_patch_content();
    test_simple_unified_diff();
    test_mixed_content();
    test_error_conditions();

    printf("\n✓ All basic tests passed!\n");
    return 0;
}
