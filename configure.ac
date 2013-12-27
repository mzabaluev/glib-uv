AC_INIT([glib-uv],[0.1.0])
AM_INIT_AUTOMAKE([foreign])
LT_INIT
AC_CONFIG_MACRO_DIR([m4])

AC_PROG_CC

PKG_CHECK_MODULES([GLIB], [glib-2.0])
PKG_CHECK_MODULES([UV], [libuv])

CPPFLAGS_save=$CPPFLAGS
CPPFLAGS="$CPPFLAGS $GLIB_CFLAGS"

AC_CHECK_DECL([g_main_context_enter_poller],
  [],[AC_MSG_ERROR([GLib poller support is required])],
  [#include <glib.h>])

CPPFLAGS=$CPPFLAGS_save

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
