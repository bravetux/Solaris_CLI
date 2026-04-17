# tree (Solaris 8 port)

List the contents of a directory as a tree. A minimal port of the Linux `tree`
utility targeted at Solaris 8 (SunOS 5.8). Builds cleanly on Linux for
development.

## Usage

    tree [OPTIONS] [DIRECTORY...]

| Flag         | Meaning                                                    |
|--------------|------------------------------------------------------------|
| `-L N`       | Max display depth (>= 1).                                  |
| `-a`         | Include hidden (dot) entries.                              |
| `-d`         | Directories only.                                          |
| `-f`         | Print full relative path instead of basename.              |
| `-F`         | Append type suffix: `/ @ * = \|`.                          |
| `-s`         | Print size in bytes.                                       |
| `-h`         | Human-readable sizes (1024-base).                          |
| `-I PATTERN` | `fnmatch` glob to exclude (repeatable, max 16).            |
| `-n`         | Force ASCII box-drawing.                                   |
| `--noreport` | Suppress the `N directories, M files` summary.             |
| `--`         | End of flags; everything after is a directory argument.    |

No directory argument defaults to `.`. Multiple arguments print each tree in
turn with a blank line between them.

Box-drawing glyphs use UTF-8 when `LC_ALL`, `LC_CTYPE`, or `LANG` contain
`UTF-8` / `utf8` (case-insensitive) and `-n` is not set; otherwise ASCII.

## Build

    make              # release build
    make CC=gcc       # force GCC
    make CC=cc        # Sun Studio (Solaris)
    make clean
    make install PREFIX=/usr/local

| Platform         | Compiler       | Flags applied                                      |
|------------------|----------------|----------------------------------------------------|
| Linux            | gcc            | `-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200112L` |
| SunOS (Solaris)  | gcc            | same as above                                      |
| SunOS (Solaris)  | cc (Sun Studio)| `-xc99 -D_POSIX_C_SOURCE=200112L`                  |

No extra linker flags are required.

## Smoke test

1. `./tree` — walks `.`, UTF-8 if locale supports it.
2. `./tree -n -L 2 /etc` — ASCII, depth 2.
3. `./tree -a /tmp/mytestdir` — hidden entries visible.
4. `./tree -s -h .` — human-readable sizes.
5. `./tree -d .` — directories only.
6. `./tree -f .` — each entry is a full relative path.
7. `./tree -F .` — type suffixes appear.
8. `./tree -I '*.o' -I '*.so' .` — excluded files absent.
9. `./tree --noreport .` — no summary.
10. `./tree nonexistent` — error to stderr, exits 1.
11. `./tree -L 0 .` — usage error, exits 1.
12. Symlinks print as `name -> target`.

## Not implemented in v1

`-D` (mtimes), `-p` (permissions), `-u` / `-g` (owner / group), `-J` / `-X`
(JSON / XML), `--dirsfirst`, `--prune`, `-P` (include glob), `-r` / `-t` / `-U`
(sorting variants), `-o FILE`, `--charset=`, `--fromfile`, `--help`,
`--version`. Planned for future iterations.
