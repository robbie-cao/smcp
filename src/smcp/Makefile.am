AM_CPPFLAGS =  -I.. -I$(top_srcdir)/src
#AM_CFLAGS = @CFLAGS@ @WARN_CFLAGS@

AUTOMAKE_OPTIONS = subdir-objects

lib_LTLIBRARIES = libsmcp.la

@CODE_COVERAGE_RULES@

AM_LIBS = $(CODE_COVERAGE_LDFLAGS)
AM_CFLAGS = $(CFLAGS) $(CODE_COVERAGE_CFLAGS)

libsmcp_la_SOURCES = smcp.c smcp-timer.c coap.c smcp-outbound.c smcp-inbound.c smcp-observable.c smcp-transaction.c smcp-dupe.c smcp-missing.c smcp-session.c smcp-async.c
libsmcp_la_SOURCES += smcp-plat-bsd.c
libsmcp_la_SOURCES += btree.c url-helpers.c fasthash.c string-utils.c

libsmcp_la_SOURCES += btree.h coap.h ll.h smcp-helpers.h smcp-internal.h smcp-logging.h url-helpers.h fasthash.h  smcp-dupe.h string-utils.h smcp-missing.h smcp-async.h smcp-defaults.h
pkginclude_HEADERS = assert-macros.h smcp-timer.h smcp.h smcp-plat-bsd.h smcp-transaction.h smcp-opts.h smcp-observable.h btree.h coap.h ll.h smcp-helpers.h smcp-session.h smcp-async.h smcp-defaults.h smcp-plat.h

# Extras
libsmcp_la_SOURCES += smcp-node-router.c smcp-list.c
pkginclude_HEADERS += smcp-node-router.h
libsmcp_la_SOURCES += smcp-variable_handler.c
pkginclude_HEADERS += smcp-variable_handler.h

EXTRA_DIST = smcp-plat-bsd-internal.h smcp-plat-uip-internal.h smcp-plat-uip.c smcp-plat-uip.h

libsmcp_la_LIBADD = $(LIBOBJS) $(ALLOCA) $(PTHREAD_LIBS) $(OPENSSL_LIBS)
libsmcp_la_CFLAGS = $(AM_CFLAGS) $(PTHREAD_CFLAGS) $(OPENSSL_AM_CPPFLAGS)

if HAVE_LIBCURL
libsmcp_la_SOURCES += smcp-curl_proxy.c
pkginclude_HEADERS +=  smcp-curl_proxy.h
endif

noinst_PROGRAMS = btreetest
btreetest_SOURCES = btree.c
btreetest_CFLAGS = $(AM_CFLAGS) -DBTREE_SELF_TEST=1

DISTCLEANFILES = .deps Makefile

TESTS = btreetest
