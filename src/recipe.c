#include "recipe.h"
#include "toml.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *STYLES[] = {"autotools", "cmake", "configure", "make", "none", NULL};
static const char *ACTIONS[] = {"rm", "move", "link", "patch", "mkdir", "chmod", "install", NULL};
static const char *PHASES[] = {"post_extract", "pre_install", "post_install", NULL};

static int in_set(const char *const *set, const char *s)
{
    for (; *set; set++)
        if (!strcmp(*set, s)) return 1;
    return 0;
}

#define NEED_STR(v) do { if ((v)->type != TV_STR) return "expected a string"; } while (0)
#define NEED_LIST(v) do { if ((v)->type != TV_LIST) return "expected a list of strings"; } while (0)

static void assign_str(char **dst, const toml_val *v)
{
    free(*dst);
    *dst = xstrdup(v->s);
}

static void copy_list(strv *dst, const toml_val *v)
{
    sv_free(dst);
    for (size_t i = 0; i < v->list.n; i++) sv_push(dst, v->list.v[i]);
}

static const char *on_entry(const char *section, int idx, const char *key, const toml_val *v, void *ud)
{
    struct recipe *r = ud;
    if (!*section) {
        if (!strcmp(key, "name")) { NEED_STR(v); assign_str(&r->name, v); }
        else if (!strcmp(key, "version")) { NEED_STR(v); assign_str(&r->version, v); }
        else if (!strcmp(key, "description")) { NEED_STR(v); assign_str(&r->description, v); }
        else if (!strcmp(key, "license")) { NEED_STR(v); assign_str(&r->license, v); }
        else if (!strcmp(key, "style")) {
            NEED_STR(v);
            if (!in_set(STYLES, v->s)) return "unknown style (autotools, cmake, configure, make or none)";
            assign_str(&r->style, v);
        } else if (!strcmp(key, "revision")) {
            if (v->type != TV_INT || v->i < 1) return "revision must be an integer >= 1";
            r->revision = (int)v->i;
        } else if (!strcmp(key, "split")) {
            NEED_LIST(v);
            for (size_t i = 0; i < v->list.n; i++)
                if (strcmp(v->list.v[i], "dev") && strcmp(v->list.v[i], "doc")) return "split only accepts \"dev\" and \"doc\"";
            copy_list(&r->split, v);
        } else if (!strcmp(key, "allow_var")) { if (v->type != TV_BOOL) return "expected true or false"; r->allow_var = v->b; }
        else if (!strcmp(key, "depends")) { NEED_LIST(v); copy_list(&r->depends, v); }
        else if (!strcmp(key, "makedepends")) { NEED_LIST(v); copy_list(&r->makedepends, v); }
        else return "unknown key";
        return NULL;
    }
    if (!strcmp(section, "source")) {
        if ((size_t)idx >= r->nsources) {
            r->sources = xrealloc(r->sources, (size_t)(idx + 1) * sizeof *r->sources);
            memset(&r->sources[idx], 0, sizeof r->sources[idx]);
            r->nsources = (size_t)idx + 1;
        }
        struct source *s = &r->sources[idx];
        if (!strcmp(key, "url")) { NEED_STR(v); assign_str(&s->url, v); }
        else if (!strcmp(key, "sha256")) { NEED_STR(v); assign_str(&s->sha256, v); }
        else return "unknown key in [[source]]";
        return NULL;
    }
    if (!strcmp(section, "build")) {
        if (!strcmp(key, "args")) { NEED_LIST(v); copy_list(&r->build_args, v); }
        else if (!strcmp(key, "make_args")) { NEED_LIST(v); copy_list(&r->make_args, v); }
        else if (!strcmp(key, "install_args")) { NEED_LIST(v); copy_list(&r->install_args, v); }
        else if (!strcmp(key, "patches")) { NEED_LIST(v); copy_list(&r->patches, v); }
        else if (!strcmp(key, "skip_build")) { if (v->type != TV_BOOL) return "expected true or false"; r->skip_build = v->b; }
        else if (!strcmp(key, "subdir")) { NEED_STR(v); assign_str(&r->subdir, v); }
        else if (!strcmp(key, "install_target")) { NEED_STR(v); assign_str(&r->install_target, v); }
        else return "unknown key in [build]";
        return NULL;
    }
    if (!strcmp(section, "step")) {
        if ((size_t)idx >= r->nsteps) {
            r->steps = xrealloc(r->steps, (size_t)(idx + 1) * sizeof *r->steps);
            memset(&r->steps[idx], 0, sizeof r->steps[idx]);
            r->nsteps = (size_t)idx + 1;
        }
        struct step *st = &r->steps[idx];
        if (!strcmp(key, "phase")) {
            NEED_STR(v);
            if (!in_set(PHASES, v->s)) return "unknown phase (post_extract, pre_install or post_install)";
            assign_str(&st->phase, v);
        } else if (!strcmp(key, "action")) {
            NEED_STR(v);
            if (!in_set(ACTIONS, v->s)) return "unknown action (rm, move, link, patch, mkdir, chmod, install)";
            assign_str(&st->action, v);
        } else if (!strcmp(key, "args")) { NEED_LIST(v); copy_list(&st->args, v); }
        else return "unknown key in [[step]]";
        return NULL;
    }
    return "unknown table";
}

