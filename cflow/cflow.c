/*
 * cflow - produce a call graph from C source files.
 * Solaris 8 port, from-scratch v1. Pattern-matching scanner, not a real parser.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

struct opts {
    int          max_depth;
    int          reverse;
    const char  *main_name;
    int          nfiles;
    const char **files;
};

struct func {
    char *name;           /* owned, never NULL */
    char *file;           /* owned; NULL if only seen as a callee */
    int   line;           /* 0 if only seen as a callee */
    int  *callees;        /* dynamic array of function ids */
    int   n_callees;
    int   cap_callees;
    int   is_defined;
};

struct symtab {
    struct func *f;
    int          n;
    int          cap;
};

static void die_oom(void) {
    fprintf(stderr, "cflow: out of memory\n");
    exit(1);
}

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) die_oom(); return p; }
static void *xrealloc(void *p, size_t n) { void *r = realloc(p, n); if (!r) die_oom(); return r; }
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)xmalloc(n);
    memcpy(r, s, n);
    return r;
}

static void usage(FILE *out) {
    fputs(
        "Usage: cflow [OPTIONS] FILE.c [FILE.c ...]\n"
        "  -d N          max display depth (>= 1)\n"
        "  -r            reverse graph (callees -> callers)\n"
        "  --main NAME   root output at NAME only\n",
        out);
}

static int parse_opts(int argc, char **argv, struct opts *o) {
    memset(o, 0, sizeof(*o));
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-r") == 0) { o->reverse = 1; continue; }
        if (strcmp(a, "-d") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cflow: -d requires an argument\n"); return -1; }
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 1 || v > 1000000) {
                fprintf(stderr, "cflow: invalid depth '%s'\n", argv[i]);
                return -1;
            }
            o->max_depth = (int)v;
            continue;
        }
        if (strcmp(a, "--main") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cflow: --main requires an argument\n"); return -1; }
            o->main_name = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "cflow: unknown option '%s'\n", a);
            return -1;
        }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "cflow: no input files\n");
        return -1;
    }
    o->files  = (const char **)(argv + i);
    o->nfiles = argc - i;
    return 0;
}

/* ---- symbol table ------------------------------------------------------- */

static int sym_find(const struct symtab *t, const char *name) {
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->f[i].name, name) == 0) return i;
    }
    return -1;
}

static int sym_intern(struct symtab *t, const char *name) {
    int id = sym_find(t, name);
    if (id >= 0) return id;
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        t->f = (struct func *)xrealloc(t->f, (size_t)nc * sizeof(*t->f));
        t->cap = nc;
    }
    struct func *f = &t->f[t->n];
    memset(f, 0, sizeof(*f));
    f->name = xstrdup(name);
    return t->n++;
}

/* Record a definition at name, file, line. Overwrites the first definition we
 * see for a given name (C allows redundant prototypes; we only care about the
 * first concrete body). */
static int sym_record_def(struct symtab *t, const char *name,
                          const char *file, int line) {
    int id = sym_intern(t, name);
    struct func *f = &t->f[id];
    if (!f->is_defined) {
        f->is_defined = 1;
        f->file = xstrdup(file);
        f->line = line;
    }
    return id;
}

/* Record a call from caller_id to callee-by-name. Dedup: skip if already present. */
static void sym_record_call(struct symtab *t, int caller_id, const char *callee_name) {
    int callee_id = sym_intern(t, callee_name);
    struct func *c = &t->f[caller_id];
    for (int i = 0; i < c->n_callees; i++) {
        if (c->callees[i] == callee_id) return;
    }
    if (c->n_callees == c->cap_callees) {
        int nc = c->cap_callees ? c->cap_callees * 2 : 8;
        c->callees = (int *)xrealloc(c->callees, (size_t)nc * sizeof(int));
        c->cap_callees = nc;
    }
    c->callees[c->n_callees++] = callee_id;
}

static void symtab_free(struct symtab *t) {
    for (int i = 0; i < t->n; i++) {
        free(t->f[i].name);
        free(t->f[i].file);
        free(t->f[i].callees);
    }
    free(t->f);
    t->f = NULL;
    t->n = t->cap = 0;
}

/* ---- scanner ------------------------------------------------------------ */

enum scan_state {
    ST_CODE,
    ST_STRING,
    ST_CHAR,
    ST_LINE_CMT,
    ST_BLOCK_CMT,
    ST_PREPROC
};

static int is_ident_start(int c) { return (isalpha(c) || c == '_'); }
static int is_ident_cont (int c) { return (isalnum(c) || c == '_'); }

