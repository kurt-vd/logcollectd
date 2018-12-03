PROGS	= logcollectd logcollect
default	: $(PROGS)

LOCALVERSION:= $(shell git describe --always --tags --dirty)
PREFIX	= /usr/local
CFLAGS	= -g0 -Os -Wall
CPPFLAGS = -D_GNU_SOURCE

-include config.mk

CPPFLAGS+= -DVERSION=\"$(LOCALVERSION)\"

.PHONY: clean install

logcollectd: lib/libt.o lib/libe.o


clean:
	rm -rf $(PROGS) $(wildcard *.o lib/*.o)

install: $(PROGS)
	@[ -d $(DESTDIR)$(PREFIX)/sbin ] || install -v -d $(DESTDIR)$(PREFIX)/sbin
	@install -v logcollectd $(DESTDIR)$(PREFIX)/sbin
	@[ -d $(DESTDIR)$(PREFIX)/bin ] || install -v -d $(DESTDIR)$(PREFIX)/bin
	@install -v $(filter-out logcollectd, $^) $(DESTDIR)$(PREFIX)/bin
