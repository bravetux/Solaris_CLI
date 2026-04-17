# `cflow` command — Solaris 8 port (v1)

**Date:** 2026-04-17
**Target platform:** Solaris 8 (SunOS 5.8), with Linux as a secondary build target.
**Source directory:** `solaris/cflow/`

## Goal

Build a minimal, pragmatic `cflow`-alike utility from scratch that runs on Solaris 8 (and Linux). It reads one or more C source files and prints an indented call graph, using a single-pass character-level state machine — **not** a proper C parser. Documented limitations are accepted as the price of the small footprint.

## Scope — v1 (agreed with user)

| Flag          | Meaning                                                               |
|---------------|-----------------------------------------------------------------------|
| `-d N`        | Limit display depth to N (N ≥ 1). Default: unlimited.                 |
| `-r`          | Reverse: print callee → callers instead of caller → callees.          |
| `--main NAME` | Root output at function NAME only. Default: print every root.        |

Positional: one or more `.c` files (at least one required).

### Explicitly out of scope (v1)

`#include` expansion, macro expansion, function-pointer-call inference, typedef awareness, K&R definitions, cross-file linking via a separate symbol index, `-T` / `-x` / `--format=posix` / `--ancestor` / `--symbol` / `--pushdown` / `--brief` / `-l` / `--omit-arguments` / `-D`. These are deferred to a future version.

### Accepted limitations (document in README)

- Function-like macros (`ASSERT(x)`, `ARRAY_SIZE(a)`) appear as "calls" to the macro name.
- Typedef-cast expressions like `uint32_t (foo)` at statement position (rare) are misclassified as calls to `uint32_t`. Normal `(uint32_t)(foo + bar)` style casts are handled correctly by the paren-depth bookkeeping.
- `#include`s are not expanded; references to functions in un-supplied files appear as "external" nodes without a file:line annotation.
- K&R-style definitions (`int foo(a, b) int a; int b; { … }`) are not recognized.

## Approach

Single-file C99 program with a **single-pass scanner + in-memory symbol table + tree printer**. No lexer class hierarchy, no token stream. One function (`scan_file`) walks the bytes, tracking state, and emits two events into the symbol table:

- `on_def(name, file, line)` when `identifier ( ... ) {` pattern is seen at brace depth 0 (the scanner then marks the current-function context).
- `on_call(caller, callee)` when `identifier (` is seen inside a function body and the identifier is not a reserved keyword.

A symbol table deduplicates names and collects per-function callee lists. A second phase builds the root set (or honors `--main` / `-r`) and prints the indented graph.

## Files

| File                         | Purpose                                                 |
|------------------------------|---------------------------------------------------------|
| `solaris/cflow/cflow.c`      | Single C source file. Target ~1000 LOC.                 |
| `solaris/cflow/Makefile`     | Portable Linux / Solaris 8 build; same pattern as `watch/` and `tree/`. |
| `solaris/cflow/README.md`    | Usage, build matrix, accepted limitations, smoke tests. |

## Component-level design

### 1. Scanner state (`scan_file`)

Per-file state variables:

```c
enum { ST_CODE, ST_STRING, ST_CHAR, ST_LINE_CMT, ST_BLOCK_CMT, ST_PREPROC };
struct scan_state {
    int        state;        /* one of the ST_* values */
    int        brace_depth;  /* increments on '{', decrements on '}' (when state == ST_CODE) */
    int        paren_depth;  /* increments on '(', decrements on ')' (when state == ST_CODE) */
    int        line;         /* 1-based, bumped on '\n' in any state */
    const char *file;        /* interned file name for annotations */
    int        cur_fn_id;    /* -1 when at brace_depth 0 and no pending definition */
    char       last_ident[128]; /* most recently completed identifier (for def detection) */
    int        last_ident_line;
    int        after_ident_paren_depth; /* tracks '(' depth after last_ident at brace_depth 0 */
    int        def_pending;  /* 1 when we matched "name ( ... )" at brace 0 — waiting for '{' */
};
```

Rules applied character-by-character while `state == ST_CODE`:

- **Comments and strings.** Entering `/*`, `//`, `"`, `'` switches state. Escapes inside strings / chars are honored. Transitions back to `ST_CODE` on matching delimiter or newline (for `//`).
- **Preprocessor.** A `#` appearing as the first non-whitespace on a line switches to `ST_PREPROC`. That state ends at the next newline that is not preceded by a backslash (line continuation handled by checking the byte before `\n`).
- **Braces.** `{` / `}` in `ST_CODE` adjust `brace_depth`.
- **Parens.** `(` / `)` in `ST_CODE` adjust `paren_depth`.
- **Identifiers.** An identifier is `[A-Za-z_][A-Za-z0-9_]*`. On completing one, store into `last_ident`. If `brace_depth == 0` and the very next non-space character after the identifier is `(`, open a pending-def tracker: record `last_ident` and watch `paren_depth`. When `paren_depth` returns to zero, expect the next non-space non-comment-non-pp character. If it is `{`, fire `on_def(last_ident, file, last_ident_line)` and set `cur_fn_id` to that function's id. Otherwise (semicolon, comma, `=`, etc.) the pending def is discarded.
- **Calls.** When `brace_depth > 0`, `cur_fn_id >= 0`, and we complete an identifier immediately followed (ignoring whitespace) by `(`, classify it as a call — unless the identifier is in the keyword blocklist. Fire `on_call(cur_fn_id, identifier_name)`.

At the close `}` that brings `brace_depth` back to 0, clear `cur_fn_id = -1`.

### Keyword blocklist

