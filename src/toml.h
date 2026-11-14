#ifndef UCRAFT_TOML_H
#define UCRAFT_TOML_H
#include "util.h"

/* TOML subset: comments, tables [x], table arrays [[x]], "..." and '...'
 * strings, integers, booleans and lists of strings (may span several
 * lines). No dates, floats, inline tables or """. */

typedef enum { TV_STR, TV_INT, TV_BOOL, TV_LIST } toml_type;

typedef struct {
    toml_type type;
    char *s;
    long i;
    int b;
    strv list;
} toml_val;

/* section: "" at the root, or the table name. idx: for [[x]], sequence
 * number (0, 1, ...); 0 for normal tables. Returns NULL if all is well or
 * an error message (shown with file and line). */
typedef const char *(*toml_cb)(const char *section, int idx, const char *key,
                               const toml_val *v, void *ud);

/* Exits via die() on a syntax error or if the callback rejects something. */
void toml_parse_file(const char *path, toml_cb cb, void *ud);

#endif
