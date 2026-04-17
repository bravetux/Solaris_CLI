# `tree` (Solaris 8 port) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a portable port of the Linux `tree` utility (feature set "C" — core + filters + sizes) that runs on Solaris 8 (SunOS 5.8) and Linux.

**Architecture:** Single-file C99 program using recursive descent. Each directory is read into a sorted, filtered array of `struct entry` records (name + `struct stat` + optional `readlink` target), then printed with accumulated prefix strings (`"│   "` / `"    "` or ASCII equivalents). No library dependencies beyond libc + POSIX.

**Tech Stack:** C99 • POSIX (`<dirent.h>`, `<sys/stat.h>`, `<fnmatch.h>`, `<ctype.h>`) • `make` with `gcc` on Linux and `gcc` / Sun Studio `cc` on Solaris.

**Note on git:** The `solaris/` directory is not a git repo in this workspace, so the plan omits `git commit` steps.

**Note on TDD:** Per the approved spec, automated tests are out of scope for v1. Verification uses the spec's manual smoke-test checklist at the end of each task.

---

## File Structure

| File                           | Purpose                                                      |
|--------------------------------|--------------------------------------------------------------|
| `solaris/tree/tree.c`          | Single C source file. Target ~400 LOC.                       |
| `solaris/tree/Makefile`        | Portable Linux/Solaris build. Same pattern as `watch/`.      |
| `solaris/tree/README.md`       | Usage, build matrix, smoke-test checklist.                   |

---

## Task 1: Skeleton — Makefile and `tree.c` with full option parsing

**Files:**
- Create: `solaris/tree/Makefile`
- Create: `solaris/tree/tree.c`

**Goal:** `make` produces a `tree` binary. Running `./tree --opts-dump .` prints the parsed option struct and exits — a debug path we'll remove in Task 2. Running `./tree` with no args does nothing yet (walker lands in Task 2).

- [ ] **Step 1: Create `solaris/tree/Makefile`**

```make
# Makefile for tree (Solaris 8 port)
# Auto-detects Linux vs SunOS. Respects CC override (e.g. CC=cc for Sun Studio).

UNAME_S := $(shell uname -s)
CC      ?= gcc
PREFIX  ?= /usr/local
BIN     := tree
SRC     := tree.c

COMMON_DEFS := -D_POSIX_C_SOURCE=200112L

ifeq ($(UNAME_S),SunOS)
  ifeq ($(CC),cc)
    CFLAGS  ?= -xc99 $(COMMON_DEFS)
  else
    CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
  endif
else
  CFLAGS  ?= -std=c99 -Wall -Wextra $(COMMON_DEFS)
endif

LDLIBS :=

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

Recipe lines under `$(BIN):`, `clean:`, `install:`, `uninstall:` MUST use a literal TAB character.

- [ ] **Step 2: Create `solaris/tree/tree.c` with option parsing only**

```c
/*
 * tree - list a directory as a tree.
 * Solaris 8 port, v1 (feature set C: core + filters + sizes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fnmatch.h>

#define MAX_EXCLUDES 16

struct opts {
    int max_depth;       /* 0 = unlimited */
    int show_hidden;     /* -a */
    int dirs_only;       /* -d */
    int full_paths;      /* -f */
    int type_suffix;     /* -F */
    int show_size;       /* -s or -h set */
    int human_size;      /* -h */
    int ascii_only;      /* -n */
    int no_report;       /* --noreport */
    int n_excludes;
    const char *excludes[MAX_EXCLUDES];
};

static void usage(FILE *out) {
    fputs(
        "Usage: tree [OPTIONS] [DIRECTORY...]\n"
        "  -L N          max display depth (>= 1)\n"
        "  -a            include hidden entries\n"
        "  -d            directories only\n"
        "  -f            print full relative paths\n"
        "  -F            append type suffix (/ @ * = |)\n"
        "  -s            print size in bytes\n"
        "  -h            human-readable sizes\n"
        "  -I PATTERN    fnmatch glob to exclude (repeatable)\n"
        "  -n            ASCII box-drawing only\n"
        "  --noreport    suppress summary line\n",
        out);
}

