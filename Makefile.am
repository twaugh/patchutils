ACLOCAL_AMFLAGS = -I m4
SUBDIRS = lib

DISTCLEANFILES = src/stamp-h[0-9]* src/config.h

bin_PROGRAMS = src/interdiff src/filterdiff src/rediff
bin_SCRIPTS = \
	scripts/fixcvsdiff \
	scripts/splitdiff \
	scripts/editdiff \
	scripts/recountdiff \
	scripts/unwrapdiff \
	scripts/dehtmldiff \
	scripts/move-to-front \
	scripts/espdiff

dist_bin_SCRIPTS = \
	patchview/patchview-wrapper$(EXEEXT)

# Bash completion files
bashcompletiondir = $(datadir)/bash-completion/completions
dist_bashcompletion_DATA = bash-completion-patchutils

AM_CFLAGS = -I$(srcdir)/src -I$(top_builddir)/lib -I$(top_srcdir)/lib
src_interdiff_SOURCES = src/interdiff.c src/util.c src/util.h src/diff.c \
		src/diff.h
src_filterdiff_SOURCES = src/filterdiff.c src/util.c src/util.h src/diff.c \
		src/diff.h
src_rediff_SOURCES = src/rediff.c src/util.c src/util.h src/diff.c src/diff.h

src_interdiff_LDADD = lib/libgnu.a @LIBOBJS@
src_filterdiff_LDADD = lib/libgnu.a @LIBOBJS@
src_rediff_LDADD = lib/libgnu.a @LIBOBJS@

if HAVE_XMLTO
# The man pages are generated from DocBook XML.
interdiff_manpage = doc/interdiff.1
other_manpages = doc/filterdiff.1 doc/fixcvsdiff.1 doc/rediff.1 \
	doc/editdiff.1 doc/combinediff.1 doc/lsdiff.1 doc/splitdiff.1 \
	doc/grepdiff.1 doc/recountdiff.1 doc/unwrapdiff.1 doc/dehtmldiff.1 \
	doc/flipdiff.1 doc/espdiff.1 doc/patchview.1 \
	doc/gitdiff.1 doc/gitdiffview.1 doc/gitshow.1 doc/gitshowview.1 \
	doc/svndiff.1 doc/svndiffview.1
man_MANS = $(interdiff_manpage) $(other_manpages)

$(other_manpages): $(interdiff_manpage)
$(interdiff_manpage): doc/patchutils.xml
	$(XMLTO) man -o doc $<
else
man_MANS =
endif

# Special rules for combinediff, flipdiff, lsdiff, grepdiff and patchview, which are
# just symlinks.

interdiff_links = \
	src/combinediff$(EXEEXT) \
	src/flipdiff$(EXEEXT)

filterdiff_links = \
	src/lsdiff$(EXEEXT) \
	src/grepdiff$(EXEEXT) \
	src/patchview$(EXEEXT)

patchview_links = \
	patchview/gitdiff$(EXEEXT) \
	patchview/gitdiffview$(EXEEXT) \
	patchview/gitshow$(EXEEXT) \
	patchview/gitshowview$(EXEEXT) \
	patchview/svndiff$(EXEEXT) \
	patchview/svndiffview$(EXEEXT)

$(interdiff_links): src/interdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

$(filterdiff_links): src/filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

$(patchview_links): patchview/patchview-wrapper$(EXEEXT)
	ln -sf $(notdir $<) $@

install-exec-hook:
	@for f in $(interdiff_links); do \
	    ln -sf "`echo interdiff$(EXEEXT) | sed '$(transform)'`" \
	             "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done
	@for f in $(filterdiff_links); do \
	    ln -sf "`echo filterdiff$(EXEEXT) | sed '$(transform)'`" \
	             "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done
	@for f in $(patchview_links); do \
	    ln -sf "`echo patchview-wrapper$(EXEEXT) | sed '$(transform)'`" \
	             "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done

