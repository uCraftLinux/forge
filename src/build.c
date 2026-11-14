#define _GNU_SOURCE
#include "db.h"
#include "pkg.h"
#include "ucraft.h"
#include "sha256.h"
#include "tar.h"
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARCH "x86_64"

struct vars { const char *dest, *src, *version, *name; char jobs[32]; const char *target; };

static char *subst(const char *s, const struct vars *v)
{
    size_t cap = strlen(s) + 64, n = 0;
    char *out = xmalloc(cap);
    for (const char *p = s; *p;) {
        const char *val = NULL;
        size_t skip = 0;
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p, '}');
            if (!end) die("unterminated variable in '%s'", s);
            size_t nl = (size_t)(end - p - 2);
            skip = nl + 3;
            if (nl == 4 && !strncmp(p + 2, "DEST", 4)) val = v->dest;
            else if (nl == 3 && !strncmp(p + 2, "SRC", 3)) val = v->src;
            else if (nl == 4 && !strncmp(p + 2, "JOBS", 4)) val = v->jobs;
            else if (nl == 7 && !strncmp(p + 2, "VERSION", 7)) val = v->version;
            else if (nl == 4 && !strncmp(p + 2, "NAME", 4)) val = v->name;
            else if (nl == 6 && !strncmp(p + 2, "TARGET", 6)) val = v->target;
            else die("unknown variable in '%s'", s);
        }
        size_t add = val ? strlen(val) : 1;
        while (n + add + 1 > cap) { cap *= 2; out = xrealloc(out, cap); }
        if (val) { memcpy(out + n, val, add); p += skip; }
        else out[n] = *p++;
        n += add;
    }
    out[n] = 0;
    return out;
}

static void check_run(char *const argv[], const char *cwd, const char *what)
{
    info("%s", what);
    int rc = run(argv, cwd);
    if (rc != 0) die("'%s' failed (exit code %d)", what, rc);
}

/* ---------- download ---------- */

static char *fetch_source(const struct source *s)
{
    const char *base = strrchr(s->url, '/');
    base = base ? base + 1 : s->url;
    if (!*base || strchr(base, '?') || !strcmp(base, "..")) die("cannot work out the file name of %s", s->url);
    char *dir = xasprintf("%s/src", cfg.cache);
    mkdir_p(dir, 0755);
    char *path = xasprintf("%s/%s", dir, base);
    char hex[65];
    if (path_exists(path)) {
        if (sha256_file(path, hex) == 0 && !strcasecmp(hex, s->sha256)) {
            info("source in cache: %s", base);
            free(dir);
            return path;
        }
        warn("cached %s does not match the recipe's sha256; downloading again", base);
        unlink(path);
    }
    info("downloading source");
    fetch_file(s->url, path, s->sha256);
    free(dir);
    return path;
}

/* ---------- built-in steps ---------- */