/* Returns index of first positional argument, or a negative value on error
 * (the caller will exit after printing usage). argv[*err] points to the bad
 * argument when err != NULL. */
static int parse_opts(int argc, char **argv, struct opts *o) {
    memset(o, 0, sizeof(*o));
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--noreport") == 0) { o->no_report = 1; continue; }
        if (strcmp(a, "-a") == 0) { o->show_hidden = 1; continue; }
        if (strcmp(a, "-d") == 0) { o->dirs_only   = 1; continue; }
        if (strcmp(a, "-f") == 0) { o->full_paths  = 1; continue; }
        if (strcmp(a, "-F") == 0) { o->type_suffix = 1; continue; }
        if (strcmp(a, "-s") == 0) { o->show_size   = 1; continue; }
        if (strcmp(a, "-h") == 0) { o->show_size   = 1; o->human_size = 1; continue; }
        if (strcmp(a, "-n") == 0) { o->ascii_only  = 1; continue; }
        if (strcmp(a, "-L") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "tree: -L requires an argument\n"); return -1; }
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 1 || v > 1000000) {
                fprintf(stderr, "tree: invalid depth '%s' (must be >= 1)\n", argv[i]);
                return -1;
            }
            o->max_depth = (int)v;
            continue;
        }
        if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "tree: -I requires an argument\n"); return -1; }
            if (o->n_excludes >= MAX_EXCLUDES) {
                fprintf(stderr, "tree: too many -I patterns (max %d)\n", MAX_EXCLUDES);
                return -1;
            }
            o->excludes[o->n_excludes++] = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "tree: unknown option '%s'\n", a);
            return -1;
        }
        break; /* start of positional args */
    }
    return i;
}

int main(int argc, char **argv) {
    struct opts o;
    int first = parse_opts(argc, argv, &o);
    if (first < 0) { usage(stderr); return 1; }

    (void)o; /* unused in Task 1 */
    (void)first;
    return 0;
}
```

- [ ] **Step 3: Build and verify flag-parsing paths**

Run on a host that has `gcc` and `make`:
```bash
cd solaris/tree && make
./tree ; echo "exit=$?"
./tree -a -n ; echo "exit=$?"
./tree -L 3 /etc ; echo "exit=$?"
./tree -L 0 ; echo "exit=$?"
./tree -Z ; echo "exit=$?"
./tree -L ; echo "exit=$?"
```

Expected:
- First two print nothing (Task 2 wires output) and exit 0.
- Third prints nothing, exits 0.
- Fourth prints `tree: invalid depth '0' (must be >= 1)` + usage on stderr, exits 1.
- Fifth prints `tree: unknown option '-Z'` + usage, exits 1.
- Sixth prints `tree: -L requires an argument` + usage, exits 1.

If no compiler is available on the host, skip the `make` step and visually confirm the source matches.

- [ ] **Step 4: Verify `make clean`**

Run:
```bash
make clean && ls tree 2>&1 || echo "removed"
```

Expected: `removed`.

---

## Task 2: Directory listing, recursive walker, ASCII-only output

**Files:**
- Modify: `solaris/tree/tree.c` — replace the entire file with the expanded version below

**Goal:** `./tree` and `./tree SOMEDIR` produce a hierarchical tree with `|--` / `` `-- `` / `|   ` ASCII branches. All flags from Task 1 are wired up and functional: `-L`, `-a`, `-d`, `-f`, `-F`, `-I`. `-s`/`-h`/`-n`/`--noreport` are still no-ops at this point. Summary line and multi-dir handling land in Task 4. UTF-8 glyphs and symlink detail land in Task 3.

- [ ] **Step 1: Replace `solaris/tree/tree.c` entirely with the following**