install-data-hook:
	if [ -d "$(DESTDIR)$(bashcompletiondir)" ]; then \
		cd "$(DESTDIR)$(bashcompletiondir)" && \
		mv bash-completion-patchutils interdiff && \
		for cmd in filterdiff lsdiff grepdiff combinediff flipdiff rediff \
		           splitdiff recountdiff unwrapdiff dehtmldiff editdiff espdiff \
		           fixcvsdiff patchview gitdiff svndiff gitdiffview svndiffview; do \
			ln -sf interdiff "$$cmd" || true; \
		done; \
	fi

uninstall-local:
	@for f in $(interdiff_links); do \
	    rm -f "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done
	@for f in $(filterdiff_links); do \
	    rm -f "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done
	@for f in $(patchview_links); do \
	    rm -f "$(DESTDIR)$(bindir)/`basename $$f | sed '$(transform)'`"; \
	done
	if [ -d "$(DESTDIR)$(bashcompletiondir)" ]; then \
		cd "$(DESTDIR)$(bashcompletiondir)" && \
		for cmd in filterdiff lsdiff grepdiff interdiff combinediff flipdiff rediff \
		           splitdiff recountdiff unwrapdiff dehtmldiff editdiff espdiff \
		           fixcvsdiff patchview gitdiff svndiff gitdiffview svndiffview; do \
			rm -f "$$cmd"; \
		done; \
		rm -f patchutils; \
	fi

CLEANFILES = $(interdiff_links) $(filterdiff_links) $(patchview_links)
MAINTAINERCLEANFILES=$(man_MANS)

# Regression tests.
TESTS_ENVIRONMENT = top_srcdir=$(top_srcdir) top_builddir=$(top_builddir) \
	ARENA=$(top_builddir)/test-arena \
	DIFF=$(DIFF) PATCH=$(PATCH)
