#
# Makefile for Asterisk espeak application
# Copyright (C) 2009 - 2016, Lefteris Zafiris
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
SAMPLENAME:=espeak.conf.sample
CONFNAME:=$(basename $(SAMPLENAME))

INSTALL:=install
CC:=gcc
OPTIMIZE:=-O2
DEBUG:=-g

LIBS+=-lespeak-ng -lsamplerate
CFLAGS+=-pipe -fPIC -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -D_REENTRANT -D_GNU_SOURCE -DAST_MODULE_SELF_SYM=__internal_app_espeak_self

all: app_espeak.so
	@echo " +-------- app_espeak Build Complete --------+"
	@echo " + app_espeak has successfully been built,   +"
	@echo " + and can be installed by running:          +"
	@echo " +                                           +"
	@echo " +               make install                +"
	@echo " +-------------------------------------------+"

app_espeak.o: app_espeak.c
	$(CC) $(CFLAGS) $(DEBUG) $(OPTIMIZE) -c -o $@ $*.c

app_espeak.so: app_espeak.o
	$(CC) -shared -Xlinker -x -o $@ $< $(LIBS)

clean:
	rm -f app_espeak.o app_espeak.so

install: all
	$(INSTALL) -m 755 -d $(DESTDIR)$(MODULES_DIR)
	$(INSTALL) -m 755 app_espeak.so $(DESTDIR)$(MODULES_DIR)
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
	@echo " ------- app_esepak confing Installed --------"
