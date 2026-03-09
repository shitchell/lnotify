# lnotify Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a universal Linux toast notification system (daemon + CLI) that detects the active session type and renders notifications via D-Bus, framebuffer, or SSH terminal, with a queue fallback.

**Architecture:** Three components — `lnotifyd` daemon (engine resolver, VT monitor, notification queue, SSH delivery), `lnotify` CLI client (fire-and-forget over Unix socket), optional D-Bus listener inside daemon. Engine vtable with layered probe pipeline selects the right backend per session.

**Tech Stack:** C (C11), Make, FreeType (v1: bitmap font), libdbus-1 (via dlopen), POSIX sockets, pthreads, Linux sysfs/ioctl.

**Design spec:** `docs/plans/2026-03-09-lnotify-design-v2.md` — read this in full before starting any task.

**Decisions log:** `docs/DECISIONS.md` — check this if you're unsure why something is designed a certain way.

**Lessons learned:** `docs/plans/2026-03-09-lessons-learned.md` — empirical findings from prototyping. Critical reading for framebuffer and D-Bus tasks.

---

## Prerequisites

Before starting any implementation task, ensure the build environment is ready:

```bash
# Required build tools
sudo apt-get install -y build-essential pkg-config

# Required libraries
sudo apt-get install -y libdbus-1-dev libfreetype-dev

# Required testing tools
sudo apt-get install -y fbgrab

# Verify
cc --version
pkg-config --exists dbus-1 && echo "dbus-1 OK"
pkg-config --exists freetype2 && echo "freetype2 OK"
which fbgrab && echo "fbgrab OK"
which script && echo "script OK"
which chvt && echo "chvt OK"
which gdbus && echo "gdbus OK"
which loginctl && echo "loginctl OK"
which tmux && echo "tmux OK"
```

If any tools are missing, install them. You have `sudo` access. Tests depend on these tools being available.

---

## Testing Philosophy

**Manual first, automate second.** For every component that interacts with hardware or system services:

1. Build the component
2. Test it manually — use `sudo chvt`, `fbgrab`, `script`, `tmux`, localhost SSH
3. Observe and understand the actual behavior
4. Capture manual test output as a reference/golden file
5. Then write the automated test that verifies against that golden

**Unit tests** cover pure logic (no hardware): wire protocol, config parser, resolver loop, queue, data model. These follow standard TDD — write the failing test first.

**Integration tests** use mock engines and mock probes to verify the resolver pipeline without real sessions.

**System tests** (manual + golden) cover framebuffer rendering, D-Bus delivery, SSH terminal output, VT detection.

**Debug/verbose logging:** All daemon components use a `log_debug()` function gated by `--debug` flag. This produces timestamped, structured output showing engine evaluation, probe results, context state. Essential for development and troubleshooting.

**`--dry-run` on CLI:** `lnotify --dry-run "test"` connects to the daemon and prints what *would* happen (which engine selected, probe results, context) without rendering. Useful for debugging engine selection on a live system.

---

## Project Structure

```
lnotify/
├── Makefile
├── include/
│   ├── lnotify.h           # notification struct, shared constants
│   ├── config.h             # config struct, parser declarations
│   ├── protocol.h           # wire format serialize/deserialize
│   ├── engine.h             # engine vtable, probe_key, session_context
│   ├── resolver.h           # engine resolver loop
│   ├── queue.h              # notification queue
│   ├── log.h                # logging (debug/info/error)
│   ├── render_util.h        # shared rendering: rounded rect, text, colors
│   ├── font_bitmap.h        # embedded 8x8 bitmap font
│   ├── engine_dbus.h        # D-Bus engine
│   ├── engine_fb.h          # framebuffer engine
│   ├── engine_terminal.h    # SSH terminal engine
│   └── engine_queue.h       # queue engine (universal fallback)
├── src/
│   ├── daemon/
│   │   ├── main.c           # daemon entry point, event loop, VT monitor
│   │   ├── socket.c         # Unix socket listener, client handling
│   │   └── ssh_delivery.c   # SSH session discovery + pty notification
│   ├── client/
│   │   └── main.c           # lnotify CLI entry point
│   ├── config.c             # config parser
│   ├── protocol.c           # wire format serialize/deserialize
│   ├── resolver.c           # engine resolver loop
│   ├── queue.c              # notification queue
│   ├── log.c                # logging
│   ├── render_util.c        # shared rendering utilities
│   ├── font_bitmap.c        # embedded bitmap font data
│   ├── engine_dbus.c        # D-Bus engine detect/render
│   ├── engine_fb.c          # framebuffer engine detect/render/defense
│   ├── engine_terminal.c    # SSH terminal engine (OSC/tmux/overlay/text)
│   └── engine_queue.c       # queue engine
├── tests/
│   ├── test_main.c          # test runner
│   ├── test_protocol.c      # wire format tests
│   ├── test_config.c        # config parser tests
│   ├── test_resolver.c      # resolver loop with mock engines
│   ├── test_queue.c         # queue behavior tests
│   ├── test_notification.c  # data model tests (dedup, expiration)
│   ├── test_render_util.c   # rendering math tests
│   ├── golden/              # golden test files
│   │   ├── fb/              # framebuffer capture PNGs
│   │   └── terminal/        # script(1) captures of terminal output
│   └── manual/              # manual test scripts + instructions
│       ├── test_fb.sh        # framebuffer manual test
│       ├── test_dbus.sh      # D-Bus manual test
│       ├── test_vt.sh        # VT switch manual test
│       ├── test_ssh.sh       # SSH terminal manual test
│       └── README.md         # instructions for running manual tests
├── docs/
│   ├── DECISIONS.md
│   └── plans/
│       ├── 2026-03-09-lnotify-design-v2.md
│       ├── 2026-03-09-lessons-learned.md
│       └── 2026-03-09-lnotify-implementation.md  # this file
└── prototype/               # moved from root: Python PoC scripts + logs
```

---

## Task 1: Project Scaffolding & Build System

**Goal:** Makefile, directory structure, a "hello world" that compiles and runs, plus the test harness.

**Files:**
- Create: `Makefile`
- Create: `include/log.h`
- Create: `src/log.c`
- Create: `tests/test_main.c`
- Move: `vt-*.py`, `vt-*.log`, `__pycache__/` → `prototype/`

**Step 1: Create directory structure**

```bash
mkdir -p include src/daemon src/client tests/golden/fb tests/golden/terminal tests/manual prototype
mv vt-*.py vt-*.log __pycache__/ prototype/
```

**Step 2: Write the logging module**

`include/log.h`:
```c
#ifndef LNOTIFY_LOG_H
#define LNOTIFY_LOG_H

#include <stdbool.h>

// Set by --debug flag at startup
extern bool log_debug_enabled;

void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif
```

`src/log.c`:
```c
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

bool log_debug_enabled = false;

static void log_msg(const char *level, const char *fmt, va_list args) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%6ld.%03lds] %s: ",
            ts.tv_sec, ts.tv_nsec / 1000000, level);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log_debug(const char *fmt, ...) {
    if (!log_debug_enabled) return;
    va_list args;
    va_start(args, fmt);
    log_msg("DEBUG", fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_msg("INFO", fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_msg("ERROR", fmt, args);
    va_end(args);
}
```

**Step 3: Write a minimal test harness**

`tests/test_main.c`:
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Minimal test framework — no dependencies
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_STR_EQ(a, b, msg) ASSERT(strcmp((a), (b)) == 0, msg)
#define ASSERT_TRUE(a, msg) ASSERT((a), msg)
#define ASSERT_FALSE(a, msg) ASSERT(!(a), msg)
#define ASSERT_NULL(a, msg) ASSERT((a) == NULL, msg)
#define ASSERT_NOT_NULL(a, msg) ASSERT((a) != NULL, msg)

