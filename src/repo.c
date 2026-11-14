#define _GNU_SOURCE
#include "db.h"
#include "sha256.h"
#include "ucraft.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* A repository is a folder served over HTTP (or just a local folder):
 *
 *   index                      text file, see below
 *   x86_64/<name>-<ver>_<rev>-x86_64.ucraft
 *
 * First line of the index: "ucraft-index 1". Then one line per package, newest version only:
 *   name TAB version TAB revision TAB arch TAB size TAB sha256 TAB depends TAB file TAB description
 * `depends` is a comma-separated list of names, or "-". The client merges the indexes of all its
 * repositories into <cache>/index, adding the repository URL as a first column.
 */

#define INDEX_MAGIC "ucraft-index 1"

/* ---------- versions ---------- */

/* 1 if v1-r1 is newer than v2-r2 */
int version_newer(const char *v1, int r1, const char *v2, int r2)
{
    int c = strverscmp(v1, v2);
    if (c) return c > 0;
    return r1 > r2;
}

/* ---------- small helpers ---------- */

static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    return n;
}

static void append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 4096;
        *buf = xrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

static int valid_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '+' && *s != '.') return 0;
    return 1;
}

static int valid_file(const char *s)
{
    size_t n = strlen(s);
    return n > 8 && !strcmp(s + n - 7, ".ucraft") && !strchr(s, '/') && !strstr(s, "..") && !strchr(s, '\t');
}

static int valid_sha(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

static char *trim_url(const char *u)
{
    char *s = xstrdup(u);
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = 0;
    return s;
}

/* ---------- the local (merged) index ---------- */

static char *index_path(void) { return xasprintf("%s/index", cfg.cache); }

int index_load(struct index *ix)
{
    memset(ix, 0, sizeof *ix);
    char *p = index_path();
    char *text = read_file(p, NULL);
    free(p);
    if (!text) return -1;
    char *save = NULL;
    char *line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, INDEX_MAGIC)) die("the local index is damaged: run 'kit sync' again");
    size_t cap = 0;
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *f[10];
        if (split_tabs(line, f, 10) != 10) continue;
        if (ix->n == cap) {
            cap = cap ? cap * 2 : 64;
            ix->v = xrealloc(ix->v, cap * sizeof *ix->v);
        }
        struct ipkg *e = &ix->v[ix->n++];
        memset(e, 0, sizeof *e);
        e->repo = xstrdup(f[0]);
        e->name = xstrdup(f[1]);
        e->version = xstrdup(f[2]);
        e->revision = atoi(f[3]);
        e->arch = xstrdup(f[4]);
        e->size = strtoull(f[5], NULL, 10);
        e->sha256 = xstrdup(f[6]);
        if (strcmp(f[7], "-")) {
            char *ds = xstrdup(f[7]), *sv = NULL;
            for (char *d = strtok_r(ds, ",", &sv); d; d = strtok_r(NULL, ",", &sv)) sv_push(&e->deps, d);
            free(ds);
        }
        e->file = xstrdup(f[8]);
        e->desc = xstrdup(f[9]);
    }
    free(text);
    return 0;
}

void index_free(struct index *ix)
{
    for (size_t i = 0; i < ix->n; i++) {
        struct ipkg *e = &ix->v[i];
        free(e->repo); free(e->name); free(e->version); free(e->arch); free(e->sha256); free(e->file); free(e->desc);
        sv_free(&e->deps);
    }
    free(ix->v);
    memset(ix, 0, sizeof *ix);
}

const struct ipkg *index_find(const struct index *ix, const char *name)
{
    for (size_t i = 0; i < ix->n; i++)
        if (!strcmp(ix->v[i].name, name) && !strcmp(ix->v[i].arch, "x86_64")) return &ix->v[i];
    return NULL;
}

/* ---------- repository list ---------- */

