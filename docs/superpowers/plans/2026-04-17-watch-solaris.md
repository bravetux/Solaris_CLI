# `watch` (Solaris 8 port) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimal port of the Linux `watch` utility that runs on Solaris 8 (SunOS 5.8) and also builds cleanly on Linux for development.

**Architecture:** Single-file C99 program using ANSI terminal escapes (`\033[2J\033[H`), `ioctl(TIOCGWINSZ)` for terminal width, `popen("sh -c ...")` for command execution, and `nanosleep()` for fractional intervals. No curses, no external libraries beyond libc + librt.

**Tech Stack:** C99 • POSIX (`<time.h>`, `<sys/ioctl.h>`, `<sys/termios.h>`, `<stdio.h>`, `<signal.h>`) • `make` with `gcc` on Linux and `gcc` / Sun Studio `cc` on Solaris.

**Note on git:** The `solaris/` directory is not a git repo in the current workspace, so the plan omits `git commit` steps. If the engineer initializes one, commit after each task.

**Note on TDD:** Per the approved spec, automated tests are out of scope for v1; the plan uses the spec's manual smoke-test checklist as the verification gate. A future v2 plan will add a scripted harness.

---

## File Structure

| File                                | Purpose                                                   |
|-------------------------------------|-----------------------------------------------------------|
| `solaris/watch/watch.c`             | Single C source file (target ~150 LOC).                   |
| `solaris/watch/Makefile`            | Portable Linux/Solaris build; supports `gcc` and `cc`.    |
| `solaris/watch/README.md`           | Usage, build matrix, and smoke-test checklist.            |

---

## Task 1: Project skeleton — Makefile and minimal compiling `watch.c`

**Files:**
- Create: `solaris/watch/Makefile`
- Create: `solaris/watch/watch.c`

**Goal:** `make` produces a `watch` binary on Linux and on Solaris 8 that prints a usage line when run with no args.

- [ ] **Step 1: Create `solaris/watch/Makefile`**

```make
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
```

- [ ] **Step 2: Create `solaris/watch/watch.c` with a usage stub**

```c
/*
 * watch - periodically run a command and display its output.
 * Solaris 8 port, v1 (core flags: -n SECONDS, -t).
 */

#include <stdio.h>
#include <stdlib.h>

static void usage(FILE *out) {
    fputs("Usage: watch [-n SECONDS] [-t] COMMAND [ARGS...]\n", out);
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    return 0;
}
```

- [ ] **Step 3: Build and verify on host platform**

Run:
```bash
cd solaris/watch
make
./watch
echo "exit=$?"
```

Expected:
- `make` completes with no warnings.
- `./watch` prints the usage line to stderr.
- `echo "exit=$?"` prints `exit=1`.

- [ ] **Step 4: Verify `make clean` works**

Run:
```bash
make clean
ls watch 2>&1 || echo "removed"
```

Expected: `removed` (or ls error), confirming the binary is gone.

---

## Task 2: Argument parsing and command execution via `popen`

**Files:**
- Modify: `solaris/watch/watch.c` (replace stub with full argv parsing + popen loop; fixed 2-second sleep for now)

**Goal:** `./watch -n 1 date` runs `date` once per second in a loop (screen clears each tick). No header yet; `-t` accepted but no-op for this task.

- [ ] **Step 1: Replace `watch.c` with argv parsing and a popen-based loop**

```c
/*
 * watch - periodically run a command and display its output.
 * Solaris 8 port, v1 (core flags: -n SECONDS, -t).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs("Usage: watch [-n SECONDS] [-t] COMMAND [ARGS...]\n", out);
}

/* Join argv[start..argc) with single spaces into a malloc'd string. */
static char *join_argv(int argc, char **argv, int start) {
    size_t total = 1; /* trailing NUL */
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]);
        if (i + 1 < argc) total += 1; /* space separator */
    }
    char *buf = (char *)malloc(total);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (int i = start; i < argc; i++) {
        strcat(buf, argv[i]);
        if (i + 1 < argc) strcat(buf, " ");
    }
    return buf;
}

int main(int argc, char **argv) {
    double interval = 2.0;
    int no_title = 0;
    int i = 1;

    /* Flag parsing loop. */
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            char *end = NULL;
            double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || v <= 0.0) {
                fprintf(stderr, "watch: invalid interval '%s'\n", argv[i]);
                return 1;
            }
            interval = v;
        } else if (strcmp(argv[i], "-t") == 0) {
            no_title = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "watch: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        } else {
            break; /* start of command */
        }
    }

    if (i >= argc) { usage(stderr); return 1; }

    char *cmd = join_argv(argc, argv, i);
    if (!cmd) {
        fprintf(stderr, "watch: out of memory\n");
        return 1;
    }

    (void)no_title; /* wired up in a later task */

    for (;;) {
        /* Clear screen + home cursor. */
        fputs("\033[2J\033[H", stdout);
        fflush(stdout);

        FILE *pipe = popen(cmd, "r");
        if (!pipe) {
            fprintf(stderr, "watch: popen failed: %s\n", cmd);
            free(cmd);
            return 2;
        }

        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
            fwrite(buf, 1, n, stdout);
        }
        pclose(pipe);
        fflush(stdout);

        /* TEMPORARY: integer-second sleep; replaced by nanosleep in a later task. */
        sleep((unsigned int)interval);
    }

    free(cmd);
    return 0;
}
```

