CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion
HARDENING = -D_FORTIFY_SOURCE=2 -fstack-protector-strong
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0 gio-unix-2.0)
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0 gio-unix-2.0)
COMMON_FLAGS = $(CFLAGS) $(WARNINGS) $(HARDENING) -std=c11 -Isrc $(GTK_CFLAGS)

APP_SRCS = src/main.c src/app.c src/ui.c src/session.c src/management.c \
           src/line_reader.c src/core.c
APP_OBJS = $(APP_SRCS:.c=.o)

.PHONY: all clean test install-user

all: openvpn-manager test_core

openvpn-manager: $(APP_OBJS)
	$(CC) $(COMMON_FLAGS) -o $@ $^ $(GTK_LIBS)

test_core: tests/test_core.o src/core.o
	$(CC) $(COMMON_FLAGS) -o $@ $^ $(GTK_LIBS)

src/%.o: src/%.c src/core.h src/app.h src/line_reader.h
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
