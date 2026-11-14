#include "db.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *db_root(const char *root)
{
    return xasprintf("%s/usr/lib/pkg/db", root);
}

char *db_path(const char *root, const char *name)
{
    return xasprintf("%s/usr/lib/pkg/db/%s", root, name);
}

int db_has(const char *root, const char *name)
{
    char *p = db_path(root, name);
    int r = is_dir(p);
    free(p);
    return r;
}

static char *slurp(const char *root, const char *name, const char *file)
{
    char *p = xasprintf("%s/usr/lib/pkg/db/%s/%s", root, name, file);
    char *s = read_file(p, NULL);
    if (!s) die("database damaged: cannot read %s", p);
    free(p);
    return s;
}

struct pkginfo *db_info(const char *root, const char *name)
{
    if (!db_has(root, name)) die("package '%s' is not installed", name);
    char *s = slurp(root, name, "info");
    struct pkginfo *p = pkginfo_parse(s);
    free(s);
    return p;
}

void db_files(const char *root, const char *name, struct flist *out)
{
    char *s = slurp(root, name, "files");
    flist_parse(out, s);
    free(s);
}

char *db_reason(const char *root, const char *name)
{
    char *s = slurp(root, name, "reason");
    s[strcspn(s, "\n")] = 0;
    return s;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void db_list(const char *root, strv *names)
{
    char *d = db_root(root);
    DIR *dir = opendir(d);
    free(d);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        sv_push(names, e->d_name);
    }
    closedir(dir);
    qsort(names->v, names->n, sizeof *names->v, cmp_str);
}

void db_write(const char *root, const char *name, const char *info, const char *files, const char *reason)
{
    char *base = db_root(root);
    mkdir_p(base, 0755);
    char *tmp = xasprintf("%s/.%s.new", base, name);
    char *old = xasprintf("%s/.%s.old", base, name);
    char *fin = xasprintf("%s/%s", base, name);
    rm_rf(tmp);
    rm_rf(old);
    mkdir_p(tmp, 0755);
    const char *fn[3] = {"info", "files", "reason"};
    const char *data[3] = {info, files, reason};
    for (int i = 0; i < 3; i++) {
        char *p = xasprintf("%s/%s", tmp, fn[i]);
        write_file_atomic(p, data[i], strlen(data[i]));
        free(p);
    }
    if (path_exists(fin) && rename(fin, old) != 0) die("db: rename %s: %s", fin, strerror(errno));
    if (rename(tmp, fin) != 0) die("db: rename %s: %s", tmp, strerror(errno));
    rm_rf(old);
    free(base); free(tmp); free(old); free(fin);
}

void db_remove(const char *root, const char *name)
{
    char *p = db_path(root, name);
    if (rm_rf(p) != 0) die("could not delete %s", p);
    free(p);
}

static int cmp_owner(const void *a, const void *b)
{
    return strcmp(((const struct owner *)a)->path, ((const struct owner *)b)->path);
}

void owners_load(const char *root, struct owners *o)
{
    memset(o, 0, sizeof *o);
    strv names = {0};
    db_list(root, &names);
    size_t cap = 0;
    for (size_t i = 0; i < names.n; i++) {
        struct flist fl = {0};
        db_files(root, names.v[i], &fl);
        for (size_t k = 0; k < fl.n; k++) {
            if (fl.v[k].type == 'D') continue;
            if (o->n == cap) {
                cap = cap ? cap * 2 : 256;
                o->v = xrealloc(o->v, cap * sizeof *o->v);
            }
            o->v[o->n].path = xstrdup(fl.v[k].path);
            o->v[o->n].pkg = xstrdup(names.v[i]);
            o->n++;
        }
        flist_free(&fl);
    }
    sv_free(&names);
    qsort(o->v, o->n, sizeof *o->v, cmp_owner);
}

const char *owners_find(const struct owners *o, const char *path)
{
    struct owner key = {.path = (char *)path};
    struct owner *r = bsearch(&key, o->v, o->n, sizeof *o->v, cmp_owner);
    return r ? r->pkg : NULL;
}

void owners_free(struct owners *o)
{
    for (size_t i = 0; i < o->n; i++) { free(o->v[i].path); free(o->v[i].pkg); }
    free(o->v);
    memset(o, 0, sizeof *o);
}