#define RUN_SUITE(name) do { \
    fprintf(stderr, "Suite: %s\n", #name); \
    name(); \
} while(0)

// Forward declarations — each test file provides a suite function
// Uncomment as test files are added:
// extern void test_protocol_suite(void);
// extern void test_config_suite(void);
// extern void test_resolver_suite(void);
// extern void test_queue_suite(void);
// extern void test_notification_suite(void);
// extern void test_render_util_suite(void);

// Placeholder suite to verify harness works
static void test_harness_suite(void) {
    ASSERT_TRUE(1, "sanity check");
    ASSERT_EQ(2 + 2, 4, "basic math");
}

int main(void) {
    fprintf(stderr, "lnotify test runner\n");
    fprintf(stderr, "===================\n\n");

    RUN_SUITE(test_harness_suite);

    fprintf(stderr, "\n===================\n");
    fprintf(stderr, "Results: %d passed, %d failed, %d total\n",
            tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
```

**Step 4: Write Makefile**

```makefile
CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -g
LDFLAGS = -lpthread

# Source files (add as created)
COMMON_SRC = src/log.c
DAEMON_SRC = src/daemon/main.c $(COMMON_SRC)
CLIENT_SRC = src/client/main.c $(COMMON_SRC)
TEST_SRC   = tests/test_main.c $(COMMON_SRC)

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
```

**Step 5: Write stub entry points so it compiles**

`src/daemon/main.c`:
```c
#include "log.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            log_debug_enabled = true;
        }
    }

    log_info("lnotifyd starting");
    log_debug("debug logging enabled");
    log_info("lnotifyd exiting (stub)");
    return 0;
}
```

`src/client/main.c`:
```c
#include "log.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    log_info("lnotify client (stub)");
    return 0;
}
```

**Step 6: Build and verify**

```bash
make all
./build/lnotifyd --debug
make test
```

Expected:
- `lnotifyd` prints startup/debug/exit messages
- Test runner prints `2 passed, 0 failed, 2 total`

**Step 7: Commit**

```bash
git add Makefile include/ src/ tests/ prototype/
git commit -m "scaffold: project structure, build system, logging, test harness"
```

---

## Task 2: Notification Data Model

**Goal:** `notification` struct with creation, dedup, expiration, and timestamp logic. Fully unit-tested.

**Files:**
- Create: `include/lnotify.h`
- Create: `src/notification.c`
- Create: `tests/test_notification.c`
- Modify: `tests/test_main.c` (register suite)
- Modify: `Makefile` (add sources)

**Step 1: Write the failing tests**

`tests/test_notification.c` — test creation, expiration, dedup:
```c
#include "lnotify.h"
#include <string.h>
#include <unistd.h>

// Uses ASSERT macros from test_main.c (included via header or
// copy the macros into a shared test_util.h — implementer's choice)

void test_notification_suite(void) {
    // Test: create notification with defaults
    {
        notification n = {0};
        notification_init(&n, "Test Title", "Test Body");
        ASSERT_STR_EQ(n.title, "Test Title", "title set");
        ASSERT_STR_EQ(n.body, "Test Body", "body set");
        ASSERT_EQ(n.priority, 1, "default priority is normal");
        ASSERT_EQ(n.timeout_ms, -1, "default timeout is -1 (use config)");
        ASSERT_NULL(n.app, "app defaults to NULL");
        ASSERT_NULL(n.group_id, "group_id defaults to NULL");
        ASSERT_TRUE(n.ts_sent > 0, "ts_sent populated");
        notification_free(&n);
    }

    // Test: notification not expired immediately
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        ASSERT_FALSE(notification_expired(&n, monotonic_ms()),
                     "not expired immediately");
        notification_free(&n);
    }

    // Test: notification expires after timeout
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 50;  // 50ms
        n.ts_mono = monotonic_ms();
        usleep(60 * 1000);  // sleep 60ms
        ASSERT_TRUE(notification_expired(&n, monotonic_ms()),
                    "expired after timeout");
        notification_free(&n);
    }

    // Test: remaining timeout calculation
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        int32_t remaining = notification_remaining_ms(&n, monotonic_ms());
        ASSERT_TRUE(remaining > 4900 && remaining <= 5000,
                    "remaining timeout approximately correct");
        notification_free(&n);
    }

    // Test: group_id dedup match
    {
        notification a = {0};
        notification_init(&a, "A", "body a");
        a.group_id = strdup("backup");

        notification b = {0};
        notification_init(&b, "B", "body b");
        b.group_id = strdup("backup");

        ASSERT_TRUE(notification_group_matches(&a, &b),
                    "same group_id matches");

        notification c = {0};
        notification_init(&c, "C", "body c");
        c.group_id = strdup("other");

        ASSERT_FALSE(notification_group_matches(&a, &c),
                     "different group_id doesn't match");

        notification d = {0};
        notification_init(&d, "D", "body d");
        // d.group_id is NULL

        ASSERT_FALSE(notification_group_matches(&a, &d),
                     "NULL group_id doesn't match non-NULL");
        ASSERT_FALSE(notification_group_matches(&d, &d),
                     "two NULL group_ids don't match (no dedup on NULL)");

        notification_free(&a);
        notification_free(&b);
        notification_free(&c);
        notification_free(&d);
    }
}
```

**Step 2: Run tests, verify they fail**

```bash
make test
```

Expected: compile error — `lnotify.h` doesn't exist yet.

**Step 3: Write the header**

`include/lnotify.h`:
```c
#ifndef LNOTIFY_H
#define LNOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint32_t    id;
    char       *title;          // optional (NULL = no title)
    char       *body;           // required
    uint8_t     priority;       // 0=low, 1=normal, 2=critical
    int32_t     timeout_ms;     // -1 = use config default
    char       *app;            // optional
    char       *group_id;       // optional, for dedup
    uint32_t    origin_uid;     // daemon-captured via SO_PEERCRED
    uint64_t    ts_sent;        // wall clock ms
    uint64_t    ts_received;    // wall clock ms
    uint64_t    ts_mono;        // monotonic ms (internal)
} notification;

// Get current monotonic time in milliseconds
uint64_t monotonic_ms(void);

// Get current wall clock time in milliseconds (Unix epoch)
uint64_t wallclock_ms(void);

// Initialize a notification with defaults. Copies title and body strings.
void notification_init(notification *n, const char *title, const char *body);

// Free owned strings in a notification.
void notification_free(notification *n);

// Check if notification has expired relative to a monotonic timestamp.
bool notification_expired(const notification *n, uint64_t now_mono);

// Get remaining timeout in ms. Returns 0 if expired.
int32_t notification_remaining_ms(const notification *n, uint64_t now_mono);

// Check if two notifications should dedup (both non-NULL group_ids match).
bool notification_group_matches(const notification *a, const notification *b);

#endif
```

**Step 4: Write the implementation**

`src/notification.c`:
```c
#include "lnotify.h"
#include <string.h>

uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t wallclock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void notification_init(notification *n, const char *title, const char *body) {
    memset(n, 0, sizeof(*n));
    n->title = title ? strdup(title) : NULL;
    n->body = body ? strdup(body) : NULL;
    n->priority = 1;        // normal
    n->timeout_ms = -1;     // use config default
    n->ts_sent = wallclock_ms();
}

void notification_free(notification *n) {
    free(n->title);
    free(n->body);
    free(n->app);
    free(n->group_id);
    n->title = n->body = n->app = n->group_id = NULL;
}

bool notification_expired(const notification *n, uint64_t now_mono) {
    if (n->timeout_ms < 0 || n->ts_mono == 0) return false;
    return (now_mono - n->ts_mono) >= (uint64_t)n->timeout_ms;
}

