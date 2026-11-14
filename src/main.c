#define _GNU_SOURCE
#include "db.h"
#include "ucraft.h"
#include "sha256.h"
#include <dirent.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct config cfg;

static void usage(FILE *o)
{
    fputs(
"usage: kit [global options] <command> [arguments]\n"
"\n"
"Commands:\n"
"  build <recipe|folder>       build the .ucraft package and leave it in the cache\n"
"  install [options] <file.ucraft | name>...   (names are resolved with their dependencies)\n"
"        --asdep                mark the package as a dependency\n"
"        --nodeps               do not require dependencies\n"
"        --overwrite <path|*>   allow overwriting files owned by nobody or others\n"
"  remove [--force] <package>  remove a package\n"
"  info <recipe|folder>        show a recipe\n"
"  list                         installed packages\n"
"  files <package>             files of an installed package\n"
"  owns <path>                 which package owns a file\n"
"  check <package>             modified or missing files\n"
"  hash <file>                 sha256 (for writing recipes)\n"
"  image build [ucraft.toml]   build a container image from packages\n"
"  init                         create the system layout and base /etc files in the root\n"
"  sync                         download the package index of every repository\n"
"  search [text]                search the index by name or description\n"
"  upgrade                      upgrade installed packages to the newest in the index\n"
"  repo build <dir>             make a repository folder from the packages in the cache\n"
"\n"
"Global options:\n"
"  --root DIR       target system (default $UCRAFT; nothing is installed without it)\n"
"  --cache DIR      package manager cache (default $UCRAFT_PROJECT/cache or ~/.cache/ucraft)\n"
"  --forge DIR      recipe folder, repeatable (default $UCRAFT_FORGE or $UCRAFT_PROJECT/forge)\n"
"  --repo URL       package repository, repeatable (default: <root>/etc/ucraft/repos)\n"
"  --keep-build     do not delete the build folder\n"
"  -h, --help\n", o);
}

/* True when this machine is a ucraft system (a container or a VM): ucraft may then act on / */
static int running_in_ucraft(void)
{
    static const char *const files[] = {"/etc/os-release", "/usr/lib/os-release", NULL};
    for (int i = 0; files[i]; i++) {
        char *txt = read_file(files[i], NULL);
        if (!txt) continue;
        int found = 0;
        for (char *l = txt; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
            if (!strncmp(l, "ID=ucraft", 9) && (l[9] == '\n' || l[9] == 0)) { found = 1; break; }
        free(txt);
        if (found) return 1;
    }
    return 0;
}

static void need_root(void)
{
    if (!cfg.root)
        die("don't know which system to act on: use --root DIR or load ucraft-env.sh (it sets $UCRAFT)");
    if (!is_dir(cfg.root)) die("--root %s is not a directory", cfg.root);
}

static struct recipe *load_recipe_arg(const char *arg)
{
    char *toml = xasprintf("%s/forge.toml", arg);
    int is_path = strchr(arg, '/') && access(toml, R_OK) == 0;
    free(toml);
    if (is_path) return recipe_load(arg);
    char *dir = recipe_find(&cfg.recipes, arg);
    if (!dir) die("recipe '%s' not found in the forge folders", arg);
    struct recipe *r = recipe_load(dir);
    free(dir);
    return r;
}

/* name -> the newest .ucraft in the cache */
char *resolve_package(const char *arg)
{
    if (strchr(arg, '/') || strstr(arg, ".ucraft")) return xstrdup(arg);
    char *dir = xasprintf("%s/pkg", cfg.cache);
    DIR *d = opendir(dir);
    char *best = NULL;
    size_t nl = strlen(arg);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, arg, nl) || e->d_name[nl] != '-') continue;
        if (!strstr(e->d_name, ".ucraft")) continue;
        /* after "name-" a digit must follow (avoids "attr-extra-1.0") */
        if (e->d_name[nl + 1] < '0' || e->d_name[nl + 1] > '9') continue;
        if (!best || strcmp(e->d_name, best) > 0) { free(best); best = xstrdup(e->d_name); }
    }
    if (d) closedir(d);
    if (!best) die("no package '%s' in the repositories or in %s (try 'kit sync' or 'kit build %s')", arg, dir, arg);
    char *full = xasprintf("%s/%s", dir, best);
    free(best); free(dir);
    return full;
}

