# Patchutils

A collection of tools that operate on patch files.

## Overview

Patchutils is a small collection of programs that operate on patch files. It provides utilities for manipulating, analyzing, and transforming patch files in various ways.

## Tools

### Core Tools

- **interdiff** - Generates an incremental patch from two patches against a common source. For example, if you have applied a pre-patch to a source tree, and wish to apply another pre-patch (which is against the same original source tree), you can use interdiff to generate the patch that you need to apply. You can also use this to review changes between two pre-patches.

- **combinediff** - Generates a single patch from two incremental patches, allowing you to merge patches together. The resulting patch file only alters each file once.

- **filterdiff** - Selects the portions of a patch file that apply to files matching (or, alternatively, not matching) a shell wildcard.

- **rediff** - Corrects hand-edited patches, by comparing the original patch with the modified one and adjusting the offsets and counts.

### Analysis Tools

- **lsdiff** - Displays a short listing of affected files in a patch file, along with (optionally) the line numbers of the start of each patch.

- **grepdiff** - Displays a list of the files modified by a patch where the patch contains a given regular expression.

### Utility Tools

- **splitdiff** - Separates out patches from a patch file so that each new patch file only alters any given file once. In this way, a file containing several incremental patches can be split into individual incremental patches.

- **fixcvsdiff** - Corrects the output of 'cvs diff'.

- **recountdiff** - Fixes up counts and offsets in a unified diff.

- **unwrapdiff** - Fixes word-wrapped unified diffs.

- **flipdiff** - Exchanges the order of two patches.

- **dehtmldiff** - Extracts a diff from an HTML page.

- **editdiff** - Edit a patch file interactively.

- **espdiff** - Apply the appropriate transformation to a patch.

### Viewer Tools

- **patchview** - View patches with syntax highlighting.

- **gitdiff** / **gitdiffview** - Git-specific diff viewing tools.

- **svndiff** / **svndiffview** - Subversion-specific diff viewing tools.

## Installation

Patchutils uses the standard GNU autotools build system:

```bash
./configure
make
make install
```

## Requirements

- A C compiler (GCC recommended)
- Standard Unix utilities (diff, patch)
- Perl (for some scripts)
- Optional: xmlto (for building documentation)
- Optional: PCRE2 library (for enhanced regex support)

## Documentation

Manual pages are available for all tools. After installation, you can access them with:

```bash
man interdiff
man filterdiff
# etc.
```