int32_t notification_remaining_ms(const notification *n, uint64_t now_mono) {
    if (n->timeout_ms < 0 || n->ts_mono == 0) return n->timeout_ms;
    uint64_t elapsed = now_mono - n->ts_mono;
    if (elapsed >= (uint64_t)n->timeout_ms) return 0;
    return (int32_t)((uint64_t)n->timeout_ms - elapsed);
}

bool notification_group_matches(const notification *a, const notification *b) {
    if (!a->group_id || !b->group_id) return false;
    return strcmp(a->group_id, b->group_id) == 0;
}
```

**Step 5: Update Makefile and test_main.c**

Add `src/notification.c` to `COMMON_SRC` and `tests/test_notification.c` to `TEST_SRC`. Uncomment the `test_notification_suite` declaration and `RUN_SUITE` call in `test_main.c`.

**Step 6: Run tests, verify they pass**

```bash
make test
```

Expected: all notification tests pass.

**Step 7: Commit**

```bash
git add include/lnotify.h src/notification.c tests/test_notification.c tests/test_main.c Makefile
git commit -m "feat: notification data model with dedup and expiration"
```

---

## Task 3: Wire Protocol

**Goal:** Serialize/deserialize notifications to/from the binary wire format. Fully unit-tested. Handles all field_mask combinations including forward-compatibility (unknown bits ignored).

**Files:**
- Create: `include/protocol.h`
- Create: `src/protocol.c`
- Create: `tests/test_protocol.c`
- Modify: `tests/test_main.c` (register suite)
- Modify: `Makefile`

**Reference:** Wire format spec in `docs/plans/2026-03-09-lnotify-design-v2.md` under "Wire Protocol."

**Step 1: Write the failing tests**

`tests/test_protocol.c` — test round-trip for various field combinations:
```c
#include "protocol.h"
#include "lnotify.h"
#include <string.h>

void test_protocol_suite(void) {
    // Test: minimal notification (body only)
    {
        notification orig = {0};
        notification_init(&orig, NULL, "hello world");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize minimal succeeds");

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len, &decoded);
        ASSERT_EQ(consumed, len, "deserialize consumes all bytes");
        ASSERT_NULL(decoded.title, "title is NULL");
        ASSERT_STR_EQ(decoded.body, "hello world", "body round-trips");
        ASSERT_EQ(decoded.priority, 1, "priority round-trips");
        ASSERT_EQ(decoded.timeout_ms, -1, "timeout round-trips");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: full notification (all fields)
    {
        notification orig = {0};
        notification_init(&orig, "Alert", "server on fire");
        orig.priority = 2;
        orig.timeout_ms = 10000;
        orig.app = strdup("monitoring");
        orig.group_id = strdup("server-health");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize full succeeds");

        notification decoded = {0};
        protocol_deserialize(buf, len, &decoded);
        ASSERT_STR_EQ(decoded.title, "Alert", "title round-trips");
        ASSERT_STR_EQ(decoded.body, "server on fire", "body round-trips");
        ASSERT_EQ(decoded.priority, 2, "priority round-trips");
        ASSERT_EQ(decoded.timeout_ms, 10000, "timeout round-trips");
        ASSERT_STR_EQ(decoded.app, "monitoring", "app round-trips");
        ASSERT_STR_EQ(decoded.group_id, "server-health",
                      "group_id round-trips");
        ASSERT_TRUE(decoded.ts_sent > 0, "ts_sent round-trips");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: buffer too small
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[2];  // way too small
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len < 0, "serialize fails on tiny buffer");
        notification_free(&orig);
    }

    // Test: truncated input
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len / 2, &decoded);
        ASSERT_TRUE(consumed < 0, "deserialize fails on truncated input");
        notification_free(&orig);
    }

    // Test: unknown field_mask bits are ignored
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialized ok");

        // Flip a high bit in field_mask (offset 4, uint16)
        // This simulates a newer client sending fields we don't know
        buf[4] |= 0x80;

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len, &decoded);
        // Should still succeed — unknown bits ignored
        ASSERT_TRUE(consumed > 0,
                    "deserialize succeeds with unknown field_mask bits");
        ASSERT_STR_EQ(decoded.body, "body",
                      "body still decoded correctly");
        notification_free(&decoded);
        notification_free(&orig);
    }
}
```

**Step 2: Run tests, verify failure**

```bash
make test
```

Expected: compile error — `protocol.h` doesn't exist yet.

**Step 3: Write the header**

`include/protocol.h`:
```c
#ifndef LNOTIFY_PROTOCOL_H
#define LNOTIFY_PROTOCOL_H

#include "lnotify.h"
#include <stdint.h>
#include <sys/types.h>

// Field mask bits
#define FIELD_TITLE   (1 << 0)
#define FIELD_APP     (1 << 1)
#define FIELD_GROUP   (1 << 2)

// Serialize a notification into buf. Returns bytes written, or -1 on error.
ssize_t protocol_serialize(const notification *n, uint8_t *buf, size_t buflen);

// Deserialize a notification from buf. Populates n (caller must free).
// Returns bytes consumed, or -1 on error.
ssize_t protocol_deserialize(const uint8_t *buf, size_t buflen, notification *n);

#endif
```

**Step 4: Write the implementation**

`src/protocol.c` — implement serialize/deserialize following the wire format from the design spec. Use `memcpy` for safe unaligned access. Validate all lengths before reading. Skip unknown field_mask bits. See the design spec's "Wire Protocol" section for the exact byte layout.

The implementation should:
- Write: `total_len`, `field_mask`, `priority`, `timeout_ms`, `ts_sent`, then length-prefixed strings for each set field
- Read: validate `total_len` against `buflen`, read fixed header, then iterate field_mask bits to read variable-length strings
- Unknown field_mask bits: log a debug warning but don't fail

**Step 5: Update Makefile and test_main.c, run tests**

```bash
make test
```

Expected: all protocol tests pass.

**Step 6: Commit**

```bash
git add include/protocol.h src/protocol.c tests/test_protocol.c tests/test_main.c Makefile
git commit -m "feat: wire protocol serialize/deserialize with field_mask"
```

---

## Task 4: Config Parser

**Goal:** Parse key=value config files, populate a config struct with defaults. Support `#RRGGBBAA` color parsing and all v1 config keys.

**Files:**
- Create: `include/config.h`
- Create: `src/config.c`
- Create: `tests/test_config.c`
- Modify: `tests/test_main.c`
- Modify: `Makefile`

**Reference:** Config section of `docs/plans/2026-03-09-lnotify-design-v2.md`.

**Step 1: Write the failing tests**

