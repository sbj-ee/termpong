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

termpong: termpong.cpp
	$(CXX) $(CXXFLAGS) $(CURSES_CFLAGS) -o $@ $< $(CURSES_LIBS)

# Headless simulation tests: physics, collisions, scoring and the opponent.
test: tests/test_pong
	./tests/test_pong

tests/test_pong: tests/test_pong.cpp termpong.cpp
	$(CXX) $(CXXFLAGS) $(CURSES_CFLAGS) -o $@ $< $(CURSES_LIBS)

install: termpong
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 termpong $(DESTDIR)$(PREFIX)/bin/termpong

clean:
	rm -f termpong tests/test_pong

.PHONY: all test install clean
