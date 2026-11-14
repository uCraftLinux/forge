#ifndef UCRAFT_H
#define UCRAFT_H
#include "pkg.h"
#include "recipe.h"

struct config {
    char *root;        /* target system (--root or $UCRAFT) */
    char *cache;       /* package manager cache: src/, pkg/, build/ (outside the rootfs) */
    strv recipes;      /* folders with recipes; the last one wins */
    strv repos;        /* package repositories (--repo); if empty, <root>/etc/ucraft/repos is used */
    int keep_build;
};
extern struct config cfg;

/* build.c: builds and returns the path of the .ucraft (free it) */
char *build_recipe(const struct recipe *r);

/* install.c */
struct install_opts {
    int asdep;
    int nodeps;
    strv overwrite;   /* absolute paths, or "*" */
};
void install_package(const char *ucraft_file, const struct install_opts *o);
void remove_package(const char *name, int force);
struct pkginfo *package_info(const char *ucraft_file);   /* needs pkg.h */
int check_package(const char *name);

/* main.c: newest .ucraft of a package name in the cache (die if none) */
char *resolve_package(const char *arg);

/* repo.c: repositories and the local package index */
struct ipkg {
    char *repo, *name, *version, *arch, *sha256, *file, *desc;
    int revision;
    unsigned long long size;
    strv deps;
};
struct index { struct ipkg *v; size_t n; };
int index_load(struct index *ix);                     /* 0 on success, -1 if there is no index yet */
void index_free(struct index *ix);
const struct ipkg *index_find(const struct index *ix, const char *name);
int version_newer(const char *v1, int r1, const char *v2, int r2);   /* 1 if v1-r1 is newer than v2-r2 */
void repo_sync(void);                                  /* kit sync */
void repo_search(const char *text);                    /* kit search */
void repo_build(const char *dir);                      /* kit repo build <dir> */

/* resolve.c: dependency resolution over the index and the cache */
struct plan { strv names, files; };                    /* names in install order; files in the cache */
void plan_build(const strv *requested, int nodeps, struct plan *p);
void plan_free(struct plan *p);
void install_names(const strv *names, const struct install_opts *base);
void upgrade_all(void);

/* fetch.c: get a URL (or local file) into dest, verifying the sha256 when one is given */
void fetch_file(const char *url, const char *dest, const char *sha256);

/* layout.c: the system template (directories and base /etc files). Creates what is missing
 * under root, never overwrites; returns the number of entries created. */
int layout_apply(const char *root);

/* image.c: builds a container image from a ucraft.toml */
void image_build(const char *spec_path);

#endif
