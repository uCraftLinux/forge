#ifndef UCRAFT_UTIL_H
#define UCRAFT_UTIL_H
#include <stddef.h>
#include <sys/types.h>

#define NORETURN __attribute__((noreturn))
#define PRINTF(a, b) __attribute__((format(printf, a, b)))

NORETURN PRINTF(1, 2) void die(const char *fmt, ...);
PRINTF(1, 2) void warn(const char *fmt, ...);
PRINTF(1, 2) void info(const char *fmt, ...);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
PRINTF(1, 2) char *xasprintf(const char *fmt, ...);

/* string list */
typedef struct {
    char **v;
    size_t n, cap;
} strv;
void sv_push(strv *s, const char *str);
int sv_has(const strv *s, const char *str);
void sv_free(strv *s);

/* files */
int path_exists(const char *path);               /* lstat */
int is_dir(const char *path);                    /* does not follow links */
void mkdir_p(const char *path, mode_t mode);     /* creates parents */
int rm_rf(const char *path);
char *read_file(const char *path, size_t *len);  /* NUL-terminated; NULL on failure */
void write_file_atomic(const char *path, const char *data, size_t len);
int copy_file(const char *from, const char *to);

/* processes: posix_spawn with a direct argv, no shell. Returns the exit code. */
int run(char *const argv[], const char *cwd);
int find_in_path(const char *prog);

#endif
