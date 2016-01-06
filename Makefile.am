
ACLOCAL_AMFLAGS = -I m4

moddir = @APACHE_MODULEDIR@
mod_LTLIBRARIES = mod_woothee.la

mod_woothee_la_SOURCES = \
	woothee/src/woothee.c \
	woothee/src/util.c \
	woothee/src/crawler.c \
	woothee/src/browser.c \
	woothee/src/os.c \
	woothee/src/mobilephone.c \
	woothee/src/appliance.c \
	woothee/src/misc.c \
	mod_woothee.c

mod_woothee_la_CFLAGS = @APACHE_CFLAGS@ -Iwoothee/src
mod_woothee_la_CPPFLAGS = @APACHE_CPPFLAGS@ -Iwoothee/src
mod_woothee_la_LDFLAGS = -avoid-version -module @APACHE_LDFLAGS@
mod_woothee_la_LIBS = @APACHE_LIBS@