static void repo_urls(strv *out)
{
    if (cfg.repos.n) {
        for (size_t i = 0; i < cfg.repos.n; i++) {
            char *u = trim_url(cfg.repos.v[i]);
            sv_push(out, u);
            free(u);
        }
        return;
    }
    if (!cfg.root) die("no repositories: use --repo URL, or list them in <root>/etc/ucraft/repos");
    char *path = xasprintf("%s/etc/ucraft/repos", cfg.root);
    char *text = read_file(path, NULL);
    if (!text) die("no repositories: use --repo URL, or list them in %s", path);
    free(path);
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (isspace((unsigned char)*line)) line++;
        size_t n = strlen(line);
        while (n && isspace((unsigned char)line[n - 1])) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *u = trim_url(line);
        sv_push(out, u);
        free(u);
    }
    free(text);
}

/* ---------- kit sync ---------- */

void repo_sync(void)
{
    strv urls = {0};
    repo_urls(&urls);
    if (!urls.n) die("no repositories are configured (see <root>/etc/ucraft/repos)");
    mkdir_p(cfg.cache, 0755);

    char *merged = NULL;
    size_t len = 0, cap = 0, count = 0;
    append(&merged, &len, &cap, INDEX_MAGIC "\n");
    strv seen = {0};
    for (size_t i = 0; i < urls.n; i++) {
        char *iurl = xasprintf("%s/index", urls.v[i]);
        char *tmp = xasprintf("%s/index.%zu.part", cfg.cache, i);
        info("syncing %s", urls.v[i]);
        fetch_file(iurl, tmp, NULL);
        char *text = read_file(tmp, NULL);
        unlink(tmp);
        if (!text) die("cannot read the index of %s", urls.v[i]);
        char *save = NULL;
        char *line = strtok_r(text, "\n", &save);
        if (!line || strcmp(line, INDEX_MAGIC)) die("%s is not a ucraft repository (bad index header)", urls.v[i]);
        size_t from_here = 0;
        while ((line = strtok_r(NULL, "\n", &save))) {
            char copy[8192];
            snprintf(copy, sizeof copy, "%s", line);
            char *f[9];
            if (split_tabs(copy, f, 9) != 9 || !valid_name(f[0]) || !valid_file(f[7]) || !valid_sha(f[5])) {
                warn("skipping a malformed index line in %s", urls.v[i]);
                continue;
            }
            if (sv_has(&seen, f[0])) continue;   /* an earlier repository has it: it wins */
            sv_push(&seen, f[0]);
            append(&merged, &len, &cap, urls.v[i]);
            append(&merged, &len, &cap, "\t");
            append(&merged, &len, &cap, line);
            append(&merged, &len, &cap, "\n");
            from_here++;
        }
        info("  %zu packages", from_here);
        count += from_here;
        free(text); free(iurl); free(tmp);
    }
    char *p = index_path();
    write_file_atomic(p, merged, len);
    free(p);
    info("index: %zu packages from %zu repositories", count, urls.n);
    free(merged);
    sv_free(&seen);
    sv_free(&urls);
}

/* ---------- kit search ---------- */

void repo_search(const char *text)
{
    struct index ix;
    if (index_load(&ix) != 0) die("no package index yet: run 'kit sync' first");
    size_t shown = 0;
    for (size_t i = 0; i < ix.n; i++) {
        const struct ipkg *e = &ix.v[i];
        if (text && !strcasestr(e->name, text) && !strcasestr(e->desc, text)) continue;
        int installed = cfg.root && db_has(cfg.root, e->name);
        printf("%-24s %s-%d%s  %s\n", e->name, e->version, e->revision, installed ? " [installed]" : "", e->desc);
        shown++;
    }
    if (!shown) printf("no packages match '%s'\n", text ? text : "");
    index_free(&ix);
}

/* ---------- kit repo build ---------- */

struct best {
    char *name, *path, *version, *arch, *desc, *deps;
    int revision;
};

static int cmp_best(const void *a, const void *b)
{
    return strcmp(((const struct best *)a)->name, ((const struct best *)b)->name);
}

