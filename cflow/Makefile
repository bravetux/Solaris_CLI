# Makefile for cflow (Solaris 8 port, from-scratch v1)

UNAME_S := $(shell uname -s)
CC      ?= gcc
PREFIX  ?= /usr/local
BIN     := cflow
SRC     := cflow.c

COMMON_DEFS := -D_POSIX_C_SOURCE=200112L

ifeq ($(UNAME_S),SunOS)
  ifeq ($(CC),cc)
    CFLAGS  ?= -xc99 $(COMMON_DEFS)
  else
    CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
  endif
else
  CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
endif

LDLIBS :=

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
