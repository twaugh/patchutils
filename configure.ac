dnl Process this file with autoconf to produce a configure script.

AC_INIT([patchutils],[0.4.2])
AC_CONFIG_SRCDIR(patchutils.spec.in)
AC_CONFIG_HEADERS(config.h)

AM_INIT_AUTOMAKE([dist-xz dist-bzip2 subdir-objects 1.6])
AC_CONFIG_MACRO_DIRS([m4])

dnl Checks for programs.
AC_PROG_CC
gl_EARLY
AM_PROG_CC_C_O
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_PROG_RANLIB
AC_PATH_PROG([XMLTO], [xmlto])
AM_CONDITIONAL(HAVE_XMLTO, test x$XMLTO != x)
if test x$XMLTO = x; then
  AC_MSG_WARN([xmlto not available so not building docs])
fi

dnl Checks for perl, for the scripts
AC_PATH_PROGS(PERL, perl perl5)

dnl Find a default editor
AC_CHECK_PROGS(DEFAULT_EDITOR, sensible-editor editor vim vi, vi)

CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
if test x"$GCC" = xyes; then
	CFLAGS="$CFLAGS -Wall"
	dnl Enable _FORTIFY_SOURCE for additional security and warnings
	CPPFLAGS="$CPPFLAGS -D_FORTIFY_SOURCE=2"
fi

dnl Checks for libraries.

dnl Checks for header files.
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS(sys/types.h unistd.h error.h)

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_PID_T
AC_TYPE_SIZE_T

dnl Checks for library functions.
AC_FUNC_ALLOCA
AC_FUNC_FNMATCH
AC_CHECK_FUNCS(strcspn strspn strtoul getline error)

dnl gnulib will handle getopt and error functions

dnl Check pcre2 availability
AC_MSG_CHECKING([whether PCRE2 support is requested])
AC_ARG_WITH([pcre2],
            [AS_HELP_STRING([--with-pcre2@<:@=PATH@:>@],
                            [use pcre2 regex library, optionally from PATH @<:@default=check@:>@])],
            [], [with_pcre2=check])
LIBPCRE2=
AS_IF([test "x$with_pcre2" != xno],
      [
       AC_MSG_RESULT($with_pcre2)
       dnl Handle custom path for PCRE2
       AS_IF([test "x$with_pcre2" != xyes && test "x$with_pcre2" != xcheck],
             [
              dnl Custom path specified
              PCRE2_CPPFLAGS="-I$with_pcre2/include"
              PCRE2_LDFLAGS="-L$with_pcre2/lib"
              save_CPPFLAGS="$CPPFLAGS"
              save_LDFLAGS="$LDFLAGS"
              CPPFLAGS="$CPPFLAGS $PCRE2_CPPFLAGS"
              LDFLAGS="$LDFLAGS $PCRE2_LDFLAGS"
             ])
       AC_CHECK_HEADERS([pcre2posix.h], [], [LIBPCRE2=_missing_header])
       AC_CHECK_LIB([pcre2-posix${LIBPCRE2}], [regexec],
                    [
                     dnl Success - add the flags permanently if custom path was used
                     AS_IF([test "x$with_pcre2" != xyes && test "x$with_pcre2" != xcheck],
                           [
                            CPPFLAGS="$save_CPPFLAGS $PCRE2_CPPFLAGS"
                            LDFLAGS="$save_LDFLAGS $PCRE2_LDFLAGS"
                           ])
                     dnl Add the library to LIBS
                     LIBS="$LIBS -lpcre2-posix${LIBPCRE2}"
                    ],
                    [
                     dnl Failure - restore original flags if custom path was used
                     AS_IF([test "x$with_pcre2" != xyes && test "x$with_pcre2" != xcheck],
                           [
                            CPPFLAGS="$save_CPPFLAGS"
                            LDFLAGS="$save_LDFLAGS"
                           ])
                     if test "x$with_pcre2" != xcheck; then
                      AC_MSG_FAILURE(
                        [--with-pcre2 was given, but test for pcre2-posix failed])
                      fi
                    ]
                    )
      ],
      AC_MSG_RESULT(no)
)

dnl Check diff/patch programs
AC_MSG_CHECKING(for diff program)
DIFF=diff
AC_ARG_WITH(diff, [  --with-diff=DIFF        name of the diff program],
  DIFF=$withval)
AC_MSG_RESULT($DIFF)

PATCH=patch
AC_MSG_CHECKING(for patch program)
AC_ARG_WITH(patch, [  --with-patch=PATCH      name of the patch program],
  PATCH=$withval)
AC_MSG_RESULT($PATCH)

AC_MSG_CHECKING(whether patch works)
pver=`$PATCH -v 2>&1`
if echo "$pver" | grep -q "version 2.0" > /dev/null 2>&1; then
   AC_MSG_ERROR([no

Sorry, you have version 2.0 of the patch program, which
is not supported by interdiff.  Please upgrade.
])
fi
AC_MSG_RESULT(yes)

AC_DEFINE_UNQUOTED(PATCH, "$PATCH", How patch(1) is called)
AC_DEFINE_UNQUOTED(DIFF, "$DIFF", How diff(1) is called)

gl_INIT

AC_CONFIG_FILES([
Makefile
lib/Makefile
scripts/splitdiff
scripts/editdiff
scripts/fixcvsdiff
scripts/recountdiff
scripts/unwrapdiff
scripts/dehtmldiff
scripts/espdiff
patchutils.spec
])
AC_OUTPUT
