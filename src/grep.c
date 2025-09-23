/*
 * grep.c - grep mode implementation (grepdiff functionality)
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_ERROR_H
# include <error.h>
#endif

#include "patchfilter.h"

/* Grep mode implementation (grepdiff functionality) */
int run_grep_mode(int argc, char *argv[])
{
    /* TODO: Implement grepdiff functionality using patch scanner */
    error(EXIT_FAILURE, 0, "grep mode not yet implemented");
    return 1;
}
