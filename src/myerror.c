/* Error handler for noninteractive utilities
   Copyright (C) 1990-1998, 2000, 2001 Free Software Foundation, Inc.
   This file is part of the GNU C Library.  Its master source is NOT part of
   the C library, however.  The master source lives in /gd/gnu/lib.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

/* Written by David MacKenzie <djm@gnu.ai.mit.edu>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef HAVE_ERROR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#include "util.h"

void
error (int status, int errnum, const char *message, ...)
{
  va_list args;

  fflush (stdout);
  fprintf (stderr, "%s: ", progname);

  va_start (args, message);
  vfprintf (stderr, message, args);
  va_end (args);

  if (errnum)
    {
      fprintf (stderr, ": %s", strerror (errnum));
    }
  putc ('\n', stderr);
  fflush (stderr);
  if (status)
    exit (status);
}

#endif