`tests/test_config.c`:
```c
#include "config.h"
#include <string.h>
#include <stdio.h>

void test_config_suite(void) {
    // Test: defaults populated without any file
    {
        lnotify_config cfg;
        config_defaults(&cfg);
        ASSERT_EQ(cfg.default_timeout, 5000, "default timeout is 5000");
        ASSERT_STR_EQ(cfg.position, "top-right", "default position");
        ASSERT_EQ(cfg.border_width, 2, "default border width");
        ASSERT_EQ(cfg.border_radius, 12, "default border radius");
        ASSERT_EQ(cfg.padding, 20, "default padding");
        ASSERT_EQ(cfg.margin, 30, "default margin");
        ASSERT_EQ(cfg.font_size, 14, "default font size");
    }

    // Test: parse a config file
    {
        // Write a temp config
        const char *path = "/tmp/lnotify_test.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "# comment line\n");
        fprintf(f, "default_timeout = 3000\n");
        fprintf(f, "position = bottom-left\n");
        fprintf(f, "bg_color = #112233FF\n");
        fprintf(f, "border_width = 4\n");
        fprintf(f, "ssh_users = alice bob\n");
        fprintf(f, "ssh_modes = overlay,text\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load succeeds");
        ASSERT_EQ(cfg.default_timeout, 3000, "timeout overridden");
        ASSERT_STR_EQ(cfg.position, "bottom-left", "position overridden");
        ASSERT_EQ(cfg.border_width, 4, "border_width overridden");
        ASSERT_STR_EQ(cfg.ssh_users, "alice bob", "ssh_users parsed");
        ASSERT_STR_EQ(cfg.ssh_modes, "overlay,text", "ssh_modes parsed");

        // Check color parsing (RRGGBBAA -> separate r,g,b,a)
        ASSERT_EQ(cfg.bg_color.r, 0x11, "bg red");
        ASSERT_EQ(cfg.bg_color.g, 0x22, "bg green");
        ASSERT_EQ(cfg.bg_color.b, 0x33, "bg blue");
        ASSERT_EQ(cfg.bg_color.a, 0xFF, "bg alpha");

        remove(path);
    }

    // Test: missing file returns error, defaults preserved
    {
        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, "/tmp/nonexistent_lnotify.conf");
        ASSERT_TRUE(rc != 0, "config_load fails on missing file");
        ASSERT_EQ(cfg.default_timeout, 5000, "defaults preserved");
    }

    // Test: malformed lines are skipped
    {
        const char *path = "/tmp/lnotify_test_bad.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "no_equals_sign\n");
        fprintf(f, "= no_key\n");
        fprintf(f, "default_timeout = 9999\n");
        fprintf(f, "unknown_key = ignored\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load succeeds despite bad lines");
        ASSERT_EQ(cfg.default_timeout, 9999, "valid line still parsed");

        remove(path);
    }

    // Test: color parsing edge cases
    {
        lnotify_color c;
        ASSERT_EQ(config_parse_color("#AABBCCDD", &c), 0,
                  "parse 8-char hex");
        ASSERT_EQ(c.r, 0xAA, "red");
        ASSERT_EQ(c.g, 0xBB, "green");
        ASSERT_EQ(c.b, 0xCC, "blue");
        ASSERT_EQ(c.a, 0xDD, "alpha");

        ASSERT_TRUE(config_parse_color("AABBCCDD", &c) != 0,
                    "reject missing #");
        ASSERT_TRUE(config_parse_color("#AABB", &c) != 0,
                    "reject short hex");
        ASSERT_TRUE(config_parse_color("#GGHHIIJJ", &c) != 0,
                    "reject non-hex chars");
    }
}
```

**Step 2: Run tests, verify failure**

**Step 3: Write `include/config.h`**

Define `lnotify_color` (r, g, b, a uint8_t), `lnotify_config` with all v1 keys, plus the internal `engine_priorities` array. Declare `config_defaults()`, `config_load()`, `config_parse_color()`.

**Step 4: Write `src/config.c`**

Line-by-line parser: skip `#` comments, split on first `=`, trim whitespace, match key name, parse value. Unknown keys logged at debug level, not errors.

**Step 5: Run tests, verify pass. Commit.**

```bash
git add include/config.h src/config.c tests/test_config.c tests/test_main.c Makefile
git commit -m "feat: config parser with color support and all v1 keys"
```

---

## Task 5: Engine Vtable & Session Context

**Goal:** Define the engine interface, session context struct, and probe infrastructure. No real engines yet — just the types and the probe dispatch table.

**Files:**
- Create: `include/engine.h`
- Create: `src/engine.c` (probe dispatch table, context builder)

**Reference:** Engine Architecture and Session Context sections of the design spec.

**Step 1: Write `include/engine.h`**

```c
#ifndef LNOTIFY_ENGINE_H
#define LNOTIFY_ENGINE_H

#include "lnotify.h"
#include <stdbool.h>
#include <stdint.h>

// Maximum number of registered engines (bitfield limit)
#define MAX_ENGINES 32

typedef enum {
    ENGINE_ACCEPT,
    ENGINE_REJECT,
    ENGINE_NEED_PROBE,
} engine_detect_result;

typedef enum {
    PROBE_HAS_DBUS_NOTIFICATIONS,
    PROBE_COMPOSITOR_NAME,
    PROBE_HAS_FRAMEBUFFER,
    PROBE_TERMINAL_CAPABILITIES,
    PROBE_FOREGROUND_PROCESS,
    PROBE_COUNT,  // sentinel — must be last
} probe_key;

typedef struct {
    // Phase 0: from logind (always available)
    uint32_t    vt;
    uint32_t    uid;
    const char *username;
    const char *session_type;
    const char *session_class;
    const char *seat;
    bool        remote;

    // Probed fields (populated on demand)
    bool        has_dbus_notifications;
    const char *compositor_name;
    bool        has_framebuffer;
    const char *terminal_type;
    bool        terminal_supports_osc;
    const char *foreground_process;

    // Probe bookkeeping
    uint32_t    probes_completed;   // bitfield of probe_key
    probe_key   requested_probe;
} session_context;

typedef struct engine {
    const char *name;
    int priority;

    engine_detect_result (*detect)(session_context *ctx);
    bool (*render)(const notification *notif, const session_context *ctx);
    void (*dismiss)(void);
} engine;

// Initialize a session context with logind data for a given VT.
// Queries loginctl for session properties.
void context_init_from_logind(session_context *ctx, uint32_t vt);

// Run a specific probe and update the context.
void context_run_probe(session_context *ctx, probe_key key);

// Check if a probe has already been run.
static inline bool context_probe_done(const session_context *ctx,
                                       probe_key key) {
    return (ctx->probes_completed & (1u << key)) != 0;
}

#endif
```

**Step 2: Write `src/engine.c`**

Implement `context_init_from_logind()` (shells out to `loginctl show-session` — same approach as the Python prototype, to be replaced with direct D-Bus later), and `context_run_probe()` as a switch on `probe_key` with stub implementations that log and set the field.

For the `PROBE_HAS_DBUS_NOTIFICATIONS` probe: run `gdbus introspect` on the session bus and check for `org.freedesktop.Notifications`. For `PROBE_HAS_FRAMEBUFFER`: `access("/dev/fb0", W_OK)`. Others can be stubs initially.

**Step 3: Commit**

```bash
git add include/engine.h src/engine.c Makefile
git commit -m "feat: engine vtable, session context, probe infrastructure"
```

---

## Task 6: Engine Resolver Loop

**Goal:** The resolver that iterates engines, handles NEED_PROBE, tracks rejections, and selects the right engine. Fully unit-tested with mock engines.

**Files:**
- Create: `include/resolver.h`
- Create: `src/resolver.c`
- Create: `tests/test_resolver.c`
- Modify: `tests/test_main.c`
- Modify: `Makefile`

**Step 1: Write the failing tests**

