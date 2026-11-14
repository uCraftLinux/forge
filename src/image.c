#define _GNU_SOURCE
#include "db.h"
#include "tar.h"
#include "toml.h"
#include "ucraft.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* kit image build: assembles a root filesystem from packages and imports it as a container image.
 *
 *   name     = "hello-php"
 *   profile  = "min"                       # base-min (default) or base-dev
 *   packages = ["php"]
 *   copy     = ["./app:/app"]
 *   cmd      = ["php", "-S", "[::]:8080", "-t", "/app"]
 *   user     = "nobody"                    # default
 */

struct spec {
    char *name, *profile, *user;
    strv packages, copy, cmd;
};

static void list_into(strv *dst, const toml_val *v)
{
    for (size_t i = 0; i < v->list.n; i++) sv_push(dst, v->list.v[i]);
}

static const char *spec_cb(const char *section, int idx, const char *key, const toml_val *v, void *ud)
{
    struct spec *s = ud;
    (void)idx;
    if (*section) return "unknown table (an image file only has top-level keys)";
    if (!strcmp(key, "name")) { if (v->type != TV_STR) return "expected a string"; s->name = xstrdup(v->s); }
    else if (!strcmp(key, "profile")) { if (v->type != TV_STR) return "expected a string"; s->profile = xstrdup(v->s); }
    else if (!strcmp(key, "user")) { if (v->type != TV_STR) return "expected a string"; s->user = xstrdup(v->s); }
    else if (!strcmp(key, "packages")) { if (v->type != TV_LIST) return "expected a list of strings"; list_into(&s->packages, v); }
    else if (!strcmp(key, "copy")) { if (v->type != TV_LIST) return "expected a list of strings"; list_into(&s->copy, v); }
    else if (!strcmp(key, "cmd")) { if (v->type != TV_LIST) return "expected a list of strings"; list_into(&s->cmd, v); }
    else return "unknown key";
    return NULL;
}

static int valid_image_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) return 0;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.') return 0;
    return 1;
}

static void pop(strv *s)
{
    if (!s->n) return;
    free(s->v[--s->n]);
    s->v[s->n] = NULL;
}

/* ---------- paths ---------- */

static void split_path(const char *p, strv *out)
{
    char *copy = xstrdup(p), *save = NULL;
    for (char *t = strtok_r(copy, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { pop(out); continue; }
        sv_push(out, t);
    }
    free(copy);
}

struct ctx { const char *lsrc_root, *ldst_root; int skip_existing; };   /* logical paths inside the image */

/* A relative link in usr/share/factory/etc must keep pointing to the same thing from /etc. */
static char *rewrite_link(const struct ctx *c, const char *target, const char *ldst_dir, const char *lsrc_dir)
{
    if (target[0] == '/' || !c) return xstrdup(target);
    char *joined = xasprintf("%s/%s", lsrc_dir, target);
    strv abs = {0}, from = {0}, sroot = {0}, droot = {0};
    split_path(joined, &abs);
    split_path(ldst_dir, &from);
    split_path(c->lsrc_root, &sroot);
    split_path(c->ldst_root, &droot);
    free(joined);

    /* a target inside the factory tree maps to the same place in the destination tree */
    int inside = abs.n >= sroot.n;
    for (size_t i = 0; inside && i < sroot.n; i++) if (strcmp(abs.v[i], sroot.v[i])) inside = 0;
    if (inside) {
        strv mapped = {0};
        for (size_t i = 0; i < droot.n; i++) sv_push(&mapped, droot.v[i]);
        for (size_t i = sroot.n; i < abs.n; i++) sv_push(&mapped, abs.v[i]);
        sv_free(&abs);
        abs = mapped;
    }
    size_t common = 0;
    while (common < abs.n && common < from.n && !strcmp(abs.v[common], from.v[common])) common++;
    char *out = xstrdup("");
    for (size_t i = common; i < from.n; i++) { char *t = xasprintf("%s../", out); free(out); out = t; }
    for (size_t i = common; i < abs.n; i++) {
        char *t = xasprintf("%s%s%s", out, abs.v[i], i + 1 < abs.n ? "/" : "");
        free(out);
        out = t;
    }
    if (!*out) { free(out); out = xstrdup("."); }
    sv_free(&abs); sv_free(&from); sv_free(&sroot); sv_free(&droot);
    return out;
}

static char *dir_of(const char *p)
{
    char *d = xstrdup(p);
    char *s = strrchr(d, '/');
    if (s && s != d) *s = 0;
    else if (s) d[1] = 0;
    return d;
}

static void copy_tree(const struct ctx *c, const char *src, const char *dst, const char *lsrc, const char *ldst)
{
    struct stat st;
    if (lstat(src, &st) != 0) die("cannot read %s: %s", src, strerror(errno));
    mode_t mode = st.st_mode & 07777;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, mode) != 0 && errno != EEXIST) die("mkdir %s: %s", dst, strerror(errno));
        chmod(dst, mode);
        DIR *d = opendir(src);
        if (!d) die("cannot open %s: %s", src, strerror(errno));
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *s = xasprintf("%s/%s", src, e->d_name), *t = xasprintf("%s/%s", dst, e->d_name);
            char *ls = xasprintf("%s/%s", lsrc, e->d_name), *lt = xasprintf("%s/%s", ldst, e->d_name);
            copy_tree(c, s, t, ls, lt);
            free(s); free(t); free(ls); free(lt);
        }
        closedir(d);
    } else if (S_ISLNK(st.st_mode)) {
        if (c && c->skip_existing && path_exists(dst)) return;
        char tgt[4096];
        ssize_t n = readlink(src, tgt, sizeof tgt - 1);
        if (n < 0) die("readlink %s: %s", src, strerror(errno));
        tgt[n] = 0;
        char *ld = dir_of(ldst), *ls = dir_of(lsrc);
        char *nt = rewrite_link(c, tgt, ld, ls);
        unlink(dst);
        if (symlink(nt, dst) != 0) die("symlink %s: %s", dst, strerror(errno));
        free(ld); free(ls); free(nt);
    } else if (S_ISREG(st.st_mode)) {
        if (c && c->skip_existing && path_exists(dst)) return;
        if (copy_file(src, dst) != 0) die("cannot copy %s to %s: %s", src, dst, strerror(errno));
        chmod(dst, mode);
    } else {
        die("unsupported file type: %s", src);
    }
}

