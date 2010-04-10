CC=gcc
RM=rm -f
INSTALL=install
SRC=gdownload.c
SHARE=$(PREF)share/gdownload
ARCH=$(shell uname -o) $(shell uname -m)
COMMIT=$(shell ./hash.sh)
CFLAGS=-std=c99 -O2 -g $(shell pkg-config --cflags gtk+-2.0)
LIBS=$(shell pkg-config --libs gtk+-2.0 gthread-2.0 libcurl)
PREF=/usr/local/
BIN=$(PREF)bin/

all: gdownload

gdownload: $(SRC)
	@echo COMPILING $^
	@$(CC) -DSHARE="\"$(SHARE)\"" -DARCH="\"$(ARCH)\"" -DCOMMIT="\"$(COMMIT)\"" $(CFLAGS) $(LIBS) $^ -o $@
	@echo ... done.
.PHONY : clean install


clean:
	$(RM) $(OBJ) gdownload

strip:
	@echo STRPPING
	@strip gdownload
	@echo ... done.

install: all
	mkdir -p $(SHARE)
	$(INSTALL) gdownload $(BIN)
	$(INSTALL) gdownload.png $(SHARE)