TESTS = tests/newline1/run-test \
	tests/newline2/run-test \
	tests/newline3/run-test \
	tests/newline4/run-test \
	tests/newline5/run-test \
	tests/newline6/run-test \
	tests/create1/run-test \
	tests/broken1/run-test \
	tests/revert1/run-test \
	tests/revert2/run-test \
	tests/dashes/run-test \
	tests/dashes1/run-test \
	tests/dashes2/run-test \
	tests/dashes3/run-test \
	tests/dashes4/run-test \
	tests/linux-2.4.0-test-pre-style/run-test \
	tests/linux-2.2.17-pre4-5/run-test \
	tests/linux-2.4.13-ac6-7/run-test \
	tests/apply1/run-test \
	tests/copy1/run-test \
	tests/edit1/run-test \
	tests/edit2/run-test \
	tests/edit3/run-test \
	tests/edit4/run-test \
	tests/edit5/run-test \
	tests/edit6/run-test \
	tests/edit7/run-test \
	tests/edit8/run-test \
	tests/edit9/run-test \
	tests/edit10/run-test \
	tests/combine1/run-test \
	tests/combine2/run-test \
	tests/combine3/run-test \
	tests/combine4/run-test \
	tests/combine5/run-test \
	tests/combine6/run-test \
	tests/combine-devnull/run-test \
	tests/combine-devnull-p/run-test \
	tests/combine-no-match/run-test \
	tests/interdiff-devnull/run-test \
	tests/interdiff-reverse-issue18/run-test \
	tests/flipdiff-devnull/run-test \
	tests/gendiff1/run-test \
	tests/gendiff2/run-test \
	tests/comma/run-test \
	tests/addhunk1/run-test \
	tests/addhunk2/run-test \
	tests/addhunk3/run-test \
	tests/delhunk1/run-test \
	tests/delhunk2/run-test \
	tests/delhunk3/run-test \
	tests/delhunk4/run-test \
	tests/delhunk5/run-test \
	tests/delhunk6/run-test \
	tests/trimcontext1/run-test \
	tests/trimlast1/run-test \
	tests/trimlast2/run-test \
	tests/nodate/run-test \
	tests/lsdiff1/run-test \
	tests/lsdiff2/run-test \
	tests/lsdiff3/run-test \
	tests/lsdiff4/run-test \
	tests/lsdiff5/run-test \
	tests/lsdiff6/run-test \
	tests/lsdiff7/run-test \
	tests/lsdiff8/run-test \
	tests/lsdiff9/run-test \
	tests/lsdiff10/run-test \
	tests/lsdiff11/run-test \
	tests/lsdiff12/run-test \
	tests/lsdiff13/run-test \
	tests/lsdiff14/run-test \
	tests/lsdiff15/run-test \
	tests/lsdiff-hunks-option/run-test \
	tests/lsdiff-lines-option/run-test \
	tests/lsdiff-exclusion-combined/run-test \
	tests/lsdiff-verbose-levels/run-test \
	tests/lsdiff-range-exclude/run-test \
	tests/patchview1/run-test \
	tests/patchview2/run-test \
	tests/fuzz1/run-test \
	tests/U0/run-test \
	tests/U3/run-test \
	tests/U5/run-test \
	tests/grepdiff1/run-test \
	tests/grepdiff2/run-test \
	tests/grepdiff3/run-test \
	tests/grepdiff4/run-test \
	tests/grepdiff5/run-test \
	tests/grepdiff6/run-test \
	tests/grepdiff7/run-test \
	tests/grepdiff8/run-test \
	tests/grepdiff9/run-test \
	tests/grepdiff-original-line-numbers/run-test \
	tests/grepdiff-status/run-test \
	tests/number1/run-test \
	tests/number2/run-test \
	tests/number3/run-test \
	tests/nondiff/run-test \
	tests/nondiff1/run-test \
	tests/lscontext1/run-test \
	tests/lscontext2/run-test \
	tests/lscontext3/run-test \
	tests/filterb/run-test \
	tests/filterp/run-test \
	tests/strip-vs-match/run-test \
	tests/select1/run-test \
	tests/select2/run-test \
	tests/select3/run-test \
	tests/select4/run-test \
	tests/select5/run-test \
	tests/select6/run-test \
	tests/select7/run-test \
	tests/select8/run-test \
	tests/select9/run-test \
	tests/select10/run-test \
	tests/select11/run-test \
	tests/preserve1/run-test \
	tests/rediff1/run-test \
	tests/rediff2/run-test \
	tests/rediff3/run-test \
	tests/rediff4/run-test \
	tests/recount1/run-test \
	tests/recount2/run-test \
	tests/recount3/run-test \
	tests/recount4/run-test \
	tests/recount5/run-test \
	tests/recount6/run-test \
	tests/recount7/run-test \
	tests/recount8/run-test \
	tests/unwrapdiff1/run-test \
	tests/overstrip/run-test \
	tests/context1/run-test \
	tests/flip1/run-test \
	tests/flip2/run-test \
	tests/flip3/run-test \
	tests/flip4/run-test \
	tests/flip5/run-test \
	tests/flip6/run-test \
	tests/flip7/run-test \
	tests/flip8/run-test \
	tests/flip9/run-test \
	tests/flip10/run-test \
	tests/flip11/run-test \
	tests/flip12/run-test \
	tests/flip13/run-test \
	tests/flip14/run-test \
	tests/flip15/run-test \
	tests/flip16/run-test \
	tests/flip17/run-test \
	tests/flip18/run-test \
	tests/flip19/run-test \
	tests/unline1/run-test \
	tests/nul0/run-test \
	tests/nul1/run-test \
	tests/nul2/run-test \
	tests/nul3/run-test \
	tests/convert1/run-test \
	tests/convert2/run-test \
	tests/clean1/run-test \
	tests/clean2/run-test \
	tests/stdin/run-test \
	tests/splitdiffD/run-test \
	tests/fullheader1/run-test \
	tests/fullheader2/run-test \
	tests/fullheader3/run-test \
	tests/fullheader4/run-test \
	tests/whitespace/run-test \
	tests/crlf/run-test \
	tests/whitespace-regression/run-test \
	tests/interdiff-whitespace-w/run-test \
	tests/interdiff-color-context/run-test \
	tests/patch-ignore-whitespace/run-test \
	tests/whitespace-w/run-test \
	tests/filterdiff-inplace1/run-test \
	tests/rediff-inplace1/run-test \
	tests/rediff-empty-hunk/run-test \
	tests/git-rename-issue22/run-test \
	tests/git-binary-issue57/run-test \
	tests/git-binary-formats/run-test \
	tests/git-mode-issue59/run-test \
	tests/git-exclude-issue27/run-test \
	tests/git-prefixes-option/run-test \
	tests/git-prefixes-with-strip/run-test \
	tests/git-diff-duplication/run-test \
	tests/git-complex-mixed/run-test \
	tests/git-error-handling/run-test \
	tests/git-copy-operations/run-test \
	tests/git-extended-headers/run-test \
	tests/git-lsdiff-status/run-test \
	tests/git-deleted-file/run-test \
	tests/git-pure-rename/run-test \
	tests/git-diff-edge-cases/run-test \
	tests/malformed-diff-headers/run-test

