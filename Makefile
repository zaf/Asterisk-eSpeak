#
# Makefile for Asterisk espeak application
# Copyright (C) 2009, Lefteris Zafiris
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 2. See the COPYING file
# at the top of the source tree.

INSTALL=install
BITS=$(shell getconf LONG_BIT)
ifeq ($(BITS),64)
   ASTLIBDIR=$(INSTALL_PREFIX)/usr/lib64/asterisk
else
   ASTLIBDIR=$(INSTALL_PREFIX)/usr/lib/asterisk
endif
MODULES_DIR=$(ASTLIBDIR)/modules
ASTETCDIR=$(INSTALL_PREFIX)/etc/asterisk
SAMPLENAME=espeak.conf.sample
CONFNAME=$(basename $(SAMPLENAME))

CC=gcc
OPTIMIZE=-O2
DEBUG=-g

LIBS+=-lm -lespeak -lsndfile -lsamplerate
CFLAGS+=-pipe -fPIC -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -D_REENTRANT -D_GNU_SOURCE

all: _all
	@echo " +-------- app_espeak Build Complete --------+"  
	@echo " + app_espeak has successfully been built,   +"  
	@echo " + and can be installed by running:          +"
	@echo " +                                           +"
	@echo " +               make install                +"  
	@echo " +-------------------------------------------+" 

_all: app_espeak.so

app_espeak.o: app_espeak.c
	$(CC) $(CFLAGS) $(DEBUG) $(OPTIMIZE) -c -o app_espeak.o app_espeak.c

app_espeak.so: app_espeak.o
	$(CC) -shared -Xlinker -x -o $@ $< $(LIBS)

clean:
	rm -f app_espeak.o app_espeak.so .*.d

install: _all
	$(INSTALL) -m 755 -d $(MODULES_DIR)
	$(INSTALL) -m 755 app_espeak.so $(MODULES_DIR)
	@echo " +---- app_espeak Installation Complete -----+"  
	@echo " +                                           +"
	@echo " + app_espeak has successfully been installed+"  
	@echo " + If you would like to install the sample   +"  
	@echo " + configuration file run:                   +"
	@echo " +                                           +"
	@echo " +              make samples                 +"
	@echo " +-------------------------------------------+"

samples:
	@mkdir -p $(ASTETCDIR)
	@if [ -f $(ASTETCDIR)/$(CONFNAME) ]; then \
		echo "Backing up previous config file as $(CONFNAME).old";\
		mv -f $(ASTETCDIR)/$(CONFNAME) $(ASTETCDIR)/$(CONFNAME).old ; \
	fi ;
	@echo "Installing new config file $(CONFNAME)";\
	$(INSTALL) -m 644 $(SAMPLENAME) $(ASTETCDIR)/$(CONFNAME);
	@echo " ------- app_esepak confing Installed --------"

