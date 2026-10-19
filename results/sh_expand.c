/* vsh's word expansion (see sh_expand.h). */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "sh_expand.h"
#include "sh_hits.h"

/* ---- lists and variables ------------------------------------------------------ */

static char *sdup(const char *s)
{
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r)
        memcpy(r, s, n + 1);
    return r;
}

void sh_list_add(sh_list *l, const char *s)
{
    if (l->n + 1 >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        char **v = (char **)realloc(l->v, cap * sizeof(char *));
        if (!v)
            return;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = sdup(s);
    l->v[l->n] = 0;
}

void sh_list_free(sh_list *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v = 0;
    l->n = l->cap = 0;
}

static void var_free(sh_var *v);

/* the own list's node of NAME, a tombstone too */
static sh_var *own_find(const sh_ctx *c, const char *name)
{
    sh_var *v;
    for (v = c->vars; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return 0;
}

static const sh_var *base_find(const sh_ctx *c, const char *name)
{
    const sh_var *v;
    for (v = c->base; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return 0;
}

/* NAME, to read: the own list, else the base (a node there is the parent's: never written through) */
static sh_var *find(const sh_ctx *c, const char *name)
{
    sh_var *v = own_find(c, name);
    if (v)
        return (v->attr & SH_ATTR_GONE) ? 0 : v;
    return (sh_var *)base_find(c, name);
}

/* NAME, to write: the own list's, the base's copied into it first; 0 when it is not set */
static sh_var *find_w(sh_ctx *c, const char *name)
{
    sh_var *v = own_find(c, name);
    const sh_var *b;
    if (v)
        return (v->attr & SH_ATTR_GONE) ? 0 : v;
    if (!(b = base_find(c, name)) || !(v = sh_var_copy(b)))
        return 0;
    SH_HIT(VAR_COPY_UP);
    v->next = c->vars;
    c->vars = v;
    return v;
}

/* the own list's node of NAME out and freed (a tombstone too) */
static void unlink_own(sh_ctx *c, const char *name)
{
    sh_var **p;
    for (p = &c->vars; *p; p = &(*p)->next)
        if (!strcmp((*p)->name, name)) {
            sh_var *v = *p;
            *p = v->next;
            var_free(v);
            return;
        }
}

const sh_var *sh_var_next(const sh_ctx *c, sh_var_iter *it)
{
    const sh_var *v = it->v ? it->v->next : 0;
    for (;;) {
        if (!v && !it->in_base) {
            it->in_base = 1;
            v = c->base;
        }
        if (!v)
            break;
        if (!it->in_base ? !(v->attr & SH_ATTR_GONE) : !own_find(c, v->name))
            break;
        v = v->next;
    }
    it->v = v;
    return v;
}

const sh_var *sh_var_first(const sh_ctx *c, sh_var_iter *it)
{
    const sh_var *v = c->vars;
    it->in_base = 0;
    while (v && (v->attr & SH_ATTR_GONE))
        v = v->next;
    if (v) {
        it->v = v;
        return v;
    }
    it->v = 0;
    return sh_var_next(c, it);
}

/* The name a reference leads to (declare -n): at most 8 links. A chain that comes back to a name
 * it has passed, or is longer than 8, gives "" (nothing: reads are empty, writes refused) and
 * raises bash's warning. */
static const char *resolve(const sh_ctx *c, const char *name)
{
    const char *start = name;
    int hop;
    for (hop = 0; hop <= 8; hop++) {
        sh_var *v = find(c, name);
        if (!v || !(v->attr & SH_ATTR_NAMEREF) || !v->sv || !*v->sv)
            return name;
        if (hop == 8 || !strcmp(v->sv, start) || !strcmp(v->sv, name)) {
            if (c->warn)
                ((sh_ctx *)c)->warn((sh_ctx *)c, start, hop == 8 ? "maximum nameref depth (8) exceeded" : "circular name reference");
            return "";
        }
        name = v->sv;
    }
    return name;
}

sh_var *sh_lookup_raw(const sh_ctx *c, const char *name)
{
    return find(c, name);
}

const char *sh_resolve(const sh_ctx *c, const char *name)
{
    return resolve(c, name);
}

sh_var *sh_lookup(const sh_ctx *c, const char *name)
{
    if (c->refresh)
        c->refresh((sh_ctx *)c, name);
    return find(c, resolve(c, name));
}

/* ---- arrays --------------------------------------------------------------------- */

static int is_assoc(const sh_var *v)
{
    return (v->attr & SH_ATTR_ASSOC) != 0;
}

/* where the element idx / key is, or would go */
static long arr_pos(const sh_var *v, long idx, const char *key, int *found)
{
    const sh_arr *a = v->arr;
    long lo = 0, hi = a->n;
    while (lo < hi) {
        long m = (lo + hi) / 2, d;
        if (is_assoc(v))
            d = strcmp(a->e[m].k.key, key);
        else
            d = a->e[m].k.idx < idx ? -1 : a->e[m].k.idx > idx;
        if (!d) {
            *found = 1;
            return m;
        }
        if (d < 0)
            lo = m + 1;
        else
            hi = m;
    }
    *found = 0;
    return lo;
}

static void elem_free(const sh_var *v, sh_elem *e)
{
    if (is_assoc(v))
        free(e->k.key);
    free(e->val);
}

static void var_free(sh_var *v)
{
    free(v->name);
    free(v->sv);
    if (v->arr) {
        long i;
        for (i = 0; i < v->arr->n; i++)
            elem_free(v, v->arr->e + i);
        free(v->arr->e);
        free(v->arr);
    }
    free(v);
}

/* the scalar becomes element 0 of a new array */
static int make_array(sh_var *v, int assoc)
{
    v->arr = (sh_arr *)calloc(1, sizeof(sh_arr));
    if (!v->arr)
        return 1;
    v->attr = (unsigned short)((v->attr & ~(SH_ATTR_ARRAY | SH_ATTR_ASSOC)) | (assoc ? SH_ATTR_ASSOC : SH_ATTR_ARRAY));
    if (v->sv) {
        v->arr->e = (sh_elem *)calloc(1, sizeof(sh_elem));
        if (!v->arr->e)
            return 1;
        v->arr->n = v->arr->cap = 1;
        v->arr->e[0].val = v->sv;
        if (assoc)
            v->arr->e[0].k.key = sdup("0");
        v->sv = 0;
    }
    return 0;
}

/* The slot of the element at sub, created when make; 0: a bad subscript or no memory.
 * Negative indexes count from the end (the last index + 1). */
static char **elem_slot(sh_ctx *c, sh_var *v, const char *sub, int make)
{
    long idx = 0, pos;
    int found;
    sh_arr *a;
    if (is_assoc(v)) {
        if (!sub)
            sub = "0";
    } else if (sub) {
        const char *err = 0;
        idx = sh_arith(c, sub, &err);
        if (err)
            return 0;
        if (idx < 0 && v->arr && v->arr->n)
            idx += v->arr->e[v->arr->n - 1].k.idx + 1;
        if (idx < 0)
            return 0;
    }
    if (!v->arr && make && make_array(v, 0))
        return 0;
    if (!v->arr)
        return 0;
    a = v->arr;
    pos = arr_pos(v, idx, sub, &found);
    if (found)
        return &a->e[pos].val;
    if (!make)
        return 0;
    if (a->n == a->cap) {
        long cap = a->cap ? a->cap * 2 : 4;
        sh_elem *t = (sh_elem *)realloc(a->e, (size_t)cap * sizeof(sh_elem));
        if (!t)
            return 0;
        a->e = t;
        a->cap = cap;
    }
    memmove(a->e + pos + 1, a->e + pos, (size_t)(a->n - pos) * sizeof(sh_elem));
    if (is_assoc(v))
        a->e[pos].k.key = sdup(sub);
    else
        a->e[pos].k.idx = idx;
    a->e[pos].val = 0;
    a->n++;
    return &a->e[pos].val;
}

const char *sh_var_str(const sh_var *v)
{
    int f;
    long p;
    if (!v)
        return 0;
    if (!v->arr)
        return v->sv;
    p = arr_pos(v, 0, "0", &f);
    return f ? v->arr->e[p].val : 0;
}

const char *sh_get(const sh_ctx *c, const char *name)
{
    return sh_var_str(sh_lookup(c, name));
}

const char *sh_get_elem(sh_ctx *c, const char *name, const char *sub)
{
    sh_var *v = sh_lookup(c, name);
    char **s;
    if (!v)
        return 0;
    if (!v->arr) {
        const char *err = 0;
        return sh_arith(c, sub, &err) == 0 && !err ? v->sv : 0;
    }
    s = elem_slot(c, v, sub, 0);
    return s ? *s : 0;
}

char **sh_values(const sh_ctx *c, const char *name, long *n)
{
    sh_var *v = sh_lookup(c, name);
    long i, k = 0, cnt = v ? (v->arr ? v->arr->n : 1) : 0;
    char **r = (char **)malloc((size_t)(cnt + 1) * sizeof(char *));
    if (!r) {
        *n = 0;
        return 0;
    }
    if (v && !v->arr && v->sv)
        r[k++] = v->sv;
    else if (v && v->arr)
        for (i = 0; i < v->arr->n; i++)
            if (v->arr->e[i].val)
                r[k++] = v->arr->e[i].val;
    *n = k;
    return r;
}

void sh_keys(const sh_ctx *c, const char *name, sh_list *out)
{
    sh_var *v = sh_lookup(c, name);
    long i;
    char d[24];
    if (!v || (!v->arr && !v->sv))
        return;
    if (!v->arr) {
        sh_list_add(out, "0");
        return;
    }
    for (i = 0; i < v->arr->n; i++) {
        if (is_assoc(v))
            sh_list_add(out, v->arr->e[i].k.key);
        else {
            sh_ltoa(v->arr->e[i].k.idx, d);
            sh_list_add(out, d);
        }
    }
}

void sh_ltoa(sh_int v, char *out)
{
    char d[24];
    int n = 0, k = 0;
    sh_uint u = v < 0 ? (sh_uint)0 - (sh_uint)v : (sh_uint)v;
    if (v < 0)
        out[k++] = '-';
    do
        d[n++] = (char)('0' + u % 10);
    while ((u /= 10) > 0);
    while (n)
        out[k++] = d[--n];
    out[k] = 0;
}

/* value as the attributes of v make it: integer (evaluated), upper, lower; malloc'ed */
static char *conv(sh_ctx *c, const sh_var *v, const char *value)
{
    char *t = sdup(value), *q;
    if (t && (v->attr & SH_ATTR_INTEGER)) {
        const char *err = 0;
        sh_int n = sh_arith(c, value, &err);
        char d[24];
        sh_ltoa(err ? 0 : n, d);
        free(t);
        t = sdup(d);
    }
    for (q = t; q && *q; q++)
        if ((v->attr & SH_ATTR_UPPER) && *q >= 'a' && *q <= 'z')
            *q -= 32;
        else if ((v->attr & SH_ATTR_LOWER) && *q >= 'A' && *q <= 'Z')
            *q += 32;
    return t;
}

int sh_assign(sh_ctx *c, const char *name, const char *sub, const char *value, int append)
{
    sh_var *v;
    char **slot, *nv;
    name = resolve(c, name);
    if (!*name)
        return 1;
    v = find(c, name);
    if (v && (v->attr & SH_ATTR_READONLY))
        return 1;
    if (v && !(v = find_w(c, name)))
        return 1; /* no memory to copy the parent's variable */
    if (v)
        v->attr &= (unsigned short)~SH_ATTR_NOVALUE;
    if (!v) {
        unlink_own(c, name); /* a tombstone of the name */
        v = (sh_var *)calloc(1, sizeof(sh_var));
        if (!v)
            return 1;
        v->name = sdup(name);
        v->next = c->vars;
        c->vars = v;
    }
    if (sub || v->arr) {
        slot = elem_slot(c, v, sub, 1);
        if (!slot)
            return 1;
        SH_HIT(ARRAY_ELEM_SET);
    } else
        slot = &v->sv;
    if (append && *slot) {
        if (v->attr & SH_ATTR_INTEGER) {
            const char *e = 0;
            char d[24];
            sh_ltoa(sh_arith(c, *slot, &e) + sh_arith(c, value, &e), d);
            nv = sdup(d);
        } else {
            nv = (char *)malloc(strlen(*slot) + strlen(value) + 1);
            if (nv) {
                strcpy(nv, *slot);
                strcat(nv, value);
            }
            if (nv) {
                char *t = conv(c, v, nv);
                free(nv);
                nv = t;
            }
        }
    } else
        nv = conv(c, v, value);
    if (!nv)
        return 1;
    free(*slot);
    *slot = nv;
    if (c->allexport)
        v->attr |= SH_ATTR_EXPORT;
    if (c->on_assign && !sub)
        c->on_assign(c, name, value);
    return 0;
}

int sh_set(sh_ctx *c, const char *name, const char *value)
{
    return sh_assign(c, name, 0, value, 0);
}

unsigned sh_attr(const sh_ctx *c, const char *name)
{
    sh_var *v = find(c, name);
    return v ? v->attr : 0;
}

void sh_attr_change(sh_ctx *c, const char *name, unsigned set, unsigned clear)
{
    sh_var *v = find_w(c, name);
    if (!v) {
        sh_set(c, name, "");
        v = find_w(c, name);
        if (v && (set & (SH_ATTR_ARRAY | SH_ATTR_ASSOC))) {
            free(v->sv);
            v->sv = 0;
            v->attr |= SH_ATTR_NOVALUE;
        }
    }
    if (!v)
        return;
    if ((set & (SH_ATTR_ARRAY | SH_ATTR_ASSOC)) && !v->arr)
        make_array(v, (set & SH_ATTR_ASSOC) != 0);
    set &= ~(unsigned)(v->arr ? SH_ATTR_ARRAY | SH_ATTR_ASSOC : 0);
    clear &= ~(unsigned)(v->arr ? SH_ATTR_ARRAY | SH_ATTR_ASSOC : 0); /* an array keeps its kind */
    v->attr = (unsigned short)((v->attr & ~clear) | set);
}

long sh_next_index(const sh_ctx *c, const char *name)
{
    const sh_var *v = sh_lookup(c, name);
    if (!v)
        return 0;
    if (!v->arr)
        return v->sv ? 1 : 0;
    if (is_assoc(v))
        return v->arr->n ? 1 : 0; /* as before the union: an associative element's idx was 0 */
    return v->arr->n ? v->arr->e[v->arr->n - 1].k.idx + 1 : 0;
}

int sh_array_reset(sh_ctx *c, const char *name, int assoc)
{
    sh_var *v;
    name = resolve(c, name);
    if (!*name)
        return 1;
    v = find(c, name);
    if (v && (v->attr & SH_ATTR_READONLY))
        return 1;
    if (v && !(v = find_w(c, name)))
        return 1;
    if (v)
        v->attr &= (unsigned short)~SH_ATTR_NOVALUE;
    if (v && v->arr && is_assoc(v) == assoc) {
        long i;
        for (i = 0; i < v->arr->n; i++)
            elem_free(v, v->arr->e + i);
        v->arr->n = 0;
        return 0;
    }
    if (v) {
        unsigned short keep = (unsigned short)(v->attr & ~(SH_ATTR_ARRAY | SH_ATTR_ASSOC));
        free(v->sv);
        v->sv = 0;
        if (v->arr) {
            long i;
            for (i = 0; i < v->arr->n; i++)
                elem_free(v, v->arr->e + i);
            free(v->arr->e);
            free(v->arr);
            v->arr = 0;
        }
        v->attr = keep;
    } else {
        sh_attr_change(c, name, assoc ? SH_ATTR_ASSOC : SH_ATTR_ARRAY, 0);
        v = find_w(c, name);
        if (v)
            v->attr &= (unsigned short)~SH_ATTR_NOVALUE;
        return 0;
    }
    return make_array(v, assoc);
}

sh_var *sh_var_copy(const sh_var *v)
{
    sh_var *r = (sh_var *)calloc(1, sizeof(sh_var));
    if (!r)
        return 0;
    r->name = sdup(v->name);
    r->attr = v->attr;
    if (v->sv)
        r->sv = sdup(v->sv);
    if (v->arr) {
        long i;
        r->arr = (sh_arr *)calloc(1, sizeof(sh_arr));
        if (!r->arr)
            return r;
        r->arr->e = (sh_elem *)calloc((size_t)(v->arr->n ? v->arr->n : 1), sizeof(sh_elem));
        if (!r->arr->e)
            return r;
        r->arr->n = r->arr->cap = v->arr->n;
        for (i = 0; i < v->arr->n; i++) {
            if (is_assoc(v))
                r->arr->e[i].k.key = sdup(v->arr->e[i].k.key);
            else
                r->arr->e[i].k.idx = v->arr->e[i].k.idx;
            if (v->arr->e[i].val)
                r->arr->e[i].val = sdup(v->arr->e[i].val);
        }
    }
    return r;
}

/* remove the variable NAME whatever its attributes; the base's is hidden by a tombstone */
static void drop(sh_ctx *c, const char *name)
{
    sh_var *t = 0;
    /* name may be the dropped node's own: the tombstone takes a copy before it goes */
    if (base_find(c, name) && (t = (sh_var *)calloc(1, sizeof(sh_var))) != 0 && !(t->name = sdup(name))) {
        free(t);
        t = 0;
    }
    unlink_own(c, t ? t->name : name);
    if (t) {
        t->attr = SH_ATTR_GONE;
        t->next = c->vars;
        c->vars = t;
    }
}

sh_var *sh_var_save(const sh_ctx *c, const char *name)
{
    const sh_var *v = find(c, name);
    return v ? sh_var_copy(v) : 0;
}

void sh_var_link(sh_ctx *c, sh_var *v)
{
    unlink_own(c, v->name);
    v->next = c->vars;
    c->vars = v;
}

void sh_var_discard(sh_var *saved)
{
    if (saved)
        var_free(saved);
}

void sh_var_restore(sh_ctx *c, const char *name, sh_var *saved)
{
    if (saved) {
        if (saved->arr)
            SH_HIT(LOCAL_RESTORE_ARRAY);
        sh_var_link(c, saved);
    } else
        drop(c, name);
}

void sh_pstat(sh_ctx *c, const long *st, int n)
{
    long *t;
    if (n == 1 && c->npstat == 1) {
        c->pstat[0] = st[0];
        return;
    }
    t = (long *)realloc(c->pstat, (size_t)(n ? n : 1) * sizeof(long));
    if (!t)
        return;
    memcpy(t, st, (size_t)n * sizeof(long));
    c->pstat = t;
    c->npstat = n;
}

/* a byte bash leaves unquoted */
static int q_safe(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch >= 128 ||
           (ch && strchr("_-./:,@%+=", ch));
}

/* bash's quoting: see sh_expand.h */
char *sh_quote(const char *s, int style)
{
    size_t n = strlen(s), k = 0, i;
    char *o = (char *)malloc(n * 4 + 8);
    int ctl = 0, plain = 1;
    if (!o)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32 || ch == 127)
            ctl = 1;
        if (!q_safe(ch))
            plain = 0;
    }
    if (!n) {
        strcpy(o, "''");
        return o;
    }
    if (!ctl && plain && style != SH_Q_ALWAYS)
        return strcpy(o, s);
    if (ctl) {
        o[k++] = '$';
        o[k++] = '\'';
        for (i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
            else if (ch == '\t') { o[k++] = '\\'; o[k++] = 't'; }
            else if (ch == '\r') { o[k++] = '\\'; o[k++] = 'r'; }
            else if (ch == 27) { o[k++] = '\\'; o[k++] = 'E'; }
            else if (ch == '\'' || ch == '\\') { o[k++] = '\\'; o[k++] = (char)ch; }
            else if (ch < 32 || ch == 127) { o[k++] = '\\'; o[k++] = (char)('0' + (ch >> 6)); o[k++] = (char)('0' + ((ch >> 3) & 7)); o[k++] = (char)('0' + (ch & 7)); }
            else o[k++] = (char)ch;
        }
        o[k++] = '\'';
    } else if (style == SH_Q_BACKSLASH) {
        for (i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (!q_safe(ch) || ch == '\'')
                o[k++] = '\\';
            o[k++] = (char)ch;
        }
    } else {
        o[k++] = '\'';
        for (i = 0; i < n; i++) {
            if (s[i] == '\'') { o[k++] = '\''; o[k++] = '\\'; o[k++] = '\''; }
            o[k++] = s[i];
        }
        o[k++] = '\'';
    }
    o[k] = 0;
    return o;
}

/* "text" with \ " $ ` escaped, as declare -p prints a value ($'..' when it holds a control character) */
char *sh_dquote(const char *t)
{
    size_t n = strlen(t), k = 0, i;
    char *o;
    for (i = 0; i < n; i++)
        if ((unsigned char)t[i] < 32 || t[i] == 127)
            return sh_quote(t, SH_Q_SINGLE);
    o = (char *)malloc(n * 2 + 3);
    if (!o)
        return 0;
    o[k++] = '"';
    for (i = 0; i < n; i++) {
        if (strchr("\"\\$`", t[i]))
            o[k++] = '\\';
        o[k++] = t[i];
    }
    o[k++] = '"';
    o[k] = 0;
    return o;
}

int sh_unset(sh_ctx *c, const char *name)
{
    sh_var *v = sh_lookup(c, name);
    if (!v)
        return 0;
    if (v->attr & SH_ATTR_READONLY)
        return 1;
    drop(c, v->name);
    return 0;
}

int sh_unset_elem(sh_ctx *c, const char *name, const char *sub)
{
    sh_var *v = sh_lookup(c, name);
    char **slot;
    long pos;
    if (!v)
        return 0;
    if (v->attr & SH_ATTR_READONLY)
        return 1;
    if (!v->arr) {
        const char *err = 0;
        if (sh_arith(c, sub, &err) == 0 && !err)
            drop(c, v->name);
        return 0;
    }
    if (!(v = find_w(c, v->name)))
        return 1;
    slot = elem_slot(c, v, sub, 0);
    if (!slot)
        return 0;
    pos = (sh_elem *)((char *)slot - offsetof(sh_elem, val)) - v->arr->e;
    elem_free(v, v->arr->e + pos);
    memmove(v->arr->e + pos, v->arr->e + pos + 1, (size_t)(v->arr->n - pos - 1) * sizeof(sh_elem));
    v->arr->n--;
    return 0;
}

void sh_export(sh_ctx *c, const char *name)
{
    sh_var *v = find_w(c, name);
    if (!v) {
        sh_set(c, name, "");
        v = find_w(c, name);
    }
    if (v)
        v->attr |= SH_ATTR_EXPORT;
}

void sh_ctx_free(sh_ctx *c)
{
    c->base = 0; /* the parent's: never freed here */
    while (c->vars) {
        drop(c, c->vars->name);
    }
    free(c->pstat);
    c->pstat = 0;
    sh_list_free(&c->args);
}

/* ---- pattern matching ---------------------------------------------------------- */

static int lower(int ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}

/* [:name:] at name (after the "[:"): 1 when ch is in the class, 0 when not, -1 when name is no class
 * (the [ is then an ordinary member). The C locale: ASCII only. nocase: upper and lower are both letters. */
static int posix_class(const char *name, int ch, int nocase)
{
    static const char *const names[] = { "alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct",
                                         "print", "graph", "cntrl", "xdigit" };
    int k, n = 0, up = ch >= 'A' && ch <= 'Z', lo = ch >= 'a' && ch <= 'z', dg = ch >= '0' && ch <= '9';
    const char *e = strstr(name, ":]");
    (void)nocase;
    if (!e)
        return -1;
    n = (int)(e - name);
    for (k = 0; k < 12; k++)
        if ((int)strlen(names[k]) == n && !strncmp(names[k], name, (size_t)n))
            break;
    switch (k) {
    case 0: return up || lo;
    case 1: return dg;
    case 2: return up || lo || dg;
    case 3: return up;   /* bash: nocaseglob and nocasematch do not fold the classes */
    case 4: return lo;
    case 5: return ch == ' ' || (ch >= 9 && ch <= 13);
    case 6: return ch == ' ' || ch == '\t';
    case 7: return ch > 32 && ch < 127 && !(up || lo || dg);
    case 8: return ch >= 32 && ch < 127;
    case 9: return ch > 32 && ch < 127;
    case 10: return (ch >= 0 && ch < 32) || ch == 127;
    case 11: return dg || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    default: return -1;
    }
}


/* p[0] is the "(" of an extended pattern: the index of its matching ")" (nested ( ), [ ], \ skipped), or -1 */
static long ext_close(const char *p)
{
    long i;
    int depth = 0;
    for (i = 0; p[i]; i++) {
        if (p[i] == '\\' && p[i + 1])
            i++;
        else if (p[i] == '[') {
            long j = i + 1;
            if (p[j] == '!' || p[j] == '^')
                j++;
            if (p[j] == ']')
                j++;
            while (p[j] && p[j] != ']')
                j++;
            if (p[j])
                i = j;
        } else if (p[i] == '(')
            depth++;
        else if (p[i] == ')' && --depth == 0)
            return i;
    }
    return -1;
}

static int match_flags(const char *p, const char *s, int flags);

/* kind ( alt | alt ) rest against s: kind is one of @ ? * + ! (p points after the "(", len = its text up to ")") */
static int ext_match(char kind, const char *alts, long len, const char *rest, const char *s, int flags)
{
    char *a = (char *)malloc((size_t)len + 1), *buf;
    long n = (long)strlen(s), k;
    int res = 0, na = 0, i;
    char *alt[64];
    int depth = 0;
    long j, from = 0;
    if (!a)
        return 0;
    SH_HIT(EXTGLOB);
    memcpy(a, alts, (size_t)len);
    a[len] = 0;
    alt[na++] = a;
    for (j = 0; j < len && na < 64; j++) {
        if (a[j] == '\\' && a[j + 1])
            j++;
        else if (a[j] == '(')
            depth++;
        else if (a[j] == ')')
            depth--;
        else if (a[j] == '|' && !depth) {
            a[j] = 0;
            alt[na++] = a + j + 1;
        }
    }
    (void)from;
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        free(a);
        return 0;
    }
    memcpy(buf, s, (size_t)n + 1);
    if ((kind == '?' || kind == '*') && match_flags(rest, s, flags)) {
        res = 1;
        goto done;
    }
    for (k = 0; k <= n && !res; k++) {
        char save = buf[k];
        int any = 0;
        buf[k] = 0;
        for (i = 0; i < na && !any; i++)
            any = match_flags(alt[i], buf, flags);
        buf[k] = save;
        if (kind == '!') {
            if (!any && match_flags(rest, s + k, flags))
                res = 1;
        } else if (any) {
            if (match_flags(rest, s + k, flags))
                res = 1;
            else if ((kind == '*' || kind == '+') && k > 0 && ext_match('*', alts, len, rest, s + k, flags))
                res = 1;
        }
    }
done:
    free(buf);
    free(a);
    return res;
}

