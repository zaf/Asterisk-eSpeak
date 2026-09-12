#
# Makefile for Asterisk espeak application
# Copyright (C) 2009 - 2026, Lefteris Zafiris
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 2. See the COPYING file
# at the top of the source tree.

ASTLIBDIR:=$(shell awk '/moddir/{print $$3}' /etc/asterisk/asterisk.conf 2> /dev/null)
ifeq ($(strip $(ASTLIBDIR)),)
	MODULES_DIR:=$(INSTALL_PREFIX)/usr/lib/asterisk/modules
else
	MODULES_DIR:=$(INSTALL_PREFIX)$(ASTLIBDIR)
endif
ASTETCDIR:=$(INSTALL_PREFIX)/etc/asterisk
ASTDATADIR:=$(shell awk '/astdatadir/{print $$3}' /etc/asterisk/asterisk.conf 2> /dev/null)
ifeq ($(strip $(ASTDATADIR)),)
	ASTDATADIR:=/var/lib/asterisk
endif
DOCSDIR:=$(INSTALL_PREFIX)$(ASTDATADIR)/documentation/thirdparty
XMLDOC:=app_espeak-en_US.xml
SAMPLENAME:=espeak.conf.sample
CONFNAME:=$(basename $(SAMPLENAME))

INSTALL:=install
CC:=gcc
OPTIMIZE:=-O2
DEBUG:=-g

LIBS+=-lespeak-ng -lsamplerate
# keep a flag only if the compiler supports it (older gcc, clang, other arches)
cc_opt=$(shell $(CC) -Werror $(1) -E -xc /dev/null >/dev/null 2>&1 && echo $(1))
CFLAGS+=-pipe -fPIC -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wshadow -Wformat=2 $(call cc_opt,-Wnull-dereference) \
	-D_REENTRANT -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
	$(call cc_opt,-fstack-protector-strong) $(call cc_opt,-fstack-clash-protection)
LDFLAGS+=-Wl,-z,relro -Wl,-z,now

.PHONY: all clean install samples

all: app_espeak.so $(XMLDOC)
	@echo " +-------- app_espeak Build Complete --------+"
	@echo " + app_espeak has successfully been built,   +"
	@echo " + and can be installed by running:          +"
	@echo " +                                           +"
	@echo " +               make install                +"
	@echo " +-------------------------------------------+"

app_espeak.o: app_espeak.c
	$(CC) $(CFLAGS) $(DEBUG) $(OPTIMIZE) -c -o $@ $*.c

app_espeak.so: app_espeak.o
	$(CC) -shared -o $@ $< $(LDFLAGS) $(LIBS)

$(XMLDOC): app_espeak.c
	@echo '<?xml version="1.0" encoding="UTF-8"?>' > $@
	@echo '<!DOCTYPE docs SYSTEM "appdocsxml.dtd">' >> $@
	@echo '<docs xmlns:xi="http://www.w3.org/2001/XInclude">' >> $@
	@sed -n '/^\/\*\*\* DOCUMENTATION/,/\*\*\*\//p' app_espeak.c | sed '1d;$$d' >> $@
	@echo '</docs>' >> $@

clean:
	rm -f app_espeak.o app_espeak.so $(XMLDOC)

install: all
	$(INSTALL) -m 755 -d $(DESTDIR)$(MODULES_DIR)
	$(INSTALL) -m 755 app_espeak.so $(DESTDIR)$(MODULES_DIR)
	$(INSTALL) -m 755 -d $(DESTDIR)$(DOCSDIR)
	$(INSTALL) -m 644 $(XMLDOC) $(DESTDIR)$(DOCSDIR)
	@echo " Run 'xmldoc reload' in the Asterisk CLI to load the docs."
	@echo " +---- app_espeak Installation Complete -----+"
	@echo " +                                           +"
	@echo " + app_espeak has successfully been installed+"
	@echo " + If you would like to install the sample   +"
	@echo " + configuration file run:                   +"
	@echo " +                                           +"
	@echo " +              make samples                 +"
	@echo " +-------------------------------------------+"

samples:
	@mkdir -p $(DESTDIR)$(ASTETCDIR)
	@if [ -f $(DESTDIR)$(ASTETCDIR)/$(CONFNAME) ]; then \
		echo "Backing up previous config file as $(CONFNAME).old";\
		mv -f $(DESTDIR)$(ASTETCDIR)/$(CONFNAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME).old ; \
	fi ;
	$(INSTALL) -m 644 $(SAMPLENAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME)
	@echo " ------- app_espeak config Installed ---------"