static void do_steps(const struct recipe *r, const char *phase, const struct vars *v, const char *srcdir)
{
    for (size_t i = 0; i < r->nsteps; i++) {
        const struct step *st = &r->steps[i];
        if (strcmp(st->phase, phase)) continue;
        strv a = {0};
        for (size_t k = 0; k < st->args.n; k++) {
            char *x = subst(st->args.v[k], v);
            sv_push(&a, x);
            free(x);
        }
        info("step %s: %s", st->phase, st->action);
        if (!strcmp(st->action, "rm")) {
            for (size_t k = 0; k < a.n; k++)
                if (rm_rf(a.v[k]) != 0) die("rm %s: %s", a.v[k], strerror(errno));
        } else if (!strcmp(st->action, "move")) {
            if (rename(a.v[0], a.v[1]) != 0) die("move %s -> %s: %s", a.v[0], a.v[1], strerror(errno));
        } else if (!strcmp(st->action, "link")) {
            unlink(a.v[1]);
            if (symlink(a.v[0], a.v[1]) != 0) die("link %s -> %s: %s", a.v[1], a.v[0], strerror(errno));
        } else if (!strcmp(st->action, "mkdir")) {
            for (size_t k = 0; k < a.n; k++) mkdir_p(a.v[k], 0755);
        } else if (!strcmp(st->action, "chmod")) {
            /* chmod <mode, octal> <path>... */
            unsigned mode = (unsigned)strtoul(a.v[0], NULL, 8);
            for (size_t k = 1; k < a.n; k++)
                if (chmod(a.v[k], mode) != 0) die("chmod %s: %s", a.v[k], strerror(errno));
        } else if (!strcmp(st->action, "install")) {
            /* install <file in the recipe folder> <destination> [mode, octal; default 0644] */
            char *from = a.v[0][0] == '/' ? xstrdup(a.v[0]) : xasprintf("%s/%s", r->dir, a.v[0]);
            char *dir = xstrdup(a.v[1]);
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) { *slash = 0; mkdir_p(dir, 0755); }
            if (copy_file(from, a.v[1]) != 0) die("install %s -> %s: %s", from, a.v[1], strerror(errno));
            unsigned mode = a.n > 2 ? (unsigned)strtoul(a.v[2], NULL, 8) : 0644;
            if (chmod(a.v[1], mode) != 0) die("chmod %s: %s", a.v[1], strerror(errno));
            free(from); free(dir);
        } else if (!strcmp(st->action, "patch")) {
            char *pf = a.v[0][0] == '/' ? xstrdup(a.v[0]) : xasprintf("%s/%s", r->dir, a.v[0]);
            char *argv[] = {"patch", "-p1", "-i", pf, NULL};
            check_run(argv, srcdir, "applying patch");
            free(pf);
        }
        sv_free(&a);
    }
}

/* ---------- styles ---------- */

static void push_subst(strv *argv, const strv *args, const struct vars *v)
{
    for (size_t i = 0; i < args->n; i++) {
        char *x = subst(args->v[i], v);
        sv_push(argv, x);
        free(x);
    }
}

static void style_autotools(const struct recipe *r, const struct vars *v, const char *srcdir, const char *builddir, const char *dest)
{
    char *configure = xasprintf("%s/configure", srcdir);
    if (access(configure, X_OK) != 0)
        die("%s does not exist: this recipe's tarball has no 'configure' (use a release tarball, not a checkout)", configure);
    mkdir_p(builddir, 0755);
    strv a = {0};
    sv_push(&a, configure);
    sv_push(&a, "--prefix=/usr");
    sv_push(&a, "--sysconfdir=/etc");
    sv_push(&a, "--localstatedir=/var");
    sv_push(&a, "--libexecdir=/usr/lib");
    char *host = xasprintf("--host=%s", v->target);
    sv_push(&a, host);
    free(host);
    push_subst(&a, &r->build_args, v);
    check_run(a.v, builddir, "configure");
    sv_free(&a);

    char jobs[40];
    snprintf(jobs, sizeof jobs, "-j%s", v->jobs);
    strv m = {0};
    sv_push(&m, "make"); sv_push(&m, jobs);
    push_subst(&m, &r->make_args, v);
    check_run(m.v, builddir, "make");
    sv_free(&m);

    do_steps(r, "pre_install", v, srcdir);
    char *dd = xasprintf("DESTDIR=%s", dest);
    strv i = {0};
    sv_push(&i, "make"); sv_push(&i, dd); sv_push(&i, r->install_target ? r->install_target : "install");
    push_subst(&i, &r->make_args, v);
    push_subst(&i, &r->install_args, v);
    check_run(i.v, builddir, "make install");
    sv_free(&i);
    free(dd); free(configure);
}

static void style_make(const struct recipe *r, const struct vars *v, const char *srcdir, const char *dest, int pass_prefix)
{
    if (!r->skip_build) {
        char jobs[40];
        snprintf(jobs, sizeof jobs, "-j%s", v->jobs);
        strv m = {0};
        sv_push(&m, "make"); sv_push(&m, jobs);
        push_subst(&m, &r->make_args, v);
        check_run(m.v, srcdir, "make");
        sv_free(&m);
    }

    do_steps(r, "pre_install", v, srcdir);
    char *dd = xasprintf("DESTDIR=%s", dest);
    strv i = {0};
    sv_push(&i, "make"); sv_push(&i, dd);
    if (pass_prefix) sv_push(&i, "PREFIX=/usr");
    sv_push(&i, r->install_target ? r->install_target : "install");
    push_subst(&i, &r->make_args, v);
    push_subst(&i, &r->install_args, v);
    check_run(i.v, srcdir, "make install");
    sv_free(&i);
    free(dd);
}