```c
/*
 * tree - list a directory as a tree.
 * Solaris 8 port, v1 (feature set C: core + filters + sizes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fnmatch.h>

#define MAX_EXCLUDES 16

struct opts {
    int max_depth;
    int show_hidden;
    int dirs_only;
    int full_paths;
    int type_suffix;
    int show_size;
    int human_size;
    int ascii_only;
    int no_report;
    int n_excludes;
    const char *excludes[MAX_EXCLUDES];
};

struct entry {
    char *name;
    struct stat st;
    char *linktarget;  /* malloc'd if S_ISLNK, else NULL */
};

/* ASCII-only glyphs for Task 2. Task 3 switches based on locale / -n. */
static const char *BR_MID   = "|-- ";
static const char *BR_LAST  = "`-- ";
static const char *BR_PASS  = "|   ";
static const char *BR_SPACE = "    ";

static void usage(FILE *out) {
    fputs(
        "Usage: tree [OPTIONS] [DIRECTORY...]\n"
        "  -L N          max display depth (>= 1)\n"
        "  -a            include hidden entries\n"
        "  -d            directories only\n"
        "  -f            print full relative paths\n"
        "  -F            append type suffix (/ @ * = |)\n"
        "  -s            print size in bytes\n"
        "  -h            human-readable sizes\n"
        "  -I PATTERN    fnmatch glob to exclude (repeatable)\n"
        "  -n            ASCII box-drawing only\n"
        "  --noreport    suppress summary line\n",
        out);
}

static int parse_opts(int argc, char **argv, struct opts *o) {
    memset(o, 0, sizeof(*o));
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--noreport") == 0) { o->no_report = 1; continue; }
        if (strcmp(a, "-a") == 0) { o->show_hidden = 1; continue; }
        if (strcmp(a, "-d") == 0) { o->dirs_only   = 1; continue; }
        if (strcmp(a, "-f") == 0) { o->full_paths  = 1; continue; }
        if (strcmp(a, "-F") == 0) { o->type_suffix = 1; continue; }
        if (strcmp(a, "-s") == 0) { o->show_size   = 1; continue; }
        if (strcmp(a, "-h") == 0) { o->show_size   = 1; o->human_size = 1; continue; }
        if (strcmp(a, "-n") == 0) { o->ascii_only  = 1; continue; }
        if (strcmp(a, "-L") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "tree: -L requires an argument\n"); return -1; }
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 1 || v > 1000000) {
                fprintf(stderr, "tree: invalid depth '%s' (must be >= 1)\n", argv[i]);
                return -1;
            }
            o->max_depth = (int)v;
            continue;
        }
        if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "tree: -I requires an argument\n"); return -1; }
            if (o->n_excludes >= MAX_EXCLUDES) {
                fprintf(stderr, "tree: too many -I patterns (max %d)\n", MAX_EXCLUDES);
                return -1;
            }
            o->excludes[o->n_excludes++] = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "tree: unknown option '%s'\n", a);
            return -1;
        }
        break;
    }
    return i;
}

static int cmp_entry(const void *a, const void *b) {
    const struct entry *ea = (const struct entry *)a;
    const struct entry *eb = (const struct entry *)b;
    return strcmp(ea->name, eb->name);
}

static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int needs_slash = (la > 0 && a[la - 1] != '/');
    char *r = (char *)malloc(la + (needs_slash ? 1 : 0) + lb + 1);
    if (!r) return NULL;
    memcpy(r, a, la);
    if (needs_slash) r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}

static int matches_exclude(const char *name, const struct opts *o) {
    for (int i = 0; i < o->n_excludes; i++) {
        if (fnmatch(o->excludes[i], name, 0) == 0) return 1;
    }
    return 0;
}

