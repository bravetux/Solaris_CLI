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

static void print_entry(const struct entry *e,
                        const char *prefix,
                        const char *branch,
                        const char *display_path,
                        const struct opts *o) {
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

    int use_utf8 = (!o.ascii_only) && is_utf8_locale();
    BR_MID   = use_utf8 ? UTF8_MID   : ASCII_MID;
    BR_LAST  = use_utf8 ? UTF8_LAST  : ASCII_LAST;
    BR_PASS  = use_utf8 ? UTF8_PASS  : ASCII_PASS;
    BR_SPACE = use_utf8 ? UTF8_SPACE : ASCII_SPACE;

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
