/*
 * css.c — parse a stylesheet into rules, then match them.
 *
 * A stylesheet is "selector { prop: value; ... }" repeated. We split on
 * the braces, expand comma-separated selectors, classify each as id /
 * class / tag, and parse the declaration block into a css_style. Matching
 * an element is then a linear scan applying every rule whose selector
 * fits — in tag, class, id order so the more specific one wins, just
 * like the real cascade's specificity (simplified to three tiers).
 */

#include "css.h"
#include "lib.h"

#define MAX_RULES 256

enum { SEL_TAG, SEL_CLASS, SEL_ID };

struct rule {
    int  kind;
    char name[32];
    struct css_style style;
};

static struct rule rules[MAX_RULES];
static int nrules;

void css_reset(void) { nrules = 0; }

static char lc(char c){ return (c>='A'&&c<='Z')?c+32:c; }

static int ieq(const char *a, const char *b)
{
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* named colors + #rgb / #rrggbb -> 0xRRGGBB; returns 1 if parsed. */
static int parse_color(const char *v, int len, uint32_t *out)
{
    char b[24]; int n = 0;
    while (n < len && n < 23 && v[n] != ';') { b[n] = lc(v[n]); n++; }
    b[n] = 0;
    while (n > 0 && (b[n-1]==' '||b[n-1]=='\t')) b[--n] = 0;
    int s = 0; while (b[s]==' ') s++;
    const char *c = b + s;

    if (c[0] == '#') {
        uint32_t v32 = 0; int digits = 0;
        for (const char *p = c+1; *p; p++) {
            int d;
            if (*p>='0'&&*p<='9') d=*p-'0';
            else if (*p>='a'&&*p<='f') d=*p-'a'+10;
            else break;
            v32 = v32*16 + d; digits++;
        }
        if (digits == 6) { *out = v32; return 1; }
        if (digits == 3) {                       /* #abc -> #aabbcc */
            uint32_t r=(v32>>8)&0xf, g=(v32>>4)&0xf, bl=v32&0xf;
            *out = (r*0x11<<16)|(g*0x11<<8)|(bl*0x11); return 1;
        }
        return 0;
    }
    struct { const char *n; uint32_t c; } named[] = {
        {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},
        {"blue",0x0000ff},{"gray",0x808080},{"grey",0x808080},{"silver",0xc0c0c0},
        {"navy",0x000080},{"teal",0x008080},{"orange",0xffa500},{"purple",0x800080},
        {"yellow",0xffff00},{"maroon",0x800000},{"lime",0x00ff00},{"aqua",0x00ffff},
    };
    for (unsigned i = 0; i < sizeof named/sizeof named[0]; i++)
        if (ieq(c, named[i].n)) { *out = named[i].c; return 1; }
    return 0;
}

/* parse one "prop: value" into *st */
static void apply_decl(const char *prop, int plen, const char *val, int vlen,
                       struct css_style *st)
{
    char p[24]; int n = 0;
    while (n < plen && n < 23) { p[n]=lc(prop[n]); n++; } p[n]=0;
    while (vlen && (val[0]==' '||val[0]=='\t')) { val++; vlen--; }

    if (ieq(p,"color")) { uint32_t c; if (parse_color(val,vlen,&c)) { st->has_color=1; st->color=c; } }
    else if (ieq(p,"background-color")||ieq(p,"background")) { uint32_t c; if (parse_color(val,vlen,&c)) { st->has_bg=1; st->bg=c; } }
    else if (ieq(p,"font-size")) {
        int px = 0; for (int i=0;i<vlen && val[i]>='0'&&val[i]<='9';i++) px=px*10+(val[i]-'0');
        if (px) { st->has_size=1; st->scale = px>=28?3 : px>=18?2 : 1; }
    }
    else if (ieq(p,"font-weight")) {
        st->bold = (vlen>=4 && (lc(val[0])=='b' || val[0]>='6')) ? 1 : 0; /* bold or >=600 */
    }
    else if (ieq(p,"text-align")) {
        st->center = (vlen>=6 && lc(val[0])=='c') ? 1 : 0;
    }
    else if (ieq(p,"display")) {
        if (vlen>=4 && lc(val[0])=='n') st->hidden = 1;     /* none */
    }
}

static void parse_decls(const char *text, int len, struct css_style *st)
{
    int i = 0;
    while (i < len) {
        while (i < len && (text[i]==' '||text[i]=='\n'||text[i]=='\t'||text[i]==';')) i++;
        int ps = i;
        while (i < len && text[i] != ':' && text[i] != '}') i++;
        if (i >= len || text[i] != ':') break;
        int pe = i; i++;
        int vs = i;
        while (i < len && text[i] != ';' && text[i] != '}') i++;
        apply_decl(text+ps, pe-ps, text+vs, i-vs, st);
    }
}

void css_parse_inline(const char *text, int len, struct css_style *out)
{
    parse_decls(text, len, out);
}

static void add_rule(const char *sel, int sellen, const struct css_style *st)
{
    if (nrules >= MAX_RULES) return;
    while (sellen && (sel[0]==' '||sel[0]=='\t'||sel[0]=='\n')) { sel++; sellen--; }
    while (sellen && (sel[sellen-1]==' '||sel[sellen-1]=='\t'||sel[sellen-1]=='\n')) sellen--;
    if (!sellen) return;
    struct rule *r = &rules[nrules];
    int off = 0;
    if (sel[0]=='#') { r->kind=SEL_ID; off=1; }
    else if (sel[0]=='.') { r->kind=SEL_CLASS; off=1; }
    else r->kind = SEL_TAG;
    int n = 0;
    for (int i = off; i < sellen && n < 31; i++) {
        char c = sel[i];
        if (c==' '||c=='>'||c=='.'||c=='#'||c==':') break;  /* one simple selector only */
        r->name[n++] = lc(c);
    }
    r->name[n] = 0;
    if (!n) return;
    r->style = *st;
    nrules++;
}

void css_add_stylesheet(const char *text, int len)
{
    int i = 0;
    while (i < len) {
        /* selector list up to '{' */
        int ss = i;
        while (i < len && text[i] != '{' && text[i] != '}') i++;
        if (i >= len || text[i] != '{') break;
        int se = i; i++;
        int ds = i;
        while (i < len && text[i] != '}') i++;
        int de = i; if (i < len) i++;

        struct css_style st; memset(&st, 0, sizeof st);
        st.bold = st.center = st.hidden = -1;
        parse_decls(text+ds, de-ds, &st);

        /* split the selector list on commas */
        int a = ss;
        while (a < se) {
            int b = a;
            while (b < se && text[b] != ',') b++;
            add_rule(text+a, b-a, &st);
            a = b + 1;
        }
    }
}

static void merge(struct css_style *o, const struct css_style *r)
{
    if (r->has_color) { o->has_color=1; o->color=r->color; }
    if (r->has_bg)    { o->has_bg=1; o->bg=r->bg; }
    if (r->has_size)  { o->has_size=1; o->scale=r->scale; }
    if (r->bold   >= 0) o->bold = r->bold;
    if (r->center >= 0) o->center = r->center;
    if (r->hidden >= 0) o->hidden = r->hidden;
}

void css_match(const char *tag, const char *cls, const char *id,
               struct css_style *out)
{
    /* tag rules, then class, then id — id is most specific */
    for (int pass = 0; pass < 3; pass++) {
        int kind = pass==0?SEL_TAG : pass==1?SEL_CLASS : SEL_ID;
        const char *key = pass==0?tag : pass==1?cls : id;
        if (!key || !key[0]) continue;
        for (int i = 0; i < nrules; i++)
            if (rules[i].kind == kind && ieq(rules[i].name, key))
                merge(out, &rules[i].style);
    }
}
