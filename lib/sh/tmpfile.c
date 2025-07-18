/*
 * tmpfile.c - functions to create and safely open temp files for the shell.
 */

/* Copyright (C) 2000-2020,2022-2024 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include <bashtypes.h>
#include <posixstat.h>
#include <posixtime.h>
#include <filecntl.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include <bashansi.h>

#include <stdio.h>
#include <errno.h>

#include <shell.h>

#ifndef errno
extern int errno;
#endif

#if defined (HAVE_GETRANDOM) || defined (HAVE_ARC4RANDOM) || defined (HAVE_GETENTROPY)
#  define USE_URANDOM32
#endif

#define BASEOPENFLAGS	(O_CREAT | O_TRUNC | O_EXCL | O_BINARY)

#define DEFAULT_TMPDIR		"."	/* bogus default, should be changed */
#define DEFAULT_NAMEROOT	"shtmp"

/* Use ANSI-C rand() interface if random(3) is not available */
#if !HAVE_RANDOM
#define random() rand()
#endif

extern pid_t dollar_dollar_pid;

static int tmpunlink (const char *);

static char *get_sys_tmpdir (void);
static char *get_tmpdir (int);

static char *sys_tmpdir = (char *)NULL;
static int ntmpfiles;
static int tmpnamelen = -1;
static unsigned long filenum = 1L;

static char *
get_sys_tmpdir (void)
{
  if (sys_tmpdir)
    return sys_tmpdir;

#ifdef P_tmpdir
  sys_tmpdir = P_tmpdir;
  if (file_iswdir (sys_tmpdir))
    return sys_tmpdir;
#endif

  sys_tmpdir = "/tmp";
  if (file_iswdir (sys_tmpdir))
    return sys_tmpdir;

  sys_tmpdir = "/var/tmp";
  if (file_iswdir (sys_tmpdir))
    return sys_tmpdir;

  sys_tmpdir = "/usr/tmp";
  if (file_iswdir (sys_tmpdir))
    return sys_tmpdir;

  sys_tmpdir = DEFAULT_TMPDIR;

  return sys_tmpdir;
}

static char *
get_tmpdir (int flags)
{
  char *tdir;

  tdir = (flags & MT_USETMPDIR) ? get_string_value ("TMPDIR") : (char *)NULL;
  if (tdir && (file_iswdir (tdir) == 0 || strlen (tdir) > PATH_MAX))
    tdir = 0;

  if (tdir == 0)
    tdir = get_sys_tmpdir ();

#if defined (HAVE_PATHCONF) && defined (_PC_NAME_MAX)
  if (tmpnamelen == -1)
    tmpnamelen = pathconf (tdir, _PC_NAME_MAX);
#else
  tmpnamelen = 0;
#endif

  return tdir;
}

static void
sh_seedrand (void)
{
#if HAVE_RANDOM
  int d;
  static int seeded = 0;
  if (seeded == 0)
    {
      struct timeval tv;
  	      
      gettimeofday (&tv, NULL);
      srandom (tv.tv_sec ^ tv.tv_usec ^ (getpid () << 16) ^ (uintptr_t)&d);
      seeded = 1;
    }
#endif
}

static int
tmpunlink (const char *fn)
{
  int r;

  r = unlink (fn);
  return r;
}

char *
sh_mktmpname (const char *nameroot, int flags)
{
  char *filename, *tdir;
  const char *lroot;
  struct stat sb;
  int r, tdlen;
  static int seeded = 0;

  filename = (char *)xmalloc (PATH_MAX + 1);
  tdir = get_tmpdir (flags);
  tdlen = strlen (tdir);

  lroot = nameroot ? nameroot : DEFAULT_NAMEROOT;
  if (nameroot == 0)
    flags &= ~MT_TEMPLATE;

  if ((flags & MT_TEMPLATE) && strlen (nameroot) > PATH_MAX)
    flags &= ~MT_TEMPLATE;

#ifdef USE_MKTEMP
  if (flags & MT_TEMPLATE)
    strcpy (filename, nameroot);
  else
    snprintf (filename, PATH_MAX, "%s/%s.XXXXXX", tdir, lroot);
  if (mktemp (filename) == 0)
    {
      free (filename);
      filename = NULL;
    }
#else  /* !USE_MKTEMP */
#ifndef USE_URANDOM32
  sh_seedrand ();
#endif
  while (1)
    {
      unsigned long x;
#ifdef USE_URANDOM32
      x = (unsigned long) ((flags & MT_USERANDOM) ? get_urandom32 () : ntmpfiles++);
#else
      x = (unsigned long) ((flags & MT_USERANDOM) ? random () : ntmpfiles++);
#endif
      filenum = (filenum << 1) ^
		(unsigned long) time ((time_t *)0) ^
		(unsigned long) dollar_dollar_pid ^
		x;
      snprintf (filename, PATH_MAX, "%s/%s-%lu", tdir, lroot, filenum);
      if (tmpnamelen > 0 && tmpnamelen < 32)		/* XXX */
	filename[tdlen + 1 + tmpnamelen] = '\0';
#  ifdef HAVE_LSTAT
      r = lstat (filename, &sb);
#  else
      r = stat (filename, &sb);
#  endif
      if (r < 0 && errno == ENOENT)
	break;
    }
#endif /* !USE_MKTEMP */

  return filename;
}

