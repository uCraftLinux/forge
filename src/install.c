#define _GNU_SOURCE
#include "db.h"
#include "pkg.h"
#include "ucraft.h"
#include "sha256.h"
#include "tar.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_META (64u * 1024 * 1024)

/* temporary files created; deleted if something fails before the final rename */
struct pend { char *tmp, *final; };
static struct pend *pending;
static size_t npending, cappending;
static int committed;

static void pend_add(const char *tmp, const char *final)
{
    if (npending == cappending) {
        cappending = cappending ? cappending * 2 : 64;
        pending = xrealloc(pending, cappending * sizeof *pending);
    }
    pending[npending].tmp = xstrdup(tmp);
    pending[npending].final = xstrdup(final);
    npending++;
}

static void pend_cleanup(void)
{
    if (committed) return;
    for (size_t i = 0; i < npending; i++) unlink(pending[i].tmp);
}

static char *full_path(const char *p)
{
    /* root "/" + "/usr/x" -> "/usr/x" without a double slash */
    size_t rl = strlen(cfg.root);
    while (rl > 1 && cfg.root[rl - 1] == '/') rl--;
    return xasprintf("%.*s%s", (int)(rl == 1 ? 0 : rl), cfg.root, p);
}

static char *read_member(FILE *f, const struct tar_hdr *h)
{
    if (h->size > MAX_META) die("package metadata too large");
    char *buf = xmalloc((size_t)h->size + 1);
    if (fread(buf, 1, (size_t)h->size, f) != (size_t)h->size) die("tar: truncated archive");
    buf[h->size] = 0;
    tar_finish_data(f, h->size);
    return buf;
}

static int overwrite_ok(const struct install_opts *o, const char *path)
{
    return sv_has(&o->overwrite, "*") || sv_has(&o->overwrite, path);
}

/* System skeleton directories: never deleted when removing packages. */
static int protected_dir(const char *path)
{
    static const char *const keep[] = {
        "/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/include", "/usr/share",
        "/usr/share/doc", "/usr/share/man", "/usr/share/info", "/usr/share/factory",
        "/usr/share/factory/etc", "/usr/lib/pkg", "/usr/lib/pkg/db", "/usr/lib/sysusers",
        "/usr/lib/tmpfiles", "/usr/lib/debug", "/usr/local", "/etc", "/var", NULL};
    for (int i = 0; keep[i]; i++)
        if (!strcmp(path, keep[i])) return 1;
    return 0;
}

/* Deletes from disk the entries of `old` that are not in `keep` (may be NULL). */
static void remove_entries(const struct flist *old, const struct flist *keep, const struct flist *other_dirs)
{
    for (size_t i = 0; i < old->n; i++) {
        const struct fent *e = &old->v[i];
        if (e->type == 'D') continue;
        if (keep && flist_find(keep, e->path)) continue;
        char *p = full_path(e->path);
        if (unlink(p) != 0 && errno != ENOENT) warn("could not delete %s: %s", p, strerror(errno));
        free(p);
    }
    /* directories, deepest first (the list is sorted) */
    for (size_t i = old->n; i-- > 0;) {
        const struct fent *e = &old->v[i];
        if (e->type != 'D') continue;
        if (keep && flist_find(keep, e->path)) continue;
        if (protected_dir(e->path)) continue;
        if (other_dirs && flist_find(other_dirs, e->path)) continue;
        char *p = full_path(e->path);
        rmdir(p);   /* if it is not empty, it stays */
        free(p);
    }
}

static void other_dirs_load(const char *skip, struct flist *out)
{
    strv names = {0};
    db_list(cfg.root, &names);
    for (size_t i = 0; i < names.n; i++) {
        if (skip && !strcmp(names.v[i], skip)) continue;
        struct flist fl = {0};
        db_files(cfg.root, names.v[i], &fl);
        for (size_t k = 0; k < fl.n; k++)
            if (fl.v[k].type == 'D') flist_add(out, 'D', fl.v[k].mode, NULL, fl.v[k].path, NULL);
        flist_free(&fl);
    }
    sv_free(&names);
    flist_sort(out);
}

/* Reads the .INFO of a package file without installing it. */
struct pkginfo *package_info(const char *ucraft_file)
{
    FILE *f = fopen(ucraft_file, "rb");
    if (!f) die("cannot open %s: %s", ucraft_file, strerror(errno));
    struct tar_hdr h;
    if (!tar_read_header(f, &h) || strcmp(h.path, ".INFO")) die("%s is not a .ucraft package (missing .INFO)", ucraft_file);
    char *txt = read_member(f, &h);
    fclose(f);
    struct pkginfo *pi = pkginfo_parse(txt);
    free(txt);
    return pi;
}