/* Returns 0 on success; -1 if opendir failed. On success, *out/*out_n are set. */
static int list_dir(const char *path, const struct opts *o,
                    struct entry **out, size_t *out_n) {
    DIR *d = opendir(path);
    if (!d) return -1;

    struct entry *arr = NULL;
    size_t cap = 0, n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (!o->show_hidden && nm[0] == '.') continue;
        if (matches_exclude(nm, o)) continue;

        char *full = path_join(path, nm);
        if (!full) continue;

        struct stat st;
        if (lstat(full, &st) != 0) { free(full); continue; }

        if (o->dirs_only && !S_ISDIR(st.st_mode)) { free(full); continue; }

        char *lt = NULL;
        if (S_ISLNK(st.st_mode)) {
            lt = (char *)malloc(1024);
            if (lt) {
                ssize_t k = readlink(full, lt, 1023);
                if (k < 0) k = 0;
                lt[k] = '\0';
            }
        }
        free(full);

        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            struct entry *na = (struct entry *)realloc(arr, nc * sizeof(*arr));
            if (!na) { free(lt); break; }
            arr = na; cap = nc;
        }
        arr[n].name = strdup(nm);
        arr[n].st   = st;
        arr[n].linktarget = lt;
        if (!arr[n].name) { free(lt); break; }
        n++;
    }
    closedir(d);

    qsort(arr, n, sizeof(*arr), cmp_entry);
    *out = arr;
    *out_n = n;
    return 0;
}

static void free_entries(struct entry *arr, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(arr[i].name);
        free(arr[i].linktarget);
    }
    free(arr);
}

static void print_entry(const struct entry *e,
                        const char *prefix,
                        const char *branch,
                        const char *display_path,
                        const struct opts *o) {
    fputs(prefix, stdout);
    fputs(branch, stdout);

    const char *name = (o->full_paths && display_path) ? display_path : e->name;
    fputs(name, stdout);

    if (o->type_suffix) {
        mode_t m = e->st.st_mode;
        if (S_ISDIR(m))       fputc('/', stdout);
        else if (S_ISLNK(m))  fputc('@', stdout);
        else if (S_ISREG(m) && (m & (S_IXUSR | S_IXGRP | S_IXOTH))) fputc('*', stdout);
#ifdef S_ISSOCK
        else if (S_ISSOCK(m)) fputc('=', stdout);
#endif
        else if (S_ISFIFO(m)) fputc('|', stdout);
    }

    fputc('\n', stdout);
}

static void walk(const struct opts *o, const char *path, const char *display,
                 const char *prefix, int depth,
                 long *dir_count, long *file_count) {
    struct entry *arr = NULL;
    size_t n = 0;
    if (list_dir(path, o, &arr, &n) != 0) {
        fprintf(stdout, "%s%s[error opening dir]\n", prefix, BR_LAST);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        int is_last = (i + 1 == n);
        const char *branch = is_last ? BR_LAST : BR_MID;

        char *child_display = path_join(display, arr[i].name);
        print_entry(&arr[i], prefix, branch, child_display, o);

        if (S_ISDIR(arr[i].st.st_mode)) (*dir_count)++;
        else (*file_count)++;

        if (S_ISDIR(arr[i].st.st_mode) &&
            (o->max_depth == 0 || depth < o->max_depth)) {
            char *child_path = path_join(path, arr[i].name);

            size_t plen = strlen(prefix);
            size_t addlen = strlen(is_last ? BR_SPACE : BR_PASS);
            char *new_prefix = (char *)malloc(plen + addlen + 1);
            if (child_path && new_prefix) {
                memcpy(new_prefix, prefix, plen);
                memcpy(new_prefix + plen, is_last ? BR_SPACE : BR_PASS, addlen + 1);
                walk(o, child_path, child_display, new_prefix, depth + 1,
                     dir_count, file_count);
            }
            free(new_prefix);
            free(child_path);
        }
        free(child_display);
    }
    free_entries(arr, n);
}

