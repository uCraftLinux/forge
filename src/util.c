#define _GNU_SOURCE
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("kit: error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    fputs("kit: warning: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void info(const char *fmt, ...)
{
    va_list ap;
    fputs(":: ", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

void sv_push(strv *s, const char *str)
{
    if (s->n + 1 >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = xstrdup(str);
    s->v[s->n] = NULL;
}

int sv_has(const strv *s, const char *str)
{
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], str)) return 1;
    return 0;
}

void sv_free(strv *s)
{
    for (size_t i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int is_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdir_p(const char *path, mode_t mode)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, mode) != 0 && errno != EEXIST) die("mkdir %s: %s", p, strerror(errno));
        *s = '/';
    }
    if (mkdir(p, mode) != 0 && errno != EEXIST) die("mkdir %s: %s", p, strerror(errno));
    free(p);
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p);
}

int rm_rf(const char *path)
{
    if (!path_exists(path)) return 0;
    return nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

void write_file_atomic(const char *path, const char *data, size_t len)
{
    char *tmp = xasprintf("%s.ucraft-new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) die("cannot write %s: %s", tmp, strerror(errno));
    if (fwrite(data, 1, len, f) != len || fflush(f) != 0 || fsync(fileno(f)) != 0)
        die("write to %s: %s", tmp, strerror(errno));
    fclose(f);
    if (rename(tmp, path) != 0) die("rename %s: %s", path, strerror(errno));
    free(tmp);
}

int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { rc = -1; break; }
    fclose(a);
    if (fclose(b) != 0) rc = -1;
    return rc;
}

int run(char *const argv[], const char *cwd)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err) {
        warn("cannot execute %s: %s", argv[0], strerror(err));
        return 127;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) die("waitpid: %s", strerror(errno));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int find_in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = xstrdup(path), *save = NULL;
    for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char *full = xasprintf("%s/%s", d, prog);
        int ok = access(full, X_OK) == 0;
        free(full);
        if (ok) { free(copy); return 1; }
    }
    free(copy);
    return 0;
}
