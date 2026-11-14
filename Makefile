# pm: the kit package manager. Built outside ucraft (on the host).
#   make           host binary, for development
#   make static    static-PIE binary with the ucraft toolchain (musl); needs ucraft-env.sh loaded
#   make install   install into $(DESTDIR)$(PREFIX)/bin (what the kit recipe runs)
#   make dist      source tarball: build/kit-$(VERSION).tar.gz
VERSION  = 0.1.0
PREFIX   = /usr
CC       = cc   # not "?=": CC from the ucraft cross environment must not leak into the host build
CFLAGS   = -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter
SRC      = $(wildcard src/*.c)
OBJ      = $(SRC:src/%.c=build/%.o)

all: build/kit

build/kit: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

static:
	$(MAKE) clean
	$(MAKE) CC="$${UCRAFT_TGT:-x86_64-ucraft-linux-musl}-cc" CFLAGS="-O2 -fPIE" LDFLAGS="-static-pie" build/kit

install: build/kit
	install -Dm755 build/kit $(DESTDIR)$(PREFIX)/bin/kit

dist:
	mkdir -p build
	tar -czf build/kit-$(VERSION).tar.gz --transform 's,^,kit-$(VERSION)/,' Makefile README.md src

clean:
	rm -rf build

.PHONY: all static install dist clean