/* Identifiers that may appear before '(' but are not function calls. */
static int is_keyword(const char *s) {
    static const char *kw[] = {
        "if", "while", "for", "switch", "return", "sizeof",
        "do", "else", "case", "default", "goto",
        "typeof", "_Alignof", "alignof", "_Generic", "offsetof",
        NULL
    };
    for (int i = 0; kw[i]; i++) {
        if (strcmp(s, kw[i]) == 0) return 1;
    }
    return 0;
}

/* Skip whitespace and single-line comments starting at buf[*i].
 * Advances *i past them. Does NOT cross block-comment boundaries (caller
 * handles those when we're in code). */
static void skip_ws_inline(const char *buf, size_t len, size_t *i, int *line_p) {
    while (*i < len) {
        char c = buf[*i];
        if (c == '\n') { (*line_p)++; (*i)++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') (*i)++;
        else break;
    }
}

/* True iff buf[i..] begins with a block-comment opener "/ *" (no space).
 * Returns the length to skip when called on the opener. */
static int starts_block_cmt(const char *buf, size_t len, size_t i) {
    return (i + 1 < len && buf[i] == '/' && buf[i + 1] == '*');
}
static int starts_line_cmt(const char *buf, size_t len, size_t i) {
    return (i + 1 < len && buf[i] == '/' && buf[i + 1] == '/');
}

static void scan_file(const char *buf, size_t len, const char *filename,
                      struct symtab *t) {
    enum scan_state state = ST_CODE;
    int brace_depth = 0;
    int paren_depth = 0;
    int line = 1;
    int at_line_start = 1;  /* for '#' preprocessor detection */

    int  cur_fn_id = -1;
    int  def_pending = 0;
    char pending_name[128] = {0};
    int  pending_line = 0;
    int  pending_paren_base = 0;

    char last_ident[128] = {0};
    int  last_ident_line = 0;
    int  have_last_ident = 0;

    size_t i = 0;
    while (i < len) {
        char c = buf[i];

        /* Universal line-tracking. */
        if (state == ST_CODE) {
            if (c == '\n') {
                line++;
                at_line_start = 1;
                i++;
                continue;
            }

            /* Preprocessor: '#' as first non-space on a line. */
            if (at_line_start && c == '#') {
                state = ST_PREPROC;
                at_line_start = 0;
                i++;
                continue;
            }
            if (!isspace((unsigned char)c)) at_line_start = 0;

            /* Line comment. */
            if (starts_line_cmt(buf, len, i)) {
                state = ST_LINE_CMT;
                i += 2;
                continue;
            }
            /* Block comment. */
            if (starts_block_cmt(buf, len, i)) {
                state = ST_BLOCK_CMT;
                i += 2;
                continue;
            }
            /* String / char literal. */
            if (c == '"') { state = ST_STRING; i++; continue; }
            if (c == '\'') { state = ST_CHAR;  i++; continue; }

            /* Identifier. */
            if (is_ident_start((unsigned char)c)) {
                size_t start = i;
                while (i < len && is_ident_cont((unsigned char)buf[i])) i++;
                size_t ilen = i - start;
                if (ilen >= sizeof(last_ident)) ilen = sizeof(last_ident) - 1;
                memcpy(last_ident, buf + start, ilen);
                last_ident[ilen] = '\0';
                last_ident_line = line;
                have_last_ident = 1;

                /* Peek for '(' after optional whitespace and comments. */
                size_t j = i;
                while (j < len) {
                    char cj = buf[j];
                    if (cj == ' ' || cj == '\t' || cj == '\r' || cj == '\v' || cj == '\f') { j++; continue; }
                    if (cj == '\n') { j++; continue; }
                    if (starts_line_cmt(buf, len, j) || starts_block_cmt(buf, len, j)) {
                        /* Skip the comment conservatively — walk byte-by-byte. */
                        if (starts_line_cmt(buf, len, j)) {
                            j += 2;
                            while (j < len && buf[j] != '\n') j++;
                        } else {
                            j += 2;
                            while (j + 1 < len && !(buf[j] == '*' && buf[j+1] == '/')) j++;
                            if (j + 1 < len) j += 2;
                        }
                        continue;
                    }
                    break;
                }

                if (j < len && buf[j] == '(') {
                    if (brace_depth == 0) {
                        /* Possible function definition: remember for later. */
                        if (!is_keyword(last_ident)) {
                            strncpy(pending_name, last_ident, sizeof(pending_name) - 1);
                            pending_name[sizeof(pending_name) - 1] = '\0';
                            pending_line = last_ident_line;
                            def_pending = 1;
                            pending_paren_base = paren_depth;
                        }
                    } else if (cur_fn_id >= 0 && !is_keyword(last_ident)) {
                        /* Function call inside a body. */
                        sym_record_call(t, cur_fn_id, last_ident);
                    }
                }
                continue;
            }

            /* Braces and parens. */
            if (c == '{') {
                if (brace_depth == 0 && def_pending) {
                    cur_fn_id = sym_record_def(t, pending_name, filename, pending_line);
                    def_pending = 0;
                }
                brace_depth++;
                i++;
                continue;
            }
            if (c == '}') {
                if (brace_depth > 0) brace_depth--;
                if (brace_depth == 0) {
                    cur_fn_id = -1;
                    def_pending = 0;
                }
                i++;
                continue;
            }
            if (c == '(') { paren_depth++; i++; continue; }
            if (c == ')') {
                if (paren_depth > 0) paren_depth--;
                /* If our pending-def parens just closed, keep def_pending set —
                 * it only fires on the following '{'. If we instead see a
                 * semicolon or anything non-'{', the '}' / ';' handler below
                 * will clear it. */
                i++;
                continue;
            }
            if (c == ';') {
                if (brace_depth == 0) def_pending = 0;
                i++;
                continue;
            }

            /* Anything else — just advance. */
            (void)have_last_ident;
            i++;
            continue;
        }

        if (state == ST_STRING) {
            if (c == '\\' && i + 1 < len) { i += 2; continue; }
            if (c == '\n') { line++; }
            if (c == '"')  { state = ST_CODE; i++; continue; }
            i++;
            continue;
        }

        if (state == ST_CHAR) {
            if (c == '\\' && i + 1 < len) { i += 2; continue; }
            if (c == '\n') { line++; }
            if (c == '\'') { state = ST_CODE; i++; continue; }
            i++;
            continue;
        }

        if (state == ST_LINE_CMT) {
            if (c == '\n') { line++; at_line_start = 1; state = ST_CODE; i++; continue; }
            i++;
            continue;
        }

        if (state == ST_BLOCK_CMT) {
            if (c == '\n') line++;
            if (c == '*' && i + 1 < len && buf[i + 1] == '/') {
                state = ST_CODE;
                i += 2;
                continue;
            }
            i++;
            continue;
        }

        if (state == ST_PREPROC) {
            /* End at newline unless preceded by backslash (line continuation). */
            if (c == '\n') {
                line++;
                int backslash = (i > 0 && buf[i - 1] == '\\');
                if (!backslash) {
                    state = ST_CODE;
                    at_line_start = 1;
                }
                i++;
                continue;
            }
            i++;
            continue;
        }
    }

    /* EOF cleanup: if we're still inside a block comment, warn (non-fatal). */
    if (state == ST_BLOCK_CMT) {
        fprintf(stderr, "cflow: %s: unterminated block comment\n", filename);
    }
}

/* ---- file loading ------------------------------------------------------- */

static int load_file(const char *path, char **out, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cflow: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "cflow: %s: %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fprintf(stderr, "cflow: %s: %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    rewind(fp);
    char *buf = (char *)xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    buf[got] = '\0';
    fclose(fp);
    *out = buf;
    *out_len = got;
    return 0;
}

static int process_file(const char *path, struct symtab *t) {
    char *buf = NULL;
    size_t len = 0;
    if (load_file(path, &buf, &len) != 0) return -1;
    scan_file(buf, len, path, t);
    free(buf);
    return 0;
}

/* ---- graph ops + printer ----------------------------------------------- */

/* Build the reverse adjacency: callers[i] = list of function ids that call i. */
struct reverse_graph {
    int **callers;       /* callers[i] -> int array */
    int  *n_callers;
    int  *cap_callers;
    int   n;
};

static void rg_init(struct reverse_graph *r, int n) {
    r->n = n;
    r->callers    = (int **)xmalloc((size_t)n * sizeof(int *));
    r->n_callers  = (int  *)xmalloc((size_t)n * sizeof(int));
    r->cap_callers= (int  *)xmalloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) {
        r->callers[i]   = NULL;
        r->n_callers[i] = 0;
        r->cap_callers[i] = 0;
    }
}

