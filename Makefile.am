DISTCLEANFILES = src/stamp-h[0-9]* src/config.h

bin_PROGRAMS = src/interdiff src/filterdiff src/rediff
bin_SCRIPTS = fixcvsdiff splitdiff editdiff recountdiff unwrapdiff dehtmldiff \
	espdiff
man_MANS = doc/interdiff.1 doc/filterdiff.1 doc/fixcvsdiff.1 doc/rediff.1 \
	doc/editdiff.1 doc/combinediff.1 doc/lsdiff.1 doc/splitdiff.1 \
	doc/grepdiff.1 doc/recountdiff.1 doc/unwrapdiff.1 doc/dehtmldiff.1 \
	doc/flipdiff.1 doc/espdiff.1

AM_CFLAGS = -I$(srcdir)/src
src_interdiff_SOURCES = src/interdiff.c src/util.c src/util.h src/diff.c \
		src/diff.h src/myerror.c
src_filterdiff_SOURCES = src/filterdiff.c src/util.c src/util.h src/diff.c \
		src/diff.h src/myerror.c
src_rediff_SOURCES = src/rediff.c src/util.c src/util.h src/diff.c src/diff.h \
		src/myerror.c

src_interdiff_LDADD = @LIBOBJS@
src_filterdiff_LDADD = @LIBOBJS@
src_rediff_LDADD = @LIBOBJS@

# The man pages are generated from DocBook XML.
$(man_MANS): doc/patchutils.xml
	xmlto man -o doc $<

# Special rules for combinediff, flipdiff, lsdiff and grepdiff, which are
# just symlinks.
src/combinediff$(EXEEXT): src/interdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/flipdiff$(EXEEXT): src/interdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/lsdiff$(EXEEXT): src/filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

src/grepdiff$(EXEEXT): src/filterdiff$(EXEEXT)
	ln -sf $(notdir $<) $@

install-exec-hook:
	ln -sf "`echo interdiff|sed '$(transform)'`" $(DESTDIR)$(bindir)/"`echo combinediff|sed '$(transform)'`"
	ln -sf "`echo interdiff|sed '$(transform)'`" $(DESTDIR)$(bindir)/"`echo flipdiff|sed '$(transform)'`"
	ln -sf "`echo filterdiff|sed '$(transform)'`" $(DESTDIR)$(bindir)/"`echo lsdiff|sed '$(transform)'`"
	ln -sf "`echo filterdiff|sed '$(transform)'`" $(DESTDIR)$(bindir)/"`echo grepdiff|sed '$(transform)'`"

uninstall-hook:
	rm -f $(DESTDIR)$(bindir)/"`echo combinediff|sed '$(transform)'`"
	rm -f $(DESTDIR)$(bindir)/"`echo flipdiff|sed '$(transform)'`"
	rm -f $(DESTDIR)$(bindir)/"`echo lsdiff|sed '$(transform)'`"
	rm -f $(DESTDIR)$(bindir)/"`echo grepdiff|sed '$(transform)'`"

CLEANFILES=src/combinediff src/flipdiff src/lsdiff src/grepdiff
MAINTAINERCLEANFILES=$(man_MANS)

tests/combine1/run-test: src/combinediff$(EXEEXT) src/flipdiff$(EXEEXT) \
		src/lsdiff$(EXEEXT) src/grepdiff$(EXEEXT)
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
	tests/trimcontext1/run-test \
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
	tests/fuzz1/run-test \
	tests/U0/run-test \
	tests/U3/run-test \
	tests/grepdiff1/run-test \
	tests/grepdiff2/run-test \
	tests/grepdiff3/run-test \
	tests/grepdiff4/run-test \
	tests/grepdiff5/run-test \
	tests/grepdiff6/run-test \
	tests/grepdiff7/run-test \
	tests/number1/run-test \
	tests/number2/run-test \
	tests/nondiff/run-test \
	tests/nondiff1/run-test \
	tests/lscontext1/run-test \
	tests/lscontext2/run-test \
	tests/filterp/run-test \
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
	tests/recount1/run-test \
	tests/recount2/run-test \
	tests/recount3/run-test \
	tests/recount4/run-test \
	tests/recount5/run-test \
	tests/recount6/run-test \
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
	tests/stdin/run-test

# These ones don't work yet.
# Feel free to send me patches. :-)
XFAIL_TESTS = \
	tests/delhunk5/run-test \
	tests/delhunk6/run-test

distclean-local:
	-rm -rf $(top_builddir)/test-arena

EXTRA_DIST = $(man_MANS) \
	tests/common.sh tests/soak-test \
	$(TESTS) $(XFAIL_TESTS) \
	BUGS COPYING TODO ChangeLog \
	bootstrap \
	patchutils.spec \
	doc/patchutils.xml \
	src/getopt.h \
	move-to-front

tag:
	cvs tag -c `echo V@VERSION@ | tr . _`