static int valid_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '+' && *s != '.') return 0;
    return 1;
}

static int valid_sha(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

struct recipe *recipe_load(const char *dir)
{
    struct recipe *r = xcalloc(1, sizeof *r);
    r->revision = 1;
    r->dir = xstrdup(dir);
    char *path = xasprintf("%s/forge.toml", dir);
    toml_parse_file(path, on_entry, r);

    if (!r->name || !valid_name(r->name)) die("%s: 'name' is missing or has invalid characters", path);
    if (!r->version || !valid_name(r->version)) die("%s: 'version' is missing or has invalid characters", path);
    if (strchr(r->version, '_') || strchr(r->version, '-')) die("%s: 'version' cannot contain '-' or '_'", path);
    if (!r->style) die("%s: missing 'style'", path);
    if (r->nsources > 1) die("%s: only one [[source]] is supported for now", path);
    if (strcmp(r->style, "none") && r->nsources == 0) die("%s: style '%s' needs a [[source]]", path, r->style);
    for (size_t i = 0; i < r->nsources; i++) {
        if (!r->sources[i].url) die("%s: [[source]] without 'url'", path);
        if (!r->sources[i].sha256 || !valid_sha(r->sources[i].sha256))
            die("%s: [[source]] needs a 64-digit hex 'sha256' (use: kit hash <file>)", path);
    }
    for (size_t i = 0; i < r->nsteps; i++) {
        struct step *st = &r->steps[i];
        if (!st->phase || !st->action) die("%s: [[step]] #%zu needs 'phase' and 'action'", path, i + 1);
        size_t need = (!strcmp(st->action, "rm") || !strcmp(st->action, "patch") || !strcmp(st->action, "mkdir")) ? 1 : 2;
        if (st->args.n < need) die("%s: [[step]] #%zu (%s) needs at least %zu argument(s)", path, i + 1, st->action, need);
    }
    free(path);
    return r;
}

char *recipe_find(const strv *recipe_dirs, const char *name)
{
    char *found = NULL;
    if (!valid_name(name)) return NULL;
    for (size_t i = 0; i < recipe_dirs->n; i++) {
        char *p = xasprintf("%s/%s/forge.toml", recipe_dirs->v[i], name);
        if (access(p, R_OK) == 0) {
            free(found);
            found = xasprintf("%s/%s", recipe_dirs->v[i], name);
        }
        free(p);
    }
    return found;
}

static void print_list(const char *label, const strv *l)
{
    printf("%-13s", label);
    if (!l->n) printf("-");
    for (size_t i = 0; i < l->n; i++) printf("%s%s", i ? ", " : "", l->v[i]);
    putchar('\n');
}

void recipe_print(const struct recipe *r)
{
    printf("%-13s%s\n", "Name", r->name);
    printf("%-13s%s-%d\n", "Version", r->version, r->revision);
    printf("%-13s%s\n", "Description", r->description ? r->description : "-");
    printf("%-13s%s\n", "License", r->license ? r->license : "-");
    printf("%-13s%s\n", "Style", r->style);
    print_list("Depends", &r->depends);
    print_list("Build deps", &r->makedepends);
    print_list("Split", &r->split);
    for (size_t i = 0; i < r->nsources; i++) printf("%-13s%s\n", "Source", r->sources[i].url);
}
