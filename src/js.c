/*
 * js.c — the tiny JavaScript interpreter.
 *
 * Three classic stages: a lexer turns source text into tokens; a
 * recursive-descent parser (with precedence climbing for binary
 * operators) builds an abstract syntax tree of nodes; an evaluator
 * walks that tree, carrying an environment chain of variable scopes.
 * Functions capture their defining scope (closures). The DOM is three
 * special objects — document, console, and an element handle from
 * getElementById — whose methods call back into the browser.
 *
 * Variadic children (a block's statements, a call's arguments, a
 * function's parameters) are kept as singly-linked sibling lists via
 * each node's `next` pointer — so a call argument that is itself a call
 * nests cleanly.
 *
 * Numbers are int64 (the kernel has no FPU); that's the one deliberate
 * deviation from JS, called out in js.h. Everything lives in arenas
 * allocated once per run; the interpreter is not reentrant (the browser
 * runs one script at a time).
 */

#include "js.h"
#include "mem.h"
#include "lib.h"

#define MAX_NODES 4000
#define STRHEAP   (64*1024)
#define MAX_VARS  16        /* vars per scope (small: keeps each env cheap) */
#define MAX_ENVS  2048      /* call frames aren't freed, so allow plenty */

/* ---- operator codes (multi-char ops get values past 256) ---------- */
enum { OP_EQ=256, OP_NE, OP_LE, OP_GE, OP_AND, OP_OR, OP_INC, OP_DEC };

/* ---- AST ---------------------------------------------------------- */
enum {
    N_NUM, N_STR, N_BOOL, N_NULL, N_IDENT, N_BIN, N_LOGICAL, N_UNARY,
    N_ASSIGN, N_MEMBER, N_MEMBER_SET, N_CALL, N_IF, N_WHILE, N_FOR,
    N_BLOCK, N_VAR, N_RETURN, N_FUNC, N_EXPR
};
struct node {
    uint8_t kind;
    int     op;
    int64_t num;
    char   *str;
    struct node *a, *b, *c, *d;     /* fixed children (cond/then/else/init/...) */
    struct node *first;             /* head of a child list (args / stmts)      */
    struct node *params;            /* function parameter list (N_IDENT chain)  */
    struct node *next;              /* sibling in a child list                  */
};

/* ---- values ------------------------------------------------------- */
enum { V_UNDEF, V_NULL, V_NUM, V_BOOL, V_STR, V_FUNC, V_OBJ, V_NATIVE };
enum { OBJ_DOCUMENT, OBJ_CONSOLE, OBJ_ELEMENT };
enum { NAT_LOG, NAT_WRITE, NAT_GETEL };

struct val {
    uint8_t type;
    int64_t num;
    char   *str;
    struct node *fn; struct env *clo;
    int     obj; char *objid;
    int     nat;
};

struct env {
    struct env *parent;
    int   n;
    char *names[MAX_VARS];
    struct val vals[MAX_VARS];
};

struct ctx {
    const char *src, *p;
    int   tk;
    int64_t tnum;
    char *tstr;
    struct node *nodes; int nnode;
    char *sh; int shn;
    struct env *envs; int nenv;
    struct js_host *host;
    int   returning; struct val retval;
    int   error;
    int   depth;            /* recursion guard so a deep script can't smash the stack */
};

enum { T_EOF=1, T_NUM, T_STR, T_ID, T_KW_VAR, T_KW_FUNC, T_KW_IF, T_KW_ELSE,
       T_KW_WHILE, T_KW_FOR, T_KW_RETURN, T_KW_TRUE, T_KW_FALSE, T_KW_NULL };

/* ---- arena helpers ------------------------------------------------- */
static struct node *nnew(struct ctx *c, int kind)
{
    if (c->nnode >= MAX_NODES) { c->error = 1; return &c->nodes[0]; }
    struct node *n = &c->nodes[c->nnode++];
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    return n;
}
static char *salloc(struct ctx *c, const char *s, int len)
{
    if (c->shn + len + 1 > STRHEAP) { c->error = 1; return c->sh; }
    char *out = c->sh + c->shn;
    memcpy(out, s, len); out[len] = 0;
    c->shn += len + 1;
    return out;
}

