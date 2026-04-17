# Makefile for watch (Solaris 8 port)
# Auto-detects Linux vs SunOS. Respects CC override (e.g. CC=cc for Sun Studio).

UNAME_S := $(shell uname -s)
CC      ?= gcc
PREFIX  ?= /usr/local
BIN     := watch
SRC     := watch.c

COMMON_DEFS := -D_POSIX_C_SOURCE=200112L

ifeq ($(UNAME_S),SunOS)
  # Solaris: nanosleep needs -lrt (librt / libposix4 alias on Solaris 8).
  ifeq ($(CC),cc)
    CFLAGS  ?= -xc99 $(COMMON_DEFS)
  else
    CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
  endif
  LDLIBS  := -lrt
else
  # Linux (development target).
  CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
  LDLIBS  :=
endif

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)

install: $(BIN)
	mkdir -p $(PREFIX)/bin
	cp $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

.PHONY: all clean install uninstall