# These ones don't work yet.
# Feel free to send me patches. :-)
XFAIL_TESTS = \
	tests/delhunk5/run-test \
	tests/delhunk6/run-test \
	tests/rediff-empty-hunk/run-test \
	tests/lsdiff-hunks-option/run-test \
	tests/lsdiff-lines-option/run-test \
	tests/lsdiff-exclusion-combined/run-test

test-perms: src/combinediff$(EXEEXT) src/flipdiff$(EXEEXT) \
		src/lsdiff$(EXEEXT) src/grepdiff$(EXEEXT) src/patchview$(EXEEXT) \
		scripts/splitdiff
	for script in $(bin_SCRIPTS); do \
		if [ -f $(top_builddir)/$$script ]; then \
			chmod a+x $(top_builddir)/$$script; \
		else \
			chmod a+x $(top_srcdir)/$$script; \
		fi \
	done
	for test in $(TESTS); do \
		chmod a+x $(top_srcdir)/$$test; \
	done

# Only require test-perms when actually running tests, not for distribution
check-local: test-perms
$(TESTS): | test-perms

# Ensure subdirectories are built before distribution
BUILT_SOURCES = lib-built

lib-built: lib/libgnu.a
	touch lib-built

lib/libgnu.a:
	$(MAKE) $(AM_MAKEFLAGS) -C lib all

distclean-local:
	-rm -rf $(top_builddir)/test-arena
	-rm -f lib-built

if ENABLE_FUZZING
# Fuzzing-specific instrumented binaries
noinst_PROGRAMS = src/fuzz-filterdiff src/fuzz-interdiff src/fuzz-rediff

src_fuzz_filterdiff_SOURCES = $(src_filterdiff_SOURCES)
src_fuzz_filterdiff_LDADD = $(src_filterdiff_LDADD)
src_fuzz_filterdiff_CFLAGS = $(AM_CFLAGS) $(FUZZ_CFLAGS)
src_fuzz_filterdiff_CPPFLAGS = $(AM_CPPFLAGS) $(FUZZ_CPPFLAGS)

src_fuzz_interdiff_SOURCES = $(src_interdiff_SOURCES)
src_fuzz_interdiff_LDADD = $(src_interdiff_LDADD)
src_fuzz_interdiff_CFLAGS = $(AM_CFLAGS) $(FUZZ_CFLAGS)
src_fuzz_interdiff_CPPFLAGS = $(AM_CPPFLAGS) $(FUZZ_CPPFLAGS)

src_fuzz_rediff_SOURCES = $(src_rediff_SOURCES)
src_fuzz_rediff_LDADD = $(src_rediff_LDADD)
src_fuzz_rediff_CFLAGS = $(AM_CFLAGS) $(FUZZ_CFLAGS)
src_fuzz_rediff_CPPFLAGS = $(AM_CPPFLAGS) $(FUZZ_CPPFLAGS)

