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

# Build the daemon by default; omit it when explicitly disabled.
if DISABLE_AGM_SERVER
# Skip building agm_server when explicitly disabled.
else
bin_PROGRAMS = agm_server

agm_server_SOURCES = ./src/agm-server-daemon.c
agm_server_CFLAGS = $(AM_CFLAGS) $(GLIB_CFLAGS) -DAGM_USE_SYSLOG -Wno-write-strings
agm_server_LDADD = libagmserverwrapper.la $(GLIB_LIBS)
agm_server_LDFLAGS = -ldl -shared -avoid-version
endif

if AGM_AUDIO_LOG_UTIL_BRIDGE
libagmserverwrapper_la_CFLAGS += -DAGM_AUDIO_LOG_UTIL_BRIDGE -DAGM_LOG_MODULE=MODULE_AGM_IPC_SERVER
libagmserverwrapper_la_LIBADD += -laudio_log_util
if DISABLE_AGM_SERVER
# Skip building agm_server when explicitly disabled.
else
agm_server_CFLAGS += -DAGM_AUDIO_LOG_UTIL_BRIDGE -DAGM_LOG_MODULE=MODULE_AGM_IPC_SERVER
agm_server_LDADD += -laudio_log_util
endif
endif