/* ---- lexer -------------------------------------------------------- */
static int kw(const char *s, int n)
{
    struct { const char *w; int t; } K[] = {
        {"var",T_KW_VAR},{"let",T_KW_VAR},{"const",T_KW_VAR},{"function",T_KW_FUNC},
        {"if",T_KW_IF},{"else",T_KW_ELSE},{"while",T_KW_WHILE},{"for",T_KW_FOR},
        {"return",T_KW_RETURN},{"true",T_KW_TRUE},{"false",T_KW_FALSE},{"null",T_KW_NULL},
    };
    for (unsigned i=0;i<sizeof K/sizeof K[0];i++)
        if ((int)strlen(K[i].w)==n && !strncmp(s,K[i].w,n)) return K[i].t;
    return 0;
}
static void next(struct ctx *c)
{
    const char *p = c->p;
    for (;;) {
        while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
        if (p[0]=='/'&&p[1]=='/') { while (*p && *p!='\n') p++; continue; }
        if (p[0]=='/'&&p[1]=='*') { p+=2; while (*p && !(p[0]=='*'&&p[1]=='/')) p++; if(*p)p+=2; continue; }
        break;
    }
    if (!*p) { c->tk = T_EOF; c->p = p; return; }

    if (*p>='0'&&*p<='9') {
        int64_t v = 0;
        while (*p>='0'&&*p<='9') v = v*10 + (*p++ - '0');
        c->tk = T_NUM; c->tnum = v; c->p = p; return;
    }
    if (*p=='"' || *p=='\'') {
        char q = *p++;
        char buf[1024]; int n=0;
        while (*p && *p!=q && n<1023) {
            if (*p=='\\' && p[1]) { p++; char ec=*p++;
                buf[n++] = ec=='n'?'\n':ec=='t'?'\t':ec; }
            else buf[n++]=*p++;
        }
        if (*p==q) p++;
        c->tk = T_STR; c->tstr = salloc(c, buf, n); c->p = p; return;
    }
    if ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='_'||*p=='$') {
        const char *s = p;
        while ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='$') p++;
        int n = (int)(p - s), k = kw(s, n);
        if (k) c->tk = k; else { c->tk = T_ID; c->tstr = salloc(c, s, n); }
        c->p = p; return;
    }
    if (p[0]=='='&&p[1]=='=') { c->tk=OP_EQ; c->p=p+2; return; }
    if (p[0]=='!'&&p[1]=='=') { c->tk=OP_NE; c->p=p+2; return; }
    if (p[0]=='<'&&p[1]=='=') { c->tk=OP_LE; c->p=p+2; return; }
    if (p[0]=='>'&&p[1]=='=') { c->tk=OP_GE; c->p=p+2; return; }
    if (p[0]=='&'&&p[1]=='&') { c->tk=OP_AND; c->p=p+2; return; }
    if (p[0]=='|'&&p[1]=='|') { c->tk=OP_OR; c->p=p+2; return; }
    if (p[0]=='+'&&p[1]=='+') { c->tk=OP_INC; c->p=p+2; return; }
    if (p[0]=='-'&&p[1]=='-') { c->tk=OP_DEC; c->p=p+2; return; }
    c->tk = (unsigned char)*p; c->p = p+1;
}

/* ---- parser ------------------------------------------------------- */
static struct node *parse_expr(struct ctx *c);
static struct node *parse_stmt(struct ctx *c);

static int accept(struct ctx *c, int t){ if (c->tk==t){ next(c); return 1; } return 0; }
static void expect(struct ctx *c, int t){ if (!accept(c,t)) c->error = 1; }

static int binprec(int op)
{
    switch (op) {
    case OP_OR: return 1;
    case OP_AND: return 2;
    case OP_EQ: case OP_NE: return 3;
    case '<': case '>': case OP_LE: case OP_GE: return 4;
    case '+': case '-': return 5;
    case '*': case '/': case '%': return 6;
    }
    return 0;
}

static struct node *parse_primary(struct ctx *c)
{
    if (c->tk==T_NUM) { struct node*n=nnew(c,N_NUM); n->num=c->tnum; next(c); return n; }
    if (c->tk==T_STR) { struct node*n=nnew(c,N_STR); n->str=c->tstr; next(c); return n; }
    if (c->tk==T_KW_TRUE){ struct node*n=nnew(c,N_BOOL); n->num=1; next(c); return n; }
    if (c->tk==T_KW_FALSE){ struct node*n=nnew(c,N_BOOL); n->num=0; next(c); return n; }
    if (c->tk==T_KW_NULL){ struct node*n=nnew(c,N_NULL); next(c); return n; }
    if (c->tk==T_ID) { struct node*n=nnew(c,N_IDENT); n->str=c->tstr; next(c); return n; }
    if (accept(c,'(')) { struct node*n=parse_expr(c); expect(c,')'); return n; }
    c->error = 1; return nnew(c, N_NULL);
}