/* cmake + ninja, out of tree. Cross defaults come from the ucraft environment
 * (CC/CXX wrappers); everything else is passed through the recipe's args. */
static void style_cmake(const struct recipe *r, const struct vars *v, const char *srcdir, const char *builddir, const char *dest)
{
    if (!find_in_path("cmake") || !find_in_path("ninja")) die("style 'cmake' needs 'cmake' and 'ninja' in PATH");
    char *root = r->subdir ? xasprintf("%s/%s", srcdir, r->subdir) : xstrdup(srcdir);
    mkdir_p(builddir, 0755);
    strv a = {0};
    sv_push(&a, "cmake"); sv_push(&a, "-S"); sv_push(&a, root);
    sv_push(&a, "-B"); sv_push(&a, builddir); sv_push(&a, "-G"); sv_push(&a, "Ninja");
    sv_push(&a, "-DCMAKE_INSTALL_PREFIX=/usr");
    sv_push(&a, "-DCMAKE_INSTALL_LIBDIR=lib");
    sv_push(&a, "-DCMAKE_BUILD_TYPE=Release");
    sv_push(&a, "-DCMAKE_SYSTEM_NAME=Linux");
    sv_push(&a, "-DCMAKE_SYSTEM_PROCESSOR=x86_64");
    const char *cc = getenv("CC"), *cxx = getenv("CXX");
    if (cc && *cc) { char *x = xasprintf("-DCMAKE_C_COMPILER=%s", cc); sv_push(&a, x); free(x); }
    if (cxx && *cxx) { char *x = xasprintf("-DCMAKE_CXX_COMPILER=%s", cxx); sv_push(&a, x); free(x); }
    push_subst(&a, &r->build_args, v);
    check_run(a.v, builddir, "cmake configure");
    sv_free(&a);

    char jobs[40];
    snprintf(jobs, sizeof jobs, "-j%s", v->jobs);
    strv m = {0};
    sv_push(&m, "cmake"); sv_push(&m, "--build"); sv_push(&m, builddir); sv_push(&m, jobs);
    push_subst(&m, &r->make_args, v);
    check_run(m.v, builddir, "cmake --build");
    sv_free(&m);

    do_steps(r, "pre_install", v, srcdir);
    setenv("DESTDIR", dest, 1);
    char *argv[] = {"cmake", "--install", (char *)builddir, NULL};
    check_run(argv, builddir, "cmake --install");
    unsetenv("DESTDIR");
    free(root);
}

/* The program's own configure (dinit, etc.): run in the source tree with
 * only the recipe's arguments. No --prefix or --host is added. */
static void style_configure(const struct recipe *r, const struct vars *v, const char *srcdir, const char *dest)
{
    char *configure = xasprintf("%s/configure", srcdir);
    if (access(configure, X_OK) != 0) die("%s does not exist or is not executable", configure);
    strv a = {0};
    sv_push(&a, configure);
    push_subst(&a, &r->build_args, v);
    check_run(a.v, srcdir, "configure");
    sv_free(&a);
    free(configure);
    style_make(r, v, srcdir, dest, 0);
}

/* ---------- DESTDIR cleanup ---------- */

static strv la_files;

static int collect_la(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)f;
    size_t l = strlen(p);
    if (flag == FTW_F && S_ISREG(st->st_mode) && l > 3 && !strcmp(p + l - 3, ".la")) sv_push(&la_files, p);
    return 0;
}

static int nondir_seen;
static int find_nondir(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)p; (void)f; (void)flag;
    if (!S_ISDIR(st->st_mode)) nondir_seen = 1;
    return 0;
}

static int rmdir_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return rmdir(p);
}

