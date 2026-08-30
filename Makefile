CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion
HARDENING = -D_FORTIFY_SOURCE=2 -fstack-protector-strong
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0 gio-unix-2.0)
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0 gio-unix-2.0)
COMMON_FLAGS = $(CFLAGS) $(WARNINGS) $(HARDENING) -std=c11 -Isrc $(GTK_CFLAGS)

.PHONY: all clean test install-user

all: openvpn-manager test_core

openvpn-manager: src/main.o src/core.o
	$(CC) $(COMMON_FLAGS) -o $@ $^ $(GTK_LIBS)

test_core: tests/test_core.o src/core.o
	$(CC) $(COMMON_FLAGS) -o $@ $^ $(GTK_LIBS)

src/%.o: src/%.c src/core.h
	$(CC) $(COMMON_FLAGS) -c -o $@ $<

tests/%.o: tests/%.c src/core.h
	$(CC) $(COMMON_FLAGS) -c -o $@ $<

test: test_core
	./test_core

install-user: openvpn-manager
	install -Dm755 openvpn-manager $(HOME)/.local/bin/openvpn-manager
	sed 's|^Exec=.*|Exec=$(HOME)/.local/bin/openvpn-manager|' openvpn-manager.desktop | install -Dm644 /dev/stdin $(HOME)/.local/share/applications/openvpn-manager.desktop

clean:
	rm -f openvpn-manager test_core src/*.o tests/*.o
