# `tree` command — Solaris 8 port (v1)

**Date:** 2026-04-17
**Target platform:** Solaris 8 (SunOS 5.8), with Linux as a secondary build target for development.
**Source directory:** `solaris/tree/`

## Goal

Port the Linux `tree` utility to Solaris 8, feature set "C" (core + filters + sizes). Builds cleanly on Linux and Solaris 8 using the same Makefile pattern established by the `watch` port.

## Feature scope (v1)

| Flag           | Meaning                                                       |
|----------------|---------------------------------------------------------------|
| `-L N`         | Limit recursion depth to N (N ≥ 1). Root is level 1.          |
| `-a`           | Include hidden (dot) entries. `.` and `..` never shown.       |
| `-d`           | Directories only (skip non-directory entries).                |
| `-f`           | Print full relative path instead of basename.                 |
| `-F`           | Append type suffix: `/` dir, `@` symlink, `*` exec regular, `=` socket, `|` FIFO. |
| `-s`           | Print size in bytes, right-aligned in an 11-char field.       |
| `-h`           | Human-readable size (1024-base: `1.2K`, `45M`, `1.3G`).       |
| `-I PATTERN`   | `fnmatch` glob to exclude. Repeatable, max 16 patterns.       |
| `-n`           | Force ASCII box-drawing (`|--`, `` `-- ``, `|   `).           |
| `--noreport`   | Suppress the `N directories, M files` summary.                |

Positional args: zero or more directories. No arg → `.`. Multiple args → each is rendered in turn with a blank line between.

### Out of scope (v1)

`-D`, `-p`, `-u`, `-g`, `-J`, `-X`, `--dirsfirst`, `--prune`, `-P`, `-r`, `-t`, `-U`, `-o FILE`, `--charset=`, `--fromfile`, `--help`, `--version`. Revisit in a later version.

## Approach

Single-file C99 program using recursive descent:

- `opendir` / `readdir` / `lstat` each directory into a dynamic array of entries.
- Sort alphabetically via `qsort` + `strcmp`.
- Apply filters (`-a`, `-d`, `-I`) in the listing pass.
- Print with a running prefix string that accumulates `"│   "` / `"    "` (or the ASCII equivalents) per depth level.
- Recurse into each directory child that is a real directory (not a symlink to one — we do not follow symlinks, matching Linux `tree`'s default).

## Files

- `solaris/tree/tree.c` — single source file, target ~400 LOC.
- `solaris/tree/Makefile` — mirrors `solaris/watch/Makefile`. No `-lrt` needed.
- `solaris/tree/README.md` — usage, build matrix, smoke tests.

## Behavior detail

### Option parsing

- Manual argv loop (matching the `watch` port's style).
- `-L` takes a numeric argument; reject non-numeric or `< 1`.
- `-I` takes a pattern argument; store into a fixed-capacity array of 16 `char *`. Overflow → error, exit 1.
- `-h` implies size display (`-s`) semantics but in human units.
- `--noreport` is the only long option in v1.
- `--` terminates flag parsing; everything after is a directory argument.
- Unknown flag → usage error, exit 1.

### Per-directory listing (`list_dir`)

Given a path, produce a sorted, filtered array of entries:

```c
struct entry {
    char *name;        /* malloc'd */
    struct stat st;    /* from lstat */
    char *linktarget;  /* malloc'd; NULL unless S_ISLNK */
};
```

Steps:

1. `opendir(path)`; on failure, caller handles display of `[error opening dir]`.
2. Loop `readdir`; skip `.` and `..`.
3. If entry name starts with `.` and `-a` is not set, skip.
4. For each `-I` pattern, if `fnmatch(pat, name, 0) == 0`, skip.
5. Build full path (`path + "/" + name`), call `lstat`. On failure, skip silently.
6. If `S_ISLNK(st.st_mode)`, `readlink` into a heap buffer (`PATH_MAX`) and store as `linktarget`.
7. If `-d` is set and entry is not a directory (via `lstat`), skip.
8. Append to entry array (reallocating by doubling).
9. `closedir`. `qsort` by name via `strcmp`.

Note: "directory" for the `-d` filter means `S_ISDIR(st.st_mode)` on the `lstat` result. A symlink to a directory is NOT kept under `-d` (symlinks are not followed anywhere in v1).

### Printing an entry (`print_entry`)

For each entry in order, print:

```
<prefix><branch><size-field><display-name><suffix><arrow-target>
```

- `prefix`: accumulated string (`"│   "`, `"    "`, or ASCII equivalents), built up by the walker.
- `branch`: `"├── "` for all entries except the last in its directory, which uses `"└── "`. ASCII equivalents: `"|-- "` and `` "`-- " ``.
- `size-field`: absent unless `-s` or `-h`. `-s` format: `"[%10lld] "` (11 chars including brackets, then one space). `-h` format: same 11-char field but with a human-readable string right-aligned inside.
- `display-name`:
  - Without `-f`: the basename.
  - With `-f`: the full relative path from the root argument, built by the walker (`root + "/" + subpath/name`).
- `suffix` (only when `-F`):
  - `/` if `S_ISDIR`
  - `@` if `S_ISLNK`
  - `*` if `S_ISREG && (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))`
  - `=` if `S_ISSOCK`
  - `|` if `S_ISFIFO`
  - none otherwise (regular non-executable, block/char device).
- `arrow-target` (only when `S_ISLNK`): `" -> %s"` with `linktarget`.

### Walker (`walk`)

Signature (conceptually):

```c
static void walk(const struct opts *o,
                 const char *path,       /* path from cwd */
                 const char *display,    /* path to prefix when -f set */
                 const char *prefix,     /* accumulated indent string */
                 int depth,              /* 1 = root level */
                 long *dir_count,
                 long *file_count);
```

1. Call `list_dir(path, ...)`. If it fails, print a line `"<prefix><branch>[error opening dir]"` (caller handles the branch before recursing down), return.
2. For each entry `i` of `n`:
   - `is_last = (i == n - 1)`.
   - Build the per-entry branch: `"├── "` or `"└── "` (or ASCII).
   - Call `print_entry(entry, prefix, branch, display, opts)`.
   - Update counts: `S_ISDIR` → `dir_count++`, else `file_count++`. `-d` mode: only `S_ISDIR` entries are in the array, so still correct.
   - If `S_ISDIR(entry->st.st_mode)` and `(depth < L_limit)`, recurse:
     - New prefix = `prefix + ("│   " if !is_last else "    ")` (ASCII: `"|   "` / `"    "`).
     - New display = `display + "/" + entry->name`.
     - Increment `depth`.
3. Free the entry array and all its strings.

### Root-level handling

For each directory argument `D`:

1. Print `D` (verbatim — Linux `tree` prints the argument as given, not normalized), `\n`.
2. If `stat(D)` fails (not `lstat` for root, since user may pass a symlink to a dir and expect entry): print `tree: D: strerror(errno)` to stderr, exit 1.
3. If `!S_ISDIR(stat)`: print `tree: D: Not a directory` to stderr, exit 1.
4. Call `walk(opts, D, D, "", 1, &dirs, &files)`.
5. Unless `--noreport`, print a blank line then `"%ld directories, %ld files\n"`.
6. If multiple arguments remain, print a blank line.

### Charset selection

At startup:

```c
static int is_utf8_locale(void) {
    const char *env[] = {"LC_ALL", "LC_CTYPE", "LANG", NULL};
    for (int i = 0; env[i]; i++) {
        const char *v = getenv(env[i]);
        if (v && (*v)) {
            /* case-insensitive search for "utf8" or "UTF-8" */
            char buf[64]; size_t n = 0;
            while (v[n] && n < sizeof(buf)-1) { buf[n] = (char)tolower((unsigned char)v[n]); n++; }
            buf[n] = '\0';
            if (strstr(buf, "utf-8") || strstr(buf, "utf8")) return 1;
            return 0;
        }
    }
    return 0;
}
```

If `-n` is set, force ASCII regardless. Otherwise, UTF-8 iff `is_utf8_locale()`.

Concrete glyph sets:

| Purpose         | UTF-8         | ASCII       |
|-----------------|---------------|-------------|
| Mid branch      | `├── `        | `\|-- `     |
| Last branch     | `└── `        | `` `-- ``   |
| Pass-through    | `│   `        | `\|   `     |
| Spacer          | `    `        | `    `      |

### Human size formatting (`human_size`)

- Takes `off_t` (or `long long`), writes into a caller buffer of ≥ 8 chars.
- Units ordered: `B`, `K`, `M`, `G`, `T`, `P`. 1024-base.
- If the scaled value `< 10`, print with one decimal (`"%.1f%c"`). Else print with zero decimals (`"%.0f%c"`). Unit letter: `B`, `K`, `M`, `G`, `T`, `P`.
- Exception: when unit is `B` (value ≤ 1023), always print integer bytes: `"%ldB"`.
- Result is then right-aligned within the 11-character size field by `print_entry`.

## Error handling summary

| Condition                                     | Behavior                                           | Exit |
|-----------------------------------------------|----------------------------------------------------|------|
| Bad flag / `-L` non-numeric or `< 1`          | Usage message to stderr, exit 1.                   | 1    |
| Root `stat` fails                             | `tree: <path>: <strerror>`, exit 1.                | 1    |
| Root not a directory                          | `tree: <path>: Not a directory`, exit 1.           | 1    |
| Sub-directory `opendir` fails                 | `[error opening dir]` printed as branch content, continue. | 0 (if no root error) |
| `lstat` on an entry fails                     | Silently skip.                                     | 0    |
| `readlink` fails                              | Treat linktarget as empty string, still print `->`.| 0    |
| `fnmatch` returns an unexpected code          | Treat as "no match" (keep entry).                  | 0    |
| Too many `-I` patterns (> 16)                 | `tree: too many -I patterns (max 16)`, exit 1.     | 1    |
| OOM                                           | `tree: out of memory`, exit 1.                    | 1    |

## Solaris 8 compatibility notes

| API                | Status on SunOS 5.8                                                   |
|--------------------|-----------------------------------------------------------------------|
| `opendir`/`readdir`/`closedir` | POSIX, available.                                         |
| `lstat`, `stat`    | Available; `struct stat` has `st_mode`, `st_size`.                    |
| `S_IS*` macros     | Available from `<sys/stat.h>`.                                         |
| `readlink`         | Available.                                                            |
| `fnmatch`          | Available from `<fnmatch.h>`.                                         |
| `qsort`            | libc.                                                                 |
| UTF-8 to `dtterm`  | Works if `LANG` set to a `.UTF-8` locale; `-n` is the safe default on bare Solaris 8 consoles. |

No extra linker flags needed (`-lsocket -lnsl -lrt` are NOT required).

## Build

| Platform              | Compiler          | Flags                                                  |
|-----------------------|-------------------|--------------------------------------------------------|
| Linux                 | gcc               | `-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200112L`     |
| SunOS (Solaris)       | gcc               | same as above                                          |
| SunOS (Solaris)       | cc (Sun Studio)   | `-xc99 -D_POSIX_C_SOURCE=200112L`                      |

Targets: `make`, `make clean`, `make install PREFIX=/usr/local`.

## Testing strategy (manual for v1)

1. `./tree` — walks `.`, UTF-8 if locale supports it.
2. `./tree -n -L 2 /etc` — ASCII, depth 2.
3. `./tree -a /tmp/mytestdir` — hidden entries visible.
4. `./tree -s -h .` — human-readable sizes.
5. `./tree -d .` — directories only; summary shows `M` = 0 for files.
6. `./tree -f .` — each entry is a full relative path.
7. `./tree -F .` — `/`, `@`, `*`, `=`, `|` suffixes on matching types.
8. `./tree -I '*.o' -I '*.so' .` — excluded files absent.
9. `./tree --noreport .` — no summary.
10. `./tree nonexistent` — `tree: nonexistent: No such file or directory`, exit 1.
11. `./tree -L 0 .` — usage error, exit 1.
12. Create a symlink in a test dir and run `./tree testdir` — prints `name -> target`.
13. Create a FIFO (`mkfifo`) and a socket and run `./tree -F testdir` — correct suffixes.

Automated tests are out of scope for v1.
