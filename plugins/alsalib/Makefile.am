pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = agmalsaplugin.pc
EXTRA_DIST = $(pkgconfig_DATA)

library_includedir = $(includedir)

AM_CFLAGS := -Wno-unused-parameter
AM_CFLAGS += -I $(top_srcdir)/include -I $(PKG_CONFIG_SYSROOT_DIR)/usr/include/agm/ -I $(PKG_CONFIG_SYSROOT_DIR)/usr/include
AM_CFLAGS += $(GLIB_CFLAGS) -Dstrlcpy=g_strlcpy -Dstrlcat=g_strlcat# -include glib.h
AM_CFLAGS += -D__unused=__attribute__\(\(__unused__\)\) -D__LINUX__

AM_CFLAGS += -I $(top_srcdir)/../../snd_parser/inc
AGM_NODELETE_LDFLAGS = -Wl,-z,nodelete

lib_LTLIBRARIES      = libasound_module_pcm_agm.la
libasound_module_pcm_agm_la_SOURCES   = src/agm_io_plugin.c
libasound_module_pcm_agm_la_CFLAGS := $(AM_CFLAGS)
libasound_module_pcm_agm_la_LDFLAGS  = -lasound -lsndcardparser -avoid-version -shared
libasound_module_pcm_agm_la_LDFLAGS += -lagmclient
libasound_module_pcm_agm_la_LDFLAGS += $(AGM_NODELETE_LDFLAGS)

lib_LTLIBRARIES      += libasound_module_ctl_agm.la
libasound_module_ctl_agm_la_SOURCES   = src/agm_ctl_plugin.c
libasound_module_ctl_agm_la_CFLAGS := $(AM_CFLAGS)
libasound_module_ctl_agm_la_LDFLAGS  = -lasound -lsndcardparser -avoid-version -shared
libasound_module_ctl_agm_la_LDFLAGS += -lagmclient
libasound_module_ctl_agm_la_LDFLAGS += $(AGM_NODELETE_LDFLAGS)