static struct node *parse_postfix(struct ctx *c)
{
    struct node *n = parse_primary(c);
    for (;;) {
        if (accept(c,'.')) {
            struct node *m = nnew(c, N_MEMBER);
            m->a = n; m->str = c->tstr; expect(c, T_ID); n = m;
        } else if (c->tk=='(') {
            next(c);
            struct node *call = nnew(c, N_CALL);
            call->a = n;
            struct node *head=0, *tail=0;
            while (c->tk != ')' && c->tk != T_EOF && !c->error) {
                struct node *arg = parse_expr(c);
                arg->next = 0;
                if (tail) tail->next = arg; else head = arg;
                tail = arg;
                if (!accept(c,',')) break;
            }
            call->first = head;
            expect(c, ')');
            n = call;
        } else break;
    }
    return n;
}

static struct node *parse_unary(struct ctx *c)
{
    if (c->tk=='!' || c->tk=='-') {
        int op=c->tk; next(c);
        struct node*n=nnew(c,N_UNARY); n->op=op; n->a=parse_unary(c); return n;
    }
    return parse_postfix(c);
}

static struct node *parse_bin(struct ctx *c, int minp)
{
    struct node *left = parse_unary(c);
    for (;;) {
        int op = c->tk, pr = binprec(op);
        if (pr == 0 || pr < minp) break;
        next(c);
        struct node *right = parse_bin(c, pr+1);
        struct node *n = nnew(c, (op==OP_AND||op==OP_OR)?N_LOGICAL:N_BIN);
        n->op = op; n->a = left; n->b = right;
        left = n;
    }
    return left;
}

static struct node *parse_assign(struct ctx *c)
{
    struct node *left = parse_bin(c, 1);
    if (c->tk=='=') {
        next(c);
        struct node *right = parse_assign(c);
        if (left->kind==N_MEMBER) {
            struct node *n=nnew(c,N_MEMBER_SET); n->a=left->a; n->str=left->str; n->b=right; return n;
        }
        struct node *n=nnew(c,N_ASSIGN); n->a=left; n->b=right; return n;
    }
    return left;
}

static struct node *parse_expr(struct ctx *c){ return parse_assign(c); }

static struct node *parse_block(struct ctx *c)
{
    struct node *n = nnew(c, N_BLOCK);
    expect(c, '{');
    struct node *head=0, *tail=0;
    while (c->tk != '}' && c->tk != T_EOF && !c->error) {
        struct node *s = parse_stmt(c);
        s->next = 0;
        if (tail) tail->next = s; else head = s;
        tail = s;
    }
    n->first = head;
    expect(c, '}');
    return n;
}

static struct node *parse_stmt(struct ctx *c)
{
    if (c->tk=='{') return parse_block(c);
    if (c->tk==T_KW_IF) {
        next(c); struct node*n=nnew(c,N_IF); expect(c,'('); n->a=parse_expr(c); expect(c,')');
        n->b=parse_stmt(c);
        if (accept(c,T_KW_ELSE)) n->c=parse_stmt(c);
        return n;
    }
    if (c->tk==T_KW_WHILE) {
        next(c); struct node*n=nnew(c,N_WHILE); expect(c,'('); n->a=parse_expr(c); expect(c,')');
        n->b=parse_stmt(c); return n;
    }
    if (c->tk==T_KW_FOR) {
        next(c); struct node*n=nnew(c,N_FOR); expect(c,'(');
        n->a = (c->tk==';')?0:parse_stmt(c);     /* init eats its own ';' */
        if (!n->a) expect(c,';');
        n->b = (c->tk==';')?0:parse_expr(c); expect(c,';');
        n->c = (c->tk==')')?0:parse_expr(c); expect(c,')');
        n->d = parse_stmt(c);
        return n;
    }
    if (c->tk==T_KW_RETURN) {
        next(c); struct node*n=nnew(c,N_RETURN);
        if (c->tk!=';' && c->tk!='}') n->a=parse_expr(c);
        accept(c,';'); return n;
    }
    if (c->tk==T_KW_VAR) {
        next(c); struct node*n=nnew(c,N_VAR); n->str=c->tstr; expect(c,T_ID);
        if (accept(c,'=')) n->a=parse_expr(c);
        accept(c,';'); return n;
    }
    if (c->tk==T_KW_FUNC) {
        next(c); struct node*n=nnew(c,N_FUNC); n->str=c->tstr; expect(c,T_ID);
        expect(c,'(');
        struct node *head=0, *tail=0;
        while (c->tk==T_ID) {
            struct node*pm=nnew(c,N_IDENT); pm->str=c->tstr; pm->next=0; next(c);
            if (tail) tail->next=pm; else head=pm; tail=pm;
            if (!accept(c,',')) break;
        }
        n->params = head;
        expect(c,')');
        n->a = parse_block(c);
        return n;
    }
    struct node *e = nnew(c, N_EXPR); e->a = parse_expr(c); accept(c,';'); return e;
}

