pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = agmserver.pc
EXTRA_DIST = $(pkgconfig_DATA)

h_sources = ./inc/agm-dbus-utils.h \
            ./inc/agm_server_wrapper_dbus.h

AM_CFLAGS = -I $(srcdir)/inc -I $(top_srcdir)/service/inc/public
AM_CFLAGS += -I $(top_srcdir)/service/inc/public/agm
AM_CFLAGS += -D__unused=__attribute__\(\(__unused__\)\)

lib_LTLIBRARIES = libagmserverwrapper.la
libagmserverwrapper_la_SOURCES = ./src/agm-dbus-utils.c ./src/agm_server_wrapper_dbus.c
libagmserverwrapper_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS) $(GLIB_CFLAGS) -DAGM_USE_SYSLOG
libagmserverwrapper_la_LIBADD = -lagm -L$(top_builddir)/service/.libs
libagmserverwrapper_la_LDFLAGS = -ldl -lpthread -lar-osal -shared -avoid-version $(DBUS_LIBS)

# Install header files
include_HEADERS = $(h_sources)

bin_PROGRAMS = agm_server

agm_server_SOURCES = ./src/agm-server-daemon.c
agm_server_CFLAGS = $(AM_CFLAGS) $(GLIB_CFLAGS) -DAGM_USE_SYSLOG -Wno-write-strings
agm_server_LDADD = libagmserverwrapper.la $(GLIB_LIBS)
agm_server_LDFLAGS = -ldl -shared -avoid-version
