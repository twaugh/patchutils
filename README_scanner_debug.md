# Scanner Debug Utility

The `scanner_debug` utility is a development tool that shows exactly what events the patch scanner API emits for any given patch file. This is invaluable for debugging scanner behavior, understanding patch parsing, and verifying scanner fixes.

## Building

The utility is built automatically with:
```bash
./configure --enable-scanner-patchfilter
make
```

The binary will be created as `src/scanner_debug` (not installed by default).

## Usage

```bash
scanner_debug [OPTIONS] [FILE]
```

### Options

- `-h, --help` - Show help message
- `-v, --verbose` - Use multi-line output instead of compact
- `-c, --content` - Show content samples for events (verbose mode)
- `-p, --positions` - Show file positions for all events (verbose mode)
- `-x, --extra` - Show extra details like Git metadata (verbose mode)
- `--color` - Use colored output (great for terminals)

### Examples

```bash
# Basic usage
scanner_debug example.patch

# Colored output with content samples
scanner_debug --color --content example.patch

# Debug from stdin
diff -u old new | scanner_debug --verbose

# Debug context diffs with full details
scanner_debug --color --verbose --content --extra example.patch
```

## Event Types

The scanner emits the following event types:

### HEADERS
Complete patch headers (file names, types, Git metadata)
- **Unified**: `--- old` / `+++ new`
- **Context**: `*** old` / `--- new`
- **Git Extended**: `diff --git` with extended metadata

### HUNK_HEADER
Hunk range information (`@@ -1,3 +1,3 @@` or `*** 1,3 ****`)

### HUNK_LINE
Individual patch lines with type and context:
- **Context (' ')**: Unchanged lines (context: both)
- **Added ('+')**: Added lines (context: both)
- **Removed ('-')**: Removed lines (context: both)
- **Changed ('!')**: Changed lines (context diffs only)
  - Emitted twice: first as context "old", then as context "new"
  - Different line content: old version first, then new version
- **No Newline ('\\')**: No newline marker lines (context: both)

**Note**: "context: both" means the line applies to both old and new file versions conceptually. Only changed lines ('!') in context diffs get special context handling (old/new).

### BINARY
Binary patch markers (`Binary files differ`, `GIT binary patch`)

### NO_NEWLINE
No newline markers (`\ No newline at end of file`)

### NON-PATCH
Content that isn't part of a patch (comments, etc.)

## Debugging Use Cases

### Verify Scanner Fixes
```bash
# Check that context diff "--- N ----" lines aren't treated as hunk lines
scanner_debug --content context_with_empty.patch | grep "HUNK_LINE.*--.*----"
# Should return nothing if bug is fixed
```

### Understand Git Diff Parsing
```bash
scanner_debug --verbose --color --extra example.patch
# Shows Git metadata parsing and type detection
```

### Debug Complex Patches
```bash
scanner_debug --color --verbose --content --extra example.patch > debug.log
# Full event trace for complex multi-file patches
```

## Output Format

For the following example patch:
```diff
--- old.txt	2024-01-01 12:00:00.000000000 +0000
+++ new.txt	2024-01-01 12:01:00.000000000 +0000
@@ -1,4 +1,4 @@
 line1
-old line
+new line
 line3
 line4
```

### Compact Mode (default)
```
Scanner Debug Output for: example.patch
================================================================
  2 HEADERS      Unified: old.txt → new.txt
  3 HUNK_HEADER  -1,4 +1,4
  4 HUNK_LINE     line1
  5 HUNK_LINE    -old line
  6 HUNK_LINE    +new line
  7 HUNK_LINE     line3
  8 HUNK_LINE     line4
================================================================
Summary: Processed 7 events, scanner finished normally
```

### Verbose Mode (-v/--verbose)
```
Scanner Debug Output for: example.patch
================================================================
[HEADERS]
  Type: Unified
  Old: old.txt
  New: new.txt

[HUNK_HEADER]
  Range: -1,4 +1,4

[HUNK_LINE]
  Type: Context (' ') Context: both

[HUNK_LINE]
  Type: Removed ('-') Context: both

[HUNK_LINE]
  Type: Added ('+') Context: both

[HUNK_LINE]
  Type: Context (' ') Context: both

[HUNK_LINE]
  Type: Context (' ') Context: both

================================================================
Summary: Processed 7 events, scanner finished normally
```

### Verbose Mode with Content (--verbose --content)
```
Scanner Debug Output for: example.patch
================================================================
[HEADERS]
  Type: Unified
  Old: old.txt
  New: new.txt

[HUNK_HEADER]
  Range: -1,4 +1,4

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line1"

[HUNK_LINE]
  Type: Removed ('-') Context: both Content: "old line"

[HUNK_LINE]
  Type: Added ('+') Context: both Content: "new line"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line3"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line4"

================================================================
Summary: Processed 7 events, scanner finished normally
```

## Context Diff Example

For comparison, here's the same patch in context format (converted using `filterdiff --format=context`):
```diff
*** old.txt	2024-01-01 12:00:00.000000000 +0000
--- new.txt	2024-01-01 12:01:00.000000000 +0000
***************
*** 1,4 ****
  line1
! old line
  line3
  line4
--- 1,4 ----
  line1
! new line
  line3
  line4
```

### Context Diff - Compact Mode
```
Scanner Debug Output for: example-context.patch
================================================================
  2 HEADERS      Context: old.txt → new.txt
  4 HUNK_HEADER  -1,4 +1,4
  9 HUNK_LINE      line1
  9 HUNK_LINE    ! old line
  9 HUNK_LINE      line3
  9 HUNK_LINE      line4
 10 HUNK_LINE      line1
 11 HUNK_LINE    ! new line
 12 HUNK_LINE      line3
 13 HUNK_LINE      line4
================================================================
Summary: Processed 10 events, scanner finished normally
```

### Context Diff - Verbose Mode with Content
```
Scanner Debug Output for: example-context.patch
================================================================
[HEADERS]
  Type: Context
  Old: old.txt
  New: new.txt

[HUNK_HEADER]
  Range: -1,4 +1,4

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line1"

[HUNK_LINE]
  Type: Changed ('!') Context: old Content: "old line"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line3"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line4"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line1"

[HUNK_LINE]
  Type: Changed ('!') Context: new Content: "new line"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line3"

[HUNK_LINE]
  Type: Context (' ') Context: both Content: "line4"

================================================================
Summary: Processed 10 events, scanner finished normally
```

**Note**: In context diffs, changed lines (`!`) are emitted twice - first with the old content (context: old), then with the new content (context: new). This demonstrates the dual emission behavior described earlier.

## Color Coding

When `--color` is used:
- **🟢 HEADERS**: Green - Patch headers
- **🟡 HUNK_HEADER**: Yellow - Hunk ranges
- **🔵 HUNK_LINE**: Blue - Patch content lines
- **🔴 BINARY**: Red - Binary content
- **🟣 NO_NEWLINE**: Magenta - No newline markers
- **⚫ NON-PATCH**: Gray - Non-patch content
