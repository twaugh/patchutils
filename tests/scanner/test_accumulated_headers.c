/*
 * Test for accumulated headers being emitted as non-patch content
 * Tests the logic added to handle incomplete patch headers
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../src/patch_scanner.h"

/* Test case 1: EOF while accumulating headers */
static void test_eof_accumulated_headers(void)
{
    printf("Testing EOF while accumulating headers...\n");

    /* Create input with incomplete headers (no +++ line) */
    const char *input =
        "diff --git a/file.txt b/file.txt\n"
        "index 1234567..abcdefg 100644\n"
        "--- a/file.txt\n";

    FILE *fp = fmemopen((void*)input, strlen(input), "r");
    assert(fp != NULL);

    patch_scanner_t *scanner = patch_scanner_create(fp);
    assert(scanner != NULL);

    const patch_content_t *content;
    enum patch_scanner_result result;

    int non_patch_count = 0;
    int header_count = 0;

    /* Should get non-patch content for each accumulated header line */
    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        switch (content->type) {
        case PATCH_CONTENT_NON_PATCH:
            non_patch_count++;
            printf("  Non-patch line: %.*s\n", (int)content->data.non_patch.length,
                   content->data.non_patch.line);
            break;
        case PATCH_CONTENT_HEADERS:
            header_count++;
            break;
        default:
            printf("  Unexpected content type: %d\n", content->type);
            break;
        }
    }

    assert(result == PATCH_SCAN_EOF);
    assert(non_patch_count == 1); /* Should emit 1 combined non-patch content */
    assert(header_count == 0);    /* No complete headers should be emitted */

    patch_scanner_destroy(scanner);
    fclose(fp);

    printf("  ✓ EOF test passed: %d non-patch lines emitted\n", non_patch_count);
}

/* Test case 2: Non-continuation line interrupts header accumulation */
static void test_non_continuation_accumulated_headers(void)
{
    printf("Testing non-continuation line interrupting headers...\n");

    /* Create input with headers followed by non-header content */
    const char *input =
        "diff --git a/file.txt b/file.txt\n"
        "index 1234567..abcdefg 100644\n"
        "This is not a header line\n"
        "Some other content\n";

    FILE *fp = fmemopen((void*)input, strlen(input), "r");
    assert(fp != NULL);

    patch_scanner_t *scanner = patch_scanner_create(fp);
    assert(scanner != NULL);

    const patch_content_t *content;
    enum patch_scanner_result result;

    int non_patch_count = 0;
    int header_count = 0;

    /* Should get non-patch content for accumulated headers, then regular non-patch */
    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        switch (content->type) {
        case PATCH_CONTENT_NON_PATCH:
            non_patch_count++;
            printf("  Non-patch line: %.*s\n", (int)content->data.non_patch.length,
                   content->data.non_patch.line);
            break;
        case PATCH_CONTENT_HEADERS:
            header_count++;
            break;
        default:
            printf("  Unexpected content type: %d\n", content->type);
            break;
        }
    }

    assert(result == PATCH_SCAN_EOF);
    assert(non_patch_count == 3); /* 1 combined accumulated headers + 2 regular non-patch lines */
    assert(header_count == 0);    /* No complete headers should be emitted */

    patch_scanner_destroy(scanner);
    fclose(fp);

    printf("  ✓ Non-continuation test passed: %d non-patch lines emitted\n", non_patch_count);
}

/* Test case 3: Complete patch should still work normally */
static void test_complete_patch_still_works(void)
{
    printf("Testing that complete patches still work normally...\n");

    /* Create input with complete patch */
    const char *input =
        "diff --git a/file.txt b/file.txt\n"
        "index 1234567..abcdefg 100644\n"
        "--- a/file.txt\n"
        "+++ b/file.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " line1\n"
        "-old line\n"
        "+new line\n"
        " line3\n";

    FILE *fp = fmemopen((void*)input, strlen(input), "r");
    assert(fp != NULL);

    patch_scanner_t *scanner = patch_scanner_create(fp);
    assert(scanner != NULL);

    const patch_content_t *content;
    enum patch_scanner_result result;

    int non_patch_count = 0;
    int header_count = 0;
    int hunk_header_count = 0;
    int hunk_line_count = 0;

    while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
        switch (content->type) {
        case PATCH_CONTENT_NON_PATCH:
            non_patch_count++;
            break;
        case PATCH_CONTENT_HEADERS:
            header_count++;
            break;
        case PATCH_CONTENT_HUNK_HEADER:
            hunk_header_count++;
            break;
        case PATCH_CONTENT_HUNK_LINE:
            hunk_line_count++;
            break;
        default:
            break;
        }
    }

    assert(result == PATCH_SCAN_EOF);
    assert(header_count == 1);      /* Should have complete headers */
    assert(hunk_header_count == 1); /* Should have hunk header */
    assert(hunk_line_count == 4);   /* Should have 4 hunk lines */
    assert(non_patch_count == 0);   /* No non-patch content */

    patch_scanner_destroy(scanner);
    fclose(fp);

    printf("  ✓ Complete patch test passed: headers=%d, hunk_headers=%d, hunk_lines=%d\n",
           header_count, hunk_header_count, hunk_line_count);
}

int main(void)
{
    printf("=== Testing Accumulated Headers as Non-Patch Logic ===\n\n");

    test_eof_accumulated_headers();
    printf("\n");

    test_non_continuation_accumulated_headers();
    printf("\n");

    test_complete_patch_still_works();
    printf("\n");

    printf("=== All tests passed! ===\n");
    return 0;
}