/* ---- environment -------------------------------------------------- */
static struct env *env_new(struct ctx *c, struct env *parent)
{
    if (c->nenv >= MAX_ENVS) { c->error=1; return &c->envs[0]; }
    struct env *e = &c->envs[c->nenv++];
    e->parent = parent; e->n = 0;
    return e;
}
static struct val *env_find(struct env *e, const char *name)
{
    for (; e; e = e->parent)
        for (int i=0;i<e->n;i++)
            if (!strcmp(e->names[i], name)) return &e->vals[i];
    return 0;
}
static void env_define(struct env *e, char *name, struct val v)
{
    for (int i=0;i<e->n;i++) if (!strcmp(e->names[i],name)) { e->vals[i]=v; return; }
    if (e->n < MAX_VARS) { e->names[e->n]=name; e->vals[e->n]=v; e->n++; }
}

/* ---- values ------------------------------------------------------- */
static struct val vnum(int64_t n){ struct val v; memset(&v,0,sizeof v); v.type=V_NUM; v.num=n; return v; }
static struct val vbool(int b){ struct val v; memset(&v,0,sizeof v); v.type=V_BOOL; v.num=b!=0; return v; }
static struct val vstr(char *s){ struct val v; memset(&v,0,sizeof v); v.type=V_STR; v.str=s; return v; }
static struct val vundef(void){ struct val v; memset(&v,0,sizeof v); v.type=V_UNDEF; return v; }

static int truthy(struct val v)
{
    switch (v.type) {
    case V_NUM: case V_BOOL: return v.num != 0;
    case V_STR: return v.str && v.str[0];
    case V_NULL: case V_UNDEF: return 0;
    default: return 1;
    }
}

static char *to_str(struct ctx *c, struct val v)
{
    char buf[24];
    switch (v.type) {
    case V_STR: return v.str ? v.str : (char*)"";
    case V_NUM: {
        int64_t n=v.num; int i=0, neg=n<0; uint64_t u=neg?-(uint64_t)n:(uint64_t)n;
        char tmp[24]; int t=0;
        do { tmp[t++]='0'+(int)(u%10); u/=10; } while (u);
        if (neg) buf[i++]='-';
        while (t) buf[i++]=tmp[--t];
        buf[i]=0; return salloc(c, buf, i);
    }
    case V_BOOL: return v.num ? (char*)"true" : (char*)"false";
    case V_NULL: return (char*)"null";
    case V_UNDEF: return (char*)"undefined";
    default: return (char*)"[object]";
    }
}

static char *concat(struct ctx *c, const char *a, const char *b)
{
    int la=(int)strlen(a), lb=(int)strlen(b);
    if (c->shn + la + lb + 1 > STRHEAP) { c->error=1; return c->sh; }
    char *out = c->sh + c->shn;
    memcpy(out, a, la); memcpy(out+la, b, lb); out[la+lb]=0;
    c->shn += la+lb+1;
    return out;
}

static struct val eval(struct ctx *c, struct node *n, struct env *e);
static struct val eval_node(struct ctx *c, struct node *n, struct env *e);

static struct val eval_block(struct ctx *c, struct node *blk, struct env *e)
{
    struct val r = vundef();
    for (struct node *s = blk->first; s && !c->error && !c->returning; s = s->next)
        r = eval(c, s, e);
    return r;
}

/* The recursion entry point: a thin frame that guards depth (so a script
 * can't run the kernel stack off a cliff) and delegates to eval_node. We
 * keep the big char buffers OUT of this and eval_node by handing them to
 * leaf helpers, so each recursive frame stays small. */
