/*
 * grepdiff - show files modified by a patch containing a regexp
 * Copyright (C) 2025 Tim Waugh <twaugh@redhat.com>
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
 *
 * This is a scanner-based implementation of grepdiff using the unified patch scanner API.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <locale.h>
#include <fnmatch.h>
#include <errno.h>
#include <regex.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif

#include "patchfilter.h"
#include "patch_common.h"

/* Output modes */
enum output_mode {
	OUTPUT_LIST = 0,    /* List filenames only (default) */
	OUTPUT_FILE,        /* Output entire matching files */
	OUTPUT_HUNK         /* Output only matching hunks */
};

/* Match filtering modes (for --only-match) */
enum match_filter {
	MATCH_ALL = 0,      /* Show all lines (default) */
	MATCH_REMOVALS,     /* Show only removed lines (-)  */
	MATCH_ADDITIONS,    /* Show only added lines (+) */
	MATCH_MODIFICATIONS /* Show only modified lines (context diff !) */
};

/* Line numbering modes (for --as-numbered-lines) */
enum numbered_mode {
	NUMBERED_NONE = 0,          /* No line numbering */
	NUMBERED_BEFORE,            /* Show original file line numbers */
	NUMBERED_AFTER,             /* Show new file line numbers */
	NUMBERED_ORIGINAL_BEFORE,   /* Show original line numbers from diff (before) */
	NUMBERED_ORIGINAL_AFTER     /* Show original line numbers from diff (after) */
};

/* Global options (grepdiff-specific) */
static enum output_mode output_mode = OUTPUT_LIST;
static enum match_filter match_filter = MATCH_ALL;
static enum numbered_mode numbered_mode = NUMBERED_NONE;
static int extended_regexp = 0;       /* -E, --extended-regexp */
static int show_status = 0;           /* -s, --status */
static int empty_files_as_absent = 0; /* --empty-files-as-absent */

/* Grep patterns */
static regex_t *grep_patterns = NULL;
static int num_grep_patterns = 0;
static int max_grep_patterns = 0;


/* Buffered hunk structure for output modes */
struct buffered_hunk {
	unsigned long orig_offset;
	unsigned long orig_count;
	unsigned long new_offset;
	unsigned long new_count;
	char *context;
	char **lines;           /* Array of line strings (with +/- prefixes) */
	char **line_contents;   /* Array of clean content strings (without prefixes) */
	int *line_types;        /* Array of line types */
	int *line_contexts;     /* Array of line contexts (PATCH_CONTEXT_*) */
	unsigned long *orig_line_nums;  /* Original file line numbers */
	unsigned long *new_line_nums;   /* New file line numbers */
	int num_lines;
	int max_lines;
	int has_match;          /* Does this hunk contain matching lines? */
	int is_context_diff;    /* Is this a context diff hunk? */
	unsigned long header_line_number;  /* Line number where hunk header appears in input */
};

/* Buffered file structure */
struct buffered_file {
	char **header_lines;    /* Original header lines */
	int num_headers;
	char *best_filename;
	char *old_filename;     /* Original old filename from patch headers */
	char *new_filename;     /* Original new filename from patch headers */
	const char *patchname;
	unsigned long header_line;
	struct buffered_hunk *hunks;
	int num_hunks;
	int max_hunks;
	int has_match;          /* Does this file have any matching hunks? */
	int is_context_diff;
	char initial_status;    /* Initial file status from headers (+, -, !) */
	int orig_is_empty;      /* Is original file empty (from first hunk)? */
	int new_is_empty;       /* Is new file empty (from first hunk)? */
};

/* Forward declarations */
static void syntax(int err) __attribute__((noreturn));
static void process_patch_file(FILE *fp, const char *filename);
static void add_grep_pattern(const char *pattern);
static void add_patterns_from_file(const char *filename);
static int line_matches_patterns(const char *line);
static void init_buffered_file(struct buffered_file *file);
static void free_buffered_file(struct buffered_file *file);
static void init_buffered_hunk(struct buffered_hunk *hunk);
static void free_buffered_hunk(struct buffered_hunk *hunk);
static void add_hunk_line(struct buffered_hunk *hunk, const struct patch_hunk_line *line,
                          unsigned long orig_line, unsigned long new_line);
static void output_buffered_file(struct buffered_file *file);
static void output_hunk(struct buffered_file *file, struct buffered_hunk *hunk, int hunk_num);
static int line_passes_filter(int line_type, int line_context, const char *content);