`tests/test_resolver.c` — use mock engines to test all resolver behaviors:
```c
#include "resolver.h"
#include "engine.h"

// --- Mock engines ---

// Engine that always accepts
static engine_detect_result mock_accept_detect(session_context *ctx) {
    (void)ctx;
    return ENGINE_ACCEPT;
}
static bool mock_accept_render(const notification *n,
                                const session_context *ctx) {
    (void)n; (void)ctx;
    return true;
}

// Engine that always rejects
static engine_detect_result mock_reject_detect(session_context *ctx) {
    (void)ctx;
    return ENGINE_REJECT;
}

// Engine that needs a probe, then accepts if probe succeeded
static engine_detect_result mock_probe_then_accept_detect(
        session_context *ctx) {
    if (!context_probe_done(ctx, PROBE_HAS_DBUS_NOTIFICATIONS)) {
        ctx->requested_probe = PROBE_HAS_DBUS_NOTIFICATIONS;
        return ENGINE_NEED_PROBE;
    }
    if (ctx->has_dbus_notifications)
        return ENGINE_ACCEPT;
    return ENGINE_REJECT;
}

// Engine that needs a probe, then always rejects regardless
static engine_detect_result mock_probe_then_reject_detect(
        session_context *ctx) {
    if (!context_probe_done(ctx, PROBE_HAS_FRAMEBUFFER)) {
        ctx->requested_probe = PROBE_HAS_FRAMEBUFFER;
        return ENGINE_NEED_PROBE;
    }
    return ENGINE_REJECT;
}

static void mock_dismiss(void) {}

void test_resolver_suite(void) {
    // Test: first engine accepts
    {
        engine engines[] = {
            {"accept", 0, mock_accept_detect, mock_accept_render,
             mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 1, &ctx);
        ASSERT_NOT_NULL(selected, "engine selected");
        ASSERT_STR_EQ(selected->name, "accept", "correct engine");
    }

    // Test: skip rejected, pick second
    {
        engine engines[] = {
            {"reject", 0, mock_reject_detect, NULL, mock_dismiss},
            {"accept", 1, mock_accept_detect, mock_accept_render,
             mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 2, &ctx);
        ASSERT_NOT_NULL(selected, "engine selected");
        ASSERT_STR_EQ(selected->name, "accept", "second engine picked");
    }

    // Test: all reject -> NULL
    {
        engine engines[] = {
            {"r1", 0, mock_reject_detect, NULL, mock_dismiss},
            {"r2", 1, mock_reject_detect, NULL, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 2, &ctx);
        ASSERT_NULL(selected, "no engine selected when all reject");
    }

    // Test: NEED_PROBE triggers probe, then re-evaluates
    {
        engine engines[] = {
            {"probe-accept", 0, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;  // probe will find this
        engine *selected = resolver_select(engines, 1, &ctx);
        ASSERT_NOT_NULL(selected, "engine selected after probe");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "probe engine accepted");
        ASSERT_TRUE(context_probe_done(&ctx,
                    PROBE_HAS_DBUS_NOTIFICATIONS),
                    "probe was marked completed");
    }

    // Test: NEED_PROBE, probe negative -> reject, fallback to next
    {
        engine engines[] = {
            {"probe-accept", 0, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
            {"fallback", 1, mock_accept_detect, mock_accept_render,
             mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = false;  // probe negative
        engine *selected = resolver_select(engines, 2, &ctx);
        ASSERT_NOT_NULL(selected, "engine selected");
        ASSERT_STR_EQ(selected->name, "fallback",
                      "fell through to fallback after probe reject");
    }

    // Test: rejected engines skipped on re-evaluation after probe
    {
        // Engine 0 rejects immediately
        // Engine 1 needs probe, then accepts
        // After probe runs, engine 0 should NOT be re-evaluated
        engine engines[] = {
            {"reject-fast", 0, mock_reject_detect, NULL, mock_dismiss},
            {"probe-accept", 1, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;
        engine *selected = resolver_select(engines, 2, &ctx);
        ASSERT_NOT_NULL(selected, "engine selected");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "probe engine selected, rejected skipped");
    }
}
```

**Step 2: Run tests, verify failure**

**Step 3: Write `include/resolver.h`**

```c
#ifndef LNOTIFY_RESOLVER_H
#define LNOTIFY_RESOLVER_H

#include "engine.h"

// Select the best engine for the given context.
// Runs the resolver loop with probe pipeline and rejection tracking.
// Returns the selected engine, or NULL if none accepted.
engine *resolver_select(engine *engines, int count, session_context *ctx);

#endif
```

**Step 4: Write `src/resolver.c`**

Implement the resolver loop exactly as specified in the design:
- Iterate engines in priority order
- Skip rejected (bitfield check)
- On ACCEPT: return engine
- On REJECT: mark rejected
- On NEED_PROBE: if probe not yet run, run it and restart from current engine. If already run, mark rejected.

**Important:** The `context_run_probe()` function needs to work in tests where real probes aren't available. For mock tests, the test sets context fields directly (e.g., `ctx.has_dbus_notifications = true`) and the probe just marks the bitfield as completed without actually probing. This means `context_run_probe()` should check if the value is already set before doing real work, or the test overrides the probe dispatch.

A clean approach: `context_run_probe()` uses a function pointer table that can be overridden in tests.

**Step 5: Run tests, verify pass. Commit.**

```bash
git add include/resolver.h src/resolver.c tests/test_resolver.c tests/test_main.c Makefile
git commit -m "feat: engine resolver loop with probe pipeline and rejection tracking"
```

---

## Task 7: Notification Queue

**Goal:** Thread-safe notification queue with enqueue, dequeue, dedup (group_id replacement), and expiration drain.

**Files:**
- Create: `include/queue.h`
- Create: `src/queue.c`
- Create: `tests/test_queue.c`
- Modify: `tests/test_main.c`
- Modify: `Makefile`

**Step 1: Write the failing tests**

`tests/test_queue.c`:
```c
#include "queue.h"
#include "lnotify.h"
#include <string.h>
#include <unistd.h>

void test_queue_suite(void) {
    // Test: enqueue and dequeue
    {
        notif_queue q;
        queue_init(&q);

        notification n = {0};
        notification_init(&n, "Title", "Body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        queue_push(&q, &n);

        ASSERT_EQ(queue_size(&q), 1, "queue has 1 item");

        notification *out = queue_pop(&q);
        ASSERT_NOT_NULL(out, "popped notification");
        ASSERT_STR_EQ(out->body, "Body", "body matches");

        ASSERT_EQ(queue_size(&q), 0, "queue empty after pop");

        notification_free(out);
        free(out);
        notification_free(&n);
        queue_destroy(&q);
    }

    // Test: dedup via group_id (replaces existing)
    {
        notif_queue q;
        queue_init(&q);

        notification a = {0};
        notification_init(&a, "Backup", "50%");
        a.group_id = strdup("backup");
        a.timeout_ms = 5000;
        a.ts_mono = monotonic_ms();
        queue_push(&q, &a);

        notification b = {0};
        notification_init(&b, "Backup", "75%");
        b.group_id = strdup("backup");
        b.timeout_ms = 5000;
        b.ts_mono = monotonic_ms();
        queue_push(&q, &b);

        ASSERT_EQ(queue_size(&q), 1, "dedup kept size at 1");

        notification *out = queue_pop(&q);
        ASSERT_STR_EQ(out->body, "75%", "dedup replaced with newer");

        notification_free(out);
        free(out);
        notification_free(&a);
        notification_free(&b);
        queue_destroy(&q);
    }

    // Test: expired notifications skipped on drain
    {
        notif_queue q;
        queue_init(&q);

        notification n = {0};
        notification_init(&n, NULL, "ephemeral");
        n.timeout_ms = 50;  // 50ms
        n.ts_mono = monotonic_ms();
        queue_push(&q, &n);

        usleep(60 * 1000);  // sleep 60ms

        notification *out = queue_pop_live(&q);
        ASSERT_NULL(out, "expired notification skipped");
        ASSERT_EQ(queue_size(&q), 0, "expired notification removed");

        notification_free(&n);
        queue_destroy(&q);
    }

    // Test: FIFO order
    {
        notif_queue q;
        queue_init(&q);

        notification a = {0};
        notification_init(&a, NULL, "first");
        a.timeout_ms = 5000;
        a.ts_mono = monotonic_ms();
        queue_push(&q, &a);

        notification b = {0};
        notification_init(&b, NULL, "second");
        b.timeout_ms = 5000;
        b.ts_mono = monotonic_ms();
        queue_push(&q, &b);

        notification *out1 = queue_pop(&q);
        notification *out2 = queue_pop(&q);
        ASSERT_STR_EQ(out1->body, "first", "FIFO: first out");
        ASSERT_STR_EQ(out2->body, "second", "FIFO: second out");

        notification_free(out1); free(out1);
        notification_free(out2); free(out2);
        notification_free(&a);
        notification_free(&b);
        queue_destroy(&q);
    }
}
```