int sh_match(const char *p, const char *s, int flags)
{
    return match_flags(p, s, flags);
}

static int match_flags(const char *p, const char *s, int flags)
{
    int nocase = (flags & SH_MATCH_NOCASE) != 0;
    for (; *p; p++, s++) {
        if ((flags & SH_MATCH_EXTGLOB) && strchr("@?*+!", *p) && p[1] == '(') {
            long e = ext_close(p + 1);
            if (e >= 0)
                return ext_match(*p, p + 2, e - 1, p + 1 + e + 1, s, flags);
        }
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (match_flags(p + 1, s, flags))
                    return 1;
            return match_flags(p + 1, s, flags);
        }
        if (!*s)
            return 0;
        if (*p == '?')
            continue;
        if (*p == '[') {
            const char *q = p + 1;
            int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') {
                neg = 1;
                q++;
            }
            do {
                int lo = *q, hi = *q;
                if (!*q)
                    return 0; /* no ]: not a bracket; treat as unmatched */
                if (*q == '[' && q[1] == ':') {
                    int cl = posix_class(q + 2, *s, nocase);
                    if (cl >= 0) {
                        if (cl)
                            ok = 1;
                        q = strstr(q + 2, ":]") + 2;
                        continue;
                    }
                }
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    hi = q[2];
                    q += 2;
                }
                if (nocase ? (lower(*s) >= lower(lo) && lower(*s) <= lower(hi))
                           : (*s >= lo && *s <= hi))
                    ok = 1;
                q++;
            } while (*q != ']');
            if (ok == neg)
                return 0;
            p = q;
            continue;
        }
        if (*p == '\\' && p[1])
            p++;
        if (nocase ? lower(*p) != lower(*s) : *p != *s)
            return 0;
    }
    return !*s;
}

