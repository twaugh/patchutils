# Makefile for patch scanner tests

# Build configuration
CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99 -DHAVE_CONFIG_H
INCLUDES = -I../../ -I../../src -I../../lib
LDFLAGS =
LIBS = ../../lib/libgnu.a

# Source files
SCANNER_SRCS = ../../src/patch_scanner.c ../../src/util.c
SCANNER_OBJS = $(SCANNER_SRCS:.c=.o)

# Test programs
TESTS = test_basic
TEST_SRCS = $(TESTS:=.c)
TEST_OBJS = $(TESTS:=.o)

# Default target
all: $(TESTS)

# Test programs
test_basic: test_basic.o $(SCANNER_OBJS)
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

.PHONY: all check clean