**Step 2: Run tests, verify failure**

**Step 3: Write `include/queue.h`**

Linked list or array-based queue. Declare `notif_queue`, `queue_init`, `queue_destroy`, `queue_push` (with group_id dedup), `queue_pop`, `queue_pop_live` (skips expired), `queue_size`.

**Step 4: Write `src/queue.c`**

`queue_push`: if incoming notification has a group_id, scan queue for match. If found, replace body/title/timeout and update `ts_mono`. If not found, append.

`queue_pop_live`: dequeue front, check `notification_expired()`. If expired, free and try next. Return NULL if all expired.

Use a mutex for thread safety (the background defense thread can push to the queue).

**Step 5: Run tests, verify pass. Commit.**

```bash
git add include/queue.h src/queue.c tests/test_queue.c tests/test_main.c Makefile
git commit -m "feat: notification queue with dedup and expiration"
```

---

## Task 8: Shared Rendering Utilities

**Goal:** Reusable drawing primitives (rounded rectangle, text, colors) that engines compose from. Unit-tested for math correctness (not visual — visual testing comes later with framebuffer engine).

**Files:**
- Create: `include/render_util.h`
- Create: `src/render_util.c`
- Create: `include/font_bitmap.h`
- Create: `src/font_bitmap.c`
- Create: `tests/test_render_util.c`
- Modify: `tests/test_main.c`
- Modify: `Makefile`

**Step 1: Write the failing tests**

`tests/test_render_util.c`:
```c
#include "render_util.h"
#include "config.h"
#include <string.h>

void test_render_util_suite(void) {
    // Test: color from config struct to BGRA bytes
    {
        lnotify_color c = {.r = 0x11, .g = 0x22, .b = 0x33, .a = 0xFF};
        uint8_t bgra[4];
        color_to_bgra(&c, bgra);
        ASSERT_EQ(bgra[0], 0x33, "blue byte");
        ASSERT_EQ(bgra[1], 0x22, "green byte");
        ASSERT_EQ(bgra[2], 0x11, "red byte");
        ASSERT_EQ(bgra[3], 0xFF, "alpha byte");
    }

    // Test: point-in-rounded-rect
    {
        // 100x50 rect at (10,10) with radius 5
        ASSERT_TRUE(point_in_rounded_rect(50, 25, 10, 10, 100, 50, 5),
                    "center is inside");
        ASSERT_FALSE(point_in_rounded_rect(10, 10, 10, 10, 100, 50, 5),
                     "corner outside radius");
        ASSERT_TRUE(point_in_rounded_rect(15, 15, 10, 10, 100, 50, 5),
                    "just inside corner radius");
        ASSERT_FALSE(point_in_rounded_rect(0, 0, 10, 10, 100, 50, 5),
                     "outside rect entirely");
    }

    // Test: toast geometry calculation
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "top-right",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 1920 - 400 - 30, "x = screen - width - margin");
        ASSERT_EQ(geom.y, 30, "y = margin");
        ASSERT_EQ(geom.w, 400, "width");
        ASSERT_EQ(geom.h, 80, "height");
    }

    // Test: toast geometry for bottom-left
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "bottom-left",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 30, "x = margin");
        ASSERT_EQ(geom.y, 1080 - 80 - 30, "y = screen - height - margin");
    }

    // Test: text width calculation
    {
        int w = text_width("Hello", 2);  // scale=2, 8px font
        ASSERT_EQ(w, 5 * 8 * 2, "5 chars * 8px * scale 2");
    }
}
```

**Step 2: Run tests, verify failure**

**Step 3: Write headers and implementation**

`include/render_util.h`: Declare `color_to_bgra`, `point_in_rounded_rect`, `toast_geometry`, `compute_toast_geometry`, `text_width`. These are pure functions with no side effects.

`include/font_bitmap.h` / `src/font_bitmap.c`: Move the 8x8 bitmap font data from the Python prototype. Provide `get_char_bitmap(char ch)` returning a `const uint8_t[8]`.

`src/render_util.c`: Implement geometry and color utilities.

**Step 4: Run tests, verify pass. Commit.**

```bash
git add include/render_util.h include/font_bitmap.h src/render_util.c src/font_bitmap.c tests/test_render_util.c tests/test_main.c Makefile
git commit -m "feat: shared rendering utilities and bitmap font"
```

---

## Task 9: Framebuffer Engine

**Goal:** Framebuffer rendering engine with mmap writes, verification, sustained defense, and background defense thread. This is the first real engine — it plugs into the vtable.

**Files:**
- Create: `include/engine_fb.h`
- Create: `src/engine_fb.c`
- Modify: `Makefile`

**Reference:** Framebuffer engine section of design spec + `docs/plans/2026-03-09-lessons-learned.md` (critical reading — especially the "compositors don't use /dev/fb0" discovery and the threading self-join bug).

**Step 1: Write the engine**

`include/engine_fb.h`:
```c
#ifndef LNOTIFY_ENGINE_FB_H
#define LNOTIFY_ENGINE_FB_H

#include "engine.h"

extern engine engine_framebuffer;

#endif
```

`src/engine_fb.c`:
- `detect()`: return REJECT if `session_type` is "wayland" or "x11" (compositor bypasses fb0). If session_type is "tty" or empty, return NEED_PROBE for `PROBE_HAS_FRAMEBUFFER`. If probe shows fb0 writable, return ACCEPT.
- `render()`: open fb0, read screen info via ioctl, mmap, save region, draw toast using shared `render_util` functions, verify, start defense thread.
- `dismiss()`: restore saved region, close fb, stop defense thread.

The defense thread logic: check `verify_visible()` every 200ms, re-render if clobbered, give up after 3 consecutive failures and push to queue. **Critical:** defense thread must NOT call through `dismiss()` — clean up directly under the lock. See lessons learned doc.

Use config struct values for all style parameters (colors, border, padding, etc.) — no magic numbers.

**Step 2: Manual test on a real TTY**

```bash
# Build
make daemon

# Switch to a raw TTY, run the daemon, trigger a notification
sudo chvt 2
# On tty2:
sudo ./build/lnotifyd --debug &
# In another terminal (or after building the client):
# For now, use a test binary that calls the engine directly:
sudo ./build/test_fb_manual

# Capture the framebuffer
sudo fbgrab /tmp/fb_toast.png

# Switch back
sudo chvt 3
```

Write `tests/manual/test_fb.sh` with these instructions and the commands.

**Step 3: Verify the capture**

Open `/tmp/fb_toast.png`. Verify:
- Toast appears in top-right (or configured position)
- Rounded corners, border, colors match config
- Text is readable

**Step 4: Save golden image**

```bash
cp /tmp/fb_toast.png tests/golden/fb/toast_default.png
```

**Step 5: Commit**

```bash
git add include/engine_fb.h src/engine_fb.c tests/manual/test_fb.sh tests/golden/fb/
git commit -m "feat: framebuffer engine with verification and defense"
```

---

## Task 10: D-Bus Engine

**Goal:** D-Bus notification engine that sends `org.freedesktop.Notifications.Notify` on the session's D-Bus, with retry/backoff. Handles cross-user delivery via `fork` + `setresuid`.

**Files:**
- Create: `include/engine_dbus.h`
- Create: `src/engine_dbus.c`
- Modify: `Makefile`

**Reference:** D-Bus engine section of design spec + lessons learned (root can't connect directly to another user's session bus).

**Step 1: Write the engine**

`src/engine_dbus.c`:
- `detect()`: if `session_type` is not "wayland" or "x11", REJECT. Otherwise, NEED_PROBE for `PROBE_HAS_DBUS_NOTIFICATIONS`. If probe succeeds, ACCEPT.
- `render()`: build the D-Bus Notify call. If `origin_uid` matches context `uid`, call directly via libdbus (or shell out to `gdbus` for v1). If cross-user, `fork()`, child calls `setresgid`/`setresuid` to target user, sets `DBUS_SESSION_BUS_ADDRESS`, executes `gdbus`. Retry with exponential backoff (200ms base, 1.5x, max 5 attempts).
- `dismiss()`: no-op (notification server handles timeout).