/* ---- arithmetic ---------------------------------------------------------------- */

/* The end of the $( ), $(( )) or ${ } at s[i] (on the $), s[0..len) being the text: quotes and nested
 * constructs skipped. Returns the index after its closing bracket, -1 when the text ends inside it.
 * dq: the construct sits inside double quotes (then a single quote in ${ } is a plain character). A $( )
 * body is read for the `case x in a) ... esac` patterns, whose ) is not the end. */
#define SK(k) ((k) < len ? s[k] : 0)
long sh_skip_sub(const char *s, long i, long len, int dq)
{
    char open = SK(i + 1), close = open == '(' ? ')' : '}';
    int depth = 1, pend = 0, incase = 0;
    i += 2;
    while (SK(i) && depth) {
        char c = s[i];
        if (c == '\\' && SK(i + 1))
            i++;
        else if (c == '\'' && !(dq && open == '{')) {
            for (i++; SK(i) && s[i] != '\''; i++)
                ;
            if (!SK(i))
                return -1;
        } else if (c == '"') {
            for (i++; SK(i) && s[i] != '"'; i++) {
                if (s[i] == '\\' && SK(i + 1))
                    i++;
                else if (s[i] == '$' && (SK(i + 1) == '(' || SK(i + 1) == '{')) {
                    i = sh_skip_sub(s, i, len, 1);
                    if (i < 0)
                        return -1;
                    i--;
                }
            }
            if (!SK(i))
                return -1;
        } else if (c == '`') {
            for (i++; SK(i) && s[i] != '`'; i++)
                if (s[i] == '\\' && SK(i + 1))
                    i++;
            if (!SK(i))
                return -1;
        } else if (c == '$' && (SK(i + 1) == '(' || SK(i + 1) == '{')) {
            i = sh_skip_sub(s, i, len, dq && open == '{');
            if (i < 0)
                return -1;
            continue;
        } else if (c == open) {
            depth++;
        } else if (c == close) {
            if (open == '(' && incase && depth == 1) {
                /* the ) ending a case pattern */
            } else
                depth--;
        } else if (open == '(' && ((c >= 'a' && c <= 'z') || c == '_') &&
                   (i == 0 || !((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                                (s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '_' || s[i - 1] == '$'))) {
            long e = i;
            while ((SK(e) >= 'a' && SK(e) <= 'z') || SK(e) == '_')
                e++;
            if (!((SK(e) >= 'A' && SK(e) <= 'Z') || (SK(e) >= '0' && SK(e) <= '9'))) {
                if (e - i == 4 && !strncmp(s + i, "case", 4))
                    pend++;
                else if (e - i == 2 && !strncmp(s + i, "in", 2) && pend) {
                    pend--;
                    incase++;
                } else if (e - i == 4 && !strncmp(s + i, "esac", 4) && incase)
                    incase--;
            }
            i = e - 1;
        }
        if (SK(i))
            i++;
    }
    return depth ? -1 : i;
}
#undef SK

/* Arithmetic: 64-bit (sh_int), bash's operator set by precedence climbing. One aval is the value of an
 * operand and, while it is a bare variable or element, its name for the assignment operators. */
typedef struct aval {
    sh_int v;
    int lv;
    char name[64];
    char sub[96];
} aval;

typedef struct arith {
    sh_ctx *c;
    const char *s;
    const char *err;
    int skip;    /* in the branch a short circuit or ?: does not take: no store, no division error */
    int depth;
} arith;

static const char *const a_lvl[] = {"||", "&&", "|", "^", "&", "== !=", "< <= > >=", "<< >>", "+ -", "* / %"};
static const char *const a_ops[] = {"<<=", ">>=", "&&", "||", "**", "<=", ">=", "==", "!=", "<<", ">>", "+=", "-=",
                                    "*=", "/=", "%=", "&=", "^=", "|=", "++", "--", "?", ":", ",", "=", "<", ">",
                                    "+", "-", "*", "/", "%", "&", "^", "|", "!", "~", 0};

static void a_assign(arith *a, aval *x);
static void a_cond(arith *a, aval *x);
static void a_comma(arith *a, aval *x);

static void a_space(arith *a)
{
    while (*a->s == ' ' || *a->s == '\t' || *a->s == '\n')
        a->s++;
}

static int a_isname(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

/* the operator at the cursor (longest match), 0 when there is none */
static const char *a_tok(arith *a)
{
    int i;
    a_space(a);
    for (i = 0; a_ops[i]; i++)
        if (!strncmp(a->s, a_ops[i], strlen(a_ops[i])))
            return a_ops[i];
    return 0;
}

static int a_in(const char *t, const char *list)
{
    size_t n = strlen(t);
    while (*list) {
        if (!strncmp(list, t, n) && (list[n] == ' ' || !list[n]))
            return 1;
        while (*list && *list != ' ')
            list++;
        while (*list == ' ')
            list++;
    }
    return 0;
}

static int a_digit(char ch, int base)
{
    int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 10
          : ch >= 'A' && ch <= 'Z' ? (base > 36 ? ch - 'A' + 36 : ch - 'A' + 10)
          : ch == '@' ? 62 : ch == '_' ? 63 : 99;
    return d < base ? d : -1;
}

static sh_int a_num(arith *a)
{
    sh_uint v = 0;
    int base = 10, d;
    const char *s = a->s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (s[0] == '0' && s[1])
        base = 8;
    for (;; s++) {
        if (base == 10 && *s == '#' && v >= 2 && v <= 64 && s != a->s) {
            base = (int)v;
            v = 0;
            s++;
            if (a_digit(*s, base) < 0)
                a->err = "arithmetic: invalid number";
            for (; (d = a_digit(*s, base)) >= 0; s++)
                v = v * (sh_uint)base + (sh_uint)d;
            break;
        }
        d = a_digit(*s, base);
        if (d < 0)
            break;
        v = v * (sh_uint)base + (sh_uint)d;
    }
    if (!a->err && (a_isname(*s) || (*s >= '0' && *s <= '9') || *s == '@'))
        a->err = "arithmetic: value too great for base";
    a->s = s;
    return (sh_int)v;
}

static sh_int a_value(arith *a, const char *val);

static void a_load(arith *a, aval *x)
{
    const char *val;
    x->v = 0;
    if (a->skip)
        return; /* not evaluated (the right side of a short circuit): no side effects */
    val = x->sub[0] ? sh_get_elem(a->c, x->name, x->sub) : sh_get(a->c, x->name);
    if (val && *val)
        x->v = a_value(a, val);
}

static sh_int a_value(arith *a, const char *val)
{
    arith b;
    sh_int v;
    if (a->depth >= 20) {
        a->err = "arithmetic: expression recursion level exceeded";
        return 0;
    }
    b.c = a->c;
    b.s = val;
    b.err = 0;
    b.skip = a->skip;
    b.depth = a->depth + 1;
    {
        aval x;
        a_comma(&b, &x);
        v = x.v;
    }
    a_space(&b);
    if (!b.err && *b.s)
        b.err = "arithmetic: syntax error: invalid arithmetic operator";
    if (b.err)
        a->err = b.err;
    return v;
}

static void a_store(arith *a, aval *x, sh_int v)
{
    char d[24];
    x->v = v;
    if (a->skip || a->err)
        return;
    sh_ltoa(v, d);
    SH_HIT(ARITH_ASSIGN);
    if (sh_assign(a->c, x->name, x->sub[0] ? x->sub : 0, d, 0))
        a->err = "arithmetic: cannot assign to the variable";
}

static void a_primary(arith *a, aval *x)
{
    x->lv = 0;
    x->v = 0;
    a_space(a);
    if (*a->s == '(') {
        a->s++;
        a_comma(a, x);
        a_space(a);
        if (*a->s == ')')
            a->s++;
        else if (!a->err)
            a->err = "arithmetic: ) is missing";
        x->lv = 0;
        return;
    }
    if (*a->s == '$')
        a->s++;
    if (*a->s >= '0' && *a->s <= '9') {
        x->v = a_num(a);
        return;
    }
    if (a_isname(*a->s)) {
        int k = 0;
        while ((a_isname(*a->s) || (*a->s >= '0' && *a->s <= '9')) && k < 63)
            x->name[k++] = *a->s++;
        x->name[k] = 0;
        x->sub[0] = 0;
        if (*a->s == '[') {
            int m = 0, d = 0;
            a->s++;
            while (*a->s && (*a->s != ']' || d) && m < 95) {
                d += *a->s == '[' ? 1 : *a->s == ']' ? -1 : 0;
                x->sub[m++] = *a->s++;
            }
            x->sub[m] = 0;
            if (*a->s == ']')
                a->s++;
            else
                a->err = "arithmetic: ] is missing";
            /* an indexed array's subscript is evaluated once, here (a[i++] += 1 steps i once):
             * the lvalue then carries the number */
            if (!a->err && !a->skip) {
                sh_var *v = sh_lookup(a->c, x->name);
                if (!v || !is_assoc(v)) {
                    const char *serr = 0;
                    sh_int idx = sh_arith(a->c, x->sub, &serr);
                    if (serr)
                        a->err = serr;
                    else
                        sh_ltoa(idx, x->sub);
                }
            }
        }
        x->lv = 1;
        a_load(a, x);
        return;
    }
    if (!a->err)
        a->err = *a->s ? "arithmetic: syntax error: operand expected" : "arithmetic: a number is missing";
}

static void a_unary(arith *a, aval *x)
{
    const char *t = a_tok(a);
    if (t && (!strcmp(t, "++") || !strcmp(t, "--"))) {
        a->s += 2;
        a_unary(a, x);
        if (!x->lv) {   /* ++3 is +(+3), --3 is -(-3) */
            if (t[0] == '-')
                x->v = (sh_int)(0 - (sh_uint)x->v);
            return;
        }
        a_store(a, x, (sh_int)((sh_uint)x->v + (sh_uint)(t[0] == '+' ? 1 : -1)));
        return;
    }
    if (t && (!strcmp(t, "+") || !strcmp(t, "-") || !strcmp(t, "!") || !strcmp(t, "~"))) {
        a->s++;
        a_unary(a, x);
        x->lv = 0;
        x->v = t[0] == '-' ? (sh_int)(0 - (sh_uint)x->v) : t[0] == '!' ? x->v == 0 : t[0] == '~' ? ~x->v : x->v;
        return;
    }
    a_primary(a, x);
    t = a_tok(a);
    if (x->lv && t && (!strcmp(t, "++") || !strcmp(t, "--"))) {
        sh_int old = x->v;
        a->s += 2;
        a_store(a, x, (sh_int)((sh_uint)old + (sh_uint)(t[0] == '+' ? 1 : -1)));
        x->v = old;
        x->lv = 0;
    }
}

static sh_int a_pow(arith *a, sh_int b, sh_int e)
{
    sh_uint r = 1, p = (sh_uint)b;
    if (e < 0) {
        if (!a->err)
            a->err = "arithmetic: exponent less than 0";
        return 0;
    }
    for (; e; e >>= 1, p *= p)
        if (e & 1)
            r *= p;
    return (sh_int)r;
}

static void a_exp(arith *a, aval *x)
{
    const char *t;
    a_unary(a, x);
    t = a_tok(a);
    if (t && !strcmp(t, "**")) {
        aval y;
        a->s += 2;
        x->lv = 0;
        a_exp(a, &y);
        x->v = a_pow(a, x->v, y.v);
    }
}

static sh_int a_op2(arith *a, const char *t, sh_int l, sh_int r)
{
    sh_uint ul = (sh_uint)l, ur = (sh_uint)r;
    sh_int min = (sh_int)((sh_uint)1 << 63);
    switch (t[0]) {
    case '+': return (sh_int)(ul + ur);
    case '-': return (sh_int)(ul - ur);
    case '*': return (sh_int)(ul * ur);
    case '/':
    case '%':
        if (!r) {
            if (!a->skip && !a->err)
                a->err = "arithmetic: division by zero";
            return 0;
        }
        if (l == min && r == -1)
            return t[0] == '/' ? min : 0;
        return t[0] == '/' ? l / r : l % r;
    case '&': return l & r;
    case '|': return l | r;
    case '^': return l ^ r;
    case '<':
        if (t[1] == '<')
            return (sh_int)(ul << (ur & 63));
        return t[1] == '=' ? l <= r : l < r;
    case '>':
        if (t[1] == '>')
            return l >> (ur & 63);
        return t[1] == '=' ? l >= r : l > r;
    case '=': return l == r;
    case '!': return l != r;
    }
    return 0;
}

static void a_bin(arith *a, int l, aval *x)
{
    if (l == 10) {
        a_exp(a, x);
        return;
    }
    a_bin(a, l + 1, x);
    for (;;) {
        const char *t = a_tok(a);
        aval y;
        int sk = 0;
        if (l == 8 && t && (!strcmp(t, "++") || !strcmp(t, "--")))
            t = t[0] == '+' ? "+" : "-";   /* 5--3 is 5 - -3 */
        if (a->err || !t || !a_in(t, a_lvl[l]))
            return;
        a->s += strlen(t);
        x->lv = 0;
        if (l < 2) {
            sk = l == 0 ? x->v != 0 : x->v == 0;
            if (sk)
                a->skip++;
            a_bin(a, l + 1, &y);
            if (sk)
                a->skip--;
            x->v = l == 0 ? (x->v != 0 || y.v != 0) : (x->v != 0 && y.v != 0);
        } else {
            a_bin(a, l + 1, &y);
            x->v = a_op2(a, t, x->v, y.v);
        }
    }
}

static void a_cond(arith *a, aval *x)
{
    const char *t;
    a_bin(a, 0, x);
    t = a_tok(a);
    if (!a->err && t && !strcmp(t, "?")) {
        int c = x->v != 0;
        aval y, z;
        a->s++;
        if (!c)
            a->skip++;
        a_assign(a, &y);
        if (!c)
            a->skip--;
        t = a_tok(a);
        if (!a->err && !(t && !strcmp(t, ":"))) {
            a->err = "arithmetic: ':' expected for conditional expression";
            return;
        }
        a->s++;
        if (c)
            a->skip++;
        a_cond(a, &z);
        if (c)
            a->skip--;
        x->v = c ? y.v : z.v;
        x->lv = 0;
    }
}

static void a_assign(arith *a, aval *x)
{
    const char *t;
    a_cond(a, x);
    t = a_tok(a);
    if (!a->err && t && t[strlen(t) - 1] == '=' && strcmp(t, "==") && strcmp(t, "!=") && strcmp(t, "<=") &&
        strcmp(t, ">=")) {
        aval y;
        char op[4];
        sh_int r;
        if (!x->lv) {
            a->err = "arithmetic: attempted assignment to non-variable";
            return;
        }
        a->s += strlen(t);
        strcpy(op, t);
        a_assign(a, &y);
        op[strlen(op) - 1] = 0;
        r = !op[0] ? y.v : a_op2(a, op, x->v, y.v);
        a_store(a, x, r);
    }
}

static void a_comma(arith *a, aval *x)
{
    const char *t;
    a_assign(a, x);
    while (!a->err && (t = a_tok(a)) != 0 && !strcmp(t, ",")) {
        a->s++;
        a_assign(a, x);
    }
}

sh_int sh_arith(sh_ctx *c, const char *expr, const char **err)
{
    arith a;
    aval x;
    a.c = c;
    a.s = expr;
    a.err = 0;
    a.skip = 0;
    a.depth = 0;
    a_space(&a);
    if (!*a.s) {
        if (err)
            *err = 0;
        return 0;
    }
    a_comma(&a, &x);
    a_space(&a);
    if (!a.err && *a.s)
        a.err = "arithmetic: syntax error: invalid arithmetic operator";
    if (err)
        *err = a.err;
    return a.err ? 0 : x.v;
}

/* ---- the expansion buffer --------------------------------------------------- */

/* Each character carries where it came from: */
#define F_QUOTED 1   /* inside quotes, or escaped: never split, never a glob character */
#define F_SPLIT  2   /* the result of an unquoted expansion: split on IFS */
#define F_BREAK  4   /* "$@": a field ends after this character */
#define F_EMPTY  8   /* placeholder: an empty field of "$@" or "${a[@]}"; it adds no text */

typedef struct cbuf {
    char *s;
    unsigned char *f;
    int n, cap;
    int had_quotes;     /* the word had quoting: an empty result is still one field */
    int force_field;    /* "$@" with no arguments must give no field; with some, one each */
} cbuf;

static void cput(cbuf *b, char ch, int flags)
{
    if (b->n + 1 >= b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        char *s = (char *)realloc(b->s, cap);
        unsigned char *f = (unsigned char *)realloc(b->f, cap);
        if (!s || !f)
            return;
        b->s = s;
        b->f = f;
        b->cap = cap;
    }
    b->s[b->n] = ch;
    b->f[b->n++] = (unsigned char)flags;
    b->s[b->n] = 0;
}

static void cputs(cbuf *b, const char *s, int flags)
{
    while (*s)
        cput(b, *s++, flags);
}

static void cfree(cbuf *b)
{
    free(b->s);
    free(b->f);
}

typedef struct ex {
    sh_ctx *c;
    const char *err;
    int assign;         /* an assignment value: ~ also after a colon */
    char pnum[24];      /* param's number ($? $$ $! $# PIPESTATUS), pval's: here, not static (vsh's */
    char vnum[24];      /* processes share its static data: a subshell's $? overwrote the shell's) */
} ex;

static int expand_into(ex *e, const char *w, long len, cbuf *b, int dquote);

/* Expand text (an operand of ${x:-text}) to a plain string. */
static char *expand_string(ex *e, const char *w, long len, int dquote)
{
    cbuf b;
    char *r;
    memset(&b, 0, sizeof(b));
    expand_into(e, w, len, &b, dquote);
    r = sdup(b.s ? b.s : "");
    cfree(&b);
    return r;
}

static int is_name_char(char ch, int first)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' ||
           (!first && ch >= '0' && ch <= '9');
}

/* The value of a parameter name ("?", "#", "1", "HOME"); 0 when unset.
 * The result is valid until the next call. */
static const char *param(ex *e, const char *name)
{
    char *num = e->pnum;
    sh_ctx *c = e->c;
    long v;
    int k;
    if (!name[1] && (name[0] == '?' || name[0] == '$' || name[0] == '!' || name[0] == '#')) {
        v = name[0] == '?' ? c->status : name[0] == '$' ? (c->top_pid ? c->top_pid : c->pid) : name[0] == '!' ? c->last_bg : c->args.n;
        if (name[0] == '!' && !c->last_bg)
            return 0;
        k = 0;
        if (v < 0) {
            num[k++] = '-';
            v = -v;
        }
        {
            char d[20];
            int n = 0;
            do
                d[n++] = (char)('0' + v % 10);
            while ((v /= 10) > 0);
            while (n)
                num[k++] = d[--n];
        }
        num[k] = 0;
        return num;
    }
    if (!name[1] && name[0] == '-')
        return c->flags ? c->flags : "";
    if (c->npstat && !strcmp(name, "PIPESTATUS")) {
        sh_ltoa(c->pstat[0], num);
        return num;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        int i = atoi(name);
        if (!i)
            return c->arg0 ? c->arg0 : "vsh";
        return i <= c->args.n ? c->args.v[i - 1] : 0;
    }
    return sh_get(c, name);
}

/* set -u: an unset NAME is an error (not $@ $*) */
static int unbound(ex *e, const char *name, const char *val)
{
    char *msg = e->c->errbuf;
    if (val || !e->c->nounset || !strcmp(name, "@") || !strcmp(name, "*"))
        return 0;
    strncpy(msg, name, 60);
    msg[60] = 0;
    strcat(msg, ": unbound variable");
    e->err = msg;
    return 1;
}

/* One seam for the value list of $@ / $* and of an array NAME[@] (PIPESTATUS included), so the
 * operators that work on lists are written once. */
typedef struct pv {
    char **v;
    long n;
    int own, deep;      /* v is malloc'ed; the strings are too */
} pv;

static void pv_get(ex *e, const char *name, pv *p)
{
    memset(p, 0, sizeof(*p));
    if (name[0] == '@' || name[0] == '*') {
        p->v = e->c->args.v;
        p->n = e->c->args.n;
    } else if (!strcmp(name, "PIPESTATUS") && e->c->npstat) {
        int i;
        char t[24];
        p->v = (char **)calloc((size_t)e->c->npstat, sizeof(char *));
        p->own = p->deep = 1;
        for (i = 0; p->v && i < e->c->npstat; i++) {
            sh_ltoa(e->c->pstat[i], t);
            p->v[p->n++] = sdup(t);
        }
    } else {
        p->v = sh_values(e->c, name, &p->n);
        p->own = 1;
        SH_HIT(PARAM_VALUES_ARRAY);
    }
}

static void pv_free(pv *p)
{
    long i;
    for (i = 0; p->deep && i < p->n; i++)
        free(p->v[i]);
    if (p->own)
        free(p->v);
}

/* $@ and $*, ${a[@]} and ${a[*]}: the values, fields apart ("$@") or joined. */
static void put_pv(ex *e, cbuf *b, const pv *pp, int at, int dquote)
{
    const pv p = *pp;
    long i;
    const char *ifs = sh_get(e->c, "IFS");
    for (i = 0; i < p.n; i++) {
        if (i) {
            if (at && dquote)
                b->f[b->n - 1] |= F_BREAK;
            else if (!dquote)
                cput(b, ' ', F_SPLIT);
            else if (!ifs || *ifs)
                cput(b, ifs ? *ifs : ' ', F_QUOTED);
        }
        if (at && dquote && !p.v[i][0])
            cput(b, ' ', F_QUOTED | F_EMPTY);
        else
            cputs(b, p.v[i], dquote ? F_QUOTED : F_SPLIT);
    }
    if (at && dquote && !p.n)
        b->force_field = -1; /* "$@" of nothing: no field at all */
}

static void put_values(ex *e, cbuf *b, const char *name, int at, int dquote)
{
    pv p;
    pv_get(e, name, &p);
    put_pv(e, b, &p, at, dquote);
    pv_free(&p);
}

/* ---- the parameter operators that rewrite a value: # ## % %% / ^ , ~ ------------------ */

typedef struct sbuf {
    char *s;
    long n, cap;
} sbuf;

static void sb_add(sbuf *b, const char *s, long n)
{
    if (b->n + n + 1 > b->cap) {
        long c = b->cap ? b->cap : 64;
        char *t;
        while (c < b->n + n + 1)
            c *= 2;
        t = (char *)realloc(b->s, (size_t)c);
        if (!t)
            return;
        b->s = t;
        b->cap = c;
    }
    memcpy(b->s + b->n, s, (size_t)n);
    b->n += n;
    b->s[b->n] = 0;
}

/* the length of the longest match of pat at s[from..], or -1; the whole of s[from..to] must match */
static long longest_at(const char *pat, const char *s, long from, long l, char *tmp, int mf)
{
    long j;
    for (j = l; j >= from; j--) {
        memcpy(tmp, s + from, (size_t)(j - from));
        tmp[j - from] = 0;
        if (sh_match(pat, tmp, mf))
            return j - from;
    }
    return -1;
}

/* ${v/pat/rep} ${v//pat/rep} ${v/#pat/rep} ${v/%pat/rep}; an unquoted & in rep (marked \001 by the
 * caller) is the matched text. Zero-length matches are not replaced (as bash). */
static char *subst_one(const char *pat, const char *rep, const char *v, int all, char anchor, int mf)
{
    long l = (long)strlen(v), i = 0;
    sbuf o;
    char *tmp = (char *)malloc((size_t)l + 1), *r;
    memset(&o, 0, sizeof(o));
    if (!tmp)
        return sdup(v);
    sb_add(&o, "", 0);
    if (*pat) {
        while (i <= l) {
            long m = -1, k;
            if (anchor == '%') {
                for (k = 0; k <= l && m < 0; k++)
                    if (sh_match(pat, v + k, mf)) {
                        i = k;
                        m = l - k;
                    }
                if (m < 0)
                    break;
                sb_add(&o, v, i);
            } else
                m = longest_at(pat, v, i, l, tmp, mf);
            if (m > 0 && (anchor != '#' || i == 0)) {
                const char *q;
                for (q = rep; *q; q++) {
                    if (*q == 1)
                        sb_add(&o, v + i, m);
                    else
                        sb_add(&o, q, 1);
                }
                i += m;
                if (anchor || !all)
                    break;
                continue;
            }
            if (anchor == '#' || anchor == '%')
                break;
            if (i < l)
                sb_add(&o, v + i, 1);
            i++;
        }
    }
    if (!o.s || !*pat) {
        free(o.s);
        free(tmp);
        return sdup(v);
    }
    if (i < l)
        sb_add(&o, v + i, l - i);
    free(tmp);
    r = o.s;
    return r;
}

/* ${v^pat} ${v^^pat} ${v,pat} ${v,,pat} ${v~pat} ${v~~pat}: the characters pat matches change case */
static char *case_one(const char *pat, const char *v, char kind, int all, int mf)
{
    long l = (long)strlen(v), i;
    char *r = (char *)malloc((size_t)l + 1);
    if (!r)
        return 0;
    for (i = 0; i < l; i++) {
        char c = v[i], one[2];
        one[0] = c;
        one[1] = 0;
        r[i] = c;
        if (i && !all)
            continue;
        if (*pat && !sh_match(pat, one, mf))
            continue;
        if ((kind == '^' || kind == '~') && c >= 'a' && c <= 'z')
            r[i] = (char)(c - 32);
        else if ((kind == ',' || kind == '~') && c >= 'A' && c <= 'Z')
            r[i] = (char)(c + 32);
    }
    r[l] = 0;
    return r;
}

/* ${v#p} ${v##p} ${v%p} ${v%%p} of one value */
static char *trim_one(const char *pat, const char *val, char kind, int longest, int mf)
{
    long l = (long)strlen(val), k, from = 0, to = l;
    char *tmp = (char *)malloc((size_t)l + 1), *r;
    if (!tmp)
        return 0;
    if (kind == '#') {
        for (k = longest ? l : 0; longest ? k >= 0 : k <= l; k += longest ? -1 : 1) {
            memcpy(tmp, val, (size_t)k);
            tmp[k] = 0;
            if (sh_match(pat, tmp, mf)) {
                from = k;
                break;
            }
        }
    } else {
        for (k = longest ? 0 : l; longest ? k <= l : k >= 0; k += longest ? 1 : -1)
            if (sh_match(pat, val + k, mf)) {
                to = k;
                break;
            }
    }
    memcpy(tmp, val + from, (size_t)(to - from));
    tmp[to - from] = 0;
    r = sdup(tmp);
    free(tmp);
    return r;
}

/* the value of NAME[sub] (sub raw text, expanded here); 0 when unset. @ and * give element 0. */
static const char *pval(ex *e, const char *name, int hassub, const char *sub)
{
    char *num = e->vnum;
    char *k;
    const char *r;
    if (!hassub)
        return param(e, name);
    if (sub[0] == '@' || sub[0] == '*')
        sub = "0";
    k = expand_string(e, sub, (long)strlen(sub), 0);
    if (!k)
        return 0;
    if (!strcmp(name, "PIPESTATUS") && e->c->npstat) {
        long i = atol(k);
        r = i >= 0 && i < e->c->npstat ? (sh_ltoa(e->c->pstat[i], num), num) : 0;
    } else
        r = sh_get_elem(e->c, name, k);
    free(k);
    return r;
}

/* The word of ${x:-word} ${x:+word} ${x:=word} into b. Unquoted, its text splits like an expansion
 * result, but what the word quotes ("a  b", \x) stays whole. */
static void put_word(ex *e, cbuf *b, const char *w, long len, int dquote)
{
    int k;
    if (dquote) {
        char *v = expand_string(e, w, len, 1);
        cputs(b, v, F_QUOTED);
        free(v);
        return;
    }
    k = b->n;
    expand_into(e, w, len, b, 0);
    for (; k < b->n; k++)
        if (!b->f[k])
            b->f[k] = F_SPLIT;
}

/* ${X:o:l}, ${@:o:l}, ${A[@]:o:l}: offset and length are arithmetic; a negative offset counts from the
 * end, a negative length is an end position. Returns 0, or -1 with e->err set. */
static int slice_range(ex *e, const char *w, long a, long end, long *off, long *len, int *haslen)
{
    long j = a, depth = 0, cut = -1;
    const char *err = 0;
    char *t;
    while (j < end) {
        if (w[j] == '(')
            depth++;
        else if (w[j] == ')')
            depth--;
        else if (w[j] == ':' && !depth) {
            cut = j;
            break;
        }
        j++;
    }
    t = expand_string(e, w + a, (cut < 0 ? end : cut) - a, 0);
    *off = t ? sh_arith(e->c, t, &err) : 0;
    free(t);
    *haslen = cut >= 0;
    *len = 0;
    if (!err && cut >= 0) {
        t = expand_string(e, w + cut + 1, end - cut - 1, 0);
        *len = t ? sh_arith(e->c, t, &err) : 0;
        free(t);
    }
    if (err) {
        e->err = err;
        return -1;
    }
    return 0;
}

static void slice_scalar(ex *e, cbuf *b, const char *v, long off, long len, int haslen, int dquote)
{
    long l = (long)strlen(v), n;
    char *t;
    if (off < 0)
        off += l;
    if (off < 0 || off > l)
        return;
    if (haslen && len < 0) {
        if (l + len < off) {
            e->err = "substring expression < 0";
            return;
        }
        n = l + len - off;
    } else
        n = haslen && len < l - off ? len : l - off;
    t = (char *)malloc((size_t)n + 1);
    if (t) {
        memcpy(t, v + off, (size_t)n);
        t[n] = 0;
        cputs(b, t, dquote ? F_QUOTED : F_SPLIT);
        free(t);
    }
}

/* The elements of a list slice: positional parameters count from 1 (0 is $0), an indexed array by
 * subscript, an associative one by position. */
static void slice_list(ex *e, cbuf *b, const char *name, int hassub, int all, const char *sub,
                       long off, long len, int haslen, int dquote)
{
    pv src, res;
    sh_list ks;
    long i, last = 0, s;
    int indexed = 0, at;
    const sh_var *v = find(e->c, name);
    memset(&ks, 0, sizeof(ks));
    memset(&res, 0, sizeof(res));
    pv_get(e, name, &src);
    if (haslen && len < 0) {
        e->err = "substring expression < 0";
        goto done;
    }
    if (hassub && v && !(v->attr & SH_ATTR_ASSOC)) {
        sh_keys(e->c, name, &ks);
        indexed = ks.n == src.n;
        if (indexed && ks.n)
            last = atol(ks.v[ks.n - 1]);
    }
    res.v = (char **)calloc((size_t)src.n + 2, sizeof(char *));
    res.own = 1;
    if (!res.v)
        goto done;
    if (!hassub) {
        /* position p = 1..n is $p; offset 0 puts $0 in front */
        s = off < 0 ? src.n + 1 + off : off;
        if (s < 0 || (off < 0 && s < 1))
            goto done;
        if (s == 0)
            res.v[res.n++] = e->c->arg0 ? e->c->arg0 : (char *)"";
        for (i = s < 1 ? 0 : s - 1; i < src.n && (!haslen || res.n < len); i++)
            res.v[res.n++] = src.v[i];
    } else if (indexed) {
        s = off < 0 ? last + 1 + off : off;
        if (s < 0)
            goto done;
        for (i = 0; i < src.n && (!haslen || res.n < len); i++)
            if (atol(ks.v[i]) >= s)
                res.v[res.n++] = src.v[i];
    } else {
        s = off < 0 ? src.n + off : off;
        if (s < 0)
            goto done;
        for (i = s; i < src.n && (!haslen || res.n < len); i++)
            res.v[res.n++] = src.v[i];
    }
    at = all ? sub[0] == '@' : name[0] == '@';
    put_pv(e, b, &res, at, dquote);
done:
    free(res.v);
    sh_list_free(&ks);
    pv_free(&src);
}

/* ${X@op}: Q quote, E escapes, P prompt, A declare text, a attributes, U u L case, K k key/value pairs;
 * one value, or every element of $@ and NAME[@]. Returns 0 when the operator letter is not known. */
static void transform(ex *e, cbuf *b, const char *name, int hassub, int all, const char *sub, char op,
                      int dquote)
{
    int list = all || (!hassub && (!strcmp(name, "@") || !strcmp(name, "*")));
    int at = list && (all ? sub[0] == '@' : name[0] == '@');
    const sh_var *v = hassub ? find(e->c, name) : 0;
    pv src, res;
    long q, n = 1;
    const char *one[1];
    memset(&res, 0, sizeof(res));
    memset(&src, 0, sizeof(src));
    if (list) {
        pv_get(e, name, &src);
        n = src.n;
    } else {
        const char *val = pval(e, name, hassub, sub);
        if (unbound(e, name, val))
            return;
        if (!val && op != 'A' && op != 'a')
            return;
        one[0] = val ? val : "";
    }
    SH_HIT(PARAM_TRANSFORM);
    res.v = (char **)calloc((size_t)(2 * n + 2), sizeof(char *));
    res.own = res.deep = 1;
    if (!res.v)
        goto done;
    if (op == 'A' || op == 'a') {
        /* A: the declare text, whole for NAME[@]; a: the letters, once per element of NAME[@] */
        if (e->c->declared && !(list && !all)) {
            if (op == 'A' || !list)
                res.v[res.n++] = e->c->declared(e->c, name, op == 'a', all);
            else
                for (q = 0; q < n; q++)
                    res.v[res.n++] = e->c->declared(e->c, name, 1, 0);
        }
    } else if ((op == 'K' || op == 'k') && all && v && (v->attr & (SH_ATTR_ARRAY | SH_ATTR_ASSOC))) {
        sh_list ks;
        memset(&ks, 0, sizeof(ks));
        sh_keys(e->c, name, &ks);
        for (q = 0; q < n && q < ks.n; q++) {
            char *k = sdup(ks.v[q]), *val = op == 'K' ? sh_dquote(src.v[q]) : sdup(src.v[q]);
            if (op == 'K' && (v->attr & SH_ATTR_ASSOC)) {
                const char *z;
                for (z = ks.v[q]; *z; z++)
                    if (!((*z >= 'a' && *z <= 'z') || (*z >= 'A' && *z <= 'Z') || (*z >= '0' && *z <= '9') || *z == '_'))
                        break;
                if (*z || !*ks.v[q]) {
                    free(k);
                    k = sh_dquote(ks.v[q]);
                }
            }
            if (op == 'K') {
                /* one word per pair: key value */
                char *j = (char *)malloc(strlen(k ? k : "") + strlen(val ? val : "") + 3);
                if (j) {
                    strcpy(j, k ? k : "");
                    strcat(j, " ");
                    strcat(j, val ? val : "");
                    if (v->attr & SH_ATTR_ASSOC)
                        strcat(j, " "); /* bash: a trailing space after every pair of an associative array */
                    res.v[res.n++] = j;
                }
                free(k);
                free(val);
            } else {
                res.v[res.n++] = k;
                res.v[res.n++] = val;
            }
        }
        sh_list_free(&ks);
    } else {
        char *pat = sdup("");
        for (q = 0; q < n; q++) {
            const char *x = list ? src.v[q] : one[0];
            char *t = 0;
            switch (op) {
            case 'Q': case 'K': case 'k': t = sh_quote(x, SH_Q_ALWAYS); break;
            case 'E': t = e->c->unescape ? e->c->unescape(e->c, x) : sdup(x); break;
            case 'P': t = e->c->prompt ? e->c->prompt(e->c, x) : sdup(x); break;
            case 'U': t = case_one(pat, x, '^', 1, SH_MATCH_OF(e->c, 0)); break;
            case 'u': t = case_one(pat, x, '^', 0, SH_MATCH_OF(e->c, 0)); break;
            case 'L': t = case_one(pat, x, ',', 1, SH_MATCH_OF(e->c, 0)); break;
            default: break;
            }
            res.v[res.n++] = t ? t : sdup("");
        }
        free(pat);
    }
    if (list && (op == 'K' || op == 'k') && all && v && (v->attr & (SH_ATTR_ARRAY | SH_ATTR_ASSOC))) {
        if (op == 'K' && dquote) {
            /* "${a[@]@K}": one word of all the pairs (bash) */
            at = 0;
            {
                long z, tot = 1;
                char *j;
                for (z = 0; z < res.n; z++)
                    tot += (long)strlen(res.v[z]) + 1;
                j = (char *)malloc((size_t)tot);
                if (j) {
                    j[0] = 0;
                    for (z = 0; z < res.n; z++) {
                        if (z && !(v->attr & SH_ATTR_ASSOC))
                            strcat(j, " ");
                        strcat(j, res.v[z]);
                        free(res.v[z]);
                    }
                    res.v[0] = j;
                    res.n = 1;
                }
            }
        }
    }
    put_pv(e, b, &res, at, dquote);
done:
    pv_free(&res);
    if (list)
        pv_free(&src);
}

/* ${...}: name, and an operator :- := :+ :? (or - = + ?) with its word. */
static long brace(ex *e, const char *w, long len, cbuf *b, int dquote)
{
    long i = 2, depth = 1, end;
    char name[64], sub[96];
    int k = 0, len_op = 0, colon = 0, hassub = 0, indirect = 0, all = 0;
    char op = 0;
    const char *val;
    {
        long k = sh_skip_sub(w, 0, len, dquote);
        depth = k < 0;
        i = k < 0 ? len : k - 1;
    }
    end = i;
    if (depth) {
        e->err = "bad substitution";
        return len;
    }
    i = 2;
    if (w[i] == '#' && end > 3) {
        len_op = 1;
        i++;
    }
    if (w[i] == '!' && i + 1 < end && (is_name_char(w[i + 1], 1))) {
        indirect = 1;
        i++;
    }
    if (strchr("?$!#@*-", w[i]) && k == 0) {
        name[k++] = w[i++];
    } else {
        while (i < end && k < 63 && (is_name_char(w[i], !k) || (w[i] >= '0' && w[i] <= '9')))
            name[k++] = w[i++];
    }
    name[k] = 0;
    if (!k) {
        e->err = "bad substitution";
        return end + 1;
    }
    if (i < end && w[i] == '[') {
        long j = i + 1;
        int d = 1, m = 0;
        while (j < end && d) {
            d += w[j] == '[' ? 1 : w[j] == ']' ? -1 : 0;
            if (d && m < 95)
                sub[m++] = w[j];
            j++;
        }
        sub[m] = 0;
        hassub = 1;
        i = j;
        all = !strcmp(sub, "@") || !strcmp(sub, "*");
    }
    if (indirect && i + 1 == end && (w[i] == '*' || w[i] == '@')) {
        /* ${!prefix*} ${!prefix@}: the names of the variables that start with prefix, sorted */
        pv ns;
        const sh_var *vp;
        sh_var_iter it;
        long q, r, pl = (long)strlen(name);
        memset(&ns, 0, sizeof(ns));
        for (vp = sh_var_first(e->c, &it); vp; vp = sh_var_next(e->c, &it))
            ns.n++;
        ns.v = (char **)calloc((size_t)ns.n + 1, sizeof(char *));
        ns.own = 1;
        ns.n = 0;
        for (vp = sh_var_first(e->c, &it); ns.v && vp; vp = sh_var_next(e->c, &it))
            if (!strncmp(vp->name, name, (size_t)pl))
                ns.v[ns.n++] = vp->name;
        for (q = 1; q < ns.n; q++) {
            char *t = ns.v[q];
            for (r = q; r > 0 && strcmp(ns.v[r - 1], t) > 0; r--)
                ns.v[r] = ns.v[r - 1];
            ns.v[r] = t;
        }
        put_pv(e, b, &ns, w[i] == '@', dquote);
        pv_free(&ns);
        return end + 1;
    }
    if (indirect) {
        const sh_var *rv = find(e->c, name);
        if (all) {
            /* ${!a[@]}: the keys, as a list */
            sh_list ks;
            int q;
            memset(&ks, 0, sizeof(ks));
            sh_keys(e->c, name, &ks);
            for (q = 0; q < ks.n; q++) {
                if (q) {
                    if (sub[0] == '@' && dquote)
                        b->f[b->n - 1] |= F_BREAK;
                    else
                        cput(b, ' ', dquote ? F_QUOTED : F_SPLIT);
                }
                cputs(b, ks.v[q], dquote ? F_QUOTED : F_SPLIT);
            }
            if (sub[0] == '@' && dquote && !ks.n)
                b->force_field = -1;
            sh_list_free(&ks);
            return end + 1;
        }
        if (rv && (rv->attr & SH_ATTR_NAMEREF)) {
            if (rv->sv)
                cputs(b, rv->sv, dquote ? F_QUOTED : F_SPLIT);
            return end + 1;
        }
        val = pval(e, name, hassub, sub);
        if (!val || !*val)
            return end + 1;
        strncpy(name, val, 63);
        name[63] = 0;
        hassub = all = 0;
        {
            char *br = strchr(name, '[');
            size_t nl = strlen(name);
            if (br && name[nl - 1] == ']') {
                *br = 0;
                name[nl - 1] = 0;
                strncpy(sub, br + 1, 95);
                sub[95] = 0;
                hassub = 1;
                all = !strcmp(sub, "@") || !strcmp(sub, "*");
            }
        }
    }
    if (i + 2 == end && w[i] == '@' && !len_op && strchr("QEPAaUuLKk", w[i + 1])) {
        transform(e, b, name, hassub, all, sub, w[i + 1], dquote);
        return end + 1;
    }
    if (i < end && strchr("#%/^,~", w[i]) && !len_op) {
        /* ${X#p} ${X%p} (## %% longest), ${X/p/r} ${X//p/r} ${X/#p/r} ${X/%p/r}, ${X^p} ${X^^p} ${X,p}
         * ${X,,p} ${X~p} ${X~~p}: one value, or every element of $@ and NAME[@], rewritten */
        char kind = w[i++], anchor = 0, *pat, *rep = 0, *tmp;
        int twice = 0, list = all || (!hassub && (!strcmp(name, "@") || !strcmp(name, "*"))), at;
        long q, n = 1;
        pv src, res;
        const char *one[1];
        if (i < end && w[i] == kind && kind != '/') {
            twice = 1;
            i++;
        }
        if (kind == '/') {
            if (i < end && w[i] == '/') {
                twice = 1;
                i++;
            } else if (i < end && (w[i] == '#' || w[i] == '%'))
                anchor = w[i++];
        }
        {
            long pend = end;
            if (kind == '/') {
                long j = i;
                while (j < end && w[j] != '/')
                    j += w[j] == '\\' && j + 1 < end ? 2 : 1;
                pend = j;
            }
            pat = expand_string(e, w + i, pend - i, dquote);
            if (kind == '/') {
                if (pend < end) {
                    long rl = end - pend - 1, z;
                    char *raw = (char *)malloc((size_t)rl + 1);
                    if (raw) {
                        memcpy(raw, w + pend + 1, (size_t)rl);
                        raw[rl] = 0;
                        for (z = 0; z < rl; z++) {
                            if (raw[z] == '\\' && z + 1 < rl)
                                z++;
                            else if (raw[z] == '&')
                                raw[z] = 1;
                        }
                        rep = expand_string(e, raw, rl, dquote);
                        free(raw);
                    }
                } else
                    rep = sdup("");
            }
        }
        if (list) {
            pv_get(e, name, &src);
            n = src.n;
        } else {
            val = pval(e, name, hassub, sub);
            if (unbound(e, name, val)) {
                free(pat);
                free(rep);
                return end + 1;
            }
            one[0] = val ? val : "";
            memset(&src, 0, sizeof(src));
        }
        memset(&res, 0, sizeof(res));
        res.v = (char **)calloc((size_t)(n ? n : 1), sizeof(char *));
        res.own = res.deep = 1;
        if (pat && res.v && (kind != '/' || rep)) {
            SH_HIT(PARAM_OP);
            for (q = 0; q < n; q++) {
                const char *v = list ? src.v[q] : one[0];
                int mf = SH_MATCH_OF(e->c, 0);
                tmp = kind == '/' ? subst_one(pat, rep, v, twice, anchor, mf)
                    : (kind == '#' || kind == '%') ? trim_one(pat, v, kind, twice, mf)
                    : case_one(pat, v, kind, twice, mf);
                res.v[res.n++] = tmp ? tmp : sdup("");
            }
        }
        at = list && (all ? sub[0] == '@' : name[0] == '@');
        put_pv(e, b, &res, at, dquote);
        pv_free(&res);
        if (list)
            pv_free(&src);
        free(pat);
        free(rep);
        return end + 1;
    }
    if (i < end && w[i] == ':' && !len_op && (i + 1 >= end || !strchr("-=+?", w[i + 1]))) {
        long off, len;
        int haslen;
        const sh_var *sv;
        if (slice_range(e, w, i + 1, end, &off, &len, &haslen) < 0)
            return end + 1;
        SH_HIT(PARAM_SLICE);
        sv = hassub ? find(e->c, name) : 0;
        if (all && sv && !(sv->attr & (SH_ATTR_ARRAY | SH_ATTR_ASSOC)))
            all = 0; /* ${x[@]:1:2} of a plain scalar is the substring */
        if (all || (!hassub && (!strcmp(name, "@") || !strcmp(name, "*")))) {
            slice_list(e, b, name, hassub, all, sub, off, len, haslen, dquote);
        } else {
            val = pval(e, name, hassub, sub);
            if (!unbound(e, name, val))
                slice_scalar(e, b, val ? val : "", off, len, haslen, dquote);
        }
        return end + 1;
    }
    if (i < end && w[i] == ':') {
        colon = 1;
        i++;
    }
    if (i < end && strchr("-=+?", w[i]))
        op = w[i++];
    else if (i < end) {
        e->err = "bad substitution";
        return end + 1;
    }
    val = pval(e, name, hassub, sub);
    if (all) {
        pv p;
        pv_get(e, name, &p);
        val = p.n ? "x" : 0;
        pv_free(&p);
    }
    if (!op && !len_op && unbound(e, name, val))
        return end + 1;
    if (len_op && !all && unbound(e, name, val))
        return end + 1;
    if (len_op) {
        char n[16];
        long l = val ? (long)strlen(val) : 0;
        int d = 0;
        char t[16];
        if (all || (!hassub && (name[0] == '@' || name[0] == '*') && !name[1])) {
            pv p;
            pv_get(e, name, &p);
            l = p.n;
            pv_free(&p);
        }
        do
            t[d++] = (char)('0' + l % 10);
        while ((l /= 10) > 0);
        k = 0;
        while (d)
            n[k++] = t[--d];
        n[k] = 0;
        cputs(b, n, dquote ? F_QUOTED : F_SPLIT);
        return end + 1;
    }
    {
        int empty = !val || (colon && !*val);
        if (op == '-' && empty) {
            put_word(e, b, w + i, end - i, dquote);
            return end + 1;
        }
        if (op == '=' && empty) {
            int k = b->n;
            char *v;
            put_word(e, b, w + i, end - i, dquote);
            v = (char *)malloc((size_t)(b->n - k) + 1);
            if (v) {
                memcpy(v, b->s + k, (size_t)(b->n - k));
                v[b->n - k] = 0;
                sh_set(e->c, name, v);
                free(v);
            }
            return end + 1;
        }
        if (op == '?' && empty) {
            e->err = "parameter not set";
            return end + 1;
        }
        if (op == '+') {
            if (!empty)
                put_word(e, b, w + i, end - i, dquote);
            return end + 1;
        }
    }
    if (!strcmp(name, "@") || !strcmp(name, "*"))
        put_values(e, b, name, name[0] == '@', dquote);
    else if (all)
        put_values(e, b, name, sub[0] == '@', dquote);
    else if (val)
        cputs(b, val, dquote ? F_QUOTED : F_SPLIT);
    return end + 1;
}

/* $(cmd) or `cmd`: the command's output, trailing newlines removed. */
static void command_output(ex *e, const char *cmd, long n, cbuf *b, int dquote, int backtick)
{
    char *text = (char *)malloc(n + 1), *out;
    long l, k;
    if (!text)
        return;
    memcpy(text, cmd, n);
    text[n] = 0;
    if (backtick) {
        /* `...`: a backslash before $, ` or \ is dropped (so \` nests) */
        for (l = k = 0; l < n; l++) {
            if (text[l] == '\\' && l + 1 < n && strchr(dquote ? "$`\\\"" : "$`\\", text[l + 1]))
                l++;
            text[k++] = text[l];
        }
        text[k] = 0;
    }
    out = e->c->subst ? e->c->subst(e->c, text) : 0;
    free(text);
    if (!out)
        return;
    l = (long)strlen(out);
    while (l && out[l - 1] == '\n')
        out[--l] = 0;
    cputs(b, out, dquote ? F_QUOTED : F_SPLIT);
    free(out);
}

/* $... at w: expand it into b; the characters consumed. */
static long dollar(ex *e, const char *w, long len, cbuf *b, int dquote)
{
    if (len >= 3 && w[1] == '(' && w[2] == '(') {
        long i = 3, depth = 2;
        while (i < len && depth) {
            if (w[i] == '(')
                depth++;
            else if (w[i] == ')')
                depth--;
            i++;
        }
        {
            char *expr = expand_string(e, w + 3, i - 5 > 0 ? i - 5 : 0, 1);
            const char *err = 0;
            sh_int v = sh_arith(e->c, expr, &err);
            char n[24];
            free(expr);
            if (err) {
                e->err = err;
                return i;
            }
            sh_ltoa(v, n);
            cputs(b, n, dquote ? F_QUOTED : F_SPLIT);
        }
        return i;
    }
    if (len >= 2 && w[1] == '\'' && !dquote) {
        /* $'...' (ANSI-C quoting): the escapes read as printf %b reads them, plus \E \cX \? */
        long j = 2;
        char *raw, *txt;
        while (j < len && w[j] != '\'') {
            if (w[j] == '\\' && j + 1 < len)
                j++;
            j++;
        }
        raw = (char *)malloc((size_t)j);
        if (raw) {
            memcpy(raw, w + 2, (size_t)j - 2);
            raw[j - 2] = 0;
            txt = e->c->unescape ? e->c->unescape(e->c, raw) : sdup(raw);
            cputs(b, txt ? txt : "", F_QUOTED);
            b->had_quotes = 1;
            free(txt);
            free(raw);
        }
        return j + 1;
    }
    if (len >= 2 && w[1] == '(') {
        long i = sh_skip_sub(w, 0, len, dquote);
        if (i < 0)
            i = len;
        command_output(e, w + 2, i - 3 < 0 ? 0 : i - 3, b, dquote, 0);
        return i;
    }
    if (len >= 2 && w[1] == '{')
        return brace(e, w, len, b, dquote);
    if (len >= 2 && strchr("?$!#@*-0123456789", w[1])) {
        char name[2];
        name[0] = w[1];
        name[1] = 0;
        if (w[1] == '@' || w[1] == '*')
            put_values(e, b, name, w[1] == '@', dquote);
        else {
            const char *v = param(e, name);
            if (unbound(e, name, v))
                return 2;
            if (v)
                cputs(b, v, dquote ? F_QUOTED : F_SPLIT);
        }
        return 2;
    }
    if (len >= 2 && is_name_char(w[1], 1)) {
        char name[64];
        long i = 1;
        int k = 0;
        const char *v;
        while (i < len && k < 63 && is_name_char(w[i], !k))
            name[k++] = w[i++];
        name[k] = 0;
        v = param(e, name);
        if (unbound(e, name, v))
            return i;
        if (v)
            cputs(b, v, dquote ? F_QUOTED : F_SPLIT);
        return i;
    }
    cput(b, '$', dquote ? F_QUOTED : 0); /* a lone $ is itself */
    return 1;
}

/* w[0..n) is NAME or NAME+ (the head of a declare argument NAME=value) */
static int assign_prefix(const char *w, long n)
{
    long k;
    if (n && w[n - 1] == '+')
        n--;
    for (k = 0; k < n; k++)
        if (!is_name_char(w[k], !k))
            return 0;
    return n > 0;
}

/* A tilde prefix at w[*i] (the ~ up to the next / or, in an assignment, :): ~ is $HOME, ~+ is $PWD, ~- is
 * $OLDPWD; ~user is left as it is (vsh has no user database). 1 when expanded, *i moved past it. */
static int tilde(ex *e, const char *w, long *i, long len, cbuf *b)
{
    long j = *i + 1;
    const char *v = 0;
    while (j < len && w[j] != '/' && !(e->assign && w[j] == ':'))
        j++;
    if (j == *i + 1)
        v = sh_get(e->c, "HOME"), v = v ? v : "SYS:";
    else if (j == *i + 2 && w[*i + 1] == '+')
        v = sh_get(e->c, "PWD");
    else if (j == *i + 2 && w[*i + 1] == '-')
        v = sh_get(e->c, "OLDPWD");
    if (!v)
        return 0;
    cputs(b, v, F_QUOTED);
    *i = j;
    return 1;
}

/* Expand w[0..len) into b: quotes and escapes resolved, expansions done. dquote 1: inside "...";
 * 2: a here-document's body (quotes are themselves, \ quotes only $ ` \ and a newline) */
static int expand_into(ex *e, const char *w, long len, cbuf *b, int dquote)
{
    long i = 0;
    while (i < len && !e->err) {
        char ch = w[i];
        if (!dquote && ch == '\'') {
            long j = i + 1;
            b->had_quotes = 1;
            while (j < len && w[j] != '\'')
                cput(b, w[j++], F_QUOTED);
            i = j + 1;
        } else if (ch == '"' && dquote != 2) {
            /* a quoted section; inside ${ } within "..." a nested one quotes as well */
            long j = i + 1;
            b->had_quotes = 1;
            /* the closing quote, skipping over $( ) and ${ } */
            while (j < len && w[j] != '"') {
                if (w[j] == '\\' && j + 1 < len)
                    j++;
                else if (w[j] == '$' && j + 1 < len && (w[j + 1] == '(' || w[j + 1] == '{')) {
                    long k = sh_skip_sub(w + j, 0, len - j, 1);
                    if (k < 0)
                        k = len - j;
                    j += k - 1;
                }
                j++;
            }
            expand_into(e, w + i + 1, j - i - 1, b, 1);
            i = j + 1;
        } else if (ch == '\\' && i + 1 < len && w[i + 1] == '\n' && dquote) {
            i += 2; /* "a\<newline>b" and a here-document's line ending in \: the lines join */
        } else if (ch == '\\' && i + 1 < len) {
            char nx = w[i + 1];
            if (dquote && !strchr(dquote == 2 ? "$`\\\n" : "$`\"\\\n", nx)) {
                cput(b, '\\', F_QUOTED); /* inside "...": \ stays before others */
                cput(b, nx, F_QUOTED);
            } else {
                cput(b, nx, F_QUOTED);
            }
            b->had_quotes = 1;
            i += 2;
        } else if (ch == '$' && !dquote && i + 1 < len && w[i + 1] == '"') {
            i++; /* $"...": no message catalogue, plain double quotes */
        } else if (ch == '$') {
            i += dollar(e, w + i, len - i, b, dquote);
        } else if (!dquote && (ch == '<' || ch == '>') && i + 1 < len && w[i + 1] == '(') {
            long k = sh_skip_sub(w + i, 0, len - i, 0);
            char *cmd, *path;
            if (k < 0)
                k = len - i;
            cmd = (char *)malloc((size_t)k);
            if (cmd) {
                memcpy(cmd, w + i + 2, (size_t)(k - 3 < 0 ? 0 : k - 3));
                cmd[k - 3 < 0 ? 0 : k - 3] = 0;
                path = e->c->procsub ? e->c->procsub(e->c, cmd, ch == '>') : 0;
                free(cmd);
                if (path) {
                    cputs(b, path, F_QUOTED);
                    b->had_quotes = 1;
                    free(path);
                }
            }
            i += k;
        } else if (ch == '`') {
            long j = i + 1;
            while (j < len && w[j] != '`') {
                if (w[j] == '\\' && j + 1 < len)
                    j++;
                j++;
            }
            command_output(e, w + i + 1, j - i - 1, b, dquote, 1);
            i = j + 1;
        } else if (!dquote && ch == '~' && (i == 0 || (e->assign && (w[i - 1] == ':' || (w[i - 1] == '=' && assign_prefix(w, i - 1))))) &&
                   tilde(e, w, &i, len, b)) {
            ; /* ~ ~+ ~- expanded, i moved past the prefix */
        } else {
            cput(b, ch, dquote ? F_QUOTED : 0);
            i++;
        }
    }
    return e->err ? -1 : 0;
}

/* ---- globbing ------------------------------------------------------------------- */

/* Does the bracket at s[i] close within s[..to)? A ']' right after '[' or
 * '[!' is a member, not the end (POSIX). Without its ']', '[' is literal:
 * the word "[" of [ $i -lt 9 ] is no pattern (vsh listed the directory
 * for every one -- 85 ms per loop turn on the rig). quoted: the flags,
 * or 0 to take every character as unquoted. */
static int bracket_end(const char *s, const unsigned char *quoted, int i, int to)
{
    int j = i + 1;
    if (j < to && (s[j] == '!' || s[j] == '^'))
        j++;
    if (j < to && s[j] == ']')
        j++;
    for (; j < to; j++) {
        if (s[j] == '[' && j + 1 < to && s[j + 1] == ':') {
            int k = j + 2;
            while (k + 1 < to && !(s[k] == ':' && s[k + 1] == ']'))
                k++;
            if (k + 1 < to) { /* [:class:] */
                j = k + 1;
                continue;
            }
        }
        if (s[j] == ']' && !(quoted && (quoted[j] & F_QUOTED)))
            return j;
    }
    return -1;
}

static int bracket_closes(const char *s, const unsigned char *quoted, int i, int to)
{
    return bracket_end(s, quoted, i, to) >= 0;
}

/* The ':' ending a volume name in a glob pattern ("SYS:*"), not one inside [...] or escaped; 0 when none */
static char *vol_colon(char *pat)
{
    int i, n = (int)strlen(pat), e;
    for (i = 0; i < n; i++) {
        if (pat[i] == '\\' && i + 1 < n)
            i++;
        else if (pat[i] == '[' && (e = bracket_end(pat, 0, i, n)) >= 0)
            i = e;
        else if (pat[i] == ':')
            return pat + i;
        else if (pat[i] == '/')
            return 0;
    }
    return 0;
}

static int has_glob(const sh_ctx *c, const cbuf *b, int from, int to)
{
    int i;
    for (i = from; i < to; i++)
        if (!(b->f[i] & F_QUOTED) &&
            (b->s[i] == '*' || b->s[i] == '?' ||
             (b->s[i] == '[' && bracket_closes(b->s, b->f, i, to)) ||
             (c->extglob && i + 1 < to && strchr("@+!", b->s[i]) && b->s[i + 1] == '(' && !(b->f[i + 1] & F_QUOTED))))
            return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* extglob: a component that starts with a group one of whose alternatives starts with a literal dot
 * (@(.a|b)) may match a name that starts with a dot */
static int ext_dot(const sh_ctx *c, const char *comp)
{
    long e;
    const char *q;
    if (!c->extglob || !strchr("@?*+!", comp[0]) || comp[1] != '(' || comp[0] == '!')
        return 0;
    e = ext_close(comp + 1);
    if (e < 0)
        return 0;
    for (q = comp + 2; q < comp + 1 + e; q++) {
        if (*q == '.')
            return 1;
        while (q < comp + 1 + e && *q != '|')
            q++;
    }
    return 0;
}

/* Expand pattern (glob characters active, \ escapes the rest) below dir,
 * component by component. The separator is '/', a leading "vol:" is kept. */
static void glob_rec(sh_ctx *c, const char *dir, const char *pat, sh_list *out);

static int path_is_dir(sh_ctx *c, const char *path)
{
    sh_list t;
    int r;
    if (c->pathkind)
        return c->pathkind(c, path) & 1;
    memset(&t, 0, sizeof(t));
    r = c->listdir && !c->listdir(c, path, &t);
    sh_list_free(&t);
    return r;
}

static void glob_join(const char *dir, const char *name, char *path, size_t cap)
{
    path[0] = 0;
    if (strlen(dir) + strlen(name) + 2 > cap)
        return;
    strcpy(path, dir);
    if (*dir && dir[strlen(dir) - 1] != ':' && dir[strlen(dir) - 1] != '/')
        strcat(path, "/");
    strcat(path, name);
}

/* shopt globstar: a "**" component. linkstop: bash applies the rest in a link to a directory only when the
 * pattern names a directory before the "**" (a leading "**" does not enter links, "a" before it does, one level). rest: what follows its slash (0: "**" ends the pattern, "": the
 * pattern ends in "**" and a slash). The directories below dir are walked without going through a
 * symbolic link (a link to a directory is itself one stop: the rest is matched in it, no deeper). */
static void glob_star(sh_ctx *c, const char *dir, const char *rest, int top, int islink, int linkstop, sh_list *out)
{
    sh_list names;
    int i;
    char path[512];
    if (!rest) {
        if (*dir && top) {
            glob_join(dir, "", path, sizeof(path));
            sh_list_add(out, path);
        }
    } else if (!*rest) {
        if (*dir) {
            glob_join(dir, "", path, sizeof(path));
            sh_list_add(out, path);
        }
    } else
        glob_rec(c, dir, rest, out);
    if (islink)
        return;
    memset(&names, 0, sizeof(names));
    if (!c->listdir || c->listdir(c, dir, &names))
        return;
    for (i = 0; i < names.n; i++) {
        int kind;
        if (names.v[i][0] == '.' && !c->dotglob)
            continue;
        glob_join(dir, names.v[i], path, sizeof(path));
        if (!path[0])
            continue;
        if (!rest)
            sh_list_add(out, path);
        kind = c->pathkind ? c->pathkind(c, path) : (path_is_dir(c, path) ? 1 : 0);
        if (kind & 1) {
            if (kind & 2) {
                if (rest && (linkstop || !*rest))
                    glob_star(c, path, rest, 0, 1, linkstop, out);
            } else
                glob_star(c, path, rest, 0, 0, linkstop, out);
        }
    }
    sh_list_free(&names);
}

static void glob_rec_comp(sh_ctx *c, const char *dir, const char *pat, sh_list *out)
{
    const char *slash = strchr(pat, '/');

    size_t clen = slash ? (size_t)(slash - pat) : strlen(pat);
    char comp[256];
    int i, magic = 0;
    sh_list names;
    if (clen >= sizeof(comp))
        return;
    memcpy(comp, pat, clen);
    comp[clen] = 0;
    for (i = 0; comp[i]; i++)
        if (comp[i] == '*' || comp[i] == '?' ||
            (c->extglob && strchr("@+!", comp[i]) && comp[i + 1] == '(') ||
            (comp[i] == '[' && bracket_closes(comp, 0, i, (int)clen)))
            magic = 1;
    if (!magic) {
        /* a literal component: no listing, the escapes removed */
        char lit[256], path[512];
        int k = 0;
        for (i = 0; comp[i]; i++) {
            if (comp[i] == '\\' && comp[i + 1])
                i++;
            lit[k++] = comp[i];
        }
        lit[k] = 0;
        strcpy(path, dir);
        if (*dir && dir[strlen(dir) - 1] != ':' && dir[strlen(dir) - 1] != '/')
            strcat(path, "/");
        strcat(path, lit);
        if (slash)
            glob_rec(c, path, slash + 1, out);
        else if (!c->pathkind || (c->pathkind(c, path) & 4))
            sh_list_add(out, path);
        return;
    }
    memset(&names, 0, sizeof(names));
    SH_HIT(GLOB);
    if (!c->listdir || c->listdir(c, dir, &names))
        return;
    for (i = 0; i < names.n; i++) {
        char path[512];
        if (names.v[i][0] == '.' && comp[0] != '.' && !c->dotglob && !ext_dot(c, comp))
            continue; /* dot files only when asked for (shopt dotglob) */
        if (!sh_match(comp, names.v[i], SH_MATCH_OF(c, c->nocase)))
            continue;
        strcpy(path, dir);
        if (*dir && dir[strlen(dir) - 1] != ':' && dir[strlen(dir) - 1] != '/')
            strcat(path, "/");
        strcat(path, names.v[i]);
        if (slash)
            glob_rec(c, path, slash + 1, out);
        else
            sh_list_add(out, path);
    }
    sh_list_free(&names);
}

static void glob_rec(sh_ctx *c, const char *dir, const char *pat, sh_list *out)
{
    if (c->globstar && !strncmp(pat, "**", 2) && (pat[2] == '/' || !pat[2])) {
        SH_HIT(GLOBSTAR);
        glob_star(c, dir, pat[2] ? pat + 3 : 0, 1, 0, *dir != 0, out);
        return;
    }
    if (!*pat) {
        /* the pattern ended in a slash: only a directory matches */
        char path[512];
        if (path_is_dir(c, dir)) {
            glob_join(dir, "", path, sizeof(path));
            sh_list_add(out, path);
        }
        return;
    }
    glob_rec_comp(c, dir, pat, out);
}

/* One field b[from..to): globbed if it has unquoted glob characters,
 * else added with quotes removed (they already are: b holds the text). */
static void add_field(sh_ctx *c, cbuf *b, int from, int to, int flags, sh_list *out)
{
    char *plain = (char *)malloc(2 * (to - from) + 1);
    int i, m = 0;
    const char *spec = (flags & SH_REGEX) ? "\\.[]()*+?{}|^$" : (flags & SH_PATTERN) ? "*?[]\\" : 0;
    if (!plain)
        return;
    for (i = from; i < to; i++)
        if (!(b->f[i] & F_EMPTY)) {
            if (spec && (b->f[i] & F_QUOTED) && strchr(spec, b->s[i]))
                plain[m++] = '\\';
            plain[m++] = b->s[i];
        }
    plain[m] = 0;
    if (!(flags & SH_NO_GLOB) && has_glob(c, b, from, to)) {
        /* the pattern: quoted characters escaped, so they match themselves */
        char *pat = (char *)malloc(2 * (to - from) + 1), *start;
        sh_list found;
        int k = 0, before = out->n;
        const char *colon;
        char root[256];
        for (i = from; i < to; i++) {
            if (b->f[i] & F_EMPTY)
                continue;
            if ((b->f[i] & F_QUOTED) && strchr(c->extglob ? "*?[]\\()|@+!" : "*?[]\\", b->s[i]))
                pat[k++] = '\\';
            pat[k++] = b->s[i];
        }
        pat[k] = 0;
        memset(&found, 0, sizeof(found));
        start = pat;
        root[0] = 0;
        colon = vol_colon(pat);
        if (colon) {
            size_t rl = (size_t)(colon - pat) + 1;
            memcpy(root, pat, rl);
            root[rl] = 0;
            start = pat + rl;
        } else if (pat[0] == '/') {
            strcpy(root, "/");
            start = pat + 1;
        }
        glob_rec(c, root, start, &found);
        if (found.n) {
            qsort(found.v, found.n, sizeof(char *), cmp_str);
            for (i = 0; i < found.n; i++)
                sh_list_add(out, found.v[i]);
        }
        sh_list_free(&found);
        free(pat);
        if (out->n > before) {
            free(plain);
            return;
        }
        /* no match: the word stays as it was (POSIX); nullglob drops it, failglob is an error */
        if (c->nullglob) {
            free(plain);
            return;
        }
        if (c->failglob) {
            strncpy(c->glob_pat, plain, sizeof(c->glob_pat) - 1);
            c->glob_pat[sizeof(c->glob_pat) - 1] = 0;
            c->glob_fail = 1;
            free(plain);
            return;
        }
    }
    sh_list_add(out, plain);
    free(plain);
}

/* ---- brace expansion: {a,b} {1..5} {a..e} {01..10..2}, before every other expansion ------------- */

#define BX_MAX 1000000L   /* more words than this is "out of memory" (the Amiga has less than that) */

/* The index of the unquoted bracket-free end of a quoted or $-construct at w[i] (or i itself when w[i]
 * starts none): the next character to look at. */
static long bx_skip(const char *w, long i, long len)
{
    char c = w[i];
    if (c == '\\')
        return i + 2 <= len ? i + 2 : len;
    if (c == '\'') {
        for (i++; i < len && w[i] != '\''; i++)
            ;
        return i < len ? i + 1 : len;
    }
    if (c == '"' || c == '`') {
        for (i++; i < len && w[i] != c; i++)
            if (w[i] == '\\')
                i++;
        return i < len ? i + 1 : len;
    }
    if ((c == '$' && i + 1 < len && (w[i + 1] == '(' || w[i + 1] == '{')) ||
        ((c == '<' || c == '>') && i + 1 < len && w[i + 1] == '(')) {
        long k = sh_skip_sub(w + i, 0, len - i, 0);
        return k < 0 ? len : i + k;
    }
    return i + 1;
}

/* the } that closes the { at w[i], or -1; *comma: index of the first top-level comma, or -1 */
static long bx_close(const char *w, long i, long len, long *comma)
{
    long j = i + 1, depth = 1;
    *comma = -1;
    while (j < len) {
        char c = w[j];
        if (c == '\\' || c == '\'' || c == '"' || c == '`' || c == '$' || ((c == '<' || c == '>') && j + 1 < len && w[j + 1] == '(')) {
            j = bx_skip(w, j, len);
            continue;
        }
        if (c == '{')
            depth++;
        else if (c == '}') {
            if (!--depth)
                return j;
        } else if (c == ',' && depth == 1 && *comma < 0)
            *comma = j;
        j++;
    }
    return -1;
}

/* a sequence body x..y or x..y..incr: integers or single letters */
static int bx_seq(const char *w, long a, long b, long *x, long *y, long *incr, int *letters, int *width)
{
    long p = a, q;
    int neg, digits;
    long v[3];
    int nv = 0, isnum[3];
    *width = 0;
    while (nv < 3) {
        q = p;
        neg = 0;
        if (q < b && w[q] == '-')
            neg = 1, q++;
        digits = 0;
        v[nv] = 0;
        while (q < b && w[q] >= '0' && w[q] <= '9' && digits < 18) {
            v[nv] = v[nv] * 10 + (w[q] - '0');
            q++, digits++;
        }
        isnum[nv] = digits > 0 && (q == b || (q + 1 < b && w[q] == '.' && w[q + 1] == '.'));
        if (isnum[nv]) {
            if (neg)
                v[nv] = -v[nv];
            if (nv < 2 && digits > 1 && w[p + neg] == '0' && *width < digits + neg)
                *width = digits + neg;
        } else if (nv < 2 && !neg && p < b && ((w[p] | 32) >= 'a' && (w[p] | 32) <= 'z') &&
                   (p + 1 == b || (p + 3 < b && w[p + 1] == '.' && w[p + 2] == '.'))) {
            q = p + 1;
            v[nv] = (unsigned char)w[p];
        } else
            return 0;
        nv++;
        if (q == b)
            break;
        if (!(q + 1 < b && w[q] == '.' && w[q + 1] == '.'))
            return 0;
        p = q + 2;
    }
    if (nv < 2 || (nv == 3 && !isnum[2]))
        return 0;
    if (isnum[0] != isnum[1])
        return 0;
    *letters = !isnum[0];
    *x = v[0];
    *y = v[1];
    *incr = nv == 3 ? v[2] : 1;
    if (*incr < 0)
        *incr = -*incr;
    if (!*incr)
        *incr = 1;
    return 1;
}

static int bx_add(sh_list *out, const char *a, long an, const char *b, long bn, const char *c, long cn)
{
    char *t;
    if (out->n >= BX_MAX)
        return -1;
    t = (char *)malloc((size_t)(an + bn + cn) + 1);
    if (!t)
        return -1;
    memcpy(t, a, (size_t)an);
    memcpy(t + an, b, (size_t)bn);
    memcpy(t + an + bn, c, (size_t)cn);
    t[an + bn + cn] = 0;
    sh_list_add(out, t);
    free(t);
    return 0;
}

/* w[0..len) with its first valid brace group expanded, the rest (the text after the group) recursively;
 * appended to out. 0, or -1 when memory or BX_MAX runs out. */
static int bx_expand(const char *w, long len, sh_list *out)
{
    long i = 0;
    while (i < len) {
        char c = w[i];
        if (c == '\\' || c == '\'' || c == '"' || c == '`' || c == '$' || ((c == '<' || c == '>') && i + 1 < len && w[i + 1] == '(')) {
            i = bx_skip(w, i, len);
            continue;
        }
        if (c == '{') {
            long comma, close = bx_close(w, i, len, &comma), x, y, incr;
            int letters, width;
            if (close > i + 1 && comma >= 0) {
                /* {a,b,c}: every alternative is itself expanded; then the text after the group */
                sh_list post, alt;
                long from = i + 1, k, pi, ai, depth = 0;
                int rc = 0;
                memset(&post, 0, sizeof(post));
                if (bx_expand(w + close + 1, len - close - 1, &post) < 0) {
                    sh_list_free(&post);
                    return -1;
                }
                for (k = i + 1; k <= close && !rc; k++) {
                    char d = w[k];
                    if (k < close && (d == '\\' || d == '\'' || d == '"' || d == '`' || d == '$')) {
                        k = bx_skip(w, k, close) - 1;
                        continue;
                    }
                    if (d == '{' && k < close)
                        depth++;
                    else if (d == '}' && k < close)
                        depth--;
                    if ((k == close) || (d == ',' && !depth)) {
                        memset(&alt, 0, sizeof(alt));
                        if (bx_expand(w + from, k - from, &alt) < 0)
                            rc = -1;
                        for (ai = 0; !rc && ai < alt.n; ai++)
                            for (pi = 0; !rc && pi < post.n; pi++)
                                rc = bx_add(out, w, i, alt.v[ai], (long)strlen(alt.v[ai]), post.v[pi],
                                            (long)strlen(post.v[pi]));
                        sh_list_free(&alt);
                        from = k + 1;
                    }
                }
                sh_list_free(&post);
                return rc;
            }
            if (close > i + 1 && bx_seq(w, i + 1, close, &x, &y, &incr, &letters, &width)) {
                sh_list post;
                long v, pi, count = (x < y ? y - x : x - y) / incr + 1;
                int rc = 0;
                if (count > BX_MAX)
                    return -1;
                memset(&post, 0, sizeof(post));
                if (bx_expand(w + close + 1, len - close - 1, &post) < 0) {
                    sh_list_free(&post);
                    return -1;
                }
                for (v = x; !rc && (x <= y ? v <= y : v >= y); v += x <= y ? incr : -incr) {
                    char num[40], *t = num;
                    if (letters) {
                        num[0] = (char)v;
                        num[1] = 0;
                    } else {
                        char dig[24];
                        long av = v < 0 ? -v : v;
                        int nd, pad;
                        sh_ltoa(av, dig);
                        nd = (int)strlen(dig);
                        t = num;
                        if (v < 0)
                            *t++ = '-';
                        for (pad = width - nd - (v < 0); pad > 0; pad--)
                            *t++ = '0';
                        strcpy(t, dig);
                        t = num;
                    }
                    for (pi = 0; !rc && pi < post.n; pi++)
                        rc = bx_add(out, w, i, t, (long)strlen(t), post.v[pi], (long)strlen(post.v[pi]));
                }
                sh_list_free(&post);
                return rc;
            }
        }
        i++;
    }
    return bx_add(out, w, len, "", 0, "", 0);
}

static int expand_word(sh_ctx *c, const char *word, int flags, sh_list *out, const char **err)
{
    ex e;
    cbuf b;
    const char *ifs = sh_get(c, "IFS");
    int i, start = 0, in_field = 0, fields = 0;
    if (c->noglob)
        flags |= SH_NO_GLOB;
    e.c = c;
    e.err = 0;
    e.assign = (flags & SH_ASSIGN) != 0;
    memset(&b, 0, sizeof(b));
    if (!ifs)
        ifs = " \t\n";
    if (expand_into(&e, word, (long)strlen(word), &b, (flags & SH_HEREDOC) ? 2 : 0) < 0) {
        if (err)
            *err = e.err;
        cfree(&b);
        return -1;
    }
    if (flags & SH_NO_SPLIT) {
        add_field(c, &b, 0, b.n, flags, out);
        cfree(&b);
        return 0;
    }
    for (i = 0; i <= b.n; i++) {
        int end = i == b.n;
        int sep = !end && (b.f[i] & F_SPLIT) && strchr(ifs, b.s[i]) && b.s[i];
        if (end || sep) {
            if (in_field) {
                add_field(c, &b, start, i, flags, out);
                fields++;
            }
            in_field = 0;
            continue;
        }
        if (!in_field) {
            in_field = 1;
            start = i;
        }
        if (b.f[i] & F_BREAK) {
            add_field(c, &b, start, i + 1, flags, out);
            fields++;
            in_field = 0;
        }
    }
    if (!fields && b.had_quotes && b.force_field >= 0)
        sh_list_add(out, ""); /* "" and "$empty" are one empty field */
    cfree(&b);
    if (c->glob_fail) {
        char *msg = c->errbuf;
        c->glob_fail = 0;
        strcpy(msg, "no match: ");
        strncat(msg, c->glob_pat, 150);
        if (err)
            *err = msg;
        return -1;
    }
    return 0;
}

char *sh_unquote(const char *word)
{
    size_t n = strlen(word);
    char *r = (char *)malloc(n + 1), *o = r;
    int single = 0, dbl = 0;
    if (!r)
        return 0;
    for (; *word; word++) {
        if (*word == '\'' && !dbl) {
            single = !single;
            continue;
        }
        if (*word == '"' && !single) {
            dbl = !dbl;
            continue;
        }
        if (*word == '\\' && !single && word[1])
            word++;
        *o++ = *word;
    }
    *o = 0;
    return r;
}

/* One word into fields appended to out: brace expansion first (words that are not assignments or
 * redirection targets), then each resulting word on its own. */
int sh_expand(sh_ctx *c, const char *word, int flags, sh_list *out, const char **err)
{
    sh_list bw;
    long i;
    int rc = 0;
    if ((flags & (SH_NO_SPLIT | SH_ASSIGN)) || !strchr(word, '{'))
        return expand_word(c, word, flags, out, err);
    memset(&bw, 0, sizeof(bw));
    if (bx_expand(word, (long)strlen(word), &bw) < 0) {
        sh_list_free(&bw);
        if (err)
            *err = "brace expansion: out of memory";
        return -1;
    }
    if (bw.n == 1 && !strcmp(bw.v[0], word)) {
        sh_list_free(&bw);
        return expand_word(c, word, flags, out, err);
    }
    SH_HIT(BRACE);
    for (i = 0; !rc && i < bw.n; i++)
        rc = expand_word(c, bw.v[i], flags, out, err);
    sh_list_free(&bw);
    return rc;
}
