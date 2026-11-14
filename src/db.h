#ifndef UCRAFT_DB_H
#define UCRAFT_DB_H
#include "pkg.h"

/* Database in <root>/usr/lib/pkg/db/<package>/{info,files,reason} */

char *db_path(const char *root, const char *name);          /* folder of a package */
int db_has(const char *root, const char *name);
struct pkginfo *db_info(const char *root, const char *name);        /* die if it does not exist */
void db_files(const char *root, const char *name, struct flist *out);
char *db_reason(const char *root, const char *name);                /* "explicit" | "dep" */
void db_list(const char *root, strv *names);                        /* sorted */
void db_write(const char *root, const char *name, const char *info, const char *files, const char *reason);
void db_remove(const char *root, const char *name);

/* Index path -> package for everything installed */
struct owner { char *path; char *pkg; };
struct owners { struct owner *v; size_t n; };
void owners_load(const char *root, struct owners *o);
const char *owners_find(const struct owners *o, const char *path);
void owners_free(struct owners *o);

#endif