static void clean_text(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

void repo_build(const char *dir)
{
    char *pkgdir = xasprintf("%s/pkg", cfg.cache);
    DIR *d = opendir(pkgdir);
    if (!d) die("no packages in %s: build some first with 'kit build'", pkgdir);

    struct best *bests = NULL;
    size_t nb = 0, cb = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 8 || strcmp(e->d_name + n - 7, ".ucraft")) continue;
        char *path = xasprintf("%s/%s", pkgdir, e->d_name);
        struct pkginfo *pi = package_info(path);
        size_t k;
        for (k = 0; k < nb; k++)
            if (!strcmp(bests[k].name, pi->name) && !strcmp(bests[k].arch, pi->arch)) break;
        if (k < nb && !version_newer(pi->version, pi->revision, bests[k].version, bests[k].revision)) {
            pkginfo_free(pi);
            free(path);
            continue;
        }
        if (k == nb) {
            if (nb == cb) { cb = cb ? cb * 2 : 32; bests = xrealloc(bests, cb * sizeof *bests); }
            nb++;
        } else {
            free(bests[k].name); free(bests[k].path); free(bests[k].version); free(bests[k].arch);
            free(bests[k].desc); free(bests[k].deps);
        }
        struct best *b = &bests[k];
        b->name = xstrdup(pi->name);
        b->path = path;
        b->version = xstrdup(pi->version);
        b->arch = xstrdup(pi->arch);
        b->desc = xstrdup(pi->description ? pi->description : "");
        clean_text(b->desc);
        b->revision = pi->revision;
        char *deps = xstrdup("");
        for (size_t i = 0; i < pi->depends.n; i++) {
            char *dn = dep_name(pi->depends.v[i]);
            char *t = xasprintf("%s%s%s", deps, i ? "," : "", dn);
            free(deps); free(dn);
            deps = t;
        }
        b->deps = *deps ? deps : (free(deps), xstrdup("-"));
        pkginfo_free(pi);
    }
    closedir(d);
    if (!nb) die("no .ucraft packages in %s", pkgdir);
    qsort(bests, nb, sizeof *bests, cmp_best);

    mkdir_p(dir, 0755);
    char *text = NULL;
    size_t len = 0, cap = 0;
    append(&text, &len, &cap, INDEX_MAGIC "\n");
    strv keep = {0};
    for (size_t i = 0; i < nb; i++) {
        struct best *b = &bests[i];
        const char *base = strrchr(b->path, '/') + 1;
        char *adir = xasprintf("%s/%s", dir, b->arch);
        mkdir_p(adir, 0755);
        char *dest = xasprintf("%s/%s", adir, base);
        char hex[65];
        if (sha256_file(b->path, hex) != 0) die("cannot read %s", b->path);
        char have[65];
        if (!path_exists(dest) || sha256_file(dest, have) != 0 || strcmp(have, hex)) {
            char *tmp = xasprintf("%s.part", dest);
            if (copy_file(b->path, tmp) != 0) die("cannot copy %s: %s", b->path, strerror(errno));
            if (rename(tmp, dest) != 0) die("rename %s: %s", dest, strerror(errno));
            free(tmp);
        }
        struct stat st;
        if (stat(dest, &st) != 0) die("stat %s: %s", dest, strerror(errno));
        char *line = xasprintf("%s\t%s\t%d\t%s\t%llu\t%s\t%s\t%s\t%s\n", b->name, b->version, b->revision, b->arch,
                               (unsigned long long)st.st_size, hex, b->deps, base, b->desc);
        append(&text, &len, &cap, line);
        sv_push(&keep, dest);
        free(line); free(adir); free(dest);
    }

    /* rolling repository: remove packages that are no longer the newest */
    size_t removed = 0;
    char *adir = xasprintf("%s/x86_64", dir);
    DIR *ad = opendir(adir);
    while (ad && (e = readdir(ad))) {
        size_t n = strlen(e->d_name);
        if (n < 8 || strcmp(e->d_name + n - 7, ".ucraft")) continue;
        char *full = xasprintf("%s/%s", adir, e->d_name);
        if (!sv_has(&keep, full)) { unlink(full); removed++; }
        free(full);
    }
    if (ad) closedir(ad);

    char *ip = xasprintf("%s/index", dir);
    write_file_atomic(ip, text, len);
    info("repository %s: %zu packages%s", dir, nb, removed ? " (older versions removed)" : "");
    free(ip); free(adir); free(text); free(pkgdir);
    sv_free(&keep);
    for (size_t i = 0; i < nb; i++) {
        free(bests[i].name); free(bests[i].path); free(bests[i].version); free(bests[i].arch);
        free(bests[i].desc); free(bests[i].deps);
    }
    free(bests);
}