static struct val eval(struct ctx *c, struct node *n, struct env *e)
{
    if (c->error || c->returning) return vundef();
    if (++c->depth > 400) { c->error = 1; c->depth--; return vundef(); }
    struct val r = eval_node(c, n, e);
    c->depth--;
    return r;
}

/* leaf: build a console.log / document.write line and hand it to the host */
static void emit_native(struct ctx *c, int nat, struct val *args, int na)
{
    char line[1200]; int o = 0;
    for (int i=0;i<na;i++) {
        char *s=to_str(c,args[i]);
        while (*s && o<1198) line[o++]=*s++;
        if (i+1<na && nat==NAT_LOG && o<1198) line[o++]=' ';
    }
    line[o]=0;
    if (nat==NAT_LOG) { if(c->host->log) c->host->log(c->host->ctx,line); }
    else { if(c->host->write) c->host->write(c->host->ctx,line); }
}

/* leaf: read an element's textContent via the host into a fresh string */
static struct val read_textcontent(struct ctx *c, const char *id)
{
    char buf[1024]; buf[0]=0;
    if (c->host->get_text) c->host->get_text(c->host->ctx, id, buf, sizeof buf);
    return vstr(salloc(c, buf, (int)strlen(buf)));
}

static struct val eval_node(struct ctx *c, struct node *n, struct env *e)
{
    if (c->error || c->returning) return vundef();
    switch (n->kind) {
    case N_NUM:  return vnum(n->num);
    case N_STR:  return vstr(n->str);
    case N_BOOL: return vbool((int)n->num);
    case N_NULL: { struct val v=vundef(); v.type=V_NULL; return v; }
    case N_IDENT: { struct val *p = env_find(e, n->str); return p ? *p : vundef(); }
    case N_UNARY: {
        struct val a = eval(c, n->a, e);
        return n->op=='!' ? vbool(!truthy(a)) : vnum(-a.num);
    }
    case N_LOGICAL: {
        struct val a = eval(c, n->a, e);
        if (n->op==OP_AND) return truthy(a) ? eval(c, n->b, e) : a;
        return truthy(a) ? a : eval(c, n->b, e);
    }
    case N_BIN: {
        struct val a = eval(c, n->a, e), b = eval(c, n->b, e);
        if (n->op=='+' && (a.type==V_STR || b.type==V_STR))
            return vstr(concat(c, to_str(c,a), to_str(c,b)));
        int64_t x=a.num, y=b.num;
        switch (n->op) {
        case '+': return vnum(x+y);
        case '-': return vnum(x-y);
        case '*': return vnum(x*y);
        case '/': return vnum(y?x/y:0);
        case '%': return vnum(y?x%y:0);
        case '<': return vbool(x<y);
        case '>': return vbool(x>y);
        case OP_LE: return vbool(x<=y);
        case OP_GE: return vbool(x>=y);
        case OP_EQ:
            if (a.type==V_STR&&b.type==V_STR) return vbool(!strcmp(a.str,b.str));
            return vbool(x==y);
        case OP_NE:
            if (a.type==V_STR&&b.type==V_STR) return vbool(strcmp(a.str,b.str)!=0);
            return vbool(x!=y);
        }
        return vundef();
    }
    case N_ASSIGN: {
        struct val v = eval(c, n->b, e);
        struct val *slot = env_find(e, n->a->str);
        if (slot) *slot = v;
        else { struct env *g=e; while (g->parent) g=g->parent; env_define(g, n->a->str, v); }
        return v;
    }
    case N_VAR: {
        struct val v = n->a ? eval(c, n->a, e) : vundef();
        env_define(e, n->str, v);
        return vundef();
    }
    case N_MEMBER: {
        struct val o = eval(c, n->a, e);
        if (o.type==V_OBJ) {
            if (o.obj==OBJ_DOCUMENT) {
                if (!strcmp(n->str,"write")) { struct val v=vundef(); v.type=V_NATIVE; v.nat=NAT_WRITE; return v; }
                if (!strcmp(n->str,"getElementById")) { struct val v=vundef(); v.type=V_NATIVE; v.nat=NAT_GETEL; return v; }
            } else if (o.obj==OBJ_CONSOLE) {
                if (!strcmp(n->str,"log")) { struct val v=vundef(); v.type=V_NATIVE; v.nat=NAT_LOG; return v; }
            } else if (o.obj==OBJ_ELEMENT) {
                if (!strcmp(n->str,"textContent"))
                    return read_textcontent(c, o.objid);
            }
        }
        return vundef();
    }
    case N_MEMBER_SET: {
        struct val o = eval(c, n->a, e);
        struct val v = eval(c, n->b, e);
        if (o.type==V_OBJ && o.obj==OBJ_ELEMENT && !strcmp(n->str,"textContent"))
            if (c->host->set_text) c->host->set_text(c->host->ctx, o.objid, to_str(c,v));
        return v;
    }
    case N_CALL: {
        struct val callee = eval(c, n->a, e);
        struct val args[8]; int na=0;
        for (struct node *ar=n->first; ar && na<8; ar=ar->next)
            args[na++] = eval(c, ar, e);
        if (callee.type==V_NATIVE) {
            if (callee.nat==NAT_LOG || callee.nat==NAT_WRITE) {
                emit_native(c, callee.nat, args, na);
                return vundef();
            }
            if (callee.nat==NAT_GETEL) {
                struct val v=vundef(); v.type=V_OBJ; v.obj=OBJ_ELEMENT;
                v.objid = na?to_str(c,args[0]):(char*)""; return v;
            }
        }
        if (callee.type==V_FUNC) {
            struct env *fe = env_new(c, callee.clo);
            int i=0;
            for (struct node *pm=callee.fn->params; pm; pm=pm->next)
                env_define(fe, pm->str, i<na?args[i++]:vundef());
            eval(c, callee.fn->a, fe);           /* the body block */
            c->returning = 0;
            struct val r = c->retval; c->retval = vundef();
            return r;
        }
        return vundef();
    }
    case N_IF:
        if (truthy(eval(c, n->a, e))) eval(c, n->b, e);
        else if (n->c) eval(c, n->c, e);
        return vundef();
    case N_WHILE: {
        int guard = 0;
        while (truthy(eval(c, n->a, e)) && !c->error && !c->returning) {
            eval(c, n->b, e);
            if (++guard > 5000000) { c->error=1; break; }
        }
        return vundef();
    }
    case N_FOR: {
        struct env *fe = env_new(c, e);
        if (n->a) eval(c, n->a, fe);
        int guard = 0;
        while ((!n->b || truthy(eval(c, n->b, fe))) && !c->error && !c->returning) {
            eval(c, n->d, fe);
            if (n->c) eval(c, n->c, fe);
            if (++guard > 5000000) { c->error=1; break; }
        }
        return vundef();
    }
    case N_BLOCK: return eval_block(c, n, e);
    case N_RETURN:
        c->retval = n->a ? eval(c, n->a, e) : vundef();
        c->returning = 1;
        return vundef();
    case N_FUNC: {
        struct val v=vundef(); v.type=V_FUNC; v.fn=n; v.clo=e;
        env_define(e, n->str, v);
        return vundef();
    }
    case N_EXPR: return eval(c, n->a, e);
    }
    return vundef();
}