static void clean_dest(const struct recipe *r, const char *dest)
{
    /* libtool .la files keep host paths and are not needed */
    char *usr = xasprintf("%s/usr", dest);
    if (is_dir(usr)) {
        memset(&la_files, 0, sizeof la_files);
        nftw(usr, collect_la, 16, FTW_PHYS);
        for (size_t i = 0; i < la_files.n; i++) unlink(la_files.v[i]);
        if (la_files.n) info("removed %zu .la files", la_files.n);
        sv_free(&la_files);
    }
    char *infodir = xasprintf("%s/usr/share/info/dir", dest);
    unlink(infodir);
    free(infodir); free(usr);

    /* the package's /etc goes to the factory configuration */
    char *etc = xasprintf("%s/etc", dest);
    if (path_exists(etc)) {
        char *fdir = xasprintf("%s/usr/share/factory", dest);
        char *fetc = xasprintf("%s/etc", fdir);
        mkdir_p(fdir, 0755);
        if (path_exists(fetc)) die("usr/share/factory/etc already exists in the package");
        if (rename(etc, fetc) != 0) die("moving etc to factory: %s", strerror(errno));
        info("etc/ moved to usr/share/factory/etc/");
        free(fdir); free(fetc);
    }
    free(etc);

    /* /var is not allowed: only accepted if it holds empty directories */
    char *var = xasprintf("%s/var", dest);
    if (path_exists(var) && !r->allow_var) {
        nondir_seen = 0;
        nftw(var, find_nondir, 16, FTW_PHYS);
        if (nondir_seen) die("the package installs files in /var, which is not allowed (use tmpfiles)");
        nftw(var, rmdir_cb, 16, FTW_PHYS | FTW_DEPTH);
    }
    free(var);
}

/* ---------- file list of DESTDIR ---------- */

static struct flist *g_files;
static size_t g_prefix;
static unsigned long long g_size;

static int collect_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)f; (void)flag;
    if (strlen(p) == g_prefix) return 0;   /* DESTDIR itself */
    const char *rel = p + g_prefix;
    unsigned mode = st->st_mode & 07777;
    if (S_ISDIR(st->st_mode)) {
        flist_add(g_files, 'D', mode, NULL, rel, NULL);
    } else if (S_ISLNK(st->st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(p, tgt, sizeof tgt - 1);
        if (n < 0) die("readlink %s: %s", p, strerror(errno));
        tgt[n] = 0;
        flist_add(g_files, 'L', 0777, NULL, rel, tgt);
    } else if (S_ISREG(st->st_mode)) {
        char hex[65];
        if (sha256_file(p, hex) != 0) die("cannot read %s", p);
        flist_add(g_files, 'F', mode, hex, rel, NULL);
        g_size += (unsigned long long)st->st_size;
    } else {
        die("unsupported file type in the package: %s", rel);
    }
    return 0;
}

/* Writes one .ucraft package: .INFO, .FILES and then the files, in path order. */
static void emit_package(const char *dest, const struct flist *files, struct pkginfo *pi, const char *out)
{
    unsigned long long total = 0;
    for (size_t i = 0; i < files->n; i++) {
        if (files->v[i].type != 'F') continue;
        char *full = xasprintf("%s%s", dest, files->v[i].path);
        struct stat st;
        if (lstat(full, &st) == 0) total += (unsigned long long)st.st_size;
        free(full);
    }
    pi->size = total;
    char *infotxt = pkginfo_format(pi);
    char *filestxt = flist_format(files);

    char *tmp = xasprintf("%s.part", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) die("cannot create %s: %s", tmp, strerror(errno));

    struct tar_hdr h;
    const char *names[2] = {".INFO", ".FILES"};
    const char *data[2] = {infotxt, filestxt};
    for (int i = 0; i < 2; i++) {
        memset(&h, 0, sizeof h);
        snprintf(h.path, sizeof h.path, "%s", names[i]);
        h.type = '0'; h.mode = 0644; h.size = strlen(data[i]);
        tar_write_header(f, &h);
        fwrite(data[i], 1, h.size, f);
        tar_write_padding(f, h.size);
    }
    for (size_t i = 0; i < files->n; i++) {
        const struct fent *e = &files->v[i];
        memset(&h, 0, sizeof h);
        snprintf(h.path, sizeof h.path, "%s", e->path + 1);
        h.mode = e->mode;
        char *full = xasprintf("%s%s", dest, e->path);
        struct stat st;
        if (lstat(full, &st) != 0) die("lstat %s: %s", full, strerror(errno));
        h.mtime = (long)st.st_mtime;
        if (e->type == 'D') h.type = '5';
        else if (e->type == 'L') { h.type = '2'; snprintf(h.link, sizeof h.link, "%s", e->link); }
        else { h.type = '0'; h.size = (unsigned long long)st.st_size; }
        tar_write_header(f, &h);
        if (e->type == 'F') {
            FILE *in = fopen(full, "rb");
            if (!in) die("cannot read %s", full);
            char buf[65536];
            unsigned long long left = h.size;
            while (left) {
                size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
                size_t got = fread(buf, 1, want, in);
                if (got == 0) die("short read from %s", full);
                if (fwrite(buf, 1, got, f) != got) die("write of %s: %s", tmp, strerror(errno));
                left -= got;
            }
            fclose(in);
            tar_write_padding(f, h.size);
        }
        free(full);
    }
    tar_write_end(f);
    if (fclose(f) != 0) die("closing %s: %s", tmp, strerror(errno));
    if (rename(tmp, out) != 0) die("rename %s: %s", out, strerror(errno));
    info("package: %s (%zu entries, %llu installed bytes)", out, files->n, total);
    free(tmp); free(infotxt); free(filestxt);
}