/* ---------- root filesystem tar ---------- */

struct tent { char *rel; mode_t mode; char type; char *link; unsigned long long size; };
static struct tent *tents;
static size_t ntents, captents, root_len;
static unsigned long long total_bytes;

static int tar_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)f; (void)flag;
    if (strlen(p) <= root_len) return 0;
    if (ntents == captents) {
        captents = captents ? captents * 2 : 256;
        tents = xrealloc(tents, captents * sizeof *tents);
    }
    struct tent *t = &tents[ntents++];
    memset(t, 0, sizeof *t);
    t->rel = xstrdup(p + root_len + 1);
    t->mode = st->st_mode & 07777;
    if (S_ISDIR(st->st_mode)) t->type = '5';
    else if (S_ISLNK(st->st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(p, tgt, sizeof tgt - 1);
        if (n < 0) die("readlink %s: %s", p, strerror(errno));
        tgt[n] = 0;
        t->type = '2';
        t->link = xstrdup(tgt);
    } else if (S_ISREG(st->st_mode)) {
        t->type = '0';
        t->size = (unsigned long long)st->st_size;
        total_bytes += t->size;
    } else die("unsupported file type in the image: %s", p);
    return 0;
}

static int cmp_tent(const void *a, const void *b)
{
    return strcmp(((const struct tent *)a)->rel, ((const struct tent *)b)->rel);
}

static void write_rootfs_tar(const char *root, const char *out)
{
    ntents = 0;
    total_bytes = 0;
    root_len = strlen(root);
    nftw(root, tar_cb, 32, FTW_PHYS);
    qsort(tents, ntents, sizeof *tents, cmp_tent);

    FILE *f = fopen(out, "wb");
    if (!f) die("cannot create %s: %s", out, strerror(errno));
    for (size_t i = 0; i < ntents; i++) {
        struct tent *t = &tents[i];
        struct tar_hdr h;
        memset(&h, 0, sizeof h);
        snprintf(h.path, sizeof h.path, "%s", t->rel);
        h.type = t->type;
        h.mode = t->mode;
        h.size = t->size;
        h.mtime = 0;   /* reproducible */
        if (t->link) snprintf(h.link, sizeof h.link, "%s", t->link);
        tar_write_header(f, &h);
        if (t->type == '0') {
            char *full = xasprintf("%s/%s", root, t->rel);
            FILE *in = fopen(full, "rb");
            if (!in) die("cannot read %s", full);
            char buf[65536];
            unsigned long long left = t->size;
            while (left) {
                size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
                size_t got = fread(buf, 1, want, in);
                if (!got) die("short read from %s", full);
                if (fwrite(buf, 1, got, f) != got) die("write to %s: %s", out, strerror(errno));
                left -= got;
            }
            fclose(in);
            free(full);
            tar_write_padding(f, t->size);
        }
        free(t->rel); free(t->link);
    }
    tar_write_end(f);
    if (fclose(f) != 0) die("closing %s: %s", out, strerror(errno));
}

/* ---------- podman ---------- */

static char *json_array(const strv *a)
{
    char *out = xstrdup("[");
    for (size_t i = 0; i < a->n; i++) {
        char *esc = xstrdup("");
        for (const char *p = a->v[i]; *p; p++) {
            char *t;
            if (*p == '"' || *p == '\\') t = xasprintf("%s\\%c", esc, *p);
            else if ((unsigned char)*p < 0x20) t = xasprintf("%s\\u%04x", esc, (unsigned char)*p);
            else t = xasprintf("%s%c", esc, *p);
            free(esc);
            esc = t;
        }
        char *t = xasprintf("%s%s\"%s\"", out, i ? "," : "", esc);
        free(out); free(esc);
        out = t;
    }
    char *t = xasprintf("%s]", out);
    free(out);
    return t;
}

