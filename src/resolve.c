#define _GNU_SOURCE
#include "db.h"
#include "sha256.h"
#include "ucraft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Dependency resolution. Packages come from the repositories (the local index, filled by
 * 'kit sync') and, for names the index does not have, from packages already in the cache.
 * The index wins when both have a package. Everything that has to be downloaded is verified
 * against the sha256 in the index before it is used. */

static void pop(strv *s)
{
    if (!s->n) return;
    free(s->v[--s->n]);
    s->v[s->n] = NULL;
}

/* Makes sure the package file is in the cache and returns its path; fills `deps` with the names
 * of the packages it depends on. */
static char *get_package(const struct index *ix, const char *name, strv *deps)
{
    const struct ipkg *e = ix ? index_find(ix, name) : NULL;
    if (e) {
        char *path = xasprintf("%s/pkg/%s", cfg.cache, e->file);
        char hex[65];
        int have = path_exists(path) && sha256_file(path, hex) == 0 && !strcasecmp(hex, e->sha256);
        if (!have) {
            char *url = xasprintf("%s/%s/%s", e->repo, e->arch, e->file);
            info("downloading %s %s-%d (%.0f KB)", e->name, e->version, e->revision, (double)e->size / 1024.0);
            fetch_file(url, path, e->sha256);
            free(url);
        }
        for (size_t i = 0; i < e->deps.n; i++) sv_push(deps, e->deps.v[i]);
        return path;
    }
    char *path = resolve_package(name);
    struct pkginfo *pi = package_info(path);
    for (size_t i = 0; i < pi->depends.n; i++) {
        char *d = dep_name(pi->depends.v[i]);
        sv_push(deps, d);
        free(d);
    }
    pkginfo_free(pi);
    return path;
}

/* Depth-first over the dependencies: each package comes after the ones it needs. */
static void closure(const struct index *ix, const char *name, struct plan *p, strv *visiting)
{
    if (sv_has(&p->names, name)) return;
    if (sv_has(visiting, name)) die("dependency cycle involving '%s'", name);
    sv_push(visiting, name);
    strv deps = {0};
    char *file = get_package(ix, name, &deps);
    for (size_t i = 0; i < deps.n; i++) closure(ix, deps.v[i], p, visiting);
    sv_free(&deps);
    pop(visiting);
    sv_push(&p->names, name);
    sv_push(&p->files, file);
    free(file);
}

void plan_build(const strv *requested, int nodeps, struct plan *p)
{
    memset(p, 0, sizeof *p);
    struct index ix;
    int have = index_load(&ix) == 0;
    strv visiting = {0};
    for (size_t i = 0; i < requested->n; i++) {
        const char *name = requested->v[i];
        if (!nodeps) {
            closure(have ? &ix : NULL, name, p, &visiting);
        } else if (!sv_has(&p->names, name)) {
            strv deps = {0};
            char *file = get_package(have ? &ix : NULL, name, &deps);
            sv_push(&p->names, name);
            sv_push(&p->files, file);
            free(file);
            sv_free(&deps);
        }
    }
    sv_free(&visiting);
    if (have) index_free(&ix);
}

void plan_free(struct plan *p)
{
    sv_free(&p->names);
    sv_free(&p->files);
}

/* kit install <name>...: resolve, download what is missing, install dependencies first. */
void install_names(const strv *names, const struct install_opts *base)
{
    struct plan p;
    plan_build(names, base->nodeps, &p);
    size_t done = 0;
    for (size_t i = 0; i < p.names.n; i++) {
        struct pkginfo *pi = package_info(p.files.v[i]);
        if (db_has(cfg.root, pi->name)) {
            struct pkginfo *cur = db_info(cfg.root, pi->name);
            int same = !strcmp(cur->version, pi->version) && cur->revision == pi->revision;
            pkginfo_free(cur);
            if (same) {
                info("%s %s-%d is already installed", pi->name, pi->version, pi->revision);
                pkginfo_free(pi);
                continue;
            }
        }
        struct install_opts io = *base;
        io.asdep = base->asdep || !sv_has(names, pi->name);   /* dependencies are marked as such */
        install_package(p.files.v[i], &io);
        done++;
        pkginfo_free(pi);
    }
    if (!done) info("nothing to install");
    plan_free(&p);
}

/* kit upgrade: every installed package that has a newer version in the index. */
void upgrade_all(void)
{
    struct index ix;
    if (index_load(&ix) != 0) die("no package index yet: run 'kit sync' first");
    strv installed = {0}, todo = {0};
    db_list(cfg.root, &installed);
    for (size_t i = 0; i < installed.n; i++) {
        struct pkginfo *cur = db_info(cfg.root, installed.v[i]);
        const struct ipkg *e = index_find(&ix, installed.v[i]);
        if (e && version_newer(e->version, e->revision, cur->version, cur->revision)) {
            info("%s %s-%d -> %s-%d", cur->name, cur->version, cur->revision, e->version, e->revision);
            sv_push(&todo, cur->name);
        }
        pkginfo_free(cur);
    }
    index_free(&ix);
    if (!todo.n) {
        info("everything is up to date");
    } else {
        struct install_opts io = {0};
        install_names(&todo, &io);
    }
    sv_free(&installed);
    sv_free(&todo);
}