static void rg_add(struct reverse_graph *r, int callee, int caller) {
    for (int i = 0; i < r->n_callers[callee]; i++) {
        if (r->callers[callee][i] == caller) return;
    }
    if (r->n_callers[callee] == r->cap_callers[callee]) {
        int nc = r->cap_callers[callee] ? r->cap_callers[callee] * 2 : 4;
        r->callers[callee] = (int *)xrealloc(r->callers[callee], (size_t)nc * sizeof(int));
        r->cap_callers[callee] = nc;
    }
    r->callers[callee][r->n_callers[callee]++] = caller;
}

static void rg_build(struct reverse_graph *r, const struct symtab *t) {
    for (int i = 0; i < t->n; i++) {
        const struct func *f = &t->f[i];
        for (int j = 0; j < f->n_callees; j++) {
            rg_add(r, f->callees[j], i);
        }
    }
}

static void rg_free(struct reverse_graph *r) {
    for (int i = 0; i < r->n; i++) free(r->callers[i]);
    free(r->callers);
    free(r->n_callers);
    free(r->cap_callers);
    memset(r, 0, sizeof(*r));
}

/* Generic tree-printer. `children(id, &n)` returns a pointer to the child list
 * and its count. */
struct children_view {
    const int *ids;
    int        n;
};
static struct children_view forward_children(const struct symtab *t, int id) {
    struct children_view v;
    v.ids = t->f[id].callees;
    v.n   = t->f[id].n_callees;
    return v;
}
static struct children_view reverse_children(const struct reverse_graph *r, int id) {
    struct children_view v;
    v.ids = r->callers[id];
    v.n   = r->n_callers[id];
    return v;
}

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) fputs("    ", stdout);
}

