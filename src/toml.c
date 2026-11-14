#include "toml.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct P {
    const char *path;
    const char *s;
    size_t pos;
    int line;
};

static void perr(struct P *p, const char *msg)
{
    die("%s:%d: %s", p->path, p->line, msg);
}

static int peek(struct P *p) { return (unsigned char)p->s[p->pos]; }

static void skip_ws(struct P *p)
{
    while (peek(p) == ' ' || peek(p) == '\t') p->pos++;
}

static void skip_comment(struct P *p)
{
    if (peek(p) == '#')
        while (peek(p) && peek(p) != '\n') p->pos++;
}

/* end of line after a value: spaces, optional comment and newline */
static void end_line(struct P *p)
{
    skip_ws(p);
    skip_comment(p);
    if (peek(p) == '\r') p->pos++;
    if (peek(p) == '\n') { p->pos++; p->line++; }
    else if (peek(p)) perr(p, "expected end of line");
}

static char *parse_string(struct P *p)
{
    int q = peek(p);
    p->pos++;
    if (p->s[p->pos] == q && p->s[p->pos + 1] == q) perr(p, "multi-line strings (\"\"\") are not supported");
    size_t cap = 32, n = 0;
    char *out = xmalloc(cap);
    for (;;) {
        int c = peek(p);
        if (!c || c == '\n') perr(p, "unterminated string");
        p->pos++;
        if (c == q) break;
        if (c == '\\' && q == '"') {
            c = peek(p);
            p->pos++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '"': case '\\': break;
            default: perr(p, "unsupported escape in string");
            }
        }
        if (n + 1 >= cap) { cap *= 2; out = xrealloc(out, cap); }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return out;
}

static void skip_ws_nl(struct P *p)
{
    for (;;) {
        int c = peek(p);
        if (c == ' ' || c == '\t' || c == '\r') p->pos++;
        else if (c == '\n') { p->pos++; p->line++; }
        else if (c == '#') skip_comment(p);
        else break;
    }
}

static void parse_value(struct P *p, toml_val *v)
{
    memset(v, 0, sizeof *v);
    int c = peek(p);
    if (c == '"' || c == '\'') {
        v->type = TV_STR;
        v->s = parse_string(p);
    } else if (c == '[') {
        v->type = TV_LIST;
        p->pos++;
        for (;;) {
            skip_ws_nl(p);
            if (peek(p) == ']') { p->pos++; break; }
            if (peek(p) != '"' && peek(p) != '\'') perr(p, "lists only accept strings");
            char *s = parse_string(p);
            sv_push(&v->list, s);
            free(s);
            skip_ws_nl(p);
            if (peek(p) == ',') p->pos++;
            else if (peek(p) != ']') perr(p, "expected ',' or ']'");
        }
    } else if (!strncmp(p->s + p->pos, "true", 4)) {
        v->type = TV_BOOL; v->b = 1; p->pos += 4;
    } else if (!strncmp(p->s + p->pos, "false", 5)) {
        v->type = TV_BOOL; v->b = 0; p->pos += 5;
    } else if (isdigit(c) || c == '-' || c == '+') {
        char *end;
        v->type = TV_INT;
        v->i = strtol(p->s + p->pos, &end, 10);
        if (end == p->s + p->pos || *end == '.') perr(p, "unsupported number (integers only)");
        p->pos = (size_t)(end - p->s);
    } else {
        perr(p, "unrecognized value");
    }
}

static void free_val(toml_val *v)
{
    free(v->s);
    sv_free(&v->list);
}

static char *parse_name(struct P *p, const char *what)
{
    size_t st = p->pos;
    while (isalnum(peek(p)) || peek(p) == '_' || peek(p) == '-' || peek(p) == '.') p->pos++;
    if (p->pos == st) perr(p, what);
    return strndup(p->s + st, p->pos - st);
}

void toml_parse_file(const char *path, toml_cb cb, void *ud)
{
    char *text = read_file(path, NULL);
    if (!text) die("cannot read %s", path);
    struct P p = {path, text, 0, 1};
    char *section = xstrdup("");
    int idx = 0;
    /* per-name counter for [[x]]: recipes do not interleave different table
     * arrays, but a small table keeps us from depending on that. */
    struct { char *name; int count; } arr[8];
    int narr = 0;

    for (;;) {
        skip_ws(&p);
        int c = peek(&p);
        if (!c) break;
        if (c == '#' || c == '\r' || c == '\n') {
            skip_comment(&p);
            if (peek(&p) == '\r') p.pos++;
            if (peek(&p) == '\n') { p.pos++; p.line++; }
            continue;
        }
        if (c == '[') {
            int dbl = p.s[p.pos + 1] == '[';
            p.pos += dbl ? 2 : 1;
            skip_ws(&p);
            char *name = parse_name(&p, "missing table name");
            skip_ws(&p);
            if (peek(&p) != ']') perr(&p, "expected ']'");
            p.pos++;
            if (dbl) {
                if (peek(&p) != ']') perr(&p, "expected ']]'");
                p.pos++;
            }
            end_line(&p);
            free(section);
            section = name;
            idx = 0;
            if (dbl) {
                int k;
                for (k = 0; k < narr; k++)
                    if (!strcmp(arr[k].name, name)) break;
                if (k == narr) {
                    if (narr == 8) perr(&p, "too many different table arrays");
                    arr[narr].name = xstrdup(name);
                    arr[narr].count = 0;
                    narr++;
                }
                idx = arr[k].count++;
            }
            continue;
        }
        int line = p.line;
        char *key = parse_name(&p, "expected a key");
        skip_ws(&p);
        if (peek(&p) != '=') perr(&p, "expected '='");
        p.pos++;
        skip_ws(&p);
        toml_val v;
        parse_value(&p, &v);
        end_line(&p);
        int save = p.line;
        p.line = line;
        const char *err = cb(section, idx, key, &v, ud);
        if (err) perr(&p, err);
        p.line = save;
        free_val(&v);
        free(key);
    }
    for (int k = 0; k < narr; k++) free(arr[k].name);
    free(section);
    free(text);
}