int main(int argc, char **argv) {
    struct opts o;
    int first = parse_opts(argc, argv, &o);
    if (first < 0) { usage(stderr); return 1; }

    const char *default_argv[] = { "." };
    const char **dirs;
    int ndirs;
    if (first >= argc) { dirs = default_argv; ndirs = 1; }
    else { dirs = (const char **)(argv + first); ndirs = argc - first; }

    int rc = 0;
    for (int k = 0; k < ndirs; k++) {
        const char *root = dirs[k];
        struct stat rst;
        if (stat(root, &rst) != 0) {
            fprintf(stderr, "tree: %s: %s\n", root, strerror(errno));
            rc = 1;
            continue;
        }
        if (!S_ISDIR(rst.st_mode)) {
            fprintf(stderr, "tree: %s: Not a directory\n", root);
            rc = 1;
            continue;
        }
        fputs(root, stdout);
        fputc('\n', stdout);

        long dirs_n = 0, files_n = 0;
        walk(&o, root, root, "", 1, &dirs_n, &files_n);
    }
    return rc;
}
```

- [ ] **Step 2: Build and smoke-test**

Run on a host with `gcc`:
```bash
cd solaris/tree && make clean && make
./tree .
./tree -L 2 .
./tree -a /tmp
./tree -I '*.o' -I '*.c' .
./tree -d .
./tree -f .
./tree -F .
./tree nonexistent
```

Expected:
- Clean build, no warnings.
- First five print a tree rooted at the named directory with `|-- ` / `` `-- `` / `|   ` branches.
- `-d` shows only directory entries.
- `-f` shows `dir/sub/leaf` instead of `leaf` on each line.
- `-F` appends `/` to directory lines (and other suffixes when applicable).
- `nonexistent` prints `tree: nonexistent: No such file or directory` to stderr and exits 1.

If no compiler is available, the subagent should still confirm the source compiles conceptually (visual review of includes, signatures, and memory flow) and flag the untested state in its report.

---

## Task 3: UTF-8 charset detection, `-n` flag activation, symlink arrows

**Files:**
- Modify: `solaris/tree/tree.c` — surgical edits with the Edit tool

**Goal:** When locale indicates UTF-8, output uses `├── └── │   `. When `-n` is set, or locale is not UTF-8, output stays ASCII. Symlinks print as `name -> target`.

- [ ] **Step 1: Make glyph pointers mutable and add `is_utf8_locale`**

Find this block in `tree.c`:
```c
/* ASCII-only glyphs for Task 2. Task 3 switches based on locale / -n. */
static const char *BR_MID   = "|-- ";
static const char *BR_LAST  = "`-- ";
static const char *BR_PASS  = "|   ";
static const char *BR_SPACE = "    ";
```
Replace it with:
```c
/* Glyphs for tree branches. Switched between UTF-8 and ASCII in main(). */
static const char *BR_MID;
static const char *BR_LAST;
static const char *BR_PASS;
static const char *BR_SPACE;

static const char *ASCII_MID   = "|-- ";
static const char *ASCII_LAST  = "`-- ";
static const char *ASCII_PASS  = "|   ";
static const char *ASCII_SPACE = "    ";

static const char *UTF8_MID    = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "; /* "├── " */
static const char *UTF8_LAST   = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "; /* "└── " */
static const char *UTF8_PASS   = "\xe2\x94\x82   ";                       /* "│   " */
static const char *UTF8_SPACE  = "    ";