static void print_node(const struct symtab *t, int id) {
    const struct func *f = &t->f[id];
    fputs(f->name, stdout);
    fputs("()", stdout);
    if (f->is_defined) {
        printf(" <%s:%d>", f->file, f->line);
    }
}

/* Recursive walk. `ancestors` is a stack of function ids for cycle detection. */
static void walk_tree(const struct symtab *t,
                      const struct reverse_graph *rg,
                      int reverse_mode,
                      int id,
                      int depth,
                      int max_depth,
                      int *ancestors,
                      int n_ancestors) {
    print_indent(depth);
    print_node(t, id);

    /* Cycle? */
    for (int i = 0; i < n_ancestors; i++) {
        if (ancestors[i] == id) {
            fputs(" {recursive}\n", stdout);
            return;
        }
    }
    fputc('\n', stdout);

    if (max_depth > 0 && depth + 1 >= max_depth) return;

    struct children_view cv = reverse_mode
        ? reverse_children(rg, id)
        : forward_children(t, id);

    ancestors[n_ancestors] = id;
    for (int i = 0; i < cv.n; i++) {
        walk_tree(t, rg, reverse_mode, cv.ids[i], depth + 1, max_depth,
                  ancestors, n_ancestors + 1);
    }
}

/* Determine root ids (caller with no callers in forward mode; leaf in reverse). */
static int *compute_roots(const struct symtab *t,
                          const struct reverse_graph *rg,
                          int reverse_mode,
                          int *out_n) {
    int  n  = t->n;
    int *rs = (int *)xmalloc((size_t)n * sizeof(int));
    int  k  = 0;

    for (int i = 0; i < n; i++) {
        int deg;
        if (!reverse_mode) {
            /* Forward: a root has no incoming callers. */
            deg = rg->n_callers[i];
        } else {
            /* Reverse: a "root" (start of display) is a function with no outgoing
             * forward edges — i.e., a leaf in the forward graph. These are what
             * a user typically asks for when running `cflow -r`: "what calls X?". */
            deg = t->f[i].n_callees;
        }
        if (deg == 0) rs[k++] = i;
    }
    *out_n = k;
    return rs;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    struct opts o;
    if (parse_opts(argc, argv, &o) != 0) { usage(stderr); return 1; }

    struct symtab t;
    memset(&t, 0, sizeof(t));

    int rc = 0;
    for (int i = 0; i < o.nfiles; i++) {
        if (process_file(o.files[i], &t) != 0) rc = 1;
    }

    if (t.n == 0) { symtab_free(&t); return rc; }

    struct reverse_graph rg;
    rg_init(&rg, t.n);
    rg_build(&rg, &t);

    int  n_roots = 0;
    int *roots   = NULL;

    if (o.main_name) {
        int id = sym_find(&t, o.main_name);
        if (id < 0) {
            fprintf(stderr, "cflow: function '%s' not found\n", o.main_name);
            rc = 1;
        } else {
            roots = (int *)xmalloc(sizeof(int));
            roots[0] = id;
            n_roots = 1;
        }
    } else {
        roots = compute_roots(&t, &rg, o.reverse, &n_roots);
        if (n_roots == 0 && t.n > 0) {
            /* Every function participates in a cycle; fall back to first defined one. */
            for (int i = 0; i < t.n; i++) {
                if (t.f[i].is_defined) {
                    roots = (int *)xrealloc(roots, sizeof(int));
                    roots[0] = i;
                    n_roots = 1;
                    break;
                }
            }
        }
    }

    int *ancestors = (int *)xmalloc((size_t)t.n * sizeof(int));
    for (int i = 0; i < n_roots; i++) {
        walk_tree(&t, &rg, o.reverse, roots[i], 0, o.max_depth, ancestors, 0);
        if (i + 1 < n_roots) fputc('\n', stdout);
    }
    free(ancestors);
    free(roots);
    rg_free(&rg);

    symtab_free(&t);
    return rc;
}
