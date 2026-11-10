PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

DEPS = xcb xcb-randr wayland-client
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -fPIC -Ibuild $(shell $(PKG_CONFIG) --cflags $(DEPS))
LDLIBS = $(shell $(PKG_CONFIG) --libs $(DEPS)) -ldl -lpthread -lm

XDG_OUTPUT = $(WAYLAND_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml

all: build/libsteamscale.so

build:
	mkdir -p build

build/xdg-output-unstable-v1-client-protocol.h: $(XDG_OUTPUT) | build
	$(WAYLAND_SCANNER) client-header $< $@

build/xdg-output-unstable-v1-protocol.c: $(XDG_OUTPUT) | build
	$(WAYLAND_SCANNER) private-code $< $@

build/libsteamscale.so: src/steamscale.c build/xdg-output-unstable-v1-protocol.c build/xdg-output-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ src/steamscale.c build/xdg-output-unstable-v1-protocol.c $(LDLIBS)

install: all
	install -Dm755 build/libsteamscale.so $(DESTDIR)$(LIBDIR)/steam-scale/libsteamscale.so
	install -d $(DESTDIR)$(BINDIR)
	sed 's|@LIBDIR@|$(LIBDIR)|' src/steam-scale.in > $(DESTDIR)$(BINDIR)/steam-scale
	chmod 755 $(DESTDIR)$(BINDIR)/steam-scale

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/steam-scale/libsteamscale.so $(DESTDIR)$(BINDIR)/steam-scale
	-rmdir $(DESTDIR)$(LIBDIR)/steam-scale

clean:
	rm -rf build

.PHONY: all install uninstall clean