/* ---------- stripping ---------- */

static strv strip_list;

static int strip_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)f;
    size_t l = strlen(p);
    if (flag != FTW_F || !S_ISREG(st->st_mode) || st->st_size < 64) return 0;
    if (l > 2 && !strcmp(p + l - 2, ".o")) return 0;     /* object files are not stripped */
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    unsigned char magic[4] = {0};
    size_t n = fread(magic, 1, 4, fp);
    fclose(fp);
    if (n == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') sv_push(&strip_list, p);
    return 0;
}

/* Removes debug information and unneeded symbols from every ELF file in DESTDIR: smaller packages,
 * smaller images, same behavior. Debug symbols could be split out later (usr/lib/debug). */
static void strip_dest(const char *dest)
{
    const char *tgt = getenv("UCRAFT_TGT");
    char *prefixed = xasprintf("%s-strip", tgt && *tgt ? tgt : "x86_64-ucraft-linux-musl");
    const char *tool = find_in_path(prefixed) ? prefixed : (find_in_path("strip") ? "strip" : NULL);
    if (!tool) { warn("no strip tool found: binaries keep their symbols"); free(prefixed); return; }
    memset(&strip_list, 0, sizeof strip_list);
    nftw(dest, strip_cb, 32, FTW_PHYS);
    size_t done = 0;
    for (size_t i = 0; i < strip_list.n; i++) {
        char *argv[] = {(char *)tool, "--strip-unneeded", strip_list.v[i], NULL};
        if (run(argv, NULL) == 0) done++;
        else warn("could not strip %s", strip_list.v[i]);
    }
    if (done) info("stripped %zu ELF file(s)", done);
    sv_free(&strip_list);
    free(prefixed);
}

/* ---------- splitting into -dev and -doc ---------- */

enum { PART_MAIN = 0, PART_DEV = 1, PART_DOC = 2, NPARTS = 3 };

