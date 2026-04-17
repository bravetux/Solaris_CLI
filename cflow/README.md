# cflow (Solaris 8 port — from-scratch v1)

A minimal call-graph utility, inspired by GNU cflow. Reads one or more C source
files and prints an indented graph of who calls whom. Built from scratch as a
single C99 file for Solaris 8 (SunOS 5.8) and Linux.

This is NOT a real C parser. It is a pattern-matching scanner. See
**Accepted limitations** below.

## Usage

    cflow [OPTIONS] FILE.c [FILE.c ...]

| Flag          | Meaning                                                   |
|---------------|-----------------------------------------------------------|
| `-d N`        | Limit display depth to N (>= 1). Default: unlimited.      |
| `-r`          | Reverse graph (callee -> callers) instead of the default. |
| `--main NAME` | Root the output at function NAME only.                    |
| `--`          | End of flags; everything after is a file argument.        |

At least one file argument is required.

### Output format

Each line is indented four spaces per depth level. Defined functions show a
`<file:line>` annotation; functions referenced only as callees (externals,
standard-library calls, function-like macros) are printed without annotation.

Example:

    main() <cflow.c:281>
        parse_opts() <cflow.c:65>
        process_file() <cflow.c:186>
            load_file() <cflow.c:158>
                fopen()
                fseek()
                ftell()
            scan_file() <cflow.c:89>
                sym_record_def() <cflow.c:40>
                sym_record_call() <cflow.c:53>

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

No extra linker flags.

## Accepted limitations

The scanner is a byte-level state machine, not a real C parser. As a result:

- `#include` directives are NOT expanded. Symbols defined in headers and not in
  the supplied `.c` files appear as "external" references without a file:line
  annotation.
- Macros are NOT expanded. Function-like macros (`ASSERT(x)`, `ARRAY_SIZE(a)`)
  appear as calls to the macro name.
- Function pointers (`callback(x)`) are recorded as calls to the local variable
  name, which is almost never useful.
- Typedefs that look like calls at statement position (rare: `mytype (expr);`)
  may be misidentified as calls. Normal casts `(mytype)expr` are handled
  correctly.
- K&R-style definitions (`int foo(a, b) int a; int b; { ... }`) are not
  recognized.
- Declarations that also look like definitions at the very end of the file
  (e.g. an `int foo(void);` followed later by `int foo(void) { ... }`) are
  fine; we only record the first definition we see.

## Smoke test

Run these manually on a host with a compiler:

1. `./cflow cflow.c` — self-graph, rooted at `main`.
2. `./cflow -d 2 cflow.c` — depth-limited.
3. `./cflow -r cflow.c` — reverse direction.
4. `./cflow --main scan_file cflow.c` — only that subtree.
5. `./cflow --main nosuchfn cflow.c` — `cflow: function 'nosuchfn' not found`, exit 1.
6. `./cflow nonexistent.c` — error to stderr, exit 1.
7. `./cflow` — usage + `no input files`, exit 1.
8. Two-file run: `./cflow ../watch/watch.c ../tree/tree.c cflow.c` — graph spans three files; calls into stdlib (`fopen`, `strcmp`, etc.) appear as externals.
9. Create a small file with recursion (`void a(void){b();} void b(void){a();}`) — output marks one node `{recursive}` and stops.

## Not implemented in v1

Macro expansion, `#include` expansion, function-pointer inference, K&R defs,
`-T` / `-x` / `--format=posix` / `--ancestor` / `--symbol` / `--pushdown` /
`--brief` / `-l` / `--omit-arguments` / `-D`. Planned for future iterations
when the naive scanner proves insufficient.