- [ ] **Step 2: Rebuild**

Run:
```bash
cd solaris/watch
make clean && make
```

Expected: Clean build, no warnings.

- [ ] **Step 3: Smoke-test basic execution**

Run (in a real terminal):
```bash
./watch -n 1 date
```

Expected: Screen clears, `date` output appears and updates every second. Press Ctrl-C to exit (it will exit abruptly for now; clean signal handling lands in Task 4).

- [ ] **Step 4: Smoke-test shell metacharacter path**

Run:
```bash
./watch -n 1 "ls | wc -l"
```

Expected: The pipe is handled by `sh -c`; the line count updates every second.

- [ ] **Step 5: Smoke-test argument errors**

Run:
```bash
./watch ; echo "exit=$?"
./watch -n 0 date ; echo "exit=$?"
./watch -n abc date ; echo "exit=$?"
./watch -z date ; echo "exit=$?"
```

Expected: Each prints an error to stderr and prints `exit=1`.

---

## Task 3: Header line — hostname, timestamp, terminal width, `-t` honoring

**Files:**
- Modify: `solaris/watch/watch.c` (add header printer and wire `-t` flag)

**Goal:** Each tick prints a header like Linux `watch`:
```
Every 2.0s: date                                           myhost: Fri Apr 17 09:12:03 2026

<command output>
```
`-t` suppresses it entirely. Width comes from `ioctl(TIOCGWINSZ)` on stdout with a fallback of 80.

- [ ] **Step 1: Add includes and the `print_header` function**

Add these includes at the top of `watch.c` (below the existing ones):

```c
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#ifndef TIOCGWINSZ
#include <sys/termios.h>
#endif
```

Add this helper above `main`:

```c
static int term_cols(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    return 80;
}

static void print_header(double interval, const char *cmd) {
    int cols = term_cols();
    if (cols < 20) cols = 20;

    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        strcpy(host, "localhost");
    }
    host[sizeof(host) - 1] = '\0';

    time_t now = time(NULL);
    struct tm tmv;
    char tbuf[64];
    if (localtime_r(&now, &tmv) != NULL) {
        strftime(tbuf, sizeof(tbuf), "%a %b %e %H:%M:%S %Y", &tmv);
    } else {
        strcpy(tbuf, "----");
    }

    /* Left: "Every N.Ns: <cmd>".  Right: "<host>: <time>". */
    char left[1024];
    snprintf(left, sizeof(left), "Every %.1fs: %s", interval, cmd);

    char right[512];
    snprintf(right, sizeof(right), "%s: %s", host, tbuf);

    int left_len  = (int)strlen(left);
    int right_len = (int)strlen(right);

    /* If combined exceeds width, truncate the left (command) with "...". */
    if (left_len + 1 + right_len > cols) {
        int max_left = cols - right_len - 4; /* space + "..." */
        if (max_left < 5) max_left = 5;
        if (left_len > max_left) {
            left[max_left - 3] = '.';
            left[max_left - 2] = '.';
            left[max_left - 1] = '.';
            left[max_left]     = '\0';
            left_len = max_left;
        }
    }

    int pad = cols - left_len - right_len;
    if (pad < 1) pad = 1;

    fputs(left, stdout);
    for (int i = 0; i < pad; i++) fputc(' ', stdout);
    fputs(right, stdout);
    fputs("\n\n", stdout);
}
```

- [ ] **Step 2: Call `print_header` from the main loop and respect `-t`**

Inside `main`, in the `for (;;)` loop, replace the lines:

```c
        /* Clear screen + home cursor. */
        fputs("\033[2J\033[H", stdout);
        fflush(stdout);
```

with:

```c
        /* Clear screen + home cursor. */
        fputs("\033[2J\033[H", stdout);
        if (!no_title) {
            print_header(interval, cmd);
        }
        fflush(stdout);
```

Also remove the `(void)no_title;` line earlier in `main`.

- [ ] **Step 3: Rebuild**

Run:
```bash
cd solaris/watch
make clean && make
```

Expected: Clean build, no warnings.

- [ ] **Step 4: Smoke-test header rendering**

Run:
```bash
./watch -n 1 date
```

Expected: First line looks like `Every 1.0s: date <spaces> <host>: <timestamp>`, spanning the terminal width. Timestamp updates each tick.

- [ ] **Step 5: Smoke-test `-t`**

Run:
```bash
./watch -n 1 -t date
```

Expected: No header line; only the `date` output scrolls every tick.

- [ ] **Step 6: Smoke-test narrow terminal**

Resize your terminal narrow (≈40 cols) and run:
```bash
./watch -n 1 "echo this is a long command with many words"
```

Expected: The command portion of the header gets truncated with `...` so the right-side (`host: timestamp`) still fits on one line.

---

## Task 4: Fractional intervals via `nanosleep`, clean signal handling, README

