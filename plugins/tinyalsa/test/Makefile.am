pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = agmtest.pc
EXTRA_DIST = $(pkgconfig_DATA)

AM_CFLAGS = -Wno-unused-parameter -Wno-unused-result
AM_CFLAGS += -I$(top_srcdir)/service/inc/public
AM_CFLAGS += $(GLIB_CFLAGS) -include glib.h @KVH2XML_CFLAGS@

if USE_G_STR_FUNC
AM_CFLAGS += -Dstrlcpy=g_strlcpy -Dstrlcat=g_strlcat
endif

AM_CFLAGS += -DBACKEND_CONF_FILE=\"/etc/backend_conf.xml\"
lib_LTLIBRARIES      = libagmmixer.la
libagmmixer_la_SOURCES   = agmmixer.c
libagmmixer_la_CFLAGS = $(AM_CFLAGS)
libagmmixer_la_LDFLAGS   = -avoid-version -shared
libagmmixer_la_LIBADD   = -ltinyalsa -ldl -lexpat @GLIB_LIBS@

bin_PROGRAMS = agmplay
agmplay_SOURCES  = agmplay.c

agmplay_la_CFLAGS = $(AM_CFLAGS)
agmplay_CFLAGS = $(AM_CFLAGS)
if ENABLE_BOOTKPI_LOGGING
agmplay_CFLAGS += -DAGM_ENABLE_BOOTKPI_LOGGING
endif
agmplay_LDADD    = -ltinyalsa libagmmixer.la

bin_PROGRAMS += agmcap
agmcap_SOURCES  = agmcap.c

agmcap_la_CFLAGS = $(AM_CFLAGS)
agmcap_LDADD    = -ltinyalsa libagmmixer.la

bin_PROGRAMS += agmhostless
agmhostless_SOURCES  = agmhostless.c

agmhostless_la_CFLAGS = $(AM_CFLAGS)
agmhostless_LDADD    = -ltinyalsa libagmmixer.la

bin_PROGRAMS += agm_shared_memory_test
agm_shared_memory_test_SOURCES  = agm_shared_memory_test.c

agm_shared_memory_test_la_CFLAGS = $(AM_CFLAGS)
agm_shared_memory_test_LDADD    = -ltinyalsa libagmmixer.la

# install xml files under /etc
root_etcdir      = "/etc"
root_etc_SCRIPTS = backend_conf.xml
install-data-hook:
	chmod  go-x $(DESTDIR)$(root_etcdir)/backend_conf.xml
