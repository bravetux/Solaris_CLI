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
