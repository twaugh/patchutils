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
- `-v, --verbose` - Show verbose output with positions and details
- `-c, --content` - Show content samples for events
- `-p, --positions` - Show file positions for all events
- `--color` - Use colored output (great for terminals)

### Examples

```bash
# Basic usage
scanner_debug patch.diff

# Colored output with content samples
scanner_debug --color --content complex.patch

# Debug from stdin
diff -u old new | scanner_debug --verbose

# Debug context diffs with full details
scanner_debug --color --content --verbose context.patch
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
  - Same line content, different context indicating old vs new version

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
scanner_debug --verbose --color git_extended.patch
# Shows Git metadata parsing and type detection
```

### Debug Complex Patches
```bash
scanner_debug --color --content --verbose complex_series.patch > debug.log
# Full event trace for complex multi-file patches
```

## Output Format

```
Scanner Debug Output for: example.patch
================================================================
[HEADERS] HEADERS (line 1, pos 0)
  Type: Unified
  Old: old.txt
  New: new.txt

[HUNK_HEADER] HUNK_HEADER (line 3, pos 25)
  Range: -1,3 +1,3

[HUNK_LINE] HUNK_LINE (line 4, pos 38)
  Type: Context (' ') Context: both Content: "line1\n"

[HUNK_LINE] HUNK_LINE (line 5, pos 45)
  Type: Removed ('-') Context: both Content: "old line\n"

================================================================
Summary: Processed 6 events, scanner finished normally
```

## Color Coding

When `--color` is used:
- **🟢 HEADERS**: Green - Patch headers
- **🟡 HUNK_HEADER**: Yellow - Hunk ranges
- **🔵 HUNK_LINE**: Blue - Patch content lines
- **🔴 BINARY**: Red - Binary content
- **🟣 NO_NEWLINE**: Magenta - No newline markers
- **⚫ NON-PATCH**: Gray - Non-patch content
