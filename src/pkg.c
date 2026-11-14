#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void flist_add(struct flist *l, char type, unsigned mode, const char *sha, const char *path, const char *link)
{
    if (strpbrk(path, "\t\n")) die("path with tab or newline: %s", path);
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    struct fent *e = &l->v[l->n++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->mode = mode;
    snprintf(e->sha, sizeof e->sha, "%s", sha ? sha : "-");
    e->path = xstrdup(path);
    e->link = link ? xstrdup(link) : NULL;
}

static int cmp_ent(const void *a, const void *b)
{
    return strcmp(((const struct fent *)a)->path, ((const struct fent *)b)->path);
}

void flist_sort(struct flist *l)
{
    qsort(l->v, l->n, sizeof *l->v, cmp_ent);
}

struct fent *flist_find(const struct flist *l, const char *path)
{
    struct fent key = {.path = (char *)path};
    return bsearch(&key, l->v, l->n, sizeof *l->v, cmp_ent);
}

char *flist_format(const struct flist *l)
{
    size_t cap = 4096, n = 0;
    char *out = xmalloc(cap);
    out[0] = 0;
    for (size_t i = 0; i < l->n; i++) {
        const struct fent *e = &l->v[i];
        char *line = xasprintf("%c\t%04o\t%s\t%s\t%s\n", e->type, e->mode, e->sha, e->path, e->link ? e->link : "-");
        size_t ll = strlen(line);
        while (n + ll + 1 > cap) { cap *= 2; out = xrealloc(out, cap); }
        memcpy(out + n, line, ll + 1);
        n += ll;
        free(line);
    }
    return out;
}

void flist_parse(struct flist *l, const char *text)
{
    char *copy = xstrdup(text), *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[5];
        char *p = line;
        for (int i = 0; i < 5; i++) {
            f[i] = p;
            if (i < 4) {
                p = strchr(p, '\t');
                if (!p) die("corrupt file list: '%s'", line);
                *p++ = 0;
            }
        }
        if (f[0][1] || !strchr("FDL", f[0][0])) die("file list: invalid type '%s'", f[0]);
        if (f[3][0] != '/') die("file list: path is not absolute '%s'", f[3]);
        if (strstr(f[3], "/../") || !strcmp(f[3] + strlen(f[3]) - 3, "/..")) die("file list: path with '..': %s", f[3]);
        flist_add(l, f[0][0], (unsigned)strtoul(f[1], NULL, 8), f[2], f[3], strcmp(f[4], "-") ? f[4] : NULL);
    }
    free(copy);
    flist_sort(l);
}

void flist_free(struct flist *l)
{
    for (size_t i = 0; i < l->n; i++) { free(l->v[i].path); free(l->v[i].link); }
    free(l->v);
    memset(l, 0, sizeof *l);
}

char *pkginfo_format(const struct pkginfo *p)
{
    char *s = xasprintf("name=%s\nversion=%s\nrevision=%d\narch=%s\nsize=%llu\ndescription=%s\nlicense=%s\n",
                        p->name, p->version, p->revision, p->arch, p->size,
                        p->description ? p->description : "", p->license ? p->license : "");
    for (size_t i = 0; i < p->depends.n; i++) {
        char *t = xasprintf("%sdepends=%s\n", s, p->depends.v[i]);
        free(s);
        s = t;
    }
    return s;
}

struct pkginfo *pkginfo_parse(const char *text)
{
    struct pkginfo *p = xcalloc(1, sizeof *p);
    p->revision = 1;
    char *copy = xstrdup(text), *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "name")) p->name = xstrdup(eq);
        else if (!strcmp(line, "version")) p->version = xstrdup(eq);
        else if (!strcmp(line, "revision")) p->revision = atoi(eq);
        else if (!strcmp(line, "arch")) p->arch = xstrdup(eq);
        else if (!strcmp(line, "size")) p->size = strtoull(eq, NULL, 10);
        else if (!strcmp(line, "description")) p->description = xstrdup(eq);
        else if (!strcmp(line, "license")) p->license = xstrdup(eq);
        else if (!strcmp(line, "depends")) sv_push(&p->depends, eq);
    }
    free(copy);
    if (!p->name || !p->version || !p->arch) die("incomplete package information (.INFO)");
    return p;
}

void pkginfo_free(struct pkginfo *p)
{
    if (!p) return;
    free(p->name); free(p->version); free(p->arch); free(p->description); free(p->license);
    sv_free(&p->depends);
    free(p);
}

char *dep_name(const char *dep)
{
    size_t n = strcspn(dep, "<>=");
    return strndup(dep, n);
}