static int has_prefix(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
static int has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

/* Standard rules. Development files: headers, static libraries, object files, pkg-config,
 * cmake and aclocal data, unversioned .so links (and tiny linker scripts such as libc++.so),
 * *-config scripts, the clang resource dir. Documentation: man, doc, info. */
static int classify(const char *dest, const struct fent *e, int want_dev, int want_doc)
{
    static const char *const doc_prefix[] = {"/usr/share/man/", "/usr/share/doc/", "/usr/share/info/", NULL};
    static const char *const dev_prefix[] = {"/usr/include/", "/usr/lib/pkgconfig/", "/usr/share/pkgconfig/",
        "/usr/lib/cmake/", "/usr/share/aclocal/", "/usr/lib/clang/", "/usr/share/libc++/", NULL};
    const char *p = e->path;
    if (want_doc)
        for (int i = 0; doc_prefix[i]; i++)
            if (has_prefix(p, doc_prefix[i])) return PART_DOC;
    if (!want_dev) return PART_MAIN;
    for (int i = 0; dev_prefix[i]; i++)
        if (has_prefix(p, dev_prefix[i])) return PART_DEV;
    if (has_prefix(p, "/usr/lib/") && !strchr(p + 9, '/')) {
        const char *name = p + 9;
        if (has_suffix(name, ".a") || has_suffix(name, ".o") || has_suffix(name, ".modules.json")) return PART_DEV;
        if (has_prefix(name, "lib") && has_suffix(name, ".so")) {
            if (e->type == 'L') return PART_DEV;
            char *full = xasprintf("%s%s", dest, p);
            struct stat st;
            int tiny = lstat(full, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= 4096;
            free(full);
            if (tiny) return PART_DEV;   /* linker script */
        }
    }
    if (has_prefix(p, "/usr/bin/") && has_suffix(p, "-config")) return PART_DEV;
    return PART_MAIN;
}

static int dir_has_entry_under(const struct flist *l, const char *dir)
{
    size_t n = strlen(dir);
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i].type != 'D' && !strncmp(l->v[i].path, dir, n) && l->v[i].path[n] == '/') return 1;
    return 0;
}

static char *package_path(const char *pkgdir, const char *name, const struct recipe *r)
{
    return xasprintf("%s/%s-%s_%d-%s.ucraft", pkgdir, name, r->version, r->revision, ARCH);
}

static void write_packages(const struct recipe *r, const char *dest, const char *pkgdir)
{
    strip_dest(dest);
    struct flist files = {0};
    g_files = &files;
    g_prefix = strlen(dest);
    g_size = 0;
    nftw(dest, collect_cb, 32, FTW_PHYS);
    /* a meta-package (style none with no files) is valid: it only carries its dependencies */
    int meta = !files.n && !strcmp(r->style, "none");
    if (!files.n && !meta) die("the package is empty: make install left nothing in %s", dest);
    flist_sort(&files);

    int want_dev = sv_has(&r->split, "dev"), want_doc = sv_has(&r->split, "doc");
    struct flist parts[NPARTS] = {{0}};
    if (!want_dev && !want_doc) {
        for (size_t i = 0; i < files.n; i++) {
            const struct fent *e = &files.v[i];
            flist_add(&parts[PART_MAIN], e->type, e->mode, e->sha, e->path, e->link);
        }
    } else {
        /* files first, so the directories can follow their contents */
        for (size_t i = 0; i < files.n; i++) {
            const struct fent *e = &files.v[i];
            if (e->type == 'D') continue;
            flist_add(&parts[classify(dest, e, want_dev, want_doc)], e->type, e->mode, e->sha, e->path, e->link);
        }
        for (size_t i = 0; i < files.n; i++) {
            const struct fent *e = &files.v[i];
            if (e->type != 'D') continue;
            int total = dir_has_entry_under(&files, e->path);
            for (int k = 0; k < NPARTS; k++) {
                if (dir_has_entry_under(&parts[k], e->path) || (k == PART_MAIN && !total))
                    flist_add(&parts[k], 'D', e->mode, NULL, e->path, NULL);
            }
        }
        for (int k = 0; k < NPARTS; k++) flist_sort(&parts[k]);
    }

    static const char *const suffix[NPARTS] = {"", "-dev", "-doc"};
    static const char *const what[NPARTS] = {"", " (development files)", " (documentation)"};
    for (int k = 0; k < NPARTS; k++) {
        if (!parts[k].n && !(meta && k == PART_MAIN)) continue;
        int has_content = 0;
        for (size_t i = 0; i < parts[k].n; i++) if (parts[k].v[i].type != 'D') { has_content = 1; break; }
        if (k != PART_MAIN && !has_content) continue;
        char *name = xasprintf("%s%s", r->name, suffix[k]);
        char *desc = xasprintf("%s%s", r->description ? r->description : r->name, what[k]);
        struct pkginfo pi = {0};
        pi.name = name; pi.version = r->version; pi.arch = (char *)ARCH;
        pi.description = desc; pi.license = r->license; pi.revision = r->revision;
        strv deps = {0};
        if (k == PART_MAIN) { for (size_t i = 0; i < r->depends.n; i++) sv_push(&deps, r->depends.v[i]); }
        else if (k == PART_DEV) sv_push(&deps, r->name);
        pi.depends = deps;
        char *out = package_path(pkgdir, name, r);
        emit_package(dest, &parts[k], &pi, out);
        free(out); free(name); free(desc); sv_free(&deps);
    }
    for (int k = 0; k < NPARTS; k++) flist_free(&parts[k]);
    flist_free(&files);
}