int
sh_mktmpfd (const char *nameroot, int flags, char **namep)
{
  char *filename, *tdir;
  const char *lroot;
  int fd, tdlen;
  
  filename = (char *)xmalloc (PATH_MAX + 1);
  tdir = get_tmpdir (flags);
  tdlen = strlen (tdir);

  lroot = nameroot ? nameroot : DEFAULT_NAMEROOT;
  if (nameroot == 0)
    flags &= ~MT_TEMPLATE;

  if ((flags & MT_TEMPLATE) && strlen (nameroot) > PATH_MAX)
    flags &= ~MT_TEMPLATE;

#ifdef USE_MKSTEMP
  if (flags & MT_TEMPLATE)
    strcpy (filename, nameroot);
  else
    snprintf (filename, PATH_MAX, "%s/%s.XXXXXX", tdir, lroot);
  fd = mkstemp (filename);
  if ((flags & MT_UNLINK) && tmpunlink (filename) < 0)
    {
      int e = errno;
      close (fd);
      fd = -1;
      errno = e;
    }
  if (fd < 0 || namep == 0)
    {
      free (filename);
      filename = NULL;
    }
  if (namep)
    *namep = filename;

  return fd;
#else /* !USE_MKSTEMP */
#ifndef USE_URANDOM32
  sh_seedrand ();
#endif
  do
    {
      unsigned long x;
#ifdef USE_URANDOM32
      x = (unsigned long) ((flags & MT_USERANDOM) ? get_urandom32 () : ntmpfiles++);
#else
      x = (unsigned long) ((flags & MT_USERANDOM) ? random () : ntmpfiles++);
#endif
      filenum = (filenum << 1) ^
		(unsigned long) time ((time_t *)0) ^
		(unsigned long) dollar_dollar_pid ^
		x;
      snprintf (filename, PATH_MAX, "%s/%s-%lu", tdir, lroot, filenum);
      if (tmpnamelen > 0 && tmpnamelen < 32)		/* XXX */
	filename[tdlen + 1 + tmpnamelen] = '\0';
      fd = open (filename, BASEOPENFLAGS | ((flags & MT_READWRITE) ? O_RDWR : O_WRONLY), 0600);
    }
  while (fd < 0 && errno == EEXIST);

  if ((flags & MT_UNLINK) && (tmpunlink (filename) < 0))
    {
      int e = errno;
      close (fd);
      fd = -1;
      errno = e;
    }
  if (fd < 0 || namep == 0)
    {
      free (filename);
      filename = NULL;
    }

  if (namep)
    *namep = filename;

  return fd;
#endif /* !USE_MKSTEMP */
}

FILE *
sh_mktmpfp (const char *nameroot, int flags, char **namep)
{
  int fd;
  FILE *fp;

  fd = sh_mktmpfd (nameroot, flags, namep);
  if (fd < 0)
    return ((FILE *)NULL);
  fp = fdopen (fd, (flags & MT_READWRITE) ? "w+" : "w");
  if (fp == 0)
    close (fd);
  return fp;
}

char *
sh_mktmpdir (const char *nameroot, int flags)
{
  char *filename;
  int fd;
#ifdef USE_MKDTEMP
  char *tdir, *dirname;
  const char *lroot;
  int tdlen;
  
  filename = (char *)xmalloc (PATH_MAX + 1);
  tdir = get_tmpdir (flags);
  tdlen = strlen (tdir);

  lroot = nameroot ? nameroot : DEFAULT_NAMEROOT;
  if (nameroot == 0)
    flags &= ~MT_TEMPLATE;

  if ((flags & MT_TEMPLATE) && strlen (nameroot) > PATH_MAX)
    flags &= ~MT_TEMPLATE;

  if (flags & MT_TEMPLATE)
    strcpy (filename, nameroot);
  else
    snprintf (filename, PATH_MAX, "%s/%s.XXXXXX", tdir, lroot);
  dirname = mkdtemp (filename);
  if (dirname == 0)
    {
      free (filename);
      filename = NULL;
    }
  return dirname;
#else /* !USE_MKDTEMP */
  filename = (char *)NULL;
  do
    {
      filename = sh_mktmpname (nameroot, flags);
      fd = mkdir (filename, 0700);
      if (fd == 0)
	break;
      free (filename);
      filename = (char *)NULL;
    }
  while (fd < 0 && errno == EEXIST);

  return (filename);
#endif /* !USE_MKDTEMP */
}