int main(int argc, char **argv)
{
    const char *env_root = getenv("UCRAFT");
    const char *proj = getenv("UCRAFT_PROJECT");
    const char *home = getenv("HOME");
    if (env_root && *env_root) cfg.root = xstrdup(env_root);

    static const struct option opts[] = {
        {"root", 1, 0, 'r'}, {"cache", 1, 0, 'c'}, {"forge", 1, 0, 'R'}, {"repo", 1, 0, 'p'},
        {"keep-build", 0, 0, 'k'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    /* '+' : stop at the first non-option argument (the command) */
    while ((c = getopt_long(argc, argv, "+h", opts, NULL)) != -1) {
        switch (c) {
        case 'r': free(cfg.root); cfg.root = xstrdup(optarg); break;
        case 'c': free(cfg.cache); cfg.cache = xstrdup(optarg); break;
        case 'R': sv_push(&cfg.recipes, optarg); break;
        case 'p': sv_push(&cfg.repos, optarg); break;
        case 'k': cfg.keep_build = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) { usage(stderr); return 2; }

    int in_system = running_in_ucraft();
    /* On the host (Arch) there is no default root: ucraft refuses to touch it by mistake. */
    if (!cfg.root && in_system) cfg.root = xstrdup("/");

    if (!cfg.cache) {
        if (proj && *proj) cfg.cache = xasprintf("%s/cache", proj);
        else if (in_system) cfg.cache = xstrdup("/var/cache/ucraft");
        else if (home) cfg.cache = xasprintf("%s/.cache/ucraft", home);
        else die("don't know where to keep the cache: use --cache");
    }
    if (!cfg.recipes.n) {
        const char *er = getenv("UCRAFT_FORGE");
        if (er && *er) {
            char *cp = xstrdup(er), *save = NULL;
            for (char *t = strtok_r(cp, ":", &save); t; t = strtok_r(NULL, ":", &save)) sv_push(&cfg.recipes, t);
            free(cp);
        } else if (proj && *proj) {
            char *d = xasprintf("%s/forge", proj);
            sv_push(&cfg.recipes, d);
            free(d);
            d = xasprintf("%s/forge-local", proj);   /* local recipes override the shared ones */
            if (is_dir(d)) sv_push(&cfg.recipes, d);
            free(d);
        } else {
            sv_push(&cfg.recipes, "forge");
        }
    }

    const char *cmd = argv[optind];
    char **a = argv + optind + 1;
    int n = argc - optind - 1;
    optind = 0;   /* reset getopt for each subcommand's parsing */

    if (!strcmp(cmd, "hash")) {
        if (n != 1) die("usage: kit hash <file>");
        char hex[65];
        if (sha256_file(a[0], hex) != 0) die("cannot read %s", a[0]);
        printf("%s  %s\n", hex, a[0]);
    } else if (!strcmp(cmd, "info")) {
        if (n != 1) die("usage: kit info <recipe|folder>");
        struct recipe *r = load_recipe_arg(a[0]);
        recipe_print(r);
        if (cfg.root && db_has(cfg.root, r->name)) {
            struct pkginfo *pi = db_info(cfg.root, r->name);
            printf("%-12s%s-%d\n", "Installed", pi->version, pi->revision);
            pkginfo_free(pi);
        }
    } else if (!strcmp(cmd, "sync")) {
        if (n != 0) die("usage: kit sync");
        repo_sync();
    } else if (!strcmp(cmd, "search")) {
        if (n > 1) die("usage: kit search [text]");
        repo_search(n == 1 ? a[0] : NULL);
    } else if (!strcmp(cmd, "upgrade")) {
        if (n != 0) die("usage: kit upgrade");
        need_root();
        upgrade_all();
    } else if (!strcmp(cmd, "repo")) {
        if (n != 2 || strcmp(a[0], "build")) die("usage: kit repo build <dir>");
        repo_build(a[1]);
    } else if (!strcmp(cmd, "init")) {
        if (n != 0) die("usage: kit init   (acts on --root or $UCRAFT)");
        need_root();
        int made = layout_apply(cfg.root);
        if (made) info("created %d entries in %s", made, cfg.root);
        else info("%s already has the complete layout", cfg.root);
    } else if (!strcmp(cmd, "image")) {
        if (n < 1 || strcmp(a[0], "build") || n > 2) die("usage: kit image build [ucraft.toml]");
        image_build(n == 2 ? a[1] : "ucraft.toml");
    } else if (!strcmp(cmd, "build")) {
        if (n != 1) die("usage: kit build <recipe|folder>");
        struct recipe *r = load_recipe_arg(a[0]);
        if (!cfg.root) die("build needs the sysroot: use --root DIR or load ucraft-env.sh (it sets $UCRAFT)");
        char *out = build_recipe(r);
        free(out);
    } else if (!strcmp(cmd, "install")) {
        struct install_opts io = {0};
        static const struct option iopts[] = {{"asdep", 0, 0, 'a'}, {"nodeps", 0, 0, 'n'},
                                              {"overwrite", 1, 0, 'o'}, {0, 0, 0, 0}};
        while ((c = getopt_long(n + 1, a - 1, "", iopts, NULL)) != -1) {
            if (c == 'a') io.asdep = 1;
            else if (c == 'n') io.nodeps = 1;
            else if (c == 'o') sv_push(&io.overwrite, optarg);
            else return 2;
        }
        int rest = n + 1 - optind;
        if (rest < 1) die("usage: kit install [options] <file.ucraft | name>...");
        need_root();
        char **args = (a - 1) + optind;
        if (rest == 1 && (strchr(args[0], '/') || strstr(args[0], ".ucraft"))) {
            install_package(args[0], &io);   /* a package file */
        } else {
            strv names = {0};
            for (int k = 0; k < rest; k++) sv_push(&names, args[k]);
            install_names(&names, &io);      /* names: resolved with their dependencies */
            sv_free(&names);
        }
    } else if (!strcmp(cmd, "remove")) {
        int force = 0;
        if (n == 2 && !strcmp(a[0], "--force")) { force = 1; a++; n--; }
        if (n != 1) die("usage: kit remove [--force] <package>");
        need_root();
        remove_package(a[0], force);
    } else if (!strcmp(cmd, "list")) {
        need_root();
        strv names = {0};
        db_list(cfg.root, &names);
        for (size_t i = 0; i < names.n; i++) {
            struct pkginfo *pi = db_info(cfg.root, names.v[i]);
            char *reason = db_reason(cfg.root, names.v[i]);
            printf("%s %s-%d%s\n", pi->name, pi->version, pi->revision, strcmp(reason, "dep") ? "" : " [dep]");
            free(reason);
            pkginfo_free(pi);
        }
        sv_free(&names);
    } else if (!strcmp(cmd, "files")) {
        if (n != 1) die("usage: kit files <package>");
        need_root();
        struct flist fl = {0};
        if (!db_has(cfg.root, a[0])) die("package '%s' is not installed", a[0]);
        db_files(cfg.root, a[0], &fl);
        for (size_t i = 0; i < fl.n; i++)
            if (fl.v[i].type != 'D') puts(fl.v[i].path);
        flist_free(&fl);
    } else if (!strcmp(cmd, "owns")) {
        if (n != 1) die("usage: kit owns <absolute path inside the system>");
        need_root();
        struct owners o;
        owners_load(cfg.root, &o);
        const char *pkg = owners_find(&o, a[0]);
        if (!pkg) { fprintf(stderr, "%s does not belong to any package\n", a[0]); return 1; }
        printf("%s is owned by %s\n", a[0], pkg);
        owners_free(&o);
    } else if (!strcmp(cmd, "check")) {
        if (n != 1) die("usage: kit check <package>");
        need_root();
        if (!db_has(cfg.root, a[0])) die("package '%s' is not installed", a[0]);
        return check_package(a[0]) ? 1 : 0;
    } else {
        fprintf(stderr, "kit: unknown command '%s'\n\n", cmd);
        usage(stderr);
        return 2;
    }
    return 0;
}
