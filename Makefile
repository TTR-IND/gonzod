CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX   ?= /usr/local
SBINDIR  := $(PREFIX)/sbin

.PHONY: all clean install uninstall

all: gonzod

gonzod: gonzod.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f gonzod

install: gonzod
	install -m 0755 -o root -g root gonzod $(SBINDIR)/gonzod

uninstall:
	rm -f $(SBINDIR)/gonzod