static int user_in_passwd(const char *root, const char *user)
{
    if (isdigit((unsigned char)user[0])) return 1;   /* numeric ids need no entry */
    char *p = xasprintf("%s/etc/passwd", root);
    char *txt = read_file(p, NULL);
    free(p);
    if (!txt) return 0;
    size_t n = strlen(user);
    int found = 0;
    for (char *l = txt; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, user, n) && l[n] == ':') { found = 1; break; }
    free(txt);
    return found;
}

/* ---------- the command ---------- */

void image_build(const char *spec_path)
{
    struct spec s = {0};
    toml_parse_file(spec_path, spec_cb, &s);
    if (!s.name) die("%s: missing 'name'", spec_path);
    if (!valid_image_name(s.name)) die("%s: 'name' must be lowercase letters, digits, '-', '_' or '.'", spec_path);
    if (!s.profile) s.profile = xstrdup("min");
    if (!s.user) s.user = xstrdup("nobody");
    if (!s.cmd.n) sv_push(&s.cmd, "/bin/sh");

    /* 1. resolve the packages, dependencies first */
    strv requested = {0};
    char *base = xasprintf("base-%s", s.profile);
    sv_push(&requested, base);
    for (size_t k = 0; k < s.packages.n; k++) sv_push(&requested, s.packages.v[k]);
    struct plan plan;
    plan_build(&requested, 0, &plan);   /* from the repositories (kit sync) or from the cache */
    info("image '%s': profile %s, %zu packages", s.name, s.profile, plan.names.n);

    /* 2. install them into a fresh root */
    char *imgdir = xasprintf("%s/images/%s", cfg.cache, s.name);
    char *root = xasprintf("%s/rootfs", imgdir);
    rm_rf(imgdir);
    mkdir_p(root, 0755);
    char *saved_root = cfg.root;
    cfg.root = root;
    layout_apply(root);   /* the system template: directories and base /etc files */
    for (size_t k = 0; k < plan.names.n; k++) {
        struct install_opts io = {0};
        io.asdep = !sv_has(&requested, plan.names.v[k]);
        install_package(plan.files.v[k], &io);
    }
    cfg.root = saved_root;

    /* 3. /etc comes from the factory defaults */
    char *factory = xasprintf("%s/usr/share/factory/etc", root), *etc = xasprintf("%s/etc", root);
    if (is_dir(factory)) {
        struct ctx c = {"/usr/share/factory/etc", "/etc", 1};
        copy_tree(&c, factory, etc, "/usr/share/factory/etc", "/etc");
    }
    free(factory); free(etc);

    /* 4. user files */
    char *specdir = dir_of(spec_path);
    for (size_t i = 0; i < s.copy.n; i++) {
        char *colon = strchr(s.copy.v[i], ':');
        if (!colon || colon == s.copy.v[i] || colon[1] != '/') die("copy '%s' must look like 'source:/destination'", s.copy.v[i]);
        char *src = strndup(s.copy.v[i], (size_t)(colon - s.copy.v[i]));
        if (!src) die("out of memory");
        char *dst = xasprintf("%s%s", root, colon + 1);
        if (strstr(colon + 1, "/../") || !strcmp(colon + strlen(colon) - 3, "/..")) die("copy destination '%s' must not contain '..'", colon + 1);
        char *full = src[0] == '/' ? xstrdup(src) : xasprintf("%s/%s", specdir, src);
        char *parent = dir_of(dst);
        mkdir_p(parent, 0755);
        copy_tree(NULL, full, dst, full, colon + 1);
        free(src); free(dst); free(full); free(parent);
    }
    free(specdir);

    if (!user_in_passwd(root, s.user)) die("user '%s' is not in the image's /etc/passwd", s.user);

    /* 5. tar and import */
    char *tarpath = xasprintf("%s/images/%s.tar", cfg.cache, s.name);
    write_rootfs_tar(root, tarpath);
    info("root filesystem: %zu entries, %.2f MB (tar: %s)", ntents, (double)total_bytes / 1048576.0, tarpath);

    if (!find_in_path("podman")) {
        info("podman not found: import it yourself with  podman import %s %s:latest", tarpath, s.name);
    } else {
        char *cmdjson = json_array(&s.cmd);
        char *c_cmd = xasprintf("CMD %s", cmdjson);
        char *c_user = xasprintf("USER %s", s.user);
        char *ref = xasprintf("%s:latest", s.name);
        char *argv[] = {"podman", "import", "-q", "--change", c_cmd, "--change", c_user, "--change",
                        "ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", tarpath, ref, NULL};
        info("importing into podman as %s", ref);
        if (run(argv, NULL) != 0) die("podman import failed");
        info("run it with:  podman run --rm --read-only --cap-drop=all localhost/%s", ref);
        free(cmdjson); free(c_cmd); free(c_user); free(ref);
    }
    free(tarpath); free(imgdir); free(root); free(base);
    plan_free(&plan); sv_free(&requested);
}