void install_package(const char *ucraft_file, const struct install_opts *o)
{
    FILE *f = fopen(ucraft_file, "rb");
    if (!f) die("cannot open %s: %s", ucraft_file, strerror(errno));

    struct tar_hdr h;
    if (!tar_read_header(f, &h) || strcmp(h.path, ".INFO")) die("%s is not a .ucraft package (missing .INFO)", ucraft_file);
    char *infotxt = read_member(f, &h);
    if (!tar_read_header(f, &h) || strcmp(h.path, ".FILES")) die("%s is not a .ucraft package (missing .FILES)", ucraft_file);
    char *filestxt = read_member(f, &h);

    struct pkginfo *pi = pkginfo_parse(infotxt);
    struct flist files = {0};
    flist_parse(&files, filestxt);
    if (strcmp(pi->arch, "x86_64")) die("unsupported architecture '%s'", pi->arch);

    int upgrade = db_has(cfg.root, pi->name);
    info("%s %s %s-%d in %s", upgrade ? "upgrading" : "installing", pi->name, pi->version, pi->revision, cfg.root);

    /* dependencies */
    if (!o->nodeps) {
        for (size_t i = 0; i < pi->depends.n; i++) {
            char *d = dep_name(pi->depends.v[i]);
            if (!db_has(cfg.root, d)) die("missing dependency '%s' (install it first, or use --nodeps)", d);
            free(d);
        }
    }

    /* conflicts */
    struct owners own;
    owners_load(cfg.root, &own);
    int conflicts = 0;
    for (size_t i = 0; i < files.n; i++) {
        const struct fent *e = &files.v[i];
        char *p = full_path(e->path);
        struct stat st;
        if (lstat(p, &st) == 0) {
            if (e->type == 'D') {
                if (!S_ISDIR(st.st_mode)) {
                    struct stat st2;
                    if (stat(p, &st2) != 0 || !S_ISDIR(st2.st_mode)) {
                        fprintf(stderr, "kit: conflict: %s exists and is not a directory\n", e->path);
                        conflicts++;
                    }
                }
            } else {
                const char *ow = owners_find(&own, e->path);
                if (S_ISDIR(st.st_mode)) {
                    fprintf(stderr, "kit: conflict: %s is a directory\n", e->path);
                    conflicts++;
                } else if (!ow && !overwrite_ok(o, e->path)) {
                    fprintf(stderr, "kit: conflict: %s exists and does not belong to any package\n", e->path);
                    conflicts++;
                } else if (ow && strcmp(ow, pi->name) && !overwrite_ok(o, e->path)) {
                    fprintf(stderr, "kit: conflict: %s belongs to '%s'\n", e->path, ow);
                    conflicts++;
                }
            }
        }
        free(p);
    }
    owners_free(&own);
    if (conflicts) die("%d conflict(s); nothing was touched (use --overwrite <path> to allow it)", conflicts);

    /* phase A: extract everything as <path>.ucraft-new, verifying sha256 */
    /* the state is per install: kit image installs many packages in one process */
    static int atexit_done;
    for (size_t k = 0; k < npending; k++) { free(pending[k].tmp); free(pending[k].final); }
    npending = 0;
    committed = 0;
    if (!atexit_done) { atexit(pend_cleanup); atexit_done = 1; }
    char *seen = xcalloc(files.n ? files.n : 1, 1);
    size_t nseen = 0;
    while (tar_read_header(f, &h)) {
        char *abs = xasprintf("/%s", h.path);
        struct fent *e = flist_find(&files, abs);
        if (!e) die("the package contains '%s', which is not listed in .FILES", abs);
        size_t idx = (size_t)(e - files.v);
        if (seen[idx]) die("duplicate entry in the package: %s", abs);
        seen[idx] = 1;
        nseen++;
        char want = h.type == '5' ? 'D' : h.type == '2' ? 'L' : 'F';
        if (want != e->type) die("type of '%s' does not match .FILES", abs);

        char *p = full_path(abs);
        if (e->type == 'D') {
            if (!path_exists(p)) {
                mkdir_p(p, 0755);
                chmod(p, e->mode);
            }
        } else {
            char *dir = xstrdup(p);
            *strrchr(dir, '/') = 0;
            if (*dir) mkdir_p(dir, 0755);
            free(dir);
            char *tmp = xasprintf("%s.ucraft-new", p);
            unlink(tmp);
            if (e->type == 'L') {
                if (symlink(h.link, tmp) != 0) die("symlink %s: %s", tmp, strerror(errno));
                if (!e->link || strcmp(e->link, h.link)) die("link target of '%s' does not match .FILES", abs);
                pend_add(tmp, p);
            } else {
                int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd < 0) die("cannot create %s: %s", tmp, strerror(errno));
                pend_add(tmp, p);
                sha256_ctx c;
                sha256_init(&c);
                char buf[65536];
                unsigned long long left = h.size;
                while (left) {
                    size_t want_n = left < sizeof buf ? (size_t)left : sizeof buf;
                    size_t got = fread(buf, 1, want_n, f);
                    if (got == 0) die("tar: truncated archive at %s", abs);
                    sha256_update(&c, buf, got);
                    if (write(fd, buf, got) != (ssize_t)got) die("write to %s: %s", tmp, strerror(errno));
                    left -= got;
                }
                tar_finish_data(f, h.size);
                char hex[65];
                sha256_final(&c, hex);
                if (strcmp(hex, e->sha)) die("wrong sha256 for '%s': the package is corrupted", abs);
                if (fchmod(fd, e->mode) != 0) die("chmod %s: %s", tmp, strerror(errno));
                if (fsync(fd) != 0 || close(fd) != 0) die("closing %s: %s", tmp, strerror(errno));
            }
            free(tmp);
        }
        free(p); free(abs);
    }
    fclose(f);
    if (nseen != files.n) die("the package is incomplete: %zu entries from .FILES are missing", files.n - nseen);
    free(seen);

    /* phase B: atomic rename per file */
    for (size_t i = 0; i < npending; i++)
        if (rename(pending[i].tmp, pending[i].final) != 0)
            die("rename %s: %s", pending[i].final, strerror(errno));
    committed = 1;

    /* phase C: database (keeps the install reason on upgrades) */
    char *reason = upgrade ? db_reason(cfg.root, pi->name) : xstrdup(o->asdep ? "dep" : "explicit\n");
    char *reason_nl = xasprintf("%s%s", reason, strchr(reason, '\n') ? "" : "\n");
    struct flist old = {0};
    if (upgrade) db_files(cfg.root, pi->name, &old);
    db_write(cfg.root, pi->name, infotxt, filestxt, reason_nl);

    /* phase D: files of the previous version that no longer exist */
    if (upgrade) {
        struct flist od = {0};
        other_dirs_load(pi->name, &od);
        remove_entries(&old, &files, &od);
        flist_free(&od);
    }
    info("%s %s-%d installed (%zu entries)", pi->name, pi->version, pi->revision, files.n);

    free(reason); free(reason_nl);
    flist_free(&old);
    flist_free(&files);
    pkginfo_free(pi);
    free(infotxt); free(filestxt);
}