int js_run(const char *src, struct js_host *host, char *err, int errlen)
{
    struct ctx *c = kmalloc(sizeof *c);
    if (!c) return -1;
    memset(c, 0, sizeof *c);
    c->nodes = kmalloc(sizeof(struct node)*MAX_NODES);
    c->sh    = kmalloc(STRHEAP);
    c->envs  = kmalloc(sizeof(struct env)*MAX_ENVS);
    c->host = host;
    if (!c->nodes||!c->sh||!c->envs) goto oom;

    c->src = c->p = src;
    next(c);

    struct env *g = env_new(c, 0);
    struct val doc=vundef(); doc.type=V_OBJ; doc.obj=OBJ_DOCUMENT;
    struct val con=vundef(); con.type=V_OBJ; con.obj=OBJ_CONSOLE;
    env_define(g, "document", doc);
    env_define(g, "console", con);

    while (c->tk != T_EOF && !c->error) {
        struct node *s = parse_stmt(c);
        eval(c, s, g);
        c->returning = 0;
    }

    int rc = c->error ? -1 : 0;
    if (rc < 0 && err) { const char *m="script error"; int i=0; while(m[i]&&i<errlen-1){err[i]=m[i];i++;} err[i]=0; }
    kfree(c->nodes); kfree(c->sh); kfree(c->envs); kfree(c);
    return rc;
oom:
    if (c->nodes) kfree(c->nodes);
    if (c->sh) kfree(c->sh);
    if (c->envs) kfree(c->envs);
    kfree(c);
    return -1;
}