static int is_utf8_locale(void) {
    const char *env_names[] = { "LC_ALL", "LC_CTYPE", "LANG", NULL };
    for (int i = 0; env_names[i]; i++) {
        const char *v = getenv(env_names[i]);
        if (!v || !*v) continue;
        char buf[64];
        size_t k = 0;
        for (; v[k] && k + 1 < sizeof(buf); k++) {
            buf[k] = (char)tolower((unsigned char)v[k]);
        }
        buf[k] = '\0';
        if (strstr(buf, "utf-8") || strstr(buf, "utf8")) return 1;
        return 0;
    }
    return 0;
}
```

- [ ] **Step 2: Pick glyph set at the top of `main`**

Find this block:
```c
int main(int argc, char **argv) {
    struct opts o;
    int first = parse_opts(argc, argv, &o);
    if (first < 0) { usage(stderr); return 1; }
```
Replace it with:
```c
int main(int argc, char **argv) {
    struct opts o;
    int first = parse_opts(argc, argv, &o);
    if (first < 0) { usage(stderr); return 1; }

    int use_utf8 = (!o.ascii_only) && is_utf8_locale();
    BR_MID   = use_utf8 ? UTF8_MID   : ASCII_MID;
    BR_LAST  = use_utf8 ? UTF8_LAST  : ASCII_LAST;
    BR_PASS  = use_utf8 ? UTF8_PASS  : ASCII_PASS;
    BR_SPACE = use_utf8 ? UTF8_SPACE : ASCII_SPACE;
```

- [ ] **Step 3: Make `print_entry` emit `-> linktarget` for symlinks**

Find this block inside `print_entry`:
```c
    if (o->type_suffix) {
        mode_t m = e->st.st_mode;
        if (S_ISDIR(m))       fputc('/', stdout);
        else if (S_ISLNK(m))  fputc('@', stdout);
        else if (S_ISREG(m) && (m & (S_IXUSR | S_IXGRP | S_IXOTH))) fputc('*', stdout);
#ifdef S_ISSOCK
        else if (S_ISSOCK(m)) fputc('=', stdout);
#endif
        else if (S_ISFIFO(m)) fputc('|', stdout);
    }

    fputc('\n', stdout);
}
```
Replace it with:
```c
    if (o->type_suffix) {
        mode_t m = e->st.st_mode;
        if (S_ISDIR(m))       fputc('/', stdout);
        else if (S_ISLNK(m))  fputc('@', stdout);
        else if (S_ISREG(m) && (m & (S_IXUSR | S_IXGRP | S_IXOTH))) fputc('*', stdout);
#ifdef S_ISSOCK
        else if (S_ISSOCK(m)) fputc('=', stdout);
#endif
        else if (S_ISFIFO(m)) fputc('|', stdout);
    }

    if (S_ISLNK(e->st.st_mode) && e->linktarget) {
        fputs(" -> ", stdout);
        fputs(e->linktarget, stdout);
    }

    fputc('\n', stdout);
}
```

- [ ] **Step 4: Rebuild and smoke-test**

Run:
```bash
cd solaris/tree && make clean && make
LANG=en_US.UTF-8 ./tree .
LANG=C ./tree .
./tree -n .
# Create a symlink to test
mkdir -p /tmp/tt && ln -sf /etc/passwd /tmp/tt/mylink
./tree /tmp/tt
```

Expected:
- First: tree drawn with UTF-8 box characters.
- Second & third: tree drawn with ASCII characters.
- Fourth: symlink line prints as `<branch>mylink -> /etc/passwd`.

---

## Task 4: Size display (`-s`, `-h`) + summary line + `--noreport`

**Files:**
- Modify: `solaris/tree/tree.c` — surgical edits

**Goal:** `-s` and `-h` print a right-aligned 11-char size field before each entry name. The `N directories, M files` summary line is printed after the walk unless `--noreport` is set. Multi-directory args get a blank line between them.

- [ ] **Step 1: Add `human_size` helper above `print_entry`**

Find this line:
```c
static void print_entry(const struct entry *e,
```
Insert this function immediately BEFORE it:
```c
static void human_size(off_t bytes, char *buf, size_t bufsize) {
    const char units[] = "BKMGTP";
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && units[u + 1] != '\0') { v /= 1024.0; u++; }
    if (u == 0) {
        snprintf(buf, bufsize, "%lldB", (long long)bytes);
    } else if (v < 10.0) {
        snprintf(buf, bufsize, "%.1f%c", v, units[u]);
    } else {
        snprintf(buf, bufsize, "%.0f%c", v, units[u]);
    }
}