Identifiers that may be followed by `(` but are not calls:

```
if while for switch return sizeof do else case default goto
typeof _Alignof alignof _Generic offsetof
```

Types that also frequently appear as `typename (expr)` in C code are NOT blocklisted, because normal casts `(type)expr` have the type inside the parens — our paren-depth bookkeeping handles those correctly.

### 2. Symbol table

```c
struct func {
    char *name;              /* owned */
    char *file;              /* owned, NULL if only seen as callee */
    int   line;              /* 0 if only seen as callee */
    int  *callees;           /* dynamic array of function ids */
    int   n_callees;
    int   cap_callees;
    int   is_defined;        /* 1 if we saw the definition */
};
```

Backing store: dynamic array of `struct func`, keyed by name via linear scan (for v1 simplicity — target input is one-to-a-few files, so a hash table is YAGNI). If a caller references a callee name not yet in the table, we add a placeholder entry with `is_defined = 0`.

Insertion into a caller's `callees` list deduplicates — the same callee from the same caller is only recorded once.

### 3. Graph operations

- `compute_roots()`: a function is a "root" if no other function's `callees` list contains its id. When `--main NAME` is given, roots = just that one function (error if not found). When `-r` is given and `--main` is not, "roots" become functions with no outgoing callers — i.e., leaves in the forward graph.
- `reverse()`: optional pass that, when `-r` is set, builds a callers list for each function by iterating the callees arrays.

### 4. Tree printer

```c
static void print_tree(int func_id, int depth, const int *ancestors, int n_ancestors);
```

- Indent by `depth * 4` spaces.
- Emit `name() <file:line>` if `is_defined`, else `name()` only.
- On recursion (func id in `ancestors`), append ` {recursive}` and return without descending.
- Honor `-d N` (stop descending when `depth >= N`).

When output root set is multiple functions, print each rooted tree in turn separated by a blank line.

## Output format

Default (forward, caller → callees):

```
main() <cflow.c:813>
    parse_opts() <cflow.c:120>
    process_file() <cflow.c:540>
        scan_file() <cflow.c:230>
            add_def()
            add_call()
        fclose()
    compute_roots() <cflow.c:620>
    print_tree() <cflow.c:700>
```

With `-r` (reverse, callee → callers):

```
add_def()
    process_file() <cflow.c:540>
        main() <cflow.c:813>
```

With `--main scan_file`:

```
scan_file() <cflow.c:230>
    add_def()
    add_call()
```

## Error handling

| Condition                                  | Behavior                                                | Exit |
|--------------------------------------------|---------------------------------------------------------|------|
| No positional arguments                    | `cflow: no input files` + usage to stderr, exit 1.      | 1    |
| `fopen` fails on a file                    | `cflow: FILE: <strerror>` to stderr; continue; exit 1 at end. | 1 |
| `-d N` non-numeric or `< 1`                | `cflow: invalid depth 'N'`, exit 1.                     | 1    |
| `-d` without argument                      | `cflow: -d requires an argument`, exit 1.               | 1    |
| `--main NAME` not found after all files scanned | `cflow: function 'NAME' not found`, exit 1.        | 1    |
| Unknown flag                               | `cflow: unknown option '...'`, exit 1.                  | 1    |
| OOM                                        | `cflow: out of memory`, exit 1.                         | 1    |

Internal error states inside the scanner (stray braces, unterminated strings at EOF) are handled by silently resetting `brace_depth`/`paren_depth` to zero at EOF — they do not abort the program. Unterminated `/* */` at EOF is reported as `cflow: FILE: unterminated block comment` to stderr (but does not exit).

## Solaris 8 compatibility notes

All C99 standard library — `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`. No POSIX-specific APIs used, no extra linker flags.

| API        | Status on SunOS 5.8           |
|------------|-------------------------------|
| `fgetc`    | libc                          |
| `isalpha`, `isalnum` | libc (with `(unsigned char)` cast) |
| `strdup`   | POSIX.1-2001, available       |
| `strcmp`, `strcpy`, `strncpy`, `memset` | libc |

## Build

| Platform         | Compiler        | Flags                                              |
|------------------|-----------------|----------------------------------------------------|
| Linux            | gcc             | `-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200112L` |
| SunOS (Solaris)  | gcc             | same as above                                      |
| SunOS (Solaris)  | cc (Sun Studio) | `-xc99 -D_POSIX_C_SOURCE=200112L`                  |

Targets: `make`, `make clean`, `make install PREFIX=/usr/local`.

## Testing strategy (manual for v1)

1. `./cflow cflow.c` — self-graph. Should print `main` rooted, with `parse_opts`, `process_file`, `scan_file`, `print_tree` as descendants.
2. `./cflow -d 2 cflow.c` — depth-limited.
3. `./cflow -r cflow.c` — reversed edges; `add_def` should show callers.
4. `./cflow --main scan_file cflow.c` — only that subtree.
5. `./cflow --main nonexistent cflow.c` — `function 'nonexistent' not found`, exit 1.
6. `./cflow nonexistent.c` — error, exit 1.
7. `./cflow` — usage, exit 1.
8. `./cflow -d abc cflow.c` — `invalid depth 'abc'`, exit 1.
9. `./cflow ../watch/watch.c ../tree/tree.c cflow.c` — multi-file graph. External calls (e.g., `popen`, `opendir`) should appear without file:line annotation.
10. Synthetic test: a tiny file `test.c` with `void a(void){b();} void b(void){a();}` — cycle detection prints `a() → b() {recursive}` and stops.

Automated tests are out of scope for v1.
