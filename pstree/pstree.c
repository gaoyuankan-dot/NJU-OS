#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* ----------------------------------------------------------------------------
 *   1. SCAN   : walk /proc, snapshot every process into a `proc_t` node.
 *   2. LINK   : turn each node's ppid into a real parent->children edge,
 *               and discover the roots (pid 1).
 *   3. PRINT  : depth-first walk, using a running `prefix` string to draw the
 *               connector characters ( |-- , `-- , |   ) for each level.
 * ------------------------------------------------------------------------- */

typedef struct {
    pid_t pid;
    pid_t ppid;
    char  comm[256];

    int  *children;     // dynamic array of *indices* into the global nodes[]
    int   n_children;   // actual number
    int   cap_children; // capability number
} proc_t;

/* The whole snapshot lives here. Globals keep the qsort comparator simple. */
// static is good practice to program
static proc_t *g_nodes = NULL;
static int     g_count = 0;
static int     g_cap   = 0;

/* command-line flags */
static int opt_show_pids = 0;   // -p / --show-pids
static int opt_numeric   = 0;   // -n / --numeric-sort

/* ---------- /proc readers (kept from the original M2 code) --------------- */

static int read_comm(pid_t pid, char *buf, size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    buf[strcspn(buf, "\n")] = 0;   // strip trailing newline
    fclose(f);
    return 0;
}

static int get_ppid_from_stat(pid_t pid, pid_t *ppid_out) {
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);

    /* stat layout: pid (comm) state ppid ...
     * comm may contain spaces/parens, so we match up to the *last* ')'
     * by reading "(%255[^)])" — good enough for typical comm names. */
    int id, ppid;
    char comm[256], state;
    if (sscanf(line, "%d (%255[^)]) %c %d", &id, comm, &state, &ppid) != 4)
        return -1;
    *ppid_out = (pid_t)ppid;
    return 0;
}

/* ---------- phase 1: scan -------------------------------------------------- */

static int add_node(pid_t pid, pid_t ppid, const char *comm) {
    if (g_count == g_cap) {
        // Set the initial value to 2048
        g_cap = g_cap ? g_cap * 2 : 2048;
        g_nodes = realloc(g_nodes, (size_t)g_cap * sizeof(*g_nodes));
        if (!g_nodes) { perror("realloc"); exit(1); }
    }
    proc_t *p = &g_nodes[g_count];
    p->pid = pid;
    p->ppid = ppid;
    snprintf(p->comm, sizeof(p->comm), "%s", comm);
    p->children = NULL;
    p->n_children = p->cap_children = 0;
    return g_count++;
}

static void scan_proc(void) {
    DIR *d = opendir("/proc");
    if (!d) { perror("opendir /proc"); exit(1); }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;   // skip non-pid entries
        pid_t pid = (pid_t)atoi(de->d_name);

        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0) continue;      // vanished mid-scan

        char comm[256] = "?";
        read_comm(pid, comm, sizeof comm);

        add_node(pid, ppid, comm);
    }
    closedir(d);
}

/* ---------- phase 2: link parents -> children ----------------------------- */

/* Find the array index of a given pid, or -1. O(n) scan; fine for a few
 * thousand processes. Could be a hash map / sorted+bsearch if it mattered. */
static int index_of_pid(pid_t pid) {
    for (int i = 0; i < g_count; i++)
        if (g_nodes[i].pid == pid)
            return i;
    return -1;
}

static void add_child(int parent_idx, int child_idx) {
    proc_t *p = &g_nodes[parent_idx];
    if (p->n_children == p->cap_children) {
        p->cap_children = p->cap_children ? p->cap_children * 2 : 4;
        p->children = realloc(p->children,
                              (size_t)p->cap_children * sizeof(int));
        if (!p->children) { perror("realloc"); exit(1); }
    }
    p->children[p->n_children++] = child_idx;
}

/* Returns the number of roots found, and fills roots[] with their indices.
 * A node is a ROOT when its parent is not part of our snapshot:
 *   - ppid == 0           -> the kernel/idle "parent" (pid 1, pid 2 kthreadd)
 *   - parent pid missing  -> a true ORPHAN: its parent died between our
 *                            reading of /proc and now (a scan race), OR it was
 *                            reparented to something outside our view.
 * On Linux, a process whose real parent exits is re-parented by the kernel to
 * pid 1 (or the nearest subreaper), so it is rarely *truly* parentless — but
 * we still handle it defensively so the tree never loses a branch. */
static int build_tree(int *roots, int max_roots) {
    int n_roots = 0;
    for (int i = 0; i < g_count; i++) {
        int parent = (g_nodes[i].ppid == 0) ? -1
                                            : index_of_pid(g_nodes[i].ppid);
        if (parent < 0) {
            if (n_roots < max_roots) roots[n_roots++] = i;
        } else {
            add_child(parent, i);
        }
    }
    return n_roots;
}

/* ---------- sorting -------------------------------------------------------- */

/* Default: alphabetical by comm (ties broken by pid), like GNU pstree.
 * With -n : numeric by pid. Both operate on child *index* arrays. */
static int cmp_children(const void *a, const void *b) {
    const proc_t *x = &g_nodes[*(const int *)a];
    const proc_t *y = &g_nodes[*(const int *)b];
    if (opt_numeric) {
        return (x->pid > y->pid) - (x->pid < y->pid);
    }
    int c = strcmp(x->comm, y->comm);
    if (c) return c;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static void sort_all_children(void) {
    for (int i = 0; i < g_count; i++)
        if (g_nodes[i].n_children > 1)
            qsort(g_nodes[i].children, (size_t)g_nodes[i].n_children,
                  sizeof(int), cmp_children);
}

/* Roots are always ordered by pid (ascending) regardless of -n, so the forest
 * is deterministic and pid 1 (systemd) always prints before pid 2 (kthreadd)
 * and before any higher-pid orphan root. readdir() order is NOT guaranteed. */
static int cmp_roots(const void *a, const void *b) {
    pid_t x = g_nodes[*(const int *)a].pid;
    pid_t y = g_nodes[*(const int *)b].pid;
    return (x > y) - (x < y);
}

/* ---------- phase 3: draw -------------------------------------------------- */

static void print_label(int idx) {
    proc_t *p = &g_nodes[idx];
    if (opt_show_pids)
        printf("%s(%d)", p->comm, p->pid);
    else
        printf("%s", p->comm);

    if (p->pid == getpid())     // wink back at the original M2 "<== me"
        printf("  <== me");
    printf("\n");
}

/* `prefix` is everything that should be printed *before* this node's own
 * connector — i.e. the vertical bars / blanks inherited from ancestors. */
static void draw(int idx, const char *prefix, int is_last, int is_root) {
    if (is_root) {
        print_label(idx);                       // roots sit flush-left
    } else {
        printf("%s%s", prefix, is_last ? "+-- " : "|-- ");
        print_label(idx);
    }

    proc_t *p = &g_nodes[idx];
    if (p->n_children == 0) return;

    /* Extend the prefix for our children:
     *   - if WE were the last child, our subtree needs blank padding "    ".
     *   - otherwise a vertical bar "|   " keeps the sibling line connected. */
    char child_prefix[4096];
    if (is_root)
        child_prefix[0] = '\0';
    else
        snprintf(child_prefix, sizeof(child_prefix), "%s%s",
                 prefix, is_last ? "    " : "|   ");

    for (int k = 0; k < p->n_children; k++)
        draw(p->children[k], child_prefix,
             k == p->n_children - 1, /*is_root=*/0);
}

/* ---------- options & main ------------------------------------------------- */

static void usage(FILE *out) {
    fprintf(out,
        "Usage: pstree [-p|--show-pids] [-n|--numeric-sort] [-V|--version]\n");
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") || !strcmp(a, "--show-pids")) {
            opt_show_pids = 1;
        } else if (!strcmp(a, "-n") || !strcmp(a, "--numeric-sort")) {
            opt_numeric = 1;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            fprintf(stderr, "pstree (oslab M2) 1.0\n");
            exit(0);
        } else {
            fprintf(stderr, "pstree: invalid option -- '%s'\n", a);
            usage(stderr);
            exit(1);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    scan_proc();

    int roots[256];
    int n_roots = build_tree(roots, 256);

    qsort(roots, (size_t)n_roots, sizeof(int), cmp_roots);
    sort_all_children();

    /* Print pid-1 first if present, then the rest (kthreadd, orphans). */
    for (int r = 0; r < n_roots; r++)
        draw(roots[r], "", /*is_last=*/1, /*is_root=*/1);

    /* free */
    for (int i = 0; i < g_count; i++)
        free(g_nodes[i].children);
    free(g_nodes);
    return 0;
}
