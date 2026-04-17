# `watch` command — Solaris 8 port (v1)

**Date:** 2026-04-17
**Target platform:** Solaris 8 (SunOS 5.8), with Linux as a secondary build target for development convenience.
**Source directory:** `solaris/watch/`

## Goal

Port the Linux `watch` utility to Solaris 8, scoped to a minimal "core only" feature set. Establishes the pattern for further Linux-userland ports that will follow.

## Feature scope (v1)

Implements only the following flags and behaviors:

| Flag          | Meaning                                             |
|---------------|-----------------------------------------------------|
| `-n SECONDS`  | Refresh interval. Fractional seconds allowed. Default `2.0`. |
| `-t`          | Suppress the title/header line.                     |
| *(positional)*| The command to run, including its arguments.        |

Out of scope for v1 (may appear in later versions): `-d`, `-b`, `-e`, `-g`, `-c`, `-x`, `-p`, `--no-title`, `--no-wrap`, long-form `--interval`, `--help`, `--version`.

## Approach

Single-file C program using:

- ANSI terminal escape codes (`\033[2J\033[H`) to clear and home the cursor. No curses library dependency.
- `ioctl(TIOCGWINSZ)` on stdout to read terminal dimensions, falling back to 80 columns when not a tty or ioctl fails.
- `popen("sh -c <joined-cmd>", "r")` to run the command each tick, streaming its stdout to ours.
- `nanosleep()` for fractional-second intervals.

## Files

- `solaris/watch/watch.c` — single source file, target ~150 LOC.
- `solaris/watch/Makefile` — mirrors the style of `ftpd-master/Makefile`: auto-detects Linux vs Solaris (`uname -s`), supports `CC=gcc` and `CC=cc` (Sun Studio).
- `solaris/watch/README.md` — short usage and build notes.

## Behavior detail

### Argument parsing

- Manually parse `argv` (no `getopt_long`; `getopt` is available but rolling a tiny loop is clearer for two flags).
- `-n` takes the next argv token as a `double` via `strtod`; reject values `<= 0` or non-numeric with a usage error.
- `-t` is a flag toggle.
- `--` terminates flag parsing; everything after is the command.
- The first non-flag argv element begins the command. Remaining argv elements are the command's arguments.
- Empty command → print usage, exit 1.

### Command execution

The command argv (from argv-after-flags) is joined back into a single string with spaces between elements, then passed to `popen("sh -c <joined>", "r")`. This delegates shell metacharacter handling to `/bin/sh` and matches what users expect from `watch "ls | wc -l"`.

The joined buffer is built dynamically (allocate `sum(strlen)+nargs+1`); no fixed-size cap.

### Display per tick

1. Write `\033[2J\033[H` to stdout.
2. Unless `-t`, print the header: `Every <N.N>s: <cmd>` on the left, `<hostname>: <timestamp>` on the right, separated by enough spaces to right-align within the terminal width. Blank line after header.
3. Read from the `popen` pipe in chunks (4 KiB buffer) with `fread`, write each chunk to stdout with `fwrite`. Stop at EOF.
4. `pclose` the pipe.
5. `nanosleep` for the interval.

### Header formatting

- Width: from `ioctl(TIOCGWINSZ)` on fd 1; fallback 80.
- Left segment: `Every %.1fs: %s`.
- Right segment: `%s: %s` with hostname from `gethostname(2)` and timestamp from `strftime("%a %b %e %H:%M:%S %Y", localtime_r(...))` (the same format Linux `watch` uses).
- If the combined length exceeds the terminal width, truncate the *command* portion and append `...`.

### Signal handling

- Install handlers for `SIGINT` and `SIGTERM` that set `static volatile sig_atomic_t stop = 1`.
- The main loop checks `stop` after every `pclose`/`nanosleep` and exits cleanly with status 0.
- No terminal mode changes (we never enter cbreak/raw), so no cleanup is required beyond a final newline.

### Exit codes

| Code | Condition                                       |
|------|-------------------------------------------------|
| 0    | Normal termination via SIGINT / SIGTERM.        |
| 1    | Usage / argument error.                         |
| 2    | `popen` failed to spawn the command.            |

## Build

`Makefile` auto-detects via `uname -s`:

| Platform         | Compiler       | Flags                                            |
|------------------|----------------|--------------------------------------------------|
| Linux            | `gcc` (default)| `-std=c99 -Wall -Wextra`                         |
| SunOS (Solaris)  | `gcc`          | `-std=c99 -Wall -Wextra -lrt`                    |
| SunOS (Solaris)  | `cc` (Sun Studio) | `-xc99 -lrt`                                  |

All builds also apply `-D_POSIX_C_SOURCE=200112L` to expose `nanosleep` and `localtime_r` prototypes consistently.

Targets:

- `make` — release build.
- `make clean` — remove binary.
- `make install PREFIX=/usr/local` — copy to `$(PREFIX)/bin/watch`.

## Solaris 8 compatibility notes

| API                | Availability on SunOS 5.8                                      |
|--------------------|----------------------------------------------------------------|
| `nanosleep`        | Yes, in `<time.h>`. Requires `-lrt` (librt / libposix4).       |
| `ioctl(TIOCGWINSZ)`| Yes, defined in `<sys/termios.h>`.                              |
| `popen` / `pclose` | Yes, standard libc.                                             |
| `localtime_r`      | Yes, reentrant form available.                                  |
| `gethostname`      | Yes.                                                            |
| ANSI escape codes  | Supported by default `dtterm`, `xterm`, and most Solaris 8 ttys.|

## Edge cases handled in v1

- Command containing quotes or pipes: shell handles it via `sh -c`.
- Output longer than terminal rows: output scrolls naturally; no truncation (matches Linux `watch` default without `--no-wrap`).
- `-n 0` or negative / non-numeric: rejected with usage error.
- Empty command: usage error.
- `TIOCGWINSZ` failure (e.g., stdout not a tty): fall back to 80 cols; output still works.
- `gethostname` failure: fall back to literal `"localhost"` in header.
- `popen` failure: print an error to stderr, exit 2.

## Testing strategy (manual for v1)

Automated tests are out of scope for v1 because the program is primarily a terminal-visual tool. Smoke checks:

1. `./watch date` — header updates every 2s.
2. `./watch -n 0.5 date` — updates every half-second.
3. `./watch -t echo hello` — no header.
4. `./watch "ls | wc -l"` — shell metacharacter path works.
5. `./watch` — prints usage, exits 1.
6. `./watch -n 0 date` — prints error, exits 1.
7. Ctrl-C exits cleanly with a newline on the following line.

A later iteration may add a scripted harness.
