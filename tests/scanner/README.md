# Patch Scanner Tests

This directory contains unit tests for the unified patch scanner API.

## Overview

The patch scanner provides a unified parsing interface for all patchutils tools. It uses a pull-based API where consumers request the next piece of content from the scanner.

## Test Structure

- `test_basic.c` - Basic functionality tests including:
  - Scanner lifecycle (create/destroy)
  - Non-patch content handling
  - Simple unified diff parsing
  - Mixed content (patch + non-patch)
  - Error condition handling

## Building and Running Tests

```bash
# Build tests
make

# Run all tests
make check

# Clean up
make clean
```

## Test Data

Tests use in-memory string data converted to FILE* streams for testing. This allows us to test various patch formats and edge cases without requiring external files.

## Current Status

**Implemented:**
- Basic scanner API structure
- State machine framework
- Content type definitions
- Simple test harness

**TODO:**
- Complete header parsing implementation
- Add hunk parsing logic
- Implement Git extended header support
- Add binary patch detection
- Add context diff support
- Add comprehensive edge case tests

## Adding New Tests

To add a new test:

1. Create a new test function in `test_basic.c` (or create a new test file)
2. Add test data as string constants
3. Use `string_to_file()` helper to create FILE* from strings
4. Follow the pattern of other tests for assertions
5. Add the test to the `main()` function

For more complex tests requiring multiple files, create separate `.c` files and update the Makefile accordingly.
