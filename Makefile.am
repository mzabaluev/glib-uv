ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = $(GLIB_CFLAGS) $(UV_CFLAGS)

lib_LTLIBRARIES = libglib-uv.la

libglib_uv_la_LIBADD = $(GLIB_LIBS) $(UV_LIBS)

libglib_uv_la_SOURCES = glib-uv.c

glib_uv_includedir = $(includedir)/glib-uv

glib_uv_include_HEADERS = glib-uv.h

check_PROGRAMS = glib-uv-test

TESTS = $(check_PROGRAMS)

glib_uv_test_SOURCES = glib-uv-test.c

glib_uv_test_LDADD = libglib-uv.la $(GLIB_LIBS) $(UV_LIBS)
