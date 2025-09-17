# Makefile for patch scanner tests

# Build configuration - inherit from parent build
CC = gcc
# Base flags
BASE_CFLAGS = -Wall -Wextra -g -std=c99 -DHAVE_CONFIG_H
INCLUDES = -I../../ -I../../src -I../../lib
LIBS = ../../lib/libgnu.a

# Inherit CFLAGS and LDFLAGS from parent build if available
# This ensures we use the same coverage, optimization, and other flags
PARENT_CFLAGS := $(shell grep '^CFLAGS' ../../Makefile 2>/dev/null | cut -d= -f2- || echo "")
PARENT_LDFLAGS := $(shell grep '^LDFLAGS' ../../Makefile 2>/dev/null | cut -d= -f2- || echo "")

CFLAGS = $(BASE_CFLAGS) $(PARENT_CFLAGS)
LDFLAGS = $(PARENT_LDFLAGS)

# Source files
SCANNER_SRCS = ../../src/patch_scanner.c ../../src/util.c ../../src/diff.c
SCANNER_OBJS = $(SCANNER_SRCS:.c=.o)

# Test programs
TESTS = test_basic test_accumulated_headers test_input_validation
TEST_SRCS = $(TESTS:=.c)
TEST_OBJS = $(TESTS:=.o)

# Default target
all: $(TESTS)

# Test programs
test_basic: test_basic.o $(SCANNER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test_accumulated_headers: test_accumulated_headers.o $(SCANNER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test_input_validation: test_input_validation.o $(SCANNER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Run tests
check: $(TESTS)
	@echo "Running scanner tests..."
	@for test in $(TESTS); do \
		echo "Running $$test..."; \
		./$$test || exit 1; \
	done
	@echo "All tests passed!"

# Clean up
clean:
	rm -f $(TESTS) $(TEST_OBJS) $(SCANNER_OBJS)

# Clean up everything including copied files
distclean: clean
	rm -f Makefile test_basic.c README.md

.PHONY: all check clean distclean