**Files:**
- Modify: `solaris/watch/watch.c` (replace `sleep` with `nanosleep`; install SIGINT/SIGTERM handlers)
- Create: `solaris/watch/README.md`

**Goal:** `-n 0.5` actually waits half a second; Ctrl-C exits with status 0 and leaves the terminal on a fresh line.

- [ ] **Step 1: Add signal includes and a stop flag**

Add these includes near the top of `watch.c`:

```c
#include <signal.h>
#include <errno.h>
```

Add this above `main`:

```c
static volatile sig_atomic_t stop_flag = 0;

static void on_signal(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void sleep_fractional(double seconds) {
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec >= 1000000000L) ts.tv_nsec = 999999999L;

    /* Resume across EINTR unless a signal asked us to stop. */
    struct timespec rem;
    while (nanosleep(&ts, &rem) != 0) {
        if (errno != EINTR) break;
        if (stop_flag) return;
        ts = rem;
    }
}
```

- [ ] **Step 2: Install handlers and replace `sleep` with `sleep_fractional`**

At the top of `main` (before the flag parsing loop), add:

```c
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
```

Inside the `for (;;)` loop, replace:

```c
        sleep((unsigned int)interval);
```

with:

```c
        if (stop_flag) break;
        sleep_fractional(interval);
        if (stop_flag) break;
```

Add `#include <sys/types.h>` (may already be present) and ensure the final `return` path after the loop writes a newline so the shell prompt doesn't land mid-line:

```c
    fputc('\n', stdout);
    fflush(stdout);
    free(cmd);
    return 0;
```

- [ ] **Step 3: Rebuild**

Run:
```bash
cd solaris/watch
make clean && make
```

Expected: Clean build, no warnings. On Solaris 8, confirm `-lrt` is in the link line (Makefile handles it automatically).

- [ ] **Step 4: Smoke-test fractional interval**

Run:
```bash
./watch -n 0.5 date
```

Expected: `date` refreshes roughly twice per second (you will see the seconds field change on every other tick).

- [ ] **Step 5: Smoke-test clean Ctrl-C exit**

Run:
```bash
./watch -n 1 date
```

Press Ctrl-C. Expected: Program exits within ≤1 second, shell prompt appears on its own line, `echo $?` prints `0`.

- [ ] **Step 6: Create `solaris/watch/README.md`**

```markdown
# watch (Solaris 8 port)

Periodically run a command and display its output. A minimal port of the Linux
`watch` utility targeted at Solaris 8 (SunOS 5.8). Builds cleanly on Linux for
development.

## Usage

    watch [-n SECONDS] [-t] COMMAND [ARGS...]

| Flag | Meaning                                                    |
|------|------------------------------------------------------------|
| `-n` | Refresh interval in seconds (fractional allowed). Default 2.0. |
| `-t` | Suppress the title/header line.                            |
| `--` | End of flags; everything after is the command.             |

The command is executed via `sh -c`, so shell metacharacters (pipes, quoting,
redirection) work as expected.

## Build

    make              # release build
    make CC=gcc       # force GCC
    make CC=cc        # Sun Studio (Solaris)
    make clean
    make install PREFIX=/usr/local

The Makefile auto-detects Linux vs SunOS and passes `-lrt` on Solaris for
`nanosleep`.

| Platform         | Compiler       | Flags applied                                    |
|------------------|----------------|--------------------------------------------------|
| Linux            | gcc            | `-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200112L` |
| SunOS (Solaris)  | gcc            | as above, plus `-lrt`                            |
| SunOS (Solaris)  | cc (Sun Studio)| `-xc99 -D_POSIX_C_SOURCE=200112L -lrt`           |

## Smoke test

Run these manually in a VT100-compatible terminal:

1. `./watch date` — header updates every 2s.
2. `./watch -n 0.5 date` — updates every half-second.
3. `./watch -t echo hello` — no header.
4. `./watch "ls | wc -l"` — shell pipeline works.
5. `./watch` — prints usage, exits 1.
6. `./watch -n 0 date` — prints error, exits 1.
7. Ctrl-C during a run exits cleanly (`echo $?` → 0).

## Not implemented in v1

`-d`, `-b`, `-e`, `-g`, `-c`, `-x`, `-p`, `--no-title`, `--no-wrap`, `--help`,
`--version`. Planned for future iterations.
```

- [ ] **Step 7: Run the full smoke-test checklist from the README**

Walk through all seven items above. Each one must behave as described.

---

## Self-review (done inline before handoff)

- **Spec coverage:** `-n` (Task 2), `-t` (Task 3), command parsing + `sh -c` (Task 2), header format (Task 3), terminal width via `ioctl` (Task 3), fractional `nanosleep` (Task 4), signal handling (Task 4), exit codes 0/1/2 (all tasks), Makefile platform matrix (Task 1), README with smoke tests (Task 4). All spec items covered.
- **Placeholder scan:** No TBDs, no "add error handling", no vague steps. Each code step shows the exact code.
- **Type consistency:** `stop_flag`, `on_signal`, `sleep_fractional`, `print_header`, `term_cols`, `join_argv` are declared and used consistently across tasks.
