/*
 * lisp.c — a tiny Lisp, running as a MicroOS user program.
 *
 * This is the capstone: a real interpreted language, written from
 * scratch, running at EL0 on top of everything we built — loaded from
 * /bin by the ELF loader, talking to the world only through read()
 * and write() syscalls. It has no library but our own user libc, and
 * no memory but a fixed arena in its .bss. Yet it does what every
 * language does: read source text into a tree, walk the tree, and
 * compute. Define a recursive factorial and watch your own OS run it.
 *
 * Lisp is the classic "smallest real language": John McCarthy's 1960
 * eval/apply is maybe a page of math. The whole interpreter is three
 * ideas —
 *   READ   text  -> S-expression tree (cons cells)
 *   EVAL   tree + environment -> value   (the heart; see eval())
 *   PRINT  value -> text
 * — wrapped in a loop (the REPL).
 *
 * Deliberately small: integers only, no garbage collector (a fixed
 * cell arena — when it fills, we say so), single-line expressions in
 * the REPL. Closures, recursion, and lexical scope are all real.
 *
 *   (define (fact n) (if (< n 2) 1 (* n (fact (- n 1)))))
 *   (fact 10)          => 3628800
 */

#include "usys.h"
#include "ulib.h"

/* ---- the object arena --------------------------------------------- */

enum { T_NIL, T_INT, T_SYM, T_CONS, T_PRIM, T_FUNC };

typedef int Value;                  /* an index into objs[] */

struct obj {
    int  type;
    long ival;                      /* T_INT value / T_SYM id / T_PRIM id */
    Value car, cdr;                 /* T_CONS; T_FUNC: car=params cdr=body */
    Value env;                      /* T_FUNC: captured environment */
};

#define MAXOBJ  16384
static struct obj objs[MAXOBJ];
static int nobj = 1;                /* obj[0] is NIL, allocated below */

#define NIL 0

static int oom;                     /* out-of-memory / error flag */

static Value alloc(int type)
{
    if (nobj >= MAXOBJ) {
        oom = 1;
        return NIL;
    }
    Value v = nobj++;
    objs[v].type = type;
    objs[v].car = objs[v].cdr = objs[v].env = NIL;
    objs[v].ival = 0;
    return v;
}

static int   type(Value v)  { return objs[v].type; }
static Value car(Value v)   { return type(v) == T_CONS ? objs[v].car : NIL; }
static Value cdr(Value v)   { return type(v) == T_CONS ? objs[v].cdr : NIL; }

static Value mkint(long n)  { Value v = alloc(T_INT);  objs[v].ival = n; return v; }
static Value cons(Value a, Value d)
{
    Value v = alloc(T_CONS);
    objs[v].car = a;
    objs[v].cdr = d;
    return v;
}

/* ---- symbols (interned, compared by id) --------------------------- */

#define MAXSYM 512
static char  symname[MAXSYM][20];
static int   nsym;

static Value intern(const char *s)
{
    for (int i = 0; i < nsym; i++)
        if (ustrcmp(symname[i], s) == 0)
            goto found;
    if (nsym >= MAXSYM) { oom = 1; return NIL; }
    {
        int j = 0;
        while (s[j] && j < 19) { symname[nsym][j] = s[j]; j++; }
        symname[nsym][j] = 0;
    }
    nsym++;
found:;
    /* a symbol value carries its id; intern returns a fresh T_SYM obj */
    Value v = alloc(T_SYM);
    for (int i = 0; i < nsym; i++)
        if (ustrcmp(symname[i], s) == 0) { objs[v].ival = i; break; }
    return v;
}
static int symid(Value v) { return (int)objs[v].ival; }

/* Special-form / common symbol ids, interned once at startup. */
static Value s_quote, s_if, s_define, s_lambda, s_begin, s_let, s_true;

