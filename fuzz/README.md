# Patchutils Fuzzing Infrastructure

This directory contains fuzz testing infrastructure for patchutils.

## Overview

The fuzz testing setup targets the main patch processing tools:
- `filterdiff` - patch filtering and manipulation
- `interdiff` - incremental patch generation
- `rediff` - patch correction
- `grepdiff` - regex matching in patches
- `lsdiff` - patch file listing

## Fuzzing Approaches

### 1. Input-Based Fuzzing
- **Target**: Command-line tools with file inputs
- **Method**: Generate malformed patch files and feed them to tools
- **Coverage**: Tests argument parsing, file I/O, and patch parsing

### 2. Library Function Fuzzing
- **Target**: Core parsing functions in `diff.c` and `util.c`
- **Method**: Direct function calls with generated inputs
- **Coverage**: Deep testing of parsing logic without CLI overhead

### 3. Property-Based Testing
- **Target**: Invariant validation (building on existing test-invariants.sh)
- **Method**: Generate patches that should maintain specific properties
- **Coverage**: Semantic correctness and consistency

## Files

- `generate_corpus.sh` - Creates initial seed corpus from existing tests
- `run_fuzz.sh` - Main fuzzing script
- `analyze_crashes.sh` - Analyzes and categorizes crashes
- `patch.dict` - AFL++ dictionary for patch-specific mutations
- `corpus/` - Seed files for fuzzing
- `crashes/` - Discovered crash cases
- `hangs/` - Discovered hang cases

## Usage

### Building with Instrumentation (Recommended)

For effective fuzzing, build patchutils with AFL++ instrumentation:

```bash
# Configure with fuzzing support
./configure --enable-fuzzing

# Build instrumented binaries
make

# Now fuzzing will use instrumented binaries automatically
make fuzz-test
```

### Via Makefile (Recommended)

```bash
# Quick start - see all available targets
make fuzz-help

# Generate/update corpus (includes latest git diffs)
make fuzz-corpus

# Quick 60-second test (uses instrumented binaries if available)
make fuzz-test

# Fuzz specific tools
make fuzz-filterdiff    # Most important
make fuzz-interdiff
make fuzz-rediff
make fuzz-grepdiff
make fuzz-lsdiff

# Analyze results
make fuzz-analyze

# Check prerequisites
make fuzz-check
```

### Direct Script Usage

```bash
# Generate initial corpus
./generate_corpus.sh

# Run fuzzing for filterdiff
./run_fuzz.sh filterdiff

# Run fuzzing for interdiff
./run_fuzz.sh interdiff

# Analyze crashes
./analyze_crashes.sh
```

### Continuous Integration

```bash
# Quick validation for CI/CD
make fuzz-ci
```

## Integration

The fuzzer integrates with the autotools build system and existing testing infrastructure:

### Autotools Integration
- `--enable-fuzzing` configure option enables AFL++ instrumentation
- Creates separate `fuzz-*` instrumented binaries alongside regular tools
- Automatic detection of instrumented vs. non-instrumented binaries
- Graceful fallback to regular binaries when instrumentation not available

### Testing Integration
- Uses existing test cases as seed corpus
- Validates discovered issues against invariant tests
- Generates regression tests for fixed bugs

### Build System Features
- Conditional compilation based on `--enable-fuzzing`
- Proper compiler detection for AFL++ (afl-clang-fast, afl-gcc)
- Fuzzing-specific CFLAGS and preprocessor definitions
- Integration with existing Makefile targets