```

- [ ] **Step 2: Emit the size field in `print_entry`**

Find this block inside `print_entry`:
```c
    fputs(prefix, stdout);
    fputs(branch, stdout);

    const char *name = (o->full_paths && display_path) ? display_path : e->name;
    fputs(name, stdout);
```
Replace it with:
```c
    fputs(prefix, stdout);
    fputs(branch, stdout);

    if (o->show_size) {
        char sbuf[32];
        if (o->human_size) {
            char hb[16];
            human_size(e->st.st_size, hb, sizeof(hb));
            snprintf(sbuf, sizeof(sbuf), "[%10s]  ", hb);
        } else {
            snprintf(sbuf, sizeof(sbuf), "[%10lld]  ", (long long)e->st.st_size);
        }
        fputs(sbuf, stdout);
    }

    const char *name = (o->full_paths && display_path) ? display_path : e->name;
    fputs(name, stdout);
```

- [ ] **Step 3: Add summary line and multi-arg separation in `main`**

Find this block at the end of `main`:
```c
        fputs(root, stdout);
        fputc('\n', stdout);

        long dirs_n = 0, files_n = 0;
        walk(&o, root, root, "", 1, &dirs_n, &files_n);
    }
    return rc;
}
```
Replace it with:
```c
        fputs(root, stdout);
        fputc('\n', stdout);

        long dirs_n = 0, files_n = 0;
        walk(&o, root, root, "", 1, &dirs_n, &files_n);

        if (!o.no_report) {
            fputc('\n', stdout);
            printf("%ld director%s, %ld file%s\n",
                   dirs_n,  dirs_n  == 1 ? "y"  : "ies",
                   files_n, files_n == 1 ? ""   : "s");
        }
        if (k + 1 < ndirs) fputc('\n', stdout);
    }
    return rc;
}
```

- [ ] **Step 4: Rebuild and smoke-test**

Run:
```bash
cd solaris/tree && make clean && make
./tree -s .
./tree -h .
./tree --noreport .
./tree . /tmp
./tree -s -h .
```

Expected:
- `-s` shows `[       NNN]  name` on each line; summary at end.
- `-h` shows `[       1.2K]  name` style human sizes.
- `--noreport` omits the summary.
- Two-argument case prints both trees with a blank line between and two summaries.
- `-s -h` behaves as `-h` (human wins; the field is present).

---

## Task 5: README and final documentation

**Files:**
- Create: `solaris/tree/README.md`

**Goal:** Ship a README matching the `watch/` port's style so users know how to build and smoke-test.

- [ ] **Step 1: Create `solaris/tree/README.md` with exactly this content**

```markdown
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
```

- [ ] **Step 2: Walk the smoke-test checklist**

On a host with a compiler, run every entry in the smoke-test section manually. Each item must behave as described.

---

## Self-review (done inline before handoff)

- **Spec coverage:** All 10 flags (`-L`, `-a`, `-d`, `-f`, `-F`, `-s`, `-h`, `-I`, `-n`, `--noreport`), summary line, multi-arg handling, symlink arrows, UTF-8 / ASCII glyph selection, `fnmatch`-based excludes with overflow rejection, error messages for bad root / bad `-L` / missing `-I` arg — every spec requirement maps to a task.
- **Placeholder scan:** No "TBD", no "add error handling", no "similar to Task N". Every code step shows full code.
- **Type consistency:** `struct opts` has identical fields across Tasks 1 and 2. `struct entry`, `list_dir`, `walk`, `print_entry`, `human_size`, `is_utf8_locale` all use consistent signatures where they appear. Glyph pointer variables (`BR_MID` / `BR_LAST` / `BR_PASS` / `BR_SPACE`) are the only names used for branch output across all tasks.