/* ---- the global environment (a flat, mutable table) --------------- */
/* Lexical scope (lambda params, let) lives in an assoc-list `env`
 * threaded through eval; anything not found there falls back to this
 * global table. Keeping globals separate is what lets a function refer
 * to another defined later, and lets closures see top-level defines. */

#define MAXG 512
static int   g_sym[MAXG];
static Value g_val[MAXG];
static int   ng;

static void g_define(int sym, Value val)
{
    for (int i = 0; i < ng; i++)
        if (g_sym[i] == sym) { g_val[i] = val; return; }
    if (ng < MAXG) { g_sym[ng] = sym; g_val[ng] = val; ng++; }
}

/* ---- the reader: text -> S-expressions ---------------------------- */

static const char *cur;             /* parser cursor over the source */

static void skip_ws(void)
{
    for (;;) {
        while (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')
            cur++;
        if (*cur == ';') {          /* comment to end of line */
            while (*cur && *cur != '\n')
                cur++;
        } else {
            break;
        }
    }
}

static Value read_expr(void);

static Value read_list(void)
{
    skip_ws();
    if (*cur == ')') { cur++; return NIL; }
    if (*cur == 0)   { oom = 1; return NIL; }   /* unterminated */
    Value head = read_expr();
    Value rest = read_list();
    return cons(head, rest);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static Value read_atom(void)
{
    char tok[20];
    int i = 0;
    while (*cur && *cur != ' ' && *cur != '\t' && *cur != '\n' &&
           *cur != '\r' && *cur != '(' && *cur != ')' && i < 19)
        tok[i++] = *cur++;
    tok[i] = 0;

    int numeric = is_digit(tok[0]) || (tok[0] == '-' && is_digit(tok[1]));
    if (numeric)
        return mkint(uatoi(tok));
    return intern(tok);
}

static Value read_expr(void)
{
    skip_ws();
    if (*cur == 0)   return -1;                 /* sentinel: end of input */
    if (*cur == '(') { cur++; return read_list(); }
    if (*cur == ')') { cur++; oom = 1; return NIL; }
    if (*cur == '\'') { cur++; return cons(s_quote, cons(read_expr(), NIL)); }
    return read_atom();
}

/* ---- print: value -> text ----------------------------------------- */

static void print_val(Value v)
{
    switch (type(v)) {
    case T_NIL:  puts("()"); break;
    case T_INT:  printf("%d", (int)objs[v].ival); break;
    case T_SYM:  puts(symname[symid(v)]); break;
    case T_PRIM: puts("#<builtin>"); break;
    case T_FUNC: puts("#<lambda>"); break;
    case T_CONS:
        puts("(");
        for (Value p = v; type(p) == T_CONS; p = cdr(p)) {
            print_val(car(p));
            if (type(cdr(p)) == T_CONS)
                puts(" ");
        }
        puts(")");
        break;
    }
}

/* ---- eval / apply: the heart -------------------------------------- */

static Value eval(Value x, Value env);

static Value lookup(Value sym, Value env)
{
    int id = symid(sym);
    for (Value e = env; type(e) == T_CONS; e = cdr(e)) {
        Value pair = car(e);                    /* (sym . val) */
        if (type(car(pair)) == T_SYM && symid(car(pair)) == id)
            return cdr(pair);
    }
    for (int i = 0; i < ng; i++)
        if (g_sym[i] == id)
            return g_val[i];
    printf("error: unbound symbol '%s'\n", symname[id]);
    oom = 1;
    return NIL;
}

static Value eval_list(Value list, Value env)  /* eval each, build new list */
{
    if (type(list) != T_CONS)
        return NIL;
    Value a = eval(car(list), env);
    return cons(a, eval_list(cdr(list), env));
}

static long ival(Value v) { return type(v) == T_INT ? objs[v].ival : 0; }

/* The builtins, dispatched by the prim id stored in the T_PRIM obj. */
enum { P_ADD, P_SUB, P_MUL, P_DIV, P_LT, P_GT, P_EQ,
       P_CONS, P_CAR, P_CDR, P_LIST, P_NULL, P_NOT, P_PRINT, P_EXIT };

static Value apply_prim(int id, Value args)
{
    Value a = car(args), b = car(cdr(args));

    /* +, *, and (multi-arg) - fold over ALL arguments, like real Lisp:
     * (+ 1 2 3) => 6. Comparisons stay binary. */
    if (id == P_ADD || id == P_MUL) {
        long acc = (id == P_MUL) ? 1 : 0;
        for (Value p = args; type(p) == T_CONS; p = cdr(p))
            acc = (id == P_MUL) ? acc * ival(car(p)) : acc + ival(car(p));
        return mkint(acc);
    }
    if (id == P_SUB) {
        if (type(cdr(args)) != T_CONS)
            return mkint(-ival(a));              /* unary negate */
        long acc = ival(a);
        for (Value p = cdr(args); type(p) == T_CONS; p = cdr(p))
            acc -= ival(car(p));
        return mkint(acc);
    }

    switch (id) {
    case P_DIV: return mkint(ival(b) ? ival(a) / ival(b) : 0);
    case P_LT:  return ival(a) <  ival(b) ? s_true : NIL;
    case P_GT:  return ival(a) >  ival(b) ? s_true : NIL;
    case P_EQ:  return ival(a) == ival(b) ? s_true : NIL;
    case P_CONS:return cons(a, b);
    case P_CAR: return car(a);
    case P_CDR: return cdr(a);
    case P_LIST:return args;
    case P_NULL:return a == NIL ? s_true : NIL;
    case P_NOT: return a == NIL ? s_true : NIL;
    case P_PRINT: print_val(a); puts("\n"); return a;
    case P_EXIT: exit(0);
    }
    return NIL;
}

static Value apply(Value fn, Value args)
{
    if (type(fn) == T_PRIM)
        return apply_prim((int)objs[fn].ival, args);

    if (type(fn) == T_FUNC) {
        /* bind params to args onto the closure's captured environment:
         * each binding is a (sym . val) pair, prepended to the env. */
        Value env = objs[fn].env;
        Value p = objs[fn].car;     /* params */
        Value a = args;
        while (type(p) == T_CONS) {
            Value pair = cons(NIL, NIL);
            objs[pair].car = car(p);
            objs[pair].cdr = car(a);
            env = cons(pair, env);
            p = cdr(p);
            a = cdr(a);
        }
        Value r = NIL;
        for (Value b = objs[fn].cdr; type(b) == T_CONS; b = cdr(b))
            r = eval(car(b), env);  /* body is an implicit (begin ...) */
        return r;
    }

    puts("error: not a function\n");
    oom = 1;
    return NIL;
}

static Value eval(Value x, Value env)
{
    if (oom)
        return NIL;

    switch (type(x)) {
    case T_INT: case T_NIL: case T_PRIM: case T_FUNC:
        return x;                   /* self-evaluating */
    case T_SYM:
        return lookup(x, env);
    case T_CONS:
        break;                      /* a form to evaluate, below */
    default:
        return x;
    }

    Value op = car(x);

    /* special forms (operator not evaluated) */
    if (type(op) == T_SYM) {
        int id = symid(op);
        if (id == symid(s_quote))
            return car(cdr(x));
        if (id == symid(s_if)) {
            Value test = eval(car(cdr(x)), env);
            if (test != NIL)
                return eval(car(cdr(cdr(x))), env);
            return eval(car(cdr(cdr(cdr(x)))), env);
        }
        if (id == symid(s_define)) {
            Value target = car(cdr(x));
            if (type(target) == T_CONS) {
                /* (define (f a b) body...) => f = (lambda (a b) body...) */
                Value fn = alloc(T_FUNC);
                objs[fn].car = cdr(target);             /* params */
                objs[fn].cdr = cdr(cdr(x));             /* body   */
                objs[fn].env = env;
                g_define(symid(car(target)), fn);
                return car(target);
            }
            Value val = eval(car(cdr(cdr(x))), env);
            g_define(symid(target), val);
            return target;
        }
        if (id == symid(s_lambda)) {
            Value fn = alloc(T_FUNC);
            objs[fn].car = car(cdr(x));                 /* params */
            objs[fn].cdr = cdr(cdr(x));                 /* body   */
            objs[fn].env = env;                         /* closure! */
            return fn;
        }
        if (id == symid(s_begin)) {
            Value r = NIL;
            for (Value b = cdr(x); type(b) == T_CONS; b = cdr(b))
                r = eval(car(b), env);
            return r;
        }
        if (id == symid(s_let)) {
            /* (let ((a 1) (b 2)) body...) */
            Value e = env;
            for (Value bs = car(cdr(x)); type(bs) == T_CONS; bs = cdr(bs)) {
                Value bind = car(bs);
                Value val = eval(car(cdr(bind)), env);
                Value pair = cons(NIL, NIL);
                objs[pair].car = car(bind);
                objs[pair].cdr = val;
                e = cons(pair, e);
            }
            Value r = NIL;
            for (Value b = cdr(cdr(x)); type(b) == T_CONS; b = cdr(b))
                r = eval(car(b), e);
            return r;
        }
    }

    /* a function call: evaluate operator and operands, then apply */
    Value fn = eval(op, env);
    Value args = eval_list(cdr(x), env);
    return apply(fn, args);
}

/* ---- setup, REPL, and script mode --------------------------------- */

static void defprim(const char *name, int id)
{
    Value v = alloc(T_PRIM);
    objs[v].ival = id;
    g_define(symid(intern(name)), v);
}

static void setup(void)
{
    objs[NIL].type = T_NIL;
    s_quote  = intern("quote");
    s_if     = intern("if");
    s_define = intern("define");
    s_lambda = intern("lambda");
    s_begin  = intern("begin");
    s_let    = intern("let");
    s_true   = intern("t");
    g_define(symid(s_true), s_true);    /* t evaluates to itself */

    defprim("+", P_ADD);   defprim("-", P_SUB);   defprim("*", P_MUL);
    defprim("/", P_DIV);   defprim("<", P_LT);    defprim(">", P_GT);
    defprim("=", P_EQ);    defprim("cons", P_CONS); defprim("car", P_CAR);
    defprim("cdr", P_CDR); defprim("list", P_LIST); defprim("null?", P_NULL);
    defprim("not", P_NOT); defprim("print", P_PRINT); defprim("exit", P_EXIT);
}

/* Evaluate every top-level form in `src`. In REPL mode (echo=1) print
 * each result. */
static void run(const char *src, int echo)
{
    cur = src;
    for (;;) {
        oom = 0;
        Value form = read_expr();
        if (form == -1)             /* end of input */
            break;
        Value result = eval(form, NIL);
        if (oom) {
            if (nobj >= MAXOBJ)
                puts("error: out of memory (arena full)\n");
        } else if (echo) {
            puts("=> ");
            print_val(result);
            puts("\n");
        }
    }
}

int main(int argc, char **argv)
{
    setup();

    if (argc > 1) {                 /* script mode: lisp <file> */
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            printf("lisp: cannot open %s\n", argv[1]);
            return 1;
        }
        static char src[8192];
        long n = 0, r;
        while ((r = read(fd, src + n, sizeof(src) - 1 - n)) > 0)
            n += r;
        src[n] = 0;
        close(fd);
        run(src, 0);
        return 0;
    }

    /* interactive REPL */
    puts("tiny-lisp on MicroOS. try: (define (fact n) "
         "(if (< n 2) 1 (* n (fact (- n 1)))))  then  (fact 10)\n");
    puts("type (exit) to leave.\n");
    char line[256];
    for (;;) {
        puts("lisp> ");
        long n = read(0, line, sizeof(line) - 1);
        if (n <= 0)
            break;                  /* EOF */
        line[n] = 0;
        run(line, 1);
    }
    return 0;
}
