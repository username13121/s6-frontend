/* ISC license. */

#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <skalibs/types.h>
#include <skalibs/strerr.h>
#include <skalibs/stralloc.h>

#include "s6f.h"

#define dienomem() strerr_diefu1sys(111, "allocate memory")

typedef enum xdgvar_e xdgvar_t ;
enum xdgvar_e
{
  XDGVAR_RUNTIME,
  XDGVAR_CONFIG,
  XDGVAR_DATA,
  XDGVAR_STATE,
  XDGVAR_CACHE,
  XDGVAR_N
} ;

static char const *const xdgname[XDGVAR_N] =
{
  "XDG_RUNTIME_DIR",
  "XDG_CONFIG_HOME",
  "XDG_DATA_HOME",
  "XDG_STATE_HOME",
  "XDG_CACHE_HOME"
} ;

 /* Defaults, relative to the user's home directory.
    XDG_RUNTIME_DIR deliberately has none: it is provided by the session
    manager, and inventing one would make the s6 command line address a
    different scandir than the one the user's s6-svscan is running on. */

static char const *const xdgdefault[XDGVAR_N] =
{
  0,
  "/.config",
  "/.local/share",
  "/.local/state",
  "/.cache"
} ;

static int isabsolutepath (char const *s)
{
  return s && s[0] == '/' ;
}

static char const *gethomedir (void)
{
  static char const *home = 0 ;
  static int done = 0 ;
  if (!done)
  {
    struct passwd *pw ;
    done = 1 ;
    home = getenv("HOME") ;
    if (isabsolutepath(home)) return home ;
    errno = 0 ;
    pw = getpwuid(getuid()) ;
    home = pw && isabsolutepath(pw->pw_dir) ? pw->pw_dir : 0 ;
  }
  return home ;
}

 /* If a variable reference starts at s+pos, returns its total length and
    stores the position and length of the variable name; else returns 0. */

static size_t varref (char const *s, size_t len, size_t pos, size_t *namepos, size_t *namelen)
{
  size_t i = pos + 1 ;
  int braces ;
  if (i >= len) return 0 ;
  braces = s[i] == '{' ;
  if (braces && ++i >= len) return 0 ;
  *namepos = i ;
  while (i < len && (s[i] == '_'
   || (s[i] >= 'A' && s[i] <= 'Z')
   || (s[i] >= 'a' && s[i] <= 'z')
   || (s[i] >= '0' && s[i] <= '9'))) i++ ;
  *namelen = i - *namepos ;
  if (!*namelen) return 0 ;
  if (braces)
  {
    if (i >= len || s[i] != '}') return 0 ;
    i++ ;
  }
  return i - pos ;
}

 /* Returns the index of the XDG variable named s[namepos..namepos+namelen],
    XDGVAR_N if the name is not an XDG variable name at all, and -1 if it
    looks like one but is not supported. */

static int xdgindex (char const *s, size_t namepos, size_t namelen)
{
  if (namelen < 4 || memcmp(s + namepos, "XDG_", 4)) return XDGVAR_N ;
  for (xdgvar_t i = 0 ; i < XDGVAR_N ; i++)
    if (!strncmp(xdgname[i], s + namepos, namelen) && !xdgname[i][namelen]) return i ;
  return -1 ;
}

void s6f_user_xdg_subst (stralloc *out, char const *s, size_t len, char const *fn)
{
  stralloc values = STRALLOC_ZERO ;
  char const *value[XDGVAR_N] = { 0 } ;
  size_t valuepos[XDGVAR_N] ;
  unsigned int used = 0 ;

  if (!len) return ;

 /* First pass: collect the variables the file needs, and reject the ones
    we would not know how to expand. */

  for (size_t i = 0 ; i < len ; i++) if (s[i] == '$')
  {
    size_t namepos, namelen ;
    size_t n = varref(s, len, i, &namepos, &namelen) ;
    int j ;
    if (!n) continue ;
    i += n - 1 ;
    j = xdgindex(s, namepos, namelen) ;
    if (j == XDGVAR_N) continue ;
    if (j < 0)
    {
      char name[namelen + 1] ;
      memcpy(name, s + namepos, namelen) ;
      name[namelen] = 0 ;
      strerr_dief4x(100, "unsupported variable ", name, " in ", fn) ;
    }
    used |= 1u << j ;
  }

 /* Resolution: the environment wins if it holds an absolute path, else we
    fall back to a $HOME-based default when the variable has one. */

  for (xdgvar_t i = 0 ; i < XDGVAR_N ; i++) if (used & (1u << i))
  {
    char const *x = getenv(xdgname[i]) ;
    valuepos[i] = values.len ;
    if (isabsolutepath(x))
    {
      if (!stralloc_catb(&values, x, strlen(x) + 1)) dienomem() ;
      continue ;
    }
    if (!xdgdefault[i])
    {
      if (x && *x)
        strerr_dief5x(100, xdgname[i], " is used in ", fn, " but is not an absolute path: ", x) ;
      else
        strerr_dief4x(100, xdgname[i], " is used in ", fn, " but is not set") ;
    }
    {
      char const *home = gethomedir() ;
      size_t homelen ;
      if (!home)
        strerr_dief6x(100, xdgname[i], " is used in ", fn, " but neither ", xdgname[i], " nor HOME is set to an absolute path") ;
      homelen = strlen(home) ;
      while (homelen > 1 && home[homelen - 1] == '/') homelen-- ;
      if (homelen == 1) homelen = 0 ;  /* $HOME is /, avoid a double slash */
      if (!stralloc_catb(&values, home, homelen)
       || !stralloc_cats(&values, xdgdefault[i])
       || !stralloc_0(&values)) dienomem() ;
    }
  }

 /* Don't add to values past this point. */

  for (xdgvar_t i = 0 ; i < XDGVAR_N ; i++)
    if (used & (1u << i)) value[i] = values.s + valuepos[i] ;

 /* Second pass: copy the file, expanding the references we resolved.
    Anything that is not an XDG variable reference is copied verbatim. */

  {
    size_t chunk = 0 ;
    size_t i = 0 ;
    while (i < len)
    {
      size_t namepos, namelen, n ;
      int j ;
      if (s[i] != '$') { i++ ; continue ; }
      n = varref(s, len, i, &namepos, &namelen) ;
      j = n ? xdgindex(s, namepos, namelen) : XDGVAR_N ;
      if (j < 0 || j == XDGVAR_N) { i += n ? n : 1 ; continue ; }
      if (!stralloc_catb(out, s + chunk, i - chunk)
       || !stralloc_cats(out, value[j])) dienomem() ;
      i += n ;
      chunk = i ;
    }
    if (!stralloc_catb(out, s + chunk, len - chunk)) dienomem() ;
  }

  stralloc_free(&values) ;
}
