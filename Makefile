CC = cc
VERSION := $(shell git describe --tags --always 2>/dev/null || echo "unknown")
SDBUS_CFLAGS := $(shell pkg-config --cflags libsystemd 2>/dev/null)
SDBUS_LIBS := $(shell pkg-config --libs libsystemd 2>/dev/null)
FREETYPE ?= $(shell pkg-config --exists freetype2 2>/dev/null && echo 1 || echo 0)
ifeq ($(FREETYPE),1)
  FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2 fontconfig)
  FREETYPE_LIBS := $(shell pkg-config --libs freetype2 fontconfig)
endif
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Iinclude -Itests -g -DLNOTIFY_VERSION=\"$(VERSION)\" $(SDBUS_CFLAGS) $(if $(filter 1,$(FREETYPE)),-DHAVE_FREETYPE) $(FREETYPE_CFLAGS)
LDFLAGS = -lpthread $(SDBUS_LIBS) $(FREETYPE_LIBS)

# Source files (add as created)
COMMON_SRC = src/log.c src/notification.c src/protocol.c src/config.c src/engine.c src/resolver.c src/logind.c src/queue.c src/render_util.c src/font.c src/font_bitmap.c src/engine_fb.c src/engine_dbus.c src/engine_queue.c src/engine_terminal.c
ifeq ($(FREETYPE),1)
  COMMON_SRC += src/font_freetype.c
endif
DAEMON_SRC = src/daemon/main.c src/daemon/socket.c src/daemon/ssh_delivery.c $(COMMON_SRC)
CLIENT_SRC = src/client/main.c src/daemon/socket.c $(COMMON_SRC)
TEST_SRC   = tests/test_main.c tests/test_notification.c tests/test_protocol.c tests/test_config.c tests/test_resolver.c tests/test_queue.c tests/test_render_util.c $(COMMON_SRC)

.PHONY: all clean test daemon client

all: daemon client

daemon: build/lnotifyd
client: build/lnotify

build/lnotifyd: $(DAEMON_SRC) | build
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(LDFLAGS)

build/lnotify: $(CLIENT_SRC) | build
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC) $(LDFLAGS)

build/test_runner: $(TEST_SRC) | build
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS)

build:
	mkdir -p build

test: build/test_runner
	./build/test_runner

clean:
	rm -rf build