static void syntax(int err)
{
	FILE *f = err ? stderr : stdout;

	fprintf(f, "Usage: %s [OPTION]... PATTERN [FILE]...\n", "grepdiff");
	fprintf(f, "Show files modified by patches containing a regexp.\n\n");
	fprintf(f, "Options:\n");
	fprintf(f, "  -s, --status                 show file additions (+), removals (-), and modifications (!)\n");
	fprintf(f, "  -n, --line-number            show line numbers\n");
	fprintf(f, "  -N, --number-files           show file numbers (for use with filterdiff --files)\n");
	fprintf(f, "  -H, --with-filename          show patch file names\n");
	fprintf(f, "  -h, --no-filename            suppress patch file names\n");
	fprintf(f, "  -p N, --strip-match=N        strip N leading path components\n");
	fprintf(f, "  --strip=N                    strip N leading path components from output\n");
	fprintf(f, "  --addprefix=PREFIX           add PREFIX to each filename\n");
	fprintf(f, "  --addoldprefix=PREFIX        add PREFIX to old filenames\n");
	fprintf(f, "  --addnewprefix=PREFIX        add PREFIX to new filenames\n");
	fprintf(f, "  --git-prefixes=strip|keep    handle a/ and b/ prefixes in Git diffs (default: keep)\n");
	fprintf(f, "  --git-extended-diffs=exclude|include\n");
	fprintf(f, "            process Git diffs without hunks: renames, copies, mode-only\n");
	fprintf(f, "            changes, binary files; default is include\n");
	fprintf(f, "  --output-matching=file|hunk  output mode: full files or matching hunks only\n");
	fprintf(f, "  --only-match=rem|add|mod|all show only removed, added, modified, or all matching lines\n");
	fprintf(f, "  --as-numbered-lines=before|after  show matching lines with line numbers\n");
	fprintf(f, "  -i PAT, --include=PAT        include only files matching PAT\n");
	fprintf(f, "  -x PAT, --exclude=PAT        exclude files matching PAT\n");
	fprintf(f, "  -v, --verbose                verbose output\n");
	fprintf(f, "  -z, --decompress             decompress .gz and .bz2 files\n");
	fprintf(f, "  -E, --extended-regexp        use extended regexps\n");
#ifdef HAVE_PCRE2POSIX_H
	fprintf(f, "                               (PCRE regexes are used by default)\n");
#endif
	fprintf(f, "  -f FILE, --file=FILE         read regular expressions from FILE\n");
	fprintf(f, "  --empty-files-as-absent      treat empty files as absent (with -s)\n");
	fprintf(f, "      --help                   display this help and exit\n");
	fprintf(f, "      --version                output version information and exit\n");
	fprintf(f, "\nReport bugs to <twaugh@redhat.com>.\n");

	exit(err);
}

static void add_grep_pattern(const char *pattern)
{
	if (num_grep_patterns >= max_grep_patterns) {
		max_grep_patterns = max_grep_patterns ? max_grep_patterns * 2 : 4;
		grep_patterns = xrealloc(grep_patterns, max_grep_patterns * sizeof(regex_t));
	}

	int flags = REG_NOSUB;
	if (extended_regexp) {
		flags |= REG_EXTENDED;
	}
#ifdef HAVE_PCRE2POSIX_H
	/* PCRE2 is available, use extended regex by default */
	flags |= REG_EXTENDED;
#endif

	int ret = regcomp(&grep_patterns[num_grep_patterns], pattern, flags);
	if (ret != 0) {
		char errbuf[256];
		regerror(ret, &grep_patterns[num_grep_patterns], errbuf, sizeof(errbuf));
		error(EXIT_FAILURE, 0, "invalid regex '%s': %s", pattern, errbuf);
	}

	num_grep_patterns++;
}