# Override CC and LINK for fuzzing targets to use AFL++ for both compilation and linking
$(src_fuzz_filterdiff_OBJECTS): CC = $(AFL_CC)
$(src_fuzz_interdiff_OBJECTS): CC = $(AFL_CC)
$(src_fuzz_rediff_OBJECTS): CC = $(AFL_CC)

# Use AFL++ compiler for linking to include AFL++ runtime
src_fuzz_filterdiff_LINK = $(AFL_CC) $(AM_CFLAGS) $(CFLAGS) $(FUZZ_CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@
src_fuzz_interdiff_LINK = $(AFL_CC) $(AM_CFLAGS) $(CFLAGS) $(FUZZ_CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@
src_fuzz_rediff_LINK = $(AFL_CC) $(AM_CFLAGS) $(CFLAGS) $(FUZZ_CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@

# Create fuzzing symlinks (like regular tools)
src/fuzz-combinediff$(EXEEXT): src/fuzz-interdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/fuzz-flipdiff$(EXEEXT): src/fuzz-interdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/fuzz-lsdiff$(EXEEXT): src/fuzz-filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/fuzz-grepdiff$(EXEEXT): src/fuzz-filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/fuzz-patchview$(EXEEXT): src/fuzz-filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

CLEANFILES += src/fuzz-combinediff src/fuzz-flipdiff src/fuzz-lsdiff src/fuzz-grepdiff src/fuzz-patchview

# Fuzzing targets - use instrumented binaries when available
fuzz-help fuzz-corpus fuzz-test fuzz-analyze fuzz-clean fuzz-check fuzz-ci fuzz-stats:
	@if [ -f Makefile.fuzz ]; then \
		$(MAKE) -f Makefile.fuzz FUZZ_BINDIR="$(top_builddir)/src" $@; \
	else \
		echo "Fuzzing not available - Makefile.fuzz not found"; \
	fi

fuzz-filterdiff fuzz-interdiff fuzz-rediff fuzz-grepdiff fuzz-lsdiff: src/fuzz-filterdiff src/fuzz-interdiff src/fuzz-rediff src/fuzz-lsdiff$(EXEEXT) src/fuzz-grepdiff$(EXEEXT)
	@if [ -f Makefile.fuzz ]; then \
		$(MAKE) -f Makefile.fuzz FUZZ_BINDIR="$(top_builddir)/src" $@; \
	else \
		echo "Fuzzing not available - Makefile.fuzz not found"; \
	fi

else
# Fuzzing targets - delegate to separate makefile (non-instrumented fallback)
fuzz-help fuzz-corpus fuzz-test fuzz-analyze fuzz-clean fuzz-check fuzz-ci fuzz-stats:
	@if [ -f Makefile.fuzz ]; then \
		echo "Warning: Using non-instrumented binaries - configure with --enable-fuzzing for better results"; \
		$(MAKE) -f Makefile.fuzz $@; \
	else \
		echo "Fuzzing not available - Makefile.fuzz not found"; \
	fi

fuzz-filterdiff fuzz-interdiff fuzz-rediff fuzz-grepdiff fuzz-lsdiff:
	@if [ -f Makefile.fuzz ]; then \
		echo "Warning: Using non-instrumented binaries - configure with --enable-fuzzing for better results"; \
		$(MAKE) -f Makefile.fuzz $@; \
	else \
		echo "Fuzzing not available - Makefile.fuzz not found"; \
	fi
endif

EXTRA_DIST = $(man_MANS) \
	tests/common.sh tests/soak-test \
	$(TESTS) $(XFAIL_TESTS) \
	README.md BUGS COPYING TODO ChangeLog \
	bootstrap \
	patchutils.spec \
	doc/patchutils.xml \
	patchview/README.patchview \
	m4/gnulib-cache.m4 \
	Makefile.fuzz \
	fuzz/README.md fuzz/generate_corpus.sh fuzz/run_fuzz.sh fuzz/analyze_crashes.sh \
	fuzz/patch.dict \
	bash-completion-patchutils

tag:
	cvs tag -c `echo V@VERSION@ | tr . _`