void remove_package(const char *name, int force)
{
    struct pkginfo *pi = db_info(cfg.root, name);
    strv names = {0};
    db_list(cfg.root, &names);
    if (!force) {
        int blocked = 0;
        for (size_t i = 0; i < names.n; i++) {
            if (!strcmp(names.v[i], name)) continue;
            struct pkginfo *o = db_info(cfg.root, names.v[i]);
            for (size_t k = 0; k < o->depends.n; k++) {
                char *d = dep_name(o->depends.v[k]);
                if (!strcmp(d, name)) {
                    fprintf(stderr, "kit: '%s' is a dependency of '%s'\n", name, o->name);
                    blocked = 1;
                }
                free(d);
            }
            pkginfo_free(o);
        }
        if (blocked) die("not removing '%s' (use --force to ignore this)", name);
    }
    sv_free(&names);

    struct flist fl = {0}, od = {0};
    db_files(cfg.root, name, &fl);
    other_dirs_load(name, &od);
    info("removing %s %s-%d from %s", pi->name, pi->version, pi->revision, cfg.root);
    remove_entries(&fl, NULL, &od);
    db_remove(cfg.root, name);
    info("%s removed (%zu entries)", name, fl.n);
    flist_free(&fl);
    flist_free(&od);
    pkginfo_free(pi);
}

int check_package(const char *name)
{
    struct flist fl = {0};
    db_files(cfg.root, name, &fl);
    int bad = 0;
    for (size_t i = 0; i < fl.n; i++) {
        const struct fent *e = &fl.v[i];
        char *p = full_path(e->path);
        struct stat st;
        if (lstat(p, &st) != 0) {
            printf("%s: missing %s\n", name, e->path);
            bad++;
        } else if (e->type == 'F') {
            char hex[65];
            if (!S_ISREG(st.st_mode) || sha256_file(p, hex) != 0 || strcmp(hex, e->sha)) {
                printf("%s: modified %s\n", name, e->path);
                bad++;
            }
        } else if (e->type == 'L') {
            char tgt[4096];
            ssize_t n = S_ISLNK(st.st_mode) ? readlink(p, tgt, sizeof tgt - 1) : -1;
            if (n < 0 || (tgt[n] = 0, strcmp(tgt, e->link))) {
                printf("%s: modified link %s\n", name, e->path);
                bad++;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            printf("%s: %s is no longer a directory\n", name, e->path);
            bad++;
        }
        free(p);
    }
    printf("%s: %zu entries, %d problem(s)\n", name, fl.n, bad);
    flist_free(&fl);
    return bad;
}