static void add_patterns_from_file(const char *filename)
{
	FILE *fp = xopen(filename, "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t read;

	while ((read = getline(&line, &len, fp)) != -1) {
		/* Remove trailing newline */
		if (read > 0 && line[read - 1] == '\n') {
			line[read - 1] = '\0';
			read--;
		}
		/* Skip empty lines */
		if (read == 0 || line[0] == '\0') {
			continue;
		}
		add_grep_pattern(line);
	}

	free(line);
	fclose(fp);
}

static int line_matches_patterns(const char *line)
{
	int i;

	for (i = 0; i < num_grep_patterns; i++) {
		if (regexec(&grep_patterns[i], line, 0, NULL, 0) == 0) {
			return 1;
		}
	}

	return 0;
}

static int line_passes_filter(int line_type, int line_context, const char *content)
{
	if (!line_matches_patterns(content)) {
		return 0;
	}

	switch (match_filter) {
	case MATCH_ALL:
		return 1;
	case MATCH_REMOVALS:
		return (line_type == PATCH_LINE_REMOVED) ||
		       (line_type == PATCH_LINE_CHANGED && line_context == PATCH_CONTEXT_OLD);
	case MATCH_ADDITIONS:
		return (line_type == PATCH_LINE_ADDED) ||
		       (line_type == PATCH_LINE_CHANGED && line_context == PATCH_CONTEXT_NEW);
	case MATCH_MODIFICATIONS:
		return (line_type == PATCH_LINE_CHANGED) ||
		       (line_type == PATCH_LINE_REMOVED);
	}
	return 0;
}


static void init_buffered_file(struct buffered_file *file)
{
	memset(file, 0, sizeof(*file));
}

static void free_buffered_file(struct buffered_file *file)
{
	int i;

	if (file->header_lines) {
		for (i = 0; i < file->num_headers; i++) {
			free(file->header_lines[i]);
		}
		free(file->header_lines);
	}

	if (file->best_filename) {
		free(file->best_filename);
	}

	if (file->old_filename) {
		free(file->old_filename);
	}

	if (file->new_filename) {
		free(file->new_filename);
	}

	if (file->hunks) {
		for (i = 0; i < file->num_hunks; i++) {
			free_buffered_hunk(&file->hunks[i]);
		}
		free(file->hunks);
	}

	memset(file, 0, sizeof(*file));
}

static void init_buffered_hunk(struct buffered_hunk *hunk)
{
	memset(hunk, 0, sizeof(*hunk));
}

static void free_buffered_hunk(struct buffered_hunk *hunk)
{
	int i;

	if (hunk->context) {
		free(hunk->context);
	}

	if (hunk->lines) {
		for (i = 0; i < hunk->num_lines; i++) {
			free(hunk->lines[i]);
		}
		free(hunk->lines);
	}

	if (hunk->line_contents) {
		for (i = 0; i < hunk->num_lines; i++) {
			free(hunk->line_contents[i]);
		}
		free(hunk->line_contents);
	}

	if (hunk->line_types) {
		free(hunk->line_types);
	}

	if (hunk->line_contexts) {
		free(hunk->line_contexts);
	}

	if (hunk->orig_line_nums) {
		free(hunk->orig_line_nums);
	}

	if (hunk->new_line_nums) {
		free(hunk->new_line_nums);
	}

	memset(hunk, 0, sizeof(*hunk));
}

static void add_hunk_line(struct buffered_hunk *hunk, const struct patch_hunk_line *line,
                          unsigned long orig_line, unsigned long new_line)
{
	if (hunk->num_lines >= hunk->max_lines) {
		hunk->max_lines = hunk->max_lines ? hunk->max_lines * 2 : 16;
		hunk->lines = xrealloc(hunk->lines, hunk->max_lines * sizeof(char *));
		hunk->line_contents = xrealloc(hunk->line_contents, hunk->max_lines * sizeof(char *));
		hunk->line_types = xrealloc(hunk->line_types, hunk->max_lines * sizeof(int));
		hunk->line_contexts = xrealloc(hunk->line_contexts, hunk->max_lines * sizeof(int));
		hunk->orig_line_nums = xrealloc(hunk->orig_line_nums, hunk->max_lines * sizeof(unsigned long));
		hunk->new_line_nums = xrealloc(hunk->new_line_nums, hunk->max_lines * sizeof(unsigned long));
	}

	/* Use full line from scanner (includes prefix, excludes newline) */
	hunk->lines[hunk->num_lines] = xstrndup(line->line, line->length);
	/* Store clean content from scanner (excludes prefix and format-specific spaces) */
	hunk->line_contents[hunk->num_lines] = xstrndup(line->content, line->content_length);
	hunk->line_types[hunk->num_lines] = line->type;
	hunk->line_contexts[hunk->num_lines] = line->context;
	hunk->orig_line_nums[hunk->num_lines] = orig_line;
	hunk->new_line_nums[hunk->num_lines] = new_line;
	hunk->num_lines++;
}


static void process_patch_file(FILE *fp, const char *filename)
{
	patch_scanner_t *scanner;
	const patch_content_t *content;
	enum patch_scanner_result result;
	struct buffered_file current_file;
	struct buffered_hunk *current_hunk = NULL;
	unsigned long orig_line = 0, new_line = 0;
	int i;

	init_buffered_file(&current_file);

	scanner = patch_scanner_create(fp);
	if (!scanner) {
		error(EXIT_FAILURE, 0, "Failed to create patch scanner");
		return;
	}

	while ((result = patch_scanner_next(scanner, &content)) == PATCH_SCAN_OK) {
		if (content->type == PATCH_CONTENT_HEADERS) {
			/* If we have a buffered file, output it now */
			if (current_file.best_filename) {
				output_buffered_file(&current_file);
				free_buffered_file(&current_file);
				init_buffered_file(&current_file);
			}

			filecount++;
			file_number++;

			/* Get best filename */
			char *best_filename = get_best_filename(content->data.headers, git_prefix_mode,
			                                        strip_output_components, add_prefix,
			                                        add_old_prefix, add_new_prefix);

			/* Check if we should process this file */
			if (!should_display_file(best_filename)) {
				free(best_filename);
				continue;
			}

			/* Store file information */
			current_file.best_filename = best_filename;
			current_file.old_filename = content->data.headers->old_name ? xstrdup(content->data.headers->old_name) : NULL;
			current_file.new_filename = content->data.headers->new_name ? xstrdup(content->data.headers->new_name) : NULL;
			current_file.patchname = filename;
			current_file.header_line = global_line_offset + content->data.headers->start_line;
			current_file.is_context_diff = (content->data.headers->type == PATCH_TYPE_CONTEXT);

			/* Determine initial status from headers (for -s/--status) */
			if (show_status) {
				current_file.initial_status = determine_file_status(content->data.headers, empty_files_as_absent);
				/* Initialize empty file tracking - assume empty until we see hunks */
				current_file.orig_is_empty = 1;
				current_file.new_is_empty = 1;
			}

			/* Copy header lines for file/hunk output modes */
			if (output_mode != OUTPUT_LIST) {
				const struct patch_headers *hdrs = content->data.headers;
				current_file.num_headers = hdrs->num_headers;
				current_file.header_lines = xmalloc(hdrs->num_headers * sizeof(char *));
				for (i = 0; i < hdrs->num_headers; i++) {
					current_file.header_lines[i] = xstrdup(hdrs->header_lines[i]);
				}
			}

			current_hunk = NULL;
		} else if (content->type == PATCH_CONTENT_HUNK_HEADER) {
			const struct patch_hunk *hunk = content->data.hunk;

			/* Add new hunk to current file */
			if (current_file.num_hunks >= current_file.max_hunks) {
				current_file.max_hunks = current_file.max_hunks ? current_file.max_hunks * 2 : 4;
				current_file.hunks = xrealloc(current_file.hunks,
				                              current_file.max_hunks * sizeof(struct buffered_hunk));
			}

			current_hunk = &current_file.hunks[current_file.num_hunks];
			init_buffered_hunk(current_hunk);
			current_file.num_hunks++;

			current_hunk->orig_offset = hunk->orig_offset;
			current_hunk->orig_count = hunk->orig_count;
			current_hunk->new_offset = hunk->new_offset;
			current_hunk->new_count = hunk->new_count;
			current_hunk->is_context_diff = current_file.is_context_diff;
			current_hunk->header_line_number = global_line_offset + content->line_number;
			if (hunk->context) {
				current_hunk->context = xstrdup(hunk->context);
			}

			/* Track empty files from first hunk only (for --empty-files-as-absent) */
			if (show_status && current_file.num_hunks == 1) {
				if (hunk->orig_count > 0) {
					current_file.orig_is_empty = 0;
				}
				if (hunk->new_count > 0) {
					current_file.new_is_empty = 0;
				}
			}

			/* Initialize line number tracking */
			orig_line = hunk->orig_offset;
			new_line = hunk->new_offset;
		} else if (content->type == PATCH_CONTENT_HUNK_LINE) {
			const struct patch_hunk_line *line = content->data.line;

			if (!current_hunk) {
				continue;  /* Shouldn't happen, but be defensive */
			}

		/* Check if this line matches grep patterns and passes match filter */
		char *temp_content = xstrndup(line->content, line->content_length);
		int passes_filter = line_passes_filter(line->type, line->context, temp_content);
		free(temp_content);

		if (passes_filter) {
			current_hunk->has_match = 1;
			current_file.has_match = 1;
		}

			/* Store the line if we're in file/hunk output mode */
			if (output_mode != OUTPUT_LIST) {
				add_hunk_line(current_hunk, line, orig_line, new_line);
			}

			/* Track line numbers */
			switch (line->type) {
			case PATCH_LINE_CONTEXT:
				orig_line++;
				new_line++;
				break;
			case PATCH_LINE_REMOVED:
				orig_line++;
				break;
			case PATCH_LINE_ADDED:
				new_line++;
				break;
			case PATCH_LINE_CHANGED:
				/* In context diffs, ! lines increment based on their context */
				if (line->context == PATCH_CONTEXT_OLD) {
					orig_line++;
				} else if (line->context == PATCH_CONTEXT_NEW) {
					new_line++;
				} else {
					/* PATCH_CONTEXT_BOTH - shouldn't happen for ! lines, but handle it */
					orig_line++;
					new_line++;
				}
				break;
			default:
				break;
			}
		} else if (content->type == PATCH_CONTENT_NO_NEWLINE) {
			/* Add "\ No newline at end of file" marker if buffering */
			if (output_mode != OUTPUT_LIST && current_hunk) {
				/* Create temporary patch_hunk_line for NO_NEWLINE marker */
				struct patch_hunk_line no_newline_marker;
				no_newline_marker.type = PATCH_LINE_NO_NEWLINE;
				no_newline_marker.line = content->data.no_newline.line;
				size_t raw_len = content->data.no_newline.length;
				/* Strip trailing newline if present */
				if (raw_len > 0 && content->data.no_newline.line[raw_len - 1] == '\n') {
					no_newline_marker.length = raw_len - 1;
				} else {
					no_newline_marker.length = raw_len;
				}
				no_newline_marker.position = content->position;
				add_hunk_line(current_hunk, &no_newline_marker, 0, 0);
			}
		}
	}

	/* Handle final buffered file */
	if (current_file.best_filename) {
		output_buffered_file(&current_file);
		free_buffered_file(&current_file);
	}

	if (result == PATCH_SCAN_ERROR) {
		if (verbose)
			fprintf(stderr, "Warning: Error parsing patch in %s\n", filename);
	}

	/* Update global line offset for next file */
	global_line_offset += patch_scanner_line_number(scanner) - 1;

	patch_scanner_destroy(scanner);
}

static void output_buffered_file(struct buffered_file *file)
{
	int i;

	if (!file || !file->best_filename) {
		return;
	}

	/* In list mode, just print filename if it has matches */
	if (output_mode == OUTPUT_LIST) {
		if (file->has_match) {
			/* Calculate final status for -s/--status */
			if (show_status) {
				char final_status = file->initial_status;

				/* Adjust status based on --empty-files-as-absent */
				if (empty_files_as_absent) {
					int orig_absent = (file->orig_is_empty != 0);
					int new_absent = (file->new_is_empty != 0);

					if (orig_absent && !new_absent) {
						final_status = '+';  /* Treat as file addition */
					} else if (!orig_absent && new_absent) {
						final_status = '-';  /* Treat as file deletion */
					} else if (!orig_absent && !new_absent) {
						final_status = '!';  /* Treat as modification */
					}
					/* If both absent, skip the file (shouldn't normally happen) */
					if (orig_absent && new_absent) {
						return;
					}
				}

				/* Display with status prefix */
				display_filename_extended(file->best_filename, file->patchname, file->header_line,
				                         final_status, show_status);
			} else {
				display_filename(file->best_filename, file->patchname, file->header_line);
			}

			/* In verbose mode with line numbers, show hunk information */
			if (verbose > 0 && show_line_numbers) {
				for (i = 0; i < file->num_hunks; i++) {
					if (file->hunks[i].has_match) {
						/* Show patch name prefix with '-' suffix for hunk lines */
						if (show_patch_names > 0)
							printf("%s-", file->patchname);

						/* Use the actual hunk header line number from the scanner */
						printf("\t%lu\tHunk #%d", file->hunks[i].header_line_number, i + 1);

						if (verbose > 1 && file->hunks[i].context) {
							printf("\t%s", file->hunks[i].context);
						}
						printf("\n");
					}
				}
			}
		}
		return;
	}

	/* For file/hunk output modes, only output if there's a match */
	if (!file->has_match) {
		return;
	}

	/* Special handling for numbered line mode */
	if (numbered_mode != NUMBERED_NONE) {
		/* Output diff headers, but filter to show only the appropriate file header based on mode */
		for (i = 0; i < file->num_headers; i++) {
			const char *line = file->header_lines[i];

			/* Always output non-file headers (diff --git, index, etc.) */
			if (strncmp(line, "--- ", 4) != 0 && strncmp(line, "+++ ", 4) != 0 &&
			    strncmp(line, "*** ", 4) != 0) {
				printf("%s", line);
			}
			/* For file headers, only output the one appropriate for the mode */
			else if (numbered_mode == NUMBERED_BEFORE || numbered_mode == NUMBERED_ORIGINAL_BEFORE) {
				/* For before modes, output old file headers */
				if (file->is_context_diff) {
					/* In context diffs: *** is old, --- is new */
					if (strncmp(line, "*** ", 4) == 0) {
						printf("%s", line);
					}
				} else {
					/* In unified diffs: --- is old, +++ is new */
					if (strncmp(line, "--- ", 4) == 0) {
						printf("%s", line);
					}
				}
			} else { /* NUMBERED_AFTER or NUMBERED_ORIGINAL_AFTER */
				/* For after modes, output new file headers */
				if (file->is_context_diff) {
					/* In context diffs: *** is old, --- is new */
					if (strncmp(line, "--- ", 4) == 0) {
						printf("%s", line);
					}
				} else {
					/* In unified diffs: --- is old, +++ is new */
					if (strncmp(line, "+++ ", 4) == 0) {
						printf("%s", line);
					}
				}
			}
		}

		/* Collect all lines from hunks that contain matches, showing only lines that exist in the target timeframe */
		struct {
			unsigned long linenum;
			char *content;
		} *display_lines = NULL;
		int num_display = 0;
		int max_display = 0;

		for (i = 0; i < file->num_hunks; i++) {
			struct buffered_hunk *hunk = &file->hunks[i];
			int j;
			int hunk_has_match = 0;

			/* Check if this hunk contains any matches */
			if (output_mode == OUTPUT_HUNK) {
				hunk_has_match = hunk->has_match;
			} else {
				/* For file mode, include hunk if the file has any matches */
				hunk_has_match = file->has_match;
			}

			if (!hunk_has_match) {
				continue;
			}

			/* Add separator for hunks after the first */
			if (num_display > 0) {
				if (num_display >= max_display) {
					max_display = max_display ? max_display * 2 : 16;
					display_lines = xrealloc(display_lines,
					                        max_display * sizeof(*display_lines));
				}
				display_lines[num_display].linenum = 0;  /* Special marker for separator */
				display_lines[num_display].content = xstrdup("...");
				num_display++;
			}

			/* Add lines from this hunk based on the numbered mode */
			/* For NUMBERED_AFTER mode in hunk output, we need to renumber the new lines to start from the original offset */
			unsigned long renumbered_line = hunk->orig_offset;

			for (j = 0; j < hunk->num_lines; j++) {
				int line_type = hunk->line_types[j];
				const char *line_content = hunk->line_contents[j];  /* Use clean content */
				unsigned long linenum;
				int should_include = 0;

				/* Determine if we should include this line based on numbered_mode */
				if (numbered_mode == NUMBERED_BEFORE) {
					/* Show lines as they exist before the patch */
					if ((line_type == PATCH_LINE_REMOVED) ||
					    (line_type == PATCH_LINE_CONTEXT) ||
					    (line_type == PATCH_LINE_CHANGED && hunk->line_contexts[j] == PATCH_CONTEXT_OLD)) {
						should_include = 1;
						linenum = hunk->orig_line_nums[j];
					}
				} else if (numbered_mode == NUMBERED_AFTER) {
					/* Show lines as they exist after the patch */
					if ((line_type == PATCH_LINE_ADDED) ||
					    (line_type == PATCH_LINE_CONTEXT) ||
					    (line_type == PATCH_LINE_CHANGED && hunk->line_contexts[j] == PATCH_CONTEXT_NEW)) {
						should_include = 1;
						if (output_mode == OUTPUT_HUNK) {
							/* For hunk mode, use renumbered line numbers that start from the original offset */
							linenum = renumbered_line;
							renumbered_line++;
						} else {
							/* For file mode, use actual new file line numbers */
							linenum = hunk->new_line_nums[j];
						}
					}
				} else if (numbered_mode == NUMBERED_ORIGINAL_BEFORE) {
					/* Show lines with original line numbers from diff (before) */
					if ((line_type == PATCH_LINE_REMOVED) ||
					    (line_type == PATCH_LINE_CONTEXT) ||
					    (line_type == PATCH_LINE_CHANGED && hunk->line_contexts[j] == PATCH_CONTEXT_OLD)) {
						should_include = 1;
						/* Use original hunk offset from diff header */
						linenum = hunk->orig_offset;
					}
				} else { /* NUMBERED_ORIGINAL_AFTER */
					/* Show lines with original line numbers from diff (after) */
					if ((line_type == PATCH_LINE_ADDED) ||
					    (line_type == PATCH_LINE_CONTEXT) ||
					    (line_type == PATCH_LINE_CHANGED && hunk->line_contexts[j] == PATCH_CONTEXT_NEW)) {
						should_include = 1;
						/* Use original hunk offset from diff header */
						linenum = hunk->new_offset;
					}
				}

				if (should_include) {
					if (num_display >= max_display) {
						max_display = max_display ? max_display * 2 : 16;
						display_lines = xrealloc(display_lines,
						                        max_display * sizeof(*display_lines));
					}
					display_lines[num_display].linenum = linenum;
					display_lines[num_display].content = xstrdup(line_content);
					num_display++;
				}
			}
		}

		/* Output all collected lines */
		for (i = 0; i < num_display; i++) {
			if (display_lines[i].linenum == 0) {
				/* Separator line */
				printf("%s\n", display_lines[i].content);
			} else {
				printf("%lu\t:%s\n", display_lines[i].linenum, display_lines[i].content);
			}
		}

		/* Clean up */
		for (i = 0; i < num_display; i++) {
			free(display_lines[i].content);
		}
		free(display_lines);
		return;
	}

	/* Output headers */
	for (i = 0; i < file->num_headers; i++) {
		/* Header lines from scanner already include newlines */
		printf("%s", file->header_lines[i]);
		/* Add newline if the header line doesn't end with one */
		size_t len = strlen(file->header_lines[i]);
		if (len == 0 || file->header_lines[i][len - 1] != '\n') {
			printf("\n");
		}
	}

	/* Output hunks */
	for (i = 0; i < file->num_hunks; i++) {
		if (output_mode == OUTPUT_HUNK && !file->hunks[i].has_match) {
			continue;  /* Skip non-matching hunks in hunk mode */
		}

		/* Add context diff separator before each hunk */
		if (file->is_context_diff) {
			printf("***************\n");
		}

		output_hunk(file, &file->hunks[i], i + 1);
	}
}

static void output_hunk(struct buffered_file *file, struct buffered_hunk *hunk, int hunk_num)
{
	int i;
	unsigned long renumbered_new_offset;

	/* For numbered line mode, don't output hunk headers/structure */
	if (numbered_mode != NUMBERED_NONE) {
		for (i = 0; i < hunk->num_lines; i++) {
			int line_type = hunk->line_types[i];
			const char *line_content = hunk->line_contents[i];  /* Use clean content */

			/* Check match filter */
			int should_show = line_passes_filter(line_type, hunk->line_contexts[i], line_content);

			if (should_show) {
				unsigned long linenum;
				if (numbered_mode == NUMBERED_BEFORE) {
					linenum = hunk->orig_line_nums[i];
				} else if (numbered_mode == NUMBERED_AFTER) {
					linenum = hunk->new_line_nums[i];
				} else if (numbered_mode == NUMBERED_ORIGINAL_BEFORE) {
					linenum = hunk->orig_offset;
				} else { /* NUMBERED_ORIGINAL_AFTER */
					linenum = hunk->new_offset;
				}
				printf("%lu\t:%s\n", linenum, line_content);
			}
		}
		return;
	}

	/* In hunk output mode, renumber the new offset to match the original offset */
	/* This is because each hunk is output independently, so the new file starts at the same line */
	renumbered_new_offset = (output_mode == OUTPUT_HUNK) ? hunk->orig_offset : hunk->new_offset;

	/* Output hunk header and lines */
	if (hunk->is_context_diff) {
		/* Context diff format: output old header, old lines, new header, new lines */

		/* Output old section header */
		if (hunk->orig_count == 1) {
			printf("*** %lu ****\n", hunk->orig_offset);
		} else {
			printf("*** %lu,%lu ****\n", hunk->orig_offset,
			       hunk->orig_offset + hunk->orig_count - 1);
		}

		/* Output old section lines */
		for (i = 0; i < hunk->orig_count && i < hunk->num_lines; i++) {
			const char *line = hunk->lines[i];

			printf("%s\n", line);
		}

		/* Output new section header */
		if (hunk->new_count == 1) {
			printf("--- %lu ----\n", renumbered_new_offset);
		} else {
			printf("--- %lu,%lu ----\n", renumbered_new_offset,
			       renumbered_new_offset + hunk->new_count - 1);
		}

		/* Output new section lines */
		for (i = hunk->orig_count; i < hunk->num_lines; i++) {
			const char *line = hunk->lines[i];

			printf("%s\n", line);
		}
	} else {
		/* Unified diff format */
		printf("@@ -");
		if (hunk->orig_count == 1) {
			printf("%lu", hunk->orig_offset);
		} else {
			printf("%lu,%lu", hunk->orig_offset, hunk->orig_count);
		}
		printf(" +");
		if (hunk->new_count == 1) {
			printf("%lu", renumbered_new_offset);
		} else {
			printf("%lu,%lu", renumbered_new_offset, hunk->new_count);
		}
		printf(" @@");
		if (hunk->context) {
			printf(" %s", hunk->context);
		}
		printf("\n");

		/* Output unified diff lines */
		for (i = 0; i < hunk->num_lines; i++) {
			const char *line = hunk->lines[i];

			printf("%s\n", line);
		}
	}

}

int run_grep_mode(int argc, char *argv[])
{
	int i;
	FILE *fp;

	/* Initialize common options */
	init_common_options();

	setlocale(LC_TIME, "C");

	while (1) {
		static struct option long_options[MAX_TOTAL_OPTIONS];
		int next_idx = 0;

		/* Add common long options */
		add_common_long_options(long_options, &next_idx);

		/* Add tool-specific long options */
		long_options[next_idx++] = (struct option){"help", 0, 0, 1000 + 'H'};
		long_options[next_idx++] = (struct option){"version", 0, 0, 1000 + 'V'};
		long_options[next_idx++] = (struct option){"status", 0, 0, 's'};
		long_options[next_idx++] = (struct option){"extended-regexp", 0, 0, 'E'};
		long_options[next_idx++] = (struct option){"file", 1, 0, 'f'};
		long_options[next_idx++] = (struct option){"output-matching", 1, 0, 1000 + 'M'};
		long_options[next_idx++] = (struct option){"only-match", 1, 0, 1000 + 'm'};
		long_options[next_idx++] = (struct option){"as-numbered-lines", 1, 0, 1000 + 'L'};
		long_options[next_idx++] = (struct option){"empty-files-as-absent", 0, 0, 1000 + 'e'};
		/* Mode options (handled by patchfilter, but need to be recognized) */
		long_options[next_idx++] = (struct option){"list", 0, 0, 1000 + 'l'};
		long_options[next_idx++] = (struct option){"filter", 0, 0, 1000 + 'F'};
		long_options[next_idx++] = (struct option){"grep", 0, 0, 1000 + 'g'};
		long_options[next_idx++] = (struct option){0, 0, 0, 0};

		/* Safety check: ensure we haven't exceeded MAX_TOTAL_OPTIONS */
		if (next_idx > MAX_TOTAL_OPTIONS) {
			error(EXIT_FAILURE, 0, "Internal error: too many total options (%d > %d). "
			      "Increase MAX_TOTAL_OPTIONS in patch_common.h", next_idx, MAX_TOTAL_OPTIONS);
		}

		/* Combine common and tool-specific short options */
		char short_options[64];
		snprintf(short_options, sizeof(short_options), "%ssEf:", get_common_short_options());

		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if (c == -1)
			break;

		/* Try common option parsing first */
		if (parse_common_option(c, optarg)) {
			continue;
		}

		/* Handle tool-specific options */
		switch (c) {
		case 1000 + 'H':
			syntax(0);
			break;
		case 1000 + 'V':
			printf("grepdiff - patchutils version %s\n", VERSION);
			exit(0);
		case 's':
			show_status = 1;
			break;
		case 'E':
			extended_regexp = 1;
			break;
		case 'f':
			add_patterns_from_file(optarg);
			break;
		case 1000 + 'e':
			empty_files_as_absent = 1;
			break;
		case 1000 + 'M':
			if (!strncmp(optarg, "file", 4)) {
				output_mode = OUTPUT_FILE;
			} else if (!strncmp(optarg, "hunk", 4)) {
				output_mode = OUTPUT_HUNK;
			} else {
				error(EXIT_FAILURE, 0, "invalid argument to --output-matching: %s (expected 'file' or 'hunk')", optarg);
			}
			break;
		case 1000 + 'm':
			if (!strncmp(optarg, "all", 3)) {
				match_filter = MATCH_ALL;
			} else if (!strncmp(optarg, "rem", 3) || !strncmp(optarg, "removal", 7)) {
				match_filter = MATCH_REMOVALS;
			} else if (!strncmp(optarg, "add", 3) || !strncmp(optarg, "addition", 8)) {
				match_filter = MATCH_ADDITIONS;
			} else if (!strncmp(optarg, "mod", 3) || !strncmp(optarg, "modification", 12)) {
				match_filter = MATCH_MODIFICATIONS;
			} else {
				error(EXIT_FAILURE, 0, "invalid argument to --only-match: %s (expected 'rem', 'add', 'mod', or 'all')", optarg);
			}
			break;
		case 1000 + 'L':
			if (!strncmp(optarg, "original-before", 15)) {
				numbered_mode = NUMBERED_ORIGINAL_BEFORE;
			} else if (!strncmp(optarg, "original-after", 14)) {
				numbered_mode = NUMBERED_ORIGINAL_AFTER;
			} else if (!strncmp(optarg, "before", 6)) {
				numbered_mode = NUMBERED_BEFORE;
			} else if (!strncmp(optarg, "after", 5)) {
				numbered_mode = NUMBERED_AFTER;
			} else {
				error(EXIT_FAILURE, 0, "invalid argument to --as-numbered-lines: %s (expected 'before', 'after', 'original-before', or 'original-after')", optarg);
			}
			break;
		case 1000 + 'l':
		case 1000 + 'F':
		case 1000 + 'g':
			/* Mode options - handled by patchfilter, ignore here */
			break;
		default:
			syntax(1);
		}
	}

	/* At least one pattern is required (either from command line or -f) */
	if (num_grep_patterns == 0) {
		/* First non-option argument is the pattern */
		if (optind >= argc) {
			fprintf(stderr, "grepdiff: missing pattern\n");
			syntax(1);
		}
		add_grep_pattern(argv[optind++]);
	}

	/* Determine show_patch_names default */
	if (show_patch_names == -1) {
		show_patch_names = (optind + 1 < argc) ? 1 : 0;
	}

	/* Handle -p without -i/-x: print warning and use as --strip */
	if (strip_components > 0 && strip_output_components == 0 && !pat_include && !pat_exclude) {
		fprintf(stderr, "-p given without -i or -x; guessing that you meant --strip instead.\n");
		strip_output_components = strip_components;
	}

	/* Process input files */
	if (optind >= argc) {
		/* Read from stdin */
		process_patch_file(stdin, "(standard input)");
	} else {
		/* Process each file */
		for (i = optind; i < argc; i++) {
			if (unzip) {
				fp = xopen_unzip(argv[i], "rb");
			} else {
				fp = xopen(argv[i], "r");
			}

			process_patch_file(fp, argv[i]);
			fclose(fp);
		}
	}

	/* Clean up */
	cleanup_common_options();
	if (grep_patterns) {
		for (i = 0; i < num_grep_patterns; i++) {
			regfree(&grep_patterns[i]);
		}
		free(grep_patterns);
	}

	return 0;
}