**v1 approach:** Shell out to `gdbus` rather than using libdbus directly. This matches the prototype and avoids dlopen complexity for v1. The engine struct is the same — swapping in direct D-Bus calls later doesn't change the interface.

**Step 2: Manual test on a Wayland session**

```bash
# Build
make daemon

# On a Wayland session (tty3 with GNOME):
sudo ./build/lnotifyd --debug &
# Trigger notification (via test binary or direct gdbus call)
# Observe: GNOME notification appears
# Check logs: "D-Bus notify succeeded"
```

Write `tests/manual/test_dbus.sh`.

**Step 3: Manual test cross-user delivery**

```bash
# As root, send notification to user "guy" on their Wayland session
sudo ./build/lnotifyd --system --debug &
# Trigger notification targeting guy's session
# Observe: notification appears on guy's GNOME desktop
# Check logs: "fork + setresuid to uid=1000"
```

**Step 4: Commit**

```bash
git add include/engine_dbus.h src/engine_dbus.c tests/manual/test_dbus.sh Makefile
git commit -m "feat: D-Bus notification engine with cross-user delivery"
```

---

## Task 11: Queue Engine

**Goal:** Universal fallback engine that always accepts and stores the notification in the queue.

**Files:**
- Create: `include/engine_queue.h`
- Create: `src/engine_queue.c`
- Modify: `Makefile`

**Step 1: Write the engine**

Trivial:
- `detect()`: always returns `ENGINE_ACCEPT`
- `render()`: push notification to queue, log it, return true
- `dismiss()`: no-op

This engine is always registered last in the priority list.

**Step 2: Commit**

```bash
git add include/engine_queue.h src/engine_queue.c Makefile
git commit -m "feat: queue engine (universal fallback)"
```

---

## Task 12: Unix Socket IPC (Daemon Side)

**Goal:** Daemon listens on a Unix socket, accepts client connections, deserializes notifications, captures `origin_uid` via `SO_PEERCRED`, dispatches to the engine resolver.

**Files:**
- Create: `src/daemon/socket.c`
- Create: `include/socket.h`
- Modify: `src/daemon/main.c`
- Modify: `Makefile`

**Step 1: Write socket listener**

`src/daemon/socket.c`:
- `socket_listen(path)`: create `AF_UNIX` socket, bind, listen. Remove stale socket file if exists.
- `socket_accept_loop()`: accept connections, read message, deserialize with `protocol_deserialize`, capture `origin_uid` via `getsockopt(SO_PEERCRED)`, set `ts_received` and `ts_mono`, dispatch to resolver.
- Handle connection errors gracefully (log and continue).

**Step 2: Write socket path logic**

Per-user: `$XDG_RUNTIME_DIR/lnotify.sock`
System: `/run/lnotify.sock`

**Step 3: Manual test**

```bash
# Start daemon
./build/lnotifyd --debug &

# Send raw bytes to socket (using socat or a test script)
echo -n "..." | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/lnotify.sock

# Or build the client (Task 13) and use that
```

**Step 4: Commit**

```bash
git add src/daemon/socket.c include/socket.h src/daemon/main.c Makefile
git commit -m "feat: Unix socket IPC listener with SO_PEERCRED"
```

---

## Task 13: CLI Client (`lnotify`)

**Goal:** CLI that parses args, serializes a notification, connects to the daemon socket, sends it, and exits.

**Files:**
- Modify: `src/client/main.c`
- Modify: `Makefile`

**Step 1: Implement argument parsing**

```
lnotify [OPTIONS] BODY

Options:
  -t, --title TITLE        Notification title
  -p, --priority PRIORITY  low, normal (default), critical
  --app APP                Application name
  --group GROUP_ID         Group ID for dedup
  --timeout MS             Timeout in milliseconds
  --dry-run                Show what would happen, don't send
  --socket PATH            Override socket path
```

Use `getopt_long`. No external arg parsing library.

**Step 2: Implement socket send**

Connect to `$XDG_RUNTIME_DIR/lnotify.sock` (or `/run/lnotify.sock` if `--system` flag or socket path override). Serialize notification with `protocol_serialize`, write to socket, close.

**Step 3: Manual test end-to-end**

```bash
# Terminal 1: start daemon
./build/lnotifyd --debug

# Terminal 2: send notification
./build/lnotify "Hello from lnotify!"
./build/lnotify -t "Alert" -p critical --app cron "Backup failed"
./build/lnotify --group backup "Backup: 50%"
./build/lnotify --group backup "Backup: 75%"  # should dedup
```

Verify in daemon logs that notifications arrive, engine resolver runs, correct engine is selected.

**Step 4: Commit**

```bash
git add src/client/main.c Makefile
git commit -m "feat: lnotify CLI client with full arg parsing"
```

---

## Task 14: VT Switch Monitor (Daemon Event Loop)

**Goal:** Daemon main loop that monitors VT switches via `poll()` on sysfs, rebuilds session context, triggers engine resolver, and drains the queue.

**Files:**
- Modify: `src/daemon/main.c`
- Modify: `Makefile`

**Reference:** VT Switch Detection section of design spec + lessons learned.

**Step 1: Implement the event loop**

`src/daemon/main.c`:
- Open `/sys/class/tty/tty0/active` for `poll()` with `POLLPRI | POLLERR`
- Also `poll()` the Unix socket for incoming client connections (multiplex both)
- On VT switch: read new VT, build session context, dismiss current notification, show new one (or drain queue)
- On socket activity: accept connection, handle notification
- Signal handler for SIGINT/SIGTERM: clean shutdown

**Step 2: Manual test VT switching**

```bash
# Start daemon on tty3
sudo ./build/lnotifyd --system --debug

# Switch to tty2
sudo chvt 2
# Observe: logs show VT switch detection, framebuffer toast appears

# Switch to tty1 (GDM greeter)
sudo chvt 1
# Observe: logs show VT switch, D-Bus fails (greeter), fb skipped, queued

# Switch to tty3 (GNOME)
sudo chvt 3
# Observe: queue drained, D-Bus notification appears
```

Write `tests/manual/test_vt.sh` with these steps and expected log output.

**Step 3: Commit**

```bash
git add src/daemon/main.c tests/manual/test_vt.sh Makefile
git commit -m "feat: VT switch monitor with sysfs poll and queue drain"
```

---

## Task 15: SSH Terminal Engine

**Goal:** Engine that writes notifications to qualifying SSH pty sessions, with the 4-tier rendering fallback (OSC → tmux → cursor overlay → plain text).

**Files:**
- Create: `include/engine_terminal.h`
- Create: `src/engine_terminal.c`
- Create: `src/daemon/ssh_delivery.c`
- Modify: `Makefile`

**Reference:** SSH Terminal Notifications section of design spec.

**Step 1: Write SSH session discovery**

`src/daemon/ssh_delivery.c`:
- `ssh_find_qualifying_ptys(config)`: query logind for remote sessions, filter by `ssh_users` / `ssh_groups` config, check `LNOTIFY_SSH` env var in `/proc/{pid}/environ`, return list of qualifying ptys.
- `ssh_check_fullscreen(pty)`: read `/proc/{pid}/stat` field 8 (tpgid), look up foreground process name, match against `ssh_fullscreen_apps` config.

**Step 2: Write terminal rendering tiers**

`src/engine_terminal.c`:

**Tier 1: OSC**
- Check cached terminal type. If `$TERM_PROGRAM` is iTerm2/WezTerm/etc., try OSC 9 (iTerm) or OSC 777 (rxvt-unicode).
- Write escape sequence to pty fd.

