/* strerror.c --- POSIX compatible system error routine

   Copyright (C) 2007 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>

#include <string.h>

#if REPLACE_STRERROR

# include <stdio.h>

# include "intprops.h"

# undef strerror
# if ! HAVE_DECL_STRERROR
#  define strerror(n) NULL
# endif

char *
rpl_strerror (int n)
{
  char *result = strerror (n);

  if (result == NULL || result[0] == '\0')
    {
      static char const fmt[] = "Unknown error (%d)";
      static char mesg[sizeof fmt + INT_STRLEN_BOUND (n)];
      sprintf (mesg, fmt, n);
      return mesg;
    }

  return result;
}

#endif