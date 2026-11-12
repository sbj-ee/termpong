# termpong - classic pong for the terminal

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
PREFIX   ?= /usr/local

# ncursesw where pkg-config knows about it, plain -lncursesw otherwise.
PKG := $(shell pkg-config --exists ncursesw 2>/dev/null && echo yes)
ifeq ($(PKG),yes)
  CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw)
  CURSES_LIBS   := $(shell pkg-config --libs ncursesw)
else
  CURSES_CFLAGS := -D_XOPEN_SOURCE_EXTENDED
  CURSES_LIBS   := -lncursesw
endif

all: termpong

termpong: termpong.cpp pong_core.h
	$(CXX) $(CXXFLAGS) $(CURSES_CFLAGS) -o $@ $< $(CURSES_LIBS)

# Headless simulation tests: physics, collisions, scoring and the opponent.
test: tests/test_pong
	./tests/test_pong

tests/test_pong: tests/test_pong.cpp termpong.cpp pong_core.h
	$(CXX) $(CXXFLAGS) $(CURSES_CFLAGS) -o $@ $< $(CURSES_LIBS)

# Windowed front end; needs GTK 4.  Not part of 'all' so the terminal game
# still builds on machines without GTK.
gtkpong: gtkpong.cpp pong_core.h
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags gtk4) -o $@ $< $(shell pkg-config --libs gtk4)

install: termpong
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 termpong $(DESTDIR)$(PREFIX)/bin/termpong

clean:
	rm -f termpong gtkpong tests/test_pong

.PHONY: all test install clean
