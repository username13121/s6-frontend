/* ISC license. */

#include <stdlib.h>
#include <unistd.h>

#include <skalibs/envexec.h>
#include <skalibs/stralloc.h>
#include <skalibs/djbunix.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/posixplz.h>

#include <execline/config.h>

#include <s6-frontend/config.h>

#include "s6f.h"

enum golb_e
{
  GOLB_HELP = 0x01,
  GOLB_VERSION = 0x02,
  GOLB_USER = 0x04,
} ;

enum gola_e
{
  GOLA_SCANDIR,
  GOLA_LIVEDIR,
  GOLA_REPODIR,
  GOLA_BOOTDB,
  GOLA_STMPDIR,
  GOLA_STORELIST,
  GOLA_VERBOSITY,
  GOLA_FDHUSER,
  GOLA_COLOR,
  GOLA_N
} ;

#define dienomem() strerr_diefusys(111, "build command line")

static inline enum gola_e gola_pos (gol_arg const *tab, size_t n, enum gola_e i)
{
  for (enum gola_e j = 0 ; j < n ; j++) if (tab[j].i == i) return j ;
  strerr_dief(101, "can't happen: rgola does not cover all gola_e values") ;
}

 /* Make s available on stdin, without keeping a child of ours around. */

static void feed_stdin (char const *s, size_t len)
{
  int p[2] ;
  if (pipe(p) == -1) strerr_diefu1sys(111, "pipe") ;
  switch (doublefork())
  {
    case -1 : strerr_diefu1sys(111, "doublefork") ;
    case 0 :
      PROG = "s6 (configuration writer)" ;
      fd_close(p[0]) ;
      if (allwrite(p[1], s, len) < len) _exit(111) ;
      fd_close(p[1]) ;
      _exit(0) ;
  }
  fd_close(p[1]) ;
  if (fd_move(0, p[0]) == -1) strerr_diefu1sys(111, "fd_move") ;
}

int main (int argc, char const *const *argv)
{
  static gol_bool const rgolb[] =
  {
    { .so = 'h', .lo = "help", .clear = 0, .set = GOLB_HELP },
    { .so = 'u', .lo = "user", .clear = 0, .set = GOLB_USER },
    { .so = 'S', .lo = "system", .clear = GOLB_USER, .set = 0 },
  } ;
  static gol_arg const rgola[] =
  {
    { .so = 's', .lo = "scandir", .i = GOLA_SCANDIR },
    { .so = 'l', .lo = "livedir", .i = GOLA_LIVEDIR },
    { .so = 'r', .lo = "repodir", .i = GOLA_REPODIR },
    { .so = 'c', .lo = "bootdb", .i = GOLA_BOOTDB },
    { .so = 0,   .lo = "stmpdir", .i = GOLA_STMPDIR },
    { .so = 0,   .lo = "storelist", .i = GOLA_STORELIST },
    { .so = 'v', .lo = "verbosity", .i = GOLA_VERBOSITY },
    { .so = 0,   .lo = "fdholder-user", .i = GOLA_FDHUSER },
    { .so = 0,   .lo = "color", .i = GOLA_COLOR },
  } ;

  stralloc sa = STRALLOC_ZERO ;
  uint64_t wgolb = 0 ;
  char const *wgola[GOLA_N] = { 0 } ;
  size_t optpos[GOLA_N] = { 0 } ;
  unsigned int conf_overrides = 0 ;
  unsigned int m = 0 ;
  PROG = "s6" ;

  {
    unsigned int golc = GOL_main(argc, argv, rgolb, rgola, &wgolb, wgola) ;
    argc -= golc ; argv += golc ;
  }

  for (enum gola_e i = 0 ; i < GOLA_N ; i++) if (wgola[i])
  {
    enum gola_e j = gola_pos(rgola, sizeof(rgola)/sizeof(gol_arg const), i) ;
    optpos[i] = sa.len ;
    if (!stralloc_catb(&sa, "--", 2)
     || !stralloc_cats(&sa, rgola[j].lo)
     || !stralloc_catb(&sa, "=", 1)
     || !stralloc_cats(&sa, wgola[i])
     || !stralloc_0(&sa))
      dienomem() ;
    conf_overrides++ ;
  }

  char const *newargv[4 + 1 + conf_overrides + 1 + (wgolb & GOLB_HELP ? 1 : argc) + 1] ;

  if (wgolb & GOLB_USER)
  {
    stralloc conf = STRALLOC_ZERO ;
    stralloc subst = STRALLOC_ZERO ;
    char const *conffile = getenv("S6_USER_CONF") ;
    if (!conffile) conffile = S6_FRONTEND_USER_CONF ;
    if (!openslurpclose(&conf, conffile))
      strerr_diefu2sys(111, "read ", conffile) ;
    s6f_user_xdg_subst(&subst, conf.s, conf.len, conffile) ;
    stralloc_free(&conf) ;
    feed_stdin(subst.s, subst.len) ;
    newargv[m++] = EXECLINE_EXTBINPREFIX "envfile" ;
    newargv[m++] = "-I" ;
    newargv[m++] = "--" ;
    newargv[m++] = "-" ;
  }
  else
  {
    char const *conffile = getenv("S6_CONF") ;
    if (!conffile) conffile = S6_FRONTEND_CONF ;
    newargv[m++] = EXECLINE_EXTBINPREFIX "envfile" ;
    newargv[m++] = "-I" ;
    newargv[m++] = "--" ;
    newargv[m++] = conffile ;
  }

  newargv[m++] = S6_FRONTEND_LIBEXECPREFIX "s6-frontend" ;
  for (enum gola_e i = 0 ; i < GOLA_N ; i++)
    if (wgola[i]) newargv[m++] = sa.s + optpos[i] ;
  newargv[m++] = "--" ;
  if (wgolb & GOLB_HELP) newargv[m++] = "help" ;
  else while (argc--) newargv[m++] = *argv++ ;

  newargv[m++] = 0 ;
  xexec(newargv) ;
}
