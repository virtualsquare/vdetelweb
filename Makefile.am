bin_PROGRAMS = vdetelweb
man_MANS = vdetelweb.1

vdeconfdir = $(sysconfdir)/vde
vdeconf_DATA = vdetelwebrc

EXTRA_DIST = ${man_MANS} vdetelwebrc

vdetelweb_SOURCES = vdetelweb.c web.c telnet.c vdetelweb.h
vdetelweb_LDADD = -llwipv6 -lvdehist

vdetelweb_CFLAGS = -Wall
