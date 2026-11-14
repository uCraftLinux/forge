#ifndef UCRAFT_PKG_H
#define UCRAFT_PKG_H
#include "util.h"

/* Entry of .FILES / db/<pkg>/files:  type \t mode \t sha256 \t path \t target */
struct fent {
    char type;            /* 'F' file, 'D' directory, 'L' symbolic link */
    unsigned mode;
    char sha[65];         /* files only; "-" for the rest */
    char *path;           /* absolute: /usr/bin/x */
    char *link;           /* links only */
};

struct flist { struct fent *v; size_t n, cap; };

void flist_add(struct flist *l, char type, unsigned mode, const char *sha, const char *path, const char *link);
void flist_sort(struct flist *l);
struct fent *flist_find(const struct flist *l, const char *path);   /* requires flist_sort */
char *flist_format(const struct flist *l);
void flist_parse(struct flist *l, const char *text);
void flist_free(struct flist *l);

struct pkginfo {
    char *name, *version, *arch, *description, *license;
    int revision;
    unsigned long long size;
    strv depends;
};

char *pkginfo_format(const struct pkginfo *p);
struct pkginfo *pkginfo_parse(const char *text);
void pkginfo_free(struct pkginfo *p);
/* "openssl>=3.0" -> "openssl" (free the result) */
char *dep_name(const char *dep);

#endif