**Tier 2: tmux**
- Check if `$TMUX` is set in pty environment.
- If tmux >= 3.2: `tmux display-popup -t $session -w 40 -h 5 -E "echo 'Title: body'; sleep DURATION"`
- Fallback: `tmux display-message -t $session "Title: body"`

**Tier 3: Cursor overlay**
- Save cursor (`\033[s`), move to top-right (`\033[1;{cols-width}H`), draw ANSI-colored box, restore cursor (`\033[u`).
- Schedule dismiss: after timeout, re-save cursor, move to overlay position, clear lines, restore cursor.
- Skip if full-screen app detected and `ssh_notify_over_fullscreen = false`. Hold notification for later.

**Tier 4: Plain text**
- Write `\r\n\033[1;44;37m lnotify \033[0m Title: body\r\n` to pty.

**Step 3: Wire into daemon**

The terminal engine's `detect()` always returns REJECT for the primary VT rendering — it's not a VT engine. SSH delivery is triggered separately in the daemon's notification handler, after the primary engine renders (or queues). The daemon calls `ssh_deliver(notification, config)` which finds ptys and iterates the terminal rendering chain for each.

**Step 4: Manual test with localhost SSH**

```bash
# Terminal 1: start daemon
sudo ./build/lnotifyd --system --debug

# Terminal 2: SSH to localhost
ssh localhost

# Terminal 3: send notification
./build/lnotify -t "SSH Test" "Hello over SSH"

# In Terminal 2: observe notification appears
```

Test each tier individually:
- tmux: start a tmux session, SSH in, send notification, observe `display-popup` or `display-message`
- Cursor overlay: SSH without tmux, send notification, observe overlay
- Plain text: set `LNOTIFY_SSH=text`, send notification, observe inline text

Capture terminal output with `script`:
```bash
# In Terminal 2:
script /tmp/ssh_test.log
ssh localhost
# ... trigger notification from Terminal 3 ...
exit
exit
# Inspect /tmp/ssh_test.log for correct escape sequences
```

Write `tests/manual/test_ssh.sh` with all these steps.

**Step 5: Save golden files**

```bash
cp /tmp/ssh_test.log tests/golden/terminal/ssh_overlay_default.log
```

**Step 6: Commit**

```bash
git add include/engine_terminal.h src/engine_terminal.c src/daemon/ssh_delivery.c tests/manual/test_ssh.sh tests/golden/terminal/ Makefile
git commit -m "feat: SSH terminal engine with 4-tier fallback"
```

---

## Task 16: Integration Testing

**Goal:** End-to-end tests verifying the full daemon lifecycle: startup, socket listen, notification receive, engine selection, VT switch handling, queue drain, SSH delivery, shutdown.

**Files:**
- Create: `tests/test_integration.sh` (shell-based integration test script)
- Create: `tests/manual/README.md`

**Step 1: Write the integration test script**

`tests/test_integration.sh` — a bash script that:
1. Starts `lnotifyd --debug` in background, captures log output
2. Sends notifications via `lnotify` CLI
3. Greps daemon logs for expected engine selection, probe results
4. Verifies dedup (send two with same group_id, check only one rendered)
5. Verifies expiration (send with short timeout, wait, check "expired" in logs)
6. Kills daemon, checks clean shutdown

This runs without root or hardware — it tests the socket/protocol/resolver/queue path.

**Step 2: Write the manual test README**

`tests/manual/README.md` — instructions for all manual tests:
- Framebuffer: `chvt` + `fbgrab` workflow
- D-Bus: on GNOME session
- VT switching: full cycle across tty1/2/3
- SSH: localhost SSH with `script` capture
- Each test lists: prerequisites, exact commands, expected output, how to capture golden files

**Step 3: Run integration tests**

```bash
bash tests/test_integration.sh
```

**Step 4: Commit**

```bash
git add tests/test_integration.sh tests/manual/README.md
git commit -m "test: integration test script and manual test documentation"
```

---

## Task 17: Dry-Run Mode

**Goal:** `lnotify --dry-run` connects to daemon, daemon responds with engine evaluation trace instead of rendering.

**Files:**
- Modify: `src/client/main.c` (add `--dry-run` flag handling)
- Modify: `src/daemon/socket.c` (add dry-run response path)
- Modify: `include/protocol.h` (add dry-run flag to wire format)

**Step 1: Add dry-run bit to field_mask**

```c
#define FIELD_DRY_RUN (1 << 7)  // high bit, won't conflict with data fields
```

**Step 2: Daemon dry-run handler**

When daemon sees `FIELD_DRY_RUN` in field_mask:
- Build session context
- Run resolver loop
- Instead of calling `engine->render()`, format a text response:
  ```
  VT: tty3
  Session: wayland, user, uid=1000
  Probes run: HAS_DBUS_NOTIFICATIONS=true
  Engine selected: dbus
  SSH targets: guy@pts/0 (overlay mode)
  ```
- Send response back on the socket before closing

**Step 3: Client dry-run output**

`lnotify --dry-run "test"` reads the daemon's response and prints it.

**Step 4: Manual test**

```bash
./build/lnotify --dry-run "test"
# Should print engine evaluation trace
```

**Step 5: Commit**

```bash
git add src/client/main.c src/daemon/socket.c include/protocol.h
git commit -m "feat: --dry-run mode for debugging engine selection"
```

---

## Task 18: Polish & Cleanup

**Goal:** Final pass — error handling, edge cases, code review, documentation.

**Step 1: Review all warning-free compilation**

```bash
make clean && make all 2>&1 | grep -i warning
```

Fix any warnings.

**Step 2: Review thread safety**

Check all shared state between defense thread and main thread:
- Queue access (must be under mutex)
- fb_toast pointer (must be under lock)
- Defense thread must NOT call through dismiss — clean up directly

**Step 3: Run all tests**

```bash
make test
bash tests/test_integration.sh
```

**Step 4: Run manual tests**

Follow `tests/manual/README.md` for each scenario. Update golden files if needed.

**Step 5: Final commit**

```bash
git add -A
git commit -m "polish: warnings, thread safety review, test pass"
```

---

## Summary

| Task | Component | Tests | Dependencies |
|------|-----------|-------|-------------|
| 1 | Project scaffolding | Harness sanity | None |
| 2 | Notification data model | Unit | None |
| 3 | Wire protocol | Unit | Task 2 |
| 4 | Config parser | Unit | None |
| 5 | Engine vtable & context | None (types) | None |
| 6 | Resolver loop | Unit (mocks) | Task 5 |
| 7 | Notification queue | Unit | Task 2 |
| 8 | Rendering utilities | Unit | Task 4 (colors) |
| 9 | Framebuffer engine | Manual + golden | Tasks 5, 6, 7, 8 |
| 10 | D-Bus engine | Manual | Tasks 5, 6 |
| 11 | Queue engine | None (trivial) | Tasks 5, 7 |
| 12 | Socket IPC (daemon) | Manual | Tasks 3, 6 |
| 13 | CLI client | Manual | Tasks 3, 12 |
| 14 | VT switch monitor | Manual | Tasks 9, 10, 11, 12 |
| 15 | SSH terminal engine | Manual + golden | Tasks 4, 14 |
| 16 | Integration testing | Integration | Tasks 12, 13, 14 |
| 17 | Dry-run mode | Manual | Tasks 12, 13 |
| 18 | Polish & cleanup | All | All |

Tasks 1-8 are pure logic with unit tests — no hardware or root needed. These can run in any CI environment.

Tasks 9-15 require a real Linux system with VTs, framebuffer, D-Bus, and SSH. These are manual-first with golden file captures.

Tasks 2-4 and 5-7 have no cross-dependencies and can be parallelized.
