#ifndef UCRAFT_RECIPE_H
#define UCRAFT_RECIPE_H
#include "util.h"

struct source { char *url, *sha256; };
struct step { char *phase, *action; strv args; };

struct recipe {
    char *name, *version, *description, *license, *style;
    int revision;
    strv depends, makedepends;
    struct source *sources; size_t nsources;
    strv build_args;      /* [build] args: for configure/cmake */
    strv make_args;       /* [build] make_args: for make */
    strv install_args;    /* [build] install_args: extra arguments for the install step */
    char *install_target; /* [build] install_target: make target (default "install") */
    strv patches;         /* [build] patches: files in the recipe folder, applied with -p1 after extraction */
    int skip_build;       /* [build] skip_build = true: run only the install step (e.g. kernel headers) */
    strv split;           /* split = ["dev", "doc"]: also write name-dev / name-doc packages */
    int allow_var;        /* allow_var = true: the package may ship /var (only the base skeleton package) */
    char *subdir;         /* [build] subdir: folder inside the source tree holding the build root (cmake) */
    struct step *steps; size_t nsteps;
    char *dir;            /* recipe folder (for patches) */
};

/* Loads and validates forge.toml; exits with a clear error if anything fails. */
struct recipe *recipe_load(const char *dir);
/* Looks for "name/forge.toml" in each folder of recipe_dirs; the last one wins. */
char *recipe_find(const strv *recipe_dirs, const char *name);
void recipe_print(const struct recipe *r);

#endif