/* ---------- building ---------- */

char *build_recipe(const struct recipe *r)
{
    const char *target = getenv("UCRAFT_TGT");
    if (!target || !*target) target = "x86_64-ucraft-linux-musl";

    char *cc = xasprintf("%s-cc", target);
    if (!getenv("CC")) {
        if (!find_in_path(cc))
            die("cannot find '%s' in PATH: load the environment with  source ucraft-env.sh", cc);
        setenv("CC", cc, 1);
        char *cxx = xasprintf("%s-c++", target);
        setenv("CXX", cxx, 1);
        free(cxx);
    }
    free(cc);
    /* the compiler wrappers use --sysroot="$UCRAFT" */
    setenv("UCRAFT", cfg.root, 1);

    struct vars v = {0};
    /* UCRAFT_JOBS limits parallelism on machines with little RAM */
    const char *ej = getenv("UCRAFT_JOBS");
    long nj = (ej && atol(ej) > 0) ? atol(ej) : sysconf(_SC_NPROCESSORS_ONLN);
    snprintf(v.jobs, sizeof v.jobs, "%ld", nj > 0 ? nj : 1);
    v.target = target; v.version = r->version; v.name = r->name;

    char *work = xasprintf("%s/build/%s-%s", cfg.cache, r->name, r->version);
    char *srcdir = xasprintf("%s/src", work);
    char *builddir = xasprintf("%s/build", work);
    char *dest = xasprintf("%s/dest", work);
    v.src = srcdir; v.dest = dest;

    info("building %s %s-%d (style %s)", r->name, r->version, r->revision, r->style);
    rm_rf(work);
    mkdir_p(srcdir, 0755);
    mkdir_p(dest, 0755);

    if (r->nsources) {
        char *tarball = fetch_source(&r->sources[0]);
        char *argv[] = {"tar", "-xf", tarball, "-C", srcdir, "--strip-components=1", NULL};
        check_run(argv, NULL, "extracting source");
        free(tarball);
    }
    for (size_t i = 0; i < r->patches.n; i++) {
        char *pf = r->patches.v[i][0] == '/' ? xstrdup(r->patches.v[i]) : xasprintf("%s/%s", r->dir, r->patches.v[i]);
        if (access(pf, R_OK) != 0) die("patch '%s' not found (looked for %s)", r->patches.v[i], pf);
        char *pargv[] = {"patch", "-p1", "-i", pf, NULL};
        char *what = xasprintf("applying patch %s", r->patches.v[i]);
        check_run(pargv, srcdir, what);
        free(what); free(pf);
    }
    do_steps(r, "post_extract", &v, srcdir);

    if (!strcmp(r->style, "autotools")) style_autotools(r, &v, srcdir, builddir, dest);
    else if (!strcmp(r->style, "make")) style_make(r, &v, srcdir, dest, 1);
    else if (!strcmp(r->style, "cmake")) style_cmake(r, &v, srcdir, builddir, dest);
    else if (!strcmp(r->style, "configure")) style_configure(r, &v, srcdir, dest);
    else do_steps(r, "pre_install", &v, srcdir);   /* style none: there is no install step */
    do_steps(r, "post_install", &v, srcdir);

    clean_dest(r, dest);

    char *pkgdir = xasprintf("%s/pkg", cfg.cache);
    mkdir_p(pkgdir, 0755);
    char *out = package_path(pkgdir, r->name, r);
    write_packages(r, dest, pkgdir);

    if (!cfg.keep_build) rm_rf(work);
    free(work); free(srcdir); free(builddir); free(dest); free(pkgdir);
    return out;
}
