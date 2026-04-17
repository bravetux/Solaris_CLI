# Solaris_CLI

A collection of small, from-scratch C99 ports of familiar Linux command-line
utilities, targeted at **Solaris 8 (SunOS 5.8)** and building cleanly on modern
Linux for development. Each tool is a single `.c` file with its own `Makefile`
and no external dependencies beyond a POSIX libc.

## Tools

| Tool                | Purpose                                                  |
|---------------------|----------------------------------------------------------|
| [`cflow/`](cflow/)  | Minimal call-graph generator (inspired by GNU cflow).    |
| [`tree/`](tree/)    | Directory-as-tree lister (port of the Linux `tree`).     |
| [`watch/`](watch/)  | Periodically run a command and show its output.          |

Each tool has its own `README.md` with full usage, build instructions, and
smoke tests.

## Build

Every tool follows the same build pattern:

    cd <tool>
    make              # release build (auto-detects platform/compiler)
    make CC=gcc       # force GCC
    make CC=cc        # Sun Studio (Solaris)
    make clean
    make install PREFIX=/usr/local

| Platform         | Compiler       | Flags applied                                      |
|------------------|----------------|----------------------------------------------------|
| Linux            | gcc            | `-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200112L` |
| SunOS (Solaris)  | gcc            | same as above                                      |
| SunOS (Solaris)  | cc (Sun Studio)| `-xc99 -D_POSIX_C_SOURCE=200112L`                  |

`watch` additionally links `-lrt` on Solaris for `nanosleep`.

## Repository layout

    cflow/    call-graph tool
    tree/     directory-tree lister
    watch/    command-repeater

## Design philosophy

- **C99, POSIX-only.** No GNU extensions, no autoconf, no third-party deps.
- **Single translation unit per tool.** Easy to read, easy to audit.
- **Small, explicit option surface.** Each tool's `README.md` lists what is
  implemented and what is deliberately deferred to a future iteration.
- **Same Makefile shape everywhere.** Learn it once; rebuild anywhere.

## License

See [LICENSE](LICENSE).
