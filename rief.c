// SPDX-License-Identifier: GPL-2.0-only
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_TOKEN = 262144,
    MAX_NODE = 262144,
    MAX_TYPE = 8192,
    MAX_SYM = 8192,
    MAX_FUNC = 64,
    MAX_RELOC = 4096,
    MAX_CODE = 4 * 1024 * 1024,
    MAX_DATA = 4 * 1024 * 1024
};
typedef struct Type Type;
typedef struct Node Node;
typedef struct Sym Sym;
typedef struct Field Field;
struct Field {
    char name[96];
    Type *type;
    int offset;
    Field *next;
};
struct Type {
    int size, align, uns, count;
    Type *base;
    Field *fields;
    char name[96];
};
typedef struct {
    int kind, line;
    uint64_t value;
    char *text;
    int length;
} Token;
struct Sym {
    char name[96];
    Type *type;
    int offset, global, tls, depth, active;
};
struct Node {
    int kind, op, line;
    Type *type;
    Node *a, *b, *c, *d, *next;
    Sym *sym;
    uint64_t value;
    int func;
};
typedef struct {
    char name[96];
    Type *result, *params[6];
    Sym *args[6];
    int argc, declared, defined, offset, frame;
    Node *body;
} Function;
typedef struct {
    uint32_t patch, type, target, reserved;
    uint64_t offset, to;
    int64_t add;
} Reloc;
typedef struct {
    const char *name;
    int nr, args;
} Builtin;
static const Builtin builtins[] = {{"exit", 0, 1},
{"write", 1, 3},
{"yield", 2, 0},
{"sleep", 3, 1},
{"getpid", 4, 0},
{"ticks", 5, 0},
{"send", 6, 3},
{"recv", 7, 2},
{"open", 8, 2},
{"read", 9, 3},
{"close", 10, 1},
{"mkdir", 11, 1},
{"sync", 12, 0},
{"spawn", 13, 4},
{"procinfo", 14, 2},
{"waitpid", 15, 3},
{"control", 16, 2},
{"admin", 17, 1},
{"identity", 18, 1},
{"service_register", 19, 1},
{"service_lookup", 20, 1},
{"getcpu", 21, 0},
{"tlsbase", 22, 0},
{"getargs", 23, 2},
{"seek", 24, 3},
{"map", 25, 3},
{"service_reply", 26, 3},
{"diskinfo", 27, 2},
{"partitioninfo", 28, 2},
{NULL, 0, 0}};
static Token tokens[MAX_TOKEN];
static int nt, pos;
static Node nodes[MAX_NODE];
static int nn;
static Type types[MAX_TYPE];
static int ntype;
static Sym symbols[MAX_SYM];
static int nsym, depth, locals, curfunc = -1;
static Function funcs[MAX_FUNC];
static int nf;
static Type *tvoid, *tu8, *tu16, *tu32, *ti64, *tu64;
static uint8_t code[MAX_CODE], data[MAX_DATA], rodata[MAX_DATA];
static int nc, nd, nro, ntls = 64, spdepth;
static Reloc relocs[MAX_RELOC];
static int nr;
static struct {
    int at, fn;
} calls[8192];
static int ncall;
static const char *input_name;
static void die(int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: ", input_name ? input_name : "rief", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static void *alloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p)
        die(0, "out of memory");
    return p;
}
static void namecopy(char out[96], const char *s)
{
    if (strlen(s) >= 96)
        die(0, "name too long");
    strcpy(out, s);
}
static char *slice(const char *p, size_t n)
{
    char *s = alloc(n + 1);
    memcpy(s, p, n);
    return s;
}
static int escape(const char **p, int line)
{
    int c = (unsigned char)*(*p)++;
    if (!c)
        die(line, "unfinished escape");
    if (c == 'n')
        return '\n';
    if (c == 'r')
        return '\r';
    if (c == 't')
        return '\t';
    if (c == '0')
        return 0;
    if (c == '\\' || c == '\'' || c == '"')
        return c;
    if (c == 'x') {
        int v = 0, n = 0;
        while (n < 2 && isxdigit((unsigned char)**p)) {
            int d = tolower((unsigned char)*(*p)++);
            v = v * 16 + (d <= '9' ? d - '0' : d - 'a' + 10);
            ++n;
        }
        if (!n)
            die(line, "expected hex escape digits");
        return v;
    }
    die(line, "unsupported escape");
    return 0;
}
static void lex(const char *p)
{
    int line = 1;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            if (*p++ == '\n')
                ++line;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                ++p;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p++ == '\n')
                    ++line;
            }
            if (!*p)
                die(line, "unclosed comment");
            p += 2;
            continue;
        }
        if (nt + 1 >= MAX_TOKEN)
            die(line, "too many tokens");
        Token *t = &tokens[nt++];
        t->line = line;
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *a = p++;
            while (isalnum((unsigned char)*p) || *p == '_')
                ++p;
            t->kind = 1;
            t->text = slice(a, (size_t)(p - a));
        } else if (isdigit((unsigned char)*p)) {
            char *end;
            errno = 0;
            t->value = strtoull(p, &end, 0);
            if (errno || end == p)
                die(line, "invalid integer");
            p = end;
            while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L')
                ++p;
            t->kind = 2;
            t->text = slice("number", 6);
        } else if (*p == '"' || *p == '\'') {
            int quote = *p++, n = 0;
            char *s = alloc(strlen(p) + 1);
            while (*p && *p != quote) {
                if (*p == '\n')
                    die(line, "newline inside literal");
                s[n++] = (char)(*p == '\\' ? (++p, escape(&p, line)) : (unsigned char)*p++);
            }
            if (*p++ != quote)
                die(line, "unclosed literal");
            if (quote == '\'') {
                if (n != 1)
                    die(line, "character literal must have one byte");
                t->kind = 2;
                t->value = (unsigned char)s[0];
            } else {
                t->kind = 3;
                t->length = n;
            }
            t->text = s;
        } else {
            static const char *ops[] = {
                "<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++",
                "--",  "+=",  "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", NULL};
                int n = 1;
                for (int i = 0; ops[i]; ++i)
                    if (!strncmp(p, ops[i], strlen(ops[i]))) {
                        n = (int)strlen(ops[i]);
                        break;
                    }
                    t->kind = 4;
                t->text = slice(p, (size_t)n);
                p += n;
        }
    }
    tokens[nt].text = "<eof>";
    tokens[nt].line = line;
}
static int is(const char *s) { return tokens[pos].kind && strcmp(tokens[pos].text, s) == 0; }
static int eat(const char *s)
{
    if (is(s)) {
        ++pos;
        return 1;
    }
    return 0;
}
static void need(const char *s)
{
    if (!eat(s))
        die(tokens[pos].line, "expected '%s', got '%s'", s, tokens[pos].text);
}
static char *ident(void)
{
    if (tokens[pos].kind != 1)
        die(tokens[pos].line, "expected identifier");
    return tokens[pos++].text;
}
static Type *type_new(int size, int align, int uns)
{
    if (ntype == MAX_TYPE)
        die(0, "too many types");
    Type *t = &types[ntype++];
    t->size = size;
    t->align = align;
    t->uns = uns;
    return t;
}
static Type *ptr(Type *base)
{
    Type *t = type_new(8, 8, 1);
    t->base = base;
    return t;
}
static Type *array(Type *base, int n)
{
    if (n < 1 || base->size < 1 || n > MAX_DATA / base->size)
        die(tokens[pos].line, "invalid array size");
    Type *t = ptr(base);
    t->count = n;
    t->size = n * base->size;
    t->align = base->align;
    return t;
}
static Type *decay(Type *t) { return t->count ? ptr(t->base) : t; }
static int same(Type *a, Type *b)
{
    if (a == b)
        return 1;
    if (a->base && b->base)
        return same(a->base, b->base) && a->count == b->count;
    return !a->base && !b->base && !a->fields && !b->fields && a->size == b->size &&
    a->uns == b->uns;
}
static int typename_(void)
{
    return is("int") || is("long") || is("i64") || is("u64") || is("u32") || is("u16") ||
    is("u8") || is("char") || is("void") || is("struct") || is("const") || is("unsigned");
}
static Type *basetype(void);
static Type *declarator(Type *t, char **name, int optional)
{
    while (eat("*")) {
        t = ptr(t);
        eat("const");
    }
    if (tokens[pos].kind == 1)
        *name = ident();
    else if (!optional)
        die(tokens[pos].line, "expected declaration name");
    if (eat("[")) {
        if (tokens[pos].kind != 2 || tokens[pos].value > MAX_DATA)
            die(tokens[pos].line, "array bound must be a positive integer literal");
        int n = (int)tokens[pos++].value;
        need("]");
        t = array(t, n);
    }
    return t;
}
static Type *basetype(void)
{
    eat("const");
    if (eat("unsigned")) {
        if (eat("char"))
            return tu8;
        eat("long");
        eat("int");
        return tu64;
    }
    if (eat("int") || eat("long") || eat("i64"))
        return ti64;
    if (eat("u64"))
        return tu64;
    if (eat("u32"))
        return tu32;
    if (eat("u16"))
        return tu16;
    if (eat("char") || eat("u8"))
        return tu8;
    if (eat("void"))
        return tvoid;
    if (eat("struct")) {
        char *name = ident();
        Type *t = NULL;
        for (int i = 0; i < ntype; ++i)
            if (types[i].name[0] && !strcmp(types[i].name, name))
                t = &types[i];
        if (!t) {
            t = type_new(0, 1, 0);
            namecopy(t->name, name);
        }
        if (eat("{")) {
            if (t->size)
                die(tokens[pos].line, "duplicate struct definition");
            Field **tail = &t->fields;
            while (!eat("}")) {
                char *fn = NULL;
                Type *ft = declarator(basetype(), &fn, 0);
                need(";");
                if (!ft->size)
                    die(tokens[pos].line, "incomplete field");
                for (Field *f = t->fields; f; f = f->next)
                    if (!strcmp(f->name, fn))
                        die(tokens[pos].line, "duplicate field");
                Field *f = alloc(sizeof(*f));
                namecopy(f->name, fn);
                f->type = ft;
                f->offset = (t->size + ft->align - 1) & -ft->align;
                if (f->offset > MAX_DATA - ft->size)
                    die(tokens[pos].line, "struct too large");
                t->size = f->offset + ft->size;
                if (ft->align > t->align)
                    t->align = ft->align;
                *tail = f;
                tail = &f->next;
            }
            t->size = (t->size + t->align - 1) & -t->align;
        }
        return t;
    }
    die(tokens[pos].line, "expected type");
    return NULL;
}
static Node *node(int kind, Node *a, Node *b)
{
    if (nn == MAX_NODE)
        die(tokens[pos].line, "too many expressions");
    Node *n = &nodes[nn++];
    n->kind = kind;
    n->a = a;
    n->b = b;
    n->line = tokens[pos].line;
    n->type = ti64;
    return n;
}
enum {
    NUM = 1,
    VAR,
    STR,
    CALL,
    ADDR,
    DEREF,
    CAST,
    SIZEOF_,
    NEG,
    NOT,
    BITNOT,
    BINARY,
    ASSIGN,
    COND,
    POST,
    PRE,
    MEMBER,
    BLOCK,
    EXPR,
    IF,
    WHILE,
    FOR,
    RETURN,
    BREAK,
    CONTINUE
};
enum { ADD = 1, SUB, MUL, DIV, MOD, SHL, SHR, LT, LE, GT, GE, EQ, NE, BAND, BXOR, BOR, LAND, LOR };
static Node *num(uint64_t v)
{
    Node *n = node(NUM, NULL, NULL);
    n->value = v;
    return n;
}
static Sym *findsym(const char *s)
{
    for (int i = nsym - 1; i >= 0; --i)
        if (symbols[i].active && !strcmp(symbols[i].name, s))
            return &symbols[i];
    return NULL;
}
static Sym *addsym(const char *name, Type *type, int global, int tls)
{
    if (nsym == MAX_SYM)
        die(0, "too many symbols");
    for (int i = nsym - 1; i >= 0; --i)
        if (symbols[i].active && symbols[i].depth == depth && !strcmp(symbols[i].name, name))
            die(tokens[pos].line, "duplicate variable '%s'", name);
    if (!type->size)
        die(tokens[pos].line, "incomplete variable type");
    Sym *s = &symbols[nsym++];
    namecopy(s->name, name);
    s->type = type;
    s->global = global;
    s->tls = tls;
    s->depth = depth;
    s->active = 1;
    int *offset = global ? (tls ? &ntls : &nd) : &locals;
    *offset = (*offset + type->align - 1) & -type->align;
    if (*offset > (global ? (tls ? 65536 : MAX_DATA) : 16384) - type->size)
        die(tokens[pos].line, "variable storage limit");
    s->offset = *offset;
    *offset += type->size;
    if (!global)
        s->offset = *offset;
    return s;
}
static Node *var(Sym *s)
{
    Node *n = node(VAR, NULL, NULL);
    n->sym = s;
    n->type = s->type;
    return n;
}
static Node *expression(void);
static Node *assign(void);
static Node *unary(void);
static int function_find(const char *name)
{
    for (int i = 0; i < nf; ++i)
        if (!strcmp(funcs[i].name, name))
            return i;
    return -1;
}
static Node *primary(void)
{
    Token *t = &tokens[pos];
    if (t->kind == 2) {
        ++pos;
        return num(t->value);
    }
    if (t->kind == 3) {
        Node *n = node(STR, NULL, NULL);
        int start = nro;
        while (tokens[pos].kind == 3) {
            t = &tokens[pos++];
            if (nro > MAX_DATA - t->length - 1)
                die(t->line, "strings too large");
            memcpy(rodata + nro, t->text, (size_t)t->length);
            nro += t->length;
        }
        rodata[nro++] = 0;
        n->value = (uint64_t)start;
        n->type = array(tu8, nro - start);
        return n;
    }
    if (eat("(")) {
        Node *n = expression();
        need(")");
        return n;
    }
    char *name = ident();
    if (eat("(")) {
        Node *n = node(CALL, NULL, NULL), **tail = &n->a;
        int argc = 0;
        while (!eat(")")) {
            if (argc == 7)
                die(t->line, "too many arguments");
            *tail = assign();
            tail = &(*tail)->next;
            ++argc;
            if (eat(")"))
                break;
            need(",");
        }
        n->value = (uint64_t)argc;
        n->func = -1;
        if (!strcmp(name, "syscall")) {
            if (!argc)
                die(t->line, "syscall needs a number");
            n->func = -2;
            return n;
        }
        for (int i = 0; builtins[i].name; ++i)
            if (!strcmp(name, builtins[i].name)) {
                if (argc != builtins[i].args)
                    die(t->line, "wrong argument count for %s", name);
                n->func = -100 - i;
                return n;
            }
            int fi = function_find(name);
        if (fi < 0)
            die(t->line, "function '%s' needs a declaration", name);
        Function *f = &funcs[fi];
        if (argc != f->argc)
            die(t->line, "wrong argument count for %s", name);
        n->func = fi;
        n->type = f->result;
        Node *a = n->a;
        for (int i = 0; i < argc; ++i, a = a->next)
            if (a->type->fields || f->params[i]->fields)
                die(t->line, "pass structs by pointer");
        return n;
    }
    if (!strcmp(name, "NULL"))
        return num(0);
    Sym *s = findsym(name);
    if (!s)
        die(t->line, "unknown variable '%s'", name);
    return var(s);
}
static int lvalue(Node *n) { return n->kind == VAR || n->kind == DEREF || n->kind == MEMBER; }
static Node *binary(int op, Node *a, Node *b)
{
    Node *n = node(BINARY, a, b);
    n->op = op;
    Type *at = decay(a->type), *bt = decay(b->type);
    if (a->type->fields || b->type->fields)
        die(n->line, "aggregate arithmetic");
    if ((op == ADD || op == SUB) && at->base) {
        if (!at->base->size)
            die(n->line, "void pointer arithmetic");
        if (bt->base) {
            if (op != SUB || !same(at->base, bt->base))
                die(n->line, "invalid pointer operation");
        } else
            n->type = at;
    } else if (op == ADD && bt->base) {
        n->a = b;
        n->b = a;
        n->type = bt;
    } else if (at->base || bt->base) {
        if (op != LAND && op != LOR &&
            (op < LT || op > BOR || op == BAND || op == BOR || op == BXOR))
            die(n->line, "invalid pointer arithmetic");
    } else if (op < LT || op == BAND || op == BXOR || op == BOR)
        n->type = (at->uns && at->size == 8) || (bt->uns && bt->size == 8) ? tu64 : ti64;
    return n;
}
static Node *postfix(void)
{
    Node *n = primary();
    for (;;) {
        if (eat("[")) {
            Node *i = expression();
            need("]");
            Node *sum = binary(ADD, n, i);
            if (!sum->type->base)
                die(n->line, "indexing needs pointer");
            n = node(DEREF, sum, NULL);
            n->type = sum->type->base;
        } else if (is(".") || is("->")) {
            int arrow = eat("->");
            if (!arrow)
                need(".");
            Type *t = n->type;
            if (arrow) {
                if (!t->base)
                    die(n->line, "arrow needs pointer");
                Node *d = node(DEREF, n, NULL);
                d->type = t->base;
                n = d;
                t = d->type;
            }
            char *name = ident();
            Field *f = t->fields;
            while (f && strcmp(f->name, name))
                f = f->next;
            if (!f)
                die(n->line, "unknown field '%s'", name);
            Node *m = node(MEMBER, n, NULL);
            m->value = (uint64_t)f->offset;
            m->type = f->type;
            n = m;
        } else if (is("++") || is("--")) {
            int op = eat("++") ? ADD : (need("--"), SUB);
            if (!lvalue(n) || n->type->count || n->type->fields)
                die(n->line, "invalid increment");
            Node *p = node(POST, n, NULL);
            p->op = op;
            p->type = n->type;
            n = p;
        } else
            return n;
    }
}
static Node *unary(void)
{
    if (eat("sizeof")) {
        Type *t;
        int saved = pos;
        if (eat("(") && typename_()) {
            char *unused = NULL;
            t = declarator(basetype(), &unused, 1);
            need(")");
        } else {
            pos = saved;
            t = unary()->type;
        }
        if (!t->size)
            die(tokens[pos].line, "sizeof incomplete type");
        return num((uint64_t)t->size);
    }
    if (is("(") && tokens[pos + 1].kind == 1) {
        int saved = pos;
        ++pos;
        if (typename_()) {
            char *unused = NULL;
            Type *t = declarator(basetype(), &unused, 1);
            need(")");
            if (t->fields || t->count)
                die(tokens[pos].line, "invalid cast");
            Node *n = node(CAST, unary(), NULL);
            n->type = t;
            return n;
        }
        pos = saved;
    }
    if (eat("&")) {
        Node *a = unary();
        if (!lvalue(a))
            die(a->line, "address needs lvalue");
        Node *n = node(ADDR, a, NULL);
        n->type = ptr(a->type);
        return n;
    }
    if (eat("*")) {
        Node *a = unary();
        Type *t = decay(a->type);
        if (!t->base || !t->base->size)
            die(a->line, "dereference needs complete pointer");
        Node *n = node(DEREF, a, NULL);
        n->type = t->base;
        return n;
    }
    if (eat("+"))
        return unary();
    if (is("-") || is("!") || is("~")) {
        int kind = eat("-") ? NEG : eat("!") ? NOT : (need("~"), BITNOT);
        Node *n = node(kind, unary(), NULL);
        if (kind != NOT)
            n->type = decay(n->a->type);
        return n;
    }
    if (is("++") || is("--")) {
        int op = eat("++") ? ADD : (need("--"), SUB);
        Node *a = unary();
        if (!lvalue(a) || a->type->count || a->type->fields)
            die(a->line, "invalid increment");
        Node *n = node(PRE, a, NULL);
        n->op = op;
        n->type = a->type;
        return n;
    }
    return postfix();
}
static int opinfo(const char *s, int *prec)
{
    static const struct {
        const char *s;
        int op, prec;
    } ops[] = {{"||", LOR, 1}, {"&&", LAND, 2}, {"|", BOR, 3},  {"^", BXOR, 4}, {"&", BAND, 5},
    {"==", EQ, 6},  {"!=", NE, 6},   {"<", LT, 7},   {"<=", LE, 7},  {">", GT, 7},
    {">=", GE, 7},  {"<<", SHL, 8},  {">>", SHR, 8}, {"+", ADD, 9},  {"-", SUB, 9},
    {"*", MUL, 10}, {"/", DIV, 10},  {"%", MOD, 10}, {NULL, 0, 0}};
    for (int i = 0; ops[i].s; ++i)
        if (!strcmp(s, ops[i].s)) {
            *prec = ops[i].prec;
            return ops[i].op;
        }
        return 0;
}
static Node *binexpr(int min)
{
    Node *n = unary();
    for (;;) {
        int prec = 0, op = opinfo(tokens[pos].text, &prec);
        if (!op || prec < min)
            break;
        ++pos;
        n = binary(op, n, binexpr(prec + 1));
    }
    return n;
}
static Node *assign(void)
{
    Node *n = binexpr(1);
    if (eat("?")) {
        Node *c = node(COND, n, expression());
        need(":");
        c->c = assign();
        c->type = decay(c->b->type);
        n = c;
    }
    const char *s = tokens[pos].text;
    int op = 0, yes = 0;
    if (is("="))
        yes = 1;
    else if (strlen(s) >= 2 && s[strlen(s) - 1] == '=') {
        char buf[4] = {0};
        if (strlen(s) < 4) {
            memcpy(buf, s, strlen(s) - 1);
            int p = 0;
            op = opinfo(buf, &p);
            if (op && op < LT)
                yes = 1;
            if (op == BAND || op == BOR || op == BXOR)
                yes = 1;
        }
    }
    if (yes) {
        ++pos;
        if (!lvalue(n) || n->type->count || n->type->fields)
            die(n->line, "assignment needs scalar lvalue");
        Node *a = node(ASSIGN, n, assign());
        a->type = n->type;
        a->op = op;
        return a;
    }
    return n;
}
static Node *expression(void) { return assign(); }
static Node *statement(void);
static Node *declaration(void)
{
    char *name = NULL;
    Type *t = declarator(basetype(), &name, 0);
    Sym *s = addsym(name, t, 0, 0);
    Node *b = node(BLOCK, NULL, NULL);
    if (eat("=")) {
        if (t->count || t->fields)
            die(tokens[pos].line, "local aggregate initializer unsupported");
        Node *a = node(ASSIGN, var(s), assign());
        a->type = t;
        b->a = node(EXPR, a, NULL);
    }
    need(";");
    return b;
}
static Node *statement(void)
{
    if (eat("{")) {
        int scope = ++depth;
        Node *n = node(BLOCK, NULL, NULL), **tail = &n->a;
        while (!eat("}")) {
            if (!tokens[pos].kind)
                die(tokens[pos].line, "unclosed block");
            *tail = statement();
            tail = &(*tail)->next;
        }
        for (int i = 0; i < nsym; ++i)
            if (symbols[i].depth == scope)
                symbols[i].active = 0;
        --depth;
        return n;
    }
    if (typename_())
        return declaration();
    if (eat("if")) {
        need("(");
        Node *n = node(IF, expression(), NULL);
        need(")");
        n->b = statement();
        if (eat("else"))
            n->c = statement();
        return n;
    }
    if (eat("while")) {
        need("(");
        Node *n = node(WHILE, expression(), NULL);
        need(")");
        n->b = statement();
        return n;
    }
    if (eat("for")) {
        need("(");
        Node *n = node(FOR, NULL, NULL);
        int scope = ++depth;
        if (typename_())
            n->a = declaration();
        else if (!eat(";")) {
            n->a = node(EXPR, expression(), NULL);
            need(";");
        }
        if (!eat(";")) {
            n->b = expression();
            need(";");
        }
        if (!eat(")")) {
            n->c = expression();
            need(")");
        }
        n->d = statement();
        for (int i = 0; i < nsym; ++i)
            if (symbols[i].depth == scope)
                symbols[i].active = 0;
        --depth;
        return n;
    }
    if (eat("return")) {
        Node *n = node(RETURN, NULL, NULL);
        if (!eat(";")) {
            n->a = expression();
            need(";");
        }
        return n;
    }
    if (eat("break")) {
        need(";");
        return node(BREAK, NULL, NULL);
    }
    if (eat("continue")) {
        need(";");
        return node(CONTINUE, NULL, NULL);
    }
    Node *n = node(EXPR, NULL, NULL);
    if (!eat(";")) {
        n->a = expression();
        need(";");
    }
    return n;
}
static uint64_t constant(Node *n)
{
    if (n->kind == NUM)
        return n->value;
    if (n->kind == NEG)
        return 0 - constant(n->a);
    if (n->kind == BITNOT)
        return ~constant(n->a);
    if (n->kind == CAST)
        return constant(n->a);
    die(n->line, "global initializer must be an integer literal (possibly negated)");
    return 0;
}
static void parse(void)
{
    while (tokens[pos].kind) {
        int tls = eat("_Thread_local");
        Type *base = basetype();
        if (eat(";"))
            continue;
        char *name = NULL;
        Type *t = declarator(base, &name, 0);
        if (eat("(")) {
            if (tls || t->fields || t->count)
                die(tokens[pos].line, "invalid function result");
            int fi = function_find(name);
            if (fi < 0) {
                if (nf == MAX_FUNC)
                    die(0, "too many functions");
                fi = nf++;
                namecopy(funcs[fi].name, name);
                funcs[fi].result = t;
            }
            Function *f = &funcs[fi];
            Type *params[6];
            char *names[6] = {0};
            int argc = 0;
            if (is("void") && !strcmp(tokens[pos + 1].text, ")"))
                ++pos;
            while (!eat(")")) {
                if (argc == 6)
                    die(tokens[pos].line, "at most six function parameters");
                params[argc] = decay(declarator(basetype(), &names[argc], 1));
                if (!params[argc]->size || params[argc]->fields)
                    die(tokens[pos].line, "parameters must be scalar");
                ++argc;
                if (eat(")"))
                    break;
                need(",");
            }
            if (f->declared) {
                if (f->argc != argc || !same(f->result, t))
                    die(tokens[pos].line, "prototype mismatch");
                for (int i = 0; i < argc; ++i)
                    if (!same(params[i], f->params[i]))
                        die(tokens[pos].line, "parameter type mismatch");
            }
            f->declared = 1;
            f->argc = argc;
            for (int i = 0; i < argc; ++i)
                f->params[i] = params[i];
            if (eat(";"))
                continue;
            if (f->defined)
                die(tokens[pos].line, "duplicate function");
            f->defined = 1;
            locals = 0;
            depth = 1;
            curfunc = fi;
            for (int i = 0; i < argc; ++i) {
                if (!names[i])
                    die(tokens[pos].line, "parameter name required");
                f->args[i] = addsym(names[i], params[i], 0, 0);
            }
            f->body = statement();
            f->frame = (locals + 15) & -16;
            for (int i = 0; i < nsym; ++i)
                if (!symbols[i].global)
                    symbols[i].active = 0;
            depth = 0;
            curfunc = -1;
        } else {
            Sym *s = addsym(name, t, 1, tls);
            if (eat("=")) {
                if (t->count || t->fields || tls)
                    die(tokens[pos].line, "only non-TLS scalar global initializers supported");
                uint64_t v = constant(assign());
                for (int i = 0; i < t->size; ++i)
                    data[s->offset + i] = (uint8_t)(v >> (8 * i));
            }
            need(";");
        }
    }
    int mainfn = function_find("main");
    if (mainfn < 0 || !funcs[mainfn].defined || funcs[mainfn].argc)
        die(0, "define main() with no parameters");
    for (int i = 0; i < nf; ++i)
        if (!funcs[i].defined)
            die(0, "undefined function '%s'", funcs[i].name);
}
static void b(int v)
{
    if (nc == MAX_CODE)
        die(0, "code size limit");
    code[nc++] = (uint8_t)v;
}
static void bytes(const uint8_t *p, int n)
{
    for (int i = 0; i < n; ++i)
        b(p[i]);
}
#define EMIT(...)                                                                                  \
do {                                                                                           \
    const uint8_t v_[] = {__VA_ARGS__};                                                        \
    bytes(v_, (int)sizeof(v_));                                                                \
} while (0)
static void u32(uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b((int)(v >> (8 * i)));
}
static void u64(uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b((int)(v >> (8 * i)));
}
static void fix(int at, int target)
{
    int64_t d = (int64_t)target - at - 4;
    if (d < INT32_MIN || d > INT32_MAX)
        die(0, "branch range");
    for (int i = 0; i < 4; ++i)
        code[at + i] = (uint8_t)((uint32_t)d >> (8 * i));
}
static int jump(int cc)
{
    if (cc) {
        EMIT(0x0f);
        b(cc);
    } else
        b(0xe9);
    int at = nc;
    u32(0);
    return at;
}
static void imm(uint64_t v)
{
    EMIT(0x48, 0xb8);
    u64(v);
}
static void ref(int region, uint64_t at)
{
    EMIT(0x48, 0xb8);
    if (nr == MAX_RELOC)
        die(0, "relocation limit");
    relocs[nr++] = (Reloc){0, 1, (uint32_t)region, 0, (uint64_t)nc, at, 0};
    u64(0);
}
static void push(void)
{
    b(0x50);
    ++spdepth;
}
static void pop10(void)
{
    EMIT(0x41, 0x5a);
    --spdepth;
}
static void adjust(int n)
{
    if (n) {
        EMIT(0x48, 0x81, 0xc4);
        u32((uint32_t)n);
        spdepth -= n / 8;
    }
}
static void cast(Type *t)
{
    if (t->base || t->size == 8 || t->fields || t->count)
        return;
    if (t->size == 1)
        EMIT(0x0f, 0xb6, 0xc0);
    else if (t->size == 2)
        EMIT(0x0f, 0xb7, 0xc0);
    else if (t->size == 4)
        EMIT(0x89, 0xc0);
}
static void load(Type *t)
{
    if (t->count || t->fields)
        return;
    if (t->size == 1)
        EMIT(0x0f, 0xb6, 0x00);
    else if (t->size == 2)
        EMIT(0x0f, 0xb7, 0x00);
    else if (t->size == 4)
        EMIT(0x8b, 0x00);
    else
        EMIT(0x48, 0x8b, 0x00);
}
static void store(Type *t)
{
    cast(t);
    if (t->size == 1)
        EMIT(0x41, 0x88, 0x02);
    else if (t->size == 2)
        EMIT(0x66, 0x41, 0x89, 0x02);
    else if (t->size == 4)
        EMIT(0x41, 0x89, 0x02);
    else
        EMIT(0x49, 0x89, 0x02);
}
static void gen(Node *n);
static void addr(Node *n)
{
    if (n->kind == VAR) {
        Sym *s = n->sym;
        if (s->tls) {
            EMIT(0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0);
            EMIT(0x48, 0x05);
            u32((uint32_t)s->offset);
        } else if (s->global)
            ref(1, (uint64_t)s->offset);
        else {
            EMIT(0x48, 0x8d, 0x85);
            u32((uint32_t)-s->offset);
        }
    } else if (n->kind == DEREF)
        gen(n->a);
    else if (n->kind == MEMBER) {
        addr(n->a);
        EMIT(0x48, 0x05);
        u32((uint32_t)n->value);
    } else
        die(n->line, "not addressable");
}
static void arithmetic(int op, Type *left, Type *right)
{
    left = decay(left);
    right = decay(right);
    int uns = (left->uns && left->size == 8) || (right->uns && right->size == 8);
    if ((op == ADD || op == SUB) && left->base && !right->base) {
        EMIT(0x48, 0x69, 0xc0);
        u32((uint32_t)left->base->size);
    }
    switch (op) {
        case ADD:
            EMIT(0x4c, 0x01, 0xd0);
            break;
        case SUB:
            EMIT(0x49, 0x29, 0xc2, 0x4c, 0x89, 0xd0);
            if (left->base && right->base) {
                EMIT(0x48, 0x99, 0x49, 0xc7, 0xc3);
                u32((uint32_t)left->base->size);
                EMIT(0x49, 0xf7, 0xfb);
            }
            break;
        case MUL:
            EMIT(0x49, 0x0f, 0xaf, 0xc2);
            break;
        case DIV:
        case MOD:
            EMIT(0x49, 0x89, 0xc3, 0x4c, 0x89, 0xd0);
            if (uns) {
                EMIT(0x31, 0xd2, 0x49, 0xf7, 0xf3);
            } else {
                EMIT(0x48, 0x99, 0x49, 0xf7, 0xfb);
            }
            if (op == MOD)
                EMIT(0x48, 0x89, 0xd0);
        break;
        case SHL:
        case SHR:
            EMIT(0x48, 0x89, 0xc1, 0x4c, 0x89, 0xd0);
            EMIT(0x48, 0xd3);
            b(op == SHL ? 0xe0 : uns ? 0xe8 : 0xf8);
            break;
        case BAND:
            EMIT(0x4c, 0x21, 0xd0);
            break;
        case BOR:
            EMIT(0x4c, 0x09, 0xd0);
            break;
        case BXOR:
            EMIT(0x4c, 0x31, 0xd0);
            break;
        default: {
            int cc = op == LT   ? (uns ? 0x92 : 0x9c)
            : op == LE ? (uns ? 0x96 : 0x9e)
            : op == GT ? (uns ? 0x97 : 0x9f)
            : op == GE ? (uns ? 0x93 : 0x9d)
            : op == EQ ? 0x94
            : 0x95;
            EMIT(0x49, 0x39, 0xc2, 0x0f);
            b(cc);
            EMIT(0xc0, 0x0f, 0xb6, 0xc0);
            break;
        }
    }
}
static void gen_call(Node *n)
{
    int count = (int)n->value, sys = n->func < 0, args = count - (n->func == -2);
    for (Node *a = n->a; a; a = a->next) {
        gen(a);
        push();
    }
    /* Preserve all pending arguments across nested calls. */
    static const uint8_t reg[] = {7, 6, 2, 1, 0, 1};
    for (int i = 0; i < args; ++i) {
        int source = i + (n->func == -2), off = (count - 1 - source) * 8;
        int rex = i >= 4 ? 0x4c : 0x48, r = reg[i];
        if (sys && i == 3) {
            rex = 0x4c;
            r = 2;
        }
        b(rex);
        b(0x8b);
        b(0x84 | (r << 3));
        b(0x24);
        u32((uint32_t)off);
    }
    if (sys) {
        if (n->func == -2) {
            EMIT(0x48, 0x8b, 0x84, 0x24);
            u32((uint32_t)((count - 1) * 8));
        } else
            imm((uint64_t)builtins[-100 - n->func].nr);
        adjust(count * 8);
        EMIT(0x0f, 0x05);
    } else {
        adjust(count * 8);
        int pad = spdepth & 1;
        if (pad) {
            EMIT(0x48, 0x83, 0xec, 8);
            ++spdepth;
        }
        b(0xe8);
        if (ncall == 8192)
            die(0, "too many calls");
        calls[ncall].at = nc;
        calls[ncall++].fn = n->func;
        u32(0);
        if (pad)
            adjust(8);
    }
    cast(n->type);
}
static void gen(Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
        case NUM:
            imm(n->value);
            break;
        case STR:
            ref(2, n->value);
            break;
        case VAR:
        case DEREF:
        case MEMBER:
            addr(n);
            load(n->type);
            break;
        case ADDR:
            addr(n->a);
            break;
        case CAST:
            gen(n->a);
            cast(n->type);
            break;
        case NEG:
            gen(n->a);
            EMIT(0x48, 0xf7, 0xd8);
            break;
        case BITNOT:
            gen(n->a);
            EMIT(0x48, 0xf7, 0xd0);
            break;
        case NOT:
            gen(n->a);
            EMIT(0x48, 0x85, 0xc0, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0);
            break;
        case CALL:
            gen_call(n);
            break;
        case BINARY:
            if (n->op == LAND || n->op == LOR) {
                gen(n->a);
                EMIT(0x48, 0x85, 0xc0);
                int end = jump(n->op == LAND ? 0x84 : 0x85);
                gen(n->b);
                fix(end, nc);
                EMIT(0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0);
            } else {
                gen(n->a);
                push();
                gen(n->b);
                pop10();
                arithmetic(n->op, n->a->type, n->b->type);
            }
            break;
        case ASSIGN:
            addr(n->a);
            push();
            if (n->op) {
                load(n->a->type);
                push();
            }
            gen(n->b);
            if (n->op) {
                pop10();
                arithmetic(n->op, n->a->type, n->b->type);
            }
            pop10();
            store(n->type);
            break;
        case PRE:
        case POST: {
            addr(n->a);
            push();
            load(n->type);
            EMIT(0x49, 0x89, 0xc3);
            int step = n->type->base ? n->type->base->size : 1;
            if (step < 1)
                die(n->line, "incomplete increment");
            EMIT(0x48, 0x05);
            u32((uint32_t)(n->op == ADD ? step : -step));
            pop10();
            store(n->type);
            if (n->kind == POST)
                EMIT(0x4c, 0x89, 0xd8);
            break;
        }
        case COND: {
            gen(n->a);
            EMIT(0x48, 0x85, 0xc0);
            int no = jump(0x84);
            gen(n->b);
            int end = jump(0);
            fix(no, nc);
            gen(n->c);
            fix(end, nc);
            break;
        }
        default:
            die(n->line, "internal expression error");
    }
}
typedef struct Loop {
    struct Loop *parent;
    int breaks[1024], continues[1024], nb, nc;
} Loop;
static int return_fix[8192], nreturn;
static void stmt(Node *n, Loop *loop)
{
    if (!n)
        return;
    switch (n->kind) {
        case BLOCK:
            for (Node *p = n->a; p; p = p->next)
                stmt(p, loop);
        break;
        case EXPR:
            gen(n->a);
            break;
        case RETURN:
            if (n->a)
                gen(n->a);
        else
            imm(0);
        if (nreturn == 8192)
            die(n->line, "too many returns");
        return_fix[nreturn++] = jump(0);
        break;
        case IF: {
            gen(n->a);
            EMIT(0x48, 0x85, 0xc0);
            int no = jump(0x84);
            stmt(n->b, loop);
            int end = jump(0);
            fix(no, nc);
            stmt(n->c, loop);
            fix(end, nc);
            break;
        }
        case WHILE:
        case FOR: {
            Loop l = {0};
            l.parent = loop;
            if (n->kind == FOR)
                stmt(n->a, loop);
            int start = nc, cond = -1;
            Node *test = n->kind == FOR ? n->b : n->a;
            if (test) {
                gen(test);
                EMIT(0x48, 0x85, 0xc0);
                cond = jump(0x84);
            }
            stmt(n->kind == FOR ? n->d : n->b, &l);
            int cont = nc;
            if (n->kind == FOR)
                gen(n->c);
            int j = jump(0);
            fix(j, start);
            if (cond >= 0)
                fix(cond, nc);
            for (int i = 0; i < l.nb; ++i)
                fix(l.breaks[i], nc);
            for (int i = 0; i < l.nc; ++i)
                fix(l.continues[i], cont);
            break;
        }
        case BREAK:
        case CONTINUE:
            if (!loop)
                die(n->line, "break/continue outside loop");
        if (n->kind == BREAK) {
            if (loop->nb == 1024)
                die(n->line, "too many breaks");
            loop->breaks[loop->nb++] = jump(0);
        } else {
            if (loop->nc == 1024)
                die(n->line, "too many continues");
            loop->continues[loop->nc++] = jump(0);
        }
        break;
        default:
            die(n->line, "internal statement error");
    }
    if (spdepth)
        die(n->line, "internal unbalanced expression stack");
}
static void generate(void)
{
    /* RIEF entry uses RSP=8 mod 16; align before calling main. */
    EMIT(0x48, 0x83, 0xec, 8, 0xe8);
    int entrycall = nc;
    u32(0);
    EMIT(0x48, 0x89, 0xc7, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b);
    for (int i = 0; i < nf; ++i) {
        Function *f = &funcs[i];
        f->offset = nc;
        spdepth = 0;
        nreturn = 0;
        EMIT(0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec);
        u32((uint32_t)f->frame);
        static const uint8_t reg[] = {7, 6, 2, 1, 0, 1};
        for (int a = 0; a < f->argc; ++a) { /* Narrow stores through an 8-byte temporary. */
            b(a >= 4 ? 0x4c : 0x48);
            b(0x89);
            b(0xc0 | (reg[a] << 3));
            push();
            addr(var(f->args[a]));
            EMIT(0x49, 0x89, 0xc2);
            b(0x58);
            --spdepth;
            store(f->args[a]->type);
        }
        stmt(f->body, NULL);
        imm(0);
        int epilogue = nc;
        for (int r = 0; r < nreturn; ++r)
            fix(return_fix[r], epilogue);
        EMIT(0xc9, 0xc3);
    }
    fix(entrycall, funcs[function_find("main")].offset);
    for (int i = 0; i < ncall; ++i)
        fix(calls[i].at, funcs[calls[i].fn].offset);
}
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static size_t align8(size_t n) { return (n + 7) & ~(size_t)7; }
static void output(const char *path)
{
    size_t relocoff = 192, exportoff = align8(relocoff + (size_t)nr * 40),
    stringoff = exportoff + (size_t)nf * 16;
    size_t names = 0;
    for (int i = 0; i < nf; ++i)
        names += strlen(funcs[i].name) + 1;
    size_t coff = align8(stringoff + names), doff = align8(coff + (size_t)nc),
    roff = align8(doff + (size_t)nd), total = roff + (size_t)nro;
    if (total > 4 * 1024 * 1024)
        die(0, "RIEF file exceeds the kernel loader limit of 4 MiB");
    uint8_t *out = alloc(total);
    put32(out, 0x46454952);
    out[4] = 1;
    out[8] = 1;
    put32(out + 12, 96);
    put32(out + 16, 3);
    put32(out + 20, (uint32_t)nr);
    put32(out + 28, (uint32_t)nf);
    put64(out + 32, 96);
    if (nr)
        put64(out + 40, relocoff);
    put64(out + 56, exportoff);
    put64(out + 64, stringoff);
    put64(out + 72, names);
    size_t offsets[] = {coff, doff, roff};
    int lengths[] = {nc, nd, nro};
    int flags[] = {5, 3, 1};
    for (int i = 0; i < 3; ++i) {
        uint8_t *r = out + 96 + i * 32;
        put32(r, (uint32_t)flags[i]);
        put32(r + 4, 4096);
        put64(r + 8, lengths[i] ? offsets[i] : 0);
        put64(r + 16, (uint64_t)lengths[i]);
        put64(r + 24, (uint64_t)(lengths[i] ? lengths[i] : 1));
    }
    for (int i = 0; i < nr; ++i) {
        uint8_t *r = out + relocoff + (size_t)i * 40;
        put32(r, relocs[i].patch);
        put32(r + 4, relocs[i].type);
        put32(r + 8, relocs[i].target);
        put64(r + 16, relocs[i].offset);
        put64(r + 24, relocs[i].to);
    }
    size_t nameoff = 0;
    for (int i = 0; i < nf; ++i) {
        uint8_t *r = out + exportoff + (size_t)i * 16;
        put32(r, (uint32_t)nameoff);
        put64(r + 8, (uint64_t)funcs[i].offset);
        size_t len = strlen(funcs[i].name) + 1;
        memcpy(out + stringoff + nameoff, funcs[i].name, len);
        nameoff += len;
    }
    memcpy(out + coff, code, (size_t)nc);
    memcpy(out + doff, data, (size_t)nd);
    memcpy(out + roff, rodata, (size_t)nro);
    FILE *f = fopen(path, "wb");
    if (!f)
        die(0, "cannot open output: %s", strerror(errno));
    int failed = fwrite(out, 1, total, f) != total;
    if (fclose(f))
        failed = 1;
    if (failed) {
        remove(path);
        die(0, "output write failed");
    }
    fprintf(stderr, "%s: %zu bytes, %d functions, %d relocations, %d TLS bytes\n", path, total, nf,
            nr, ntls);
    free(out);
}
static const char *example(const char *name)
{
    if (!strcmp(name, "hello"))
        return "int main() { write(1, \"Hello from compiled RIEF!\\n\", 26); "
        "return 42; }\n";
    if (!strcmp(name, "worker"))
        return "_Thread_local u64 counter;\nint main() { for (int i=0; i<1000; "
        "i++) { counter++; sleep(1); } return counter == 1000 ? 0 : 1; }\n";
    if (!strcmp(name, "helper"))
        return "struct Message { u32 sender; u32 size; u8 bytes[128]; };\n"
        "u64 hash(u8 *p, u64 n) { u64 h=14695981039346656037; for (u64 "
        "i=0;i<n;i++) { h ^= p[i]; h *= 1099511628211; } return h; }\n"
        "int main() { struct Message msg; u64 reply; if "
        "(service_register(\"hash\") < 0) return 1;\n"
        "  while (1) { int e=recv(&msg,sizeof(msg)); if (e == -10) { "
        "sleep(1); continue; }\n"
        "    if (e < 0) return 2; reply=hash(msg.bytes,msg.size); "
        "service_reply(msg.sender,&reply,8); } }\n";
    if (!strcmp(name, "client"))
        return "struct Message { u32 sender; u32 size; u8 bytes[128]; };\n"
        "int main() { struct Message msg; int t=service_lookup(\"hash\"); "
        "if(t<0)return 1;\n"
        " if(send(t,\"hello\",5)<0)return 2; for(int "
        "i=0;i<1000;i++){if(recv(&msg,sizeof(msg))==0){write(1,\"helper "
        "replied\\n\",15);return msg.sender==t && msg.size==8 && "
        "*(u64*)msg.bytes==0xa430d84680aabd0b?0:3;}sleep(1);}return 4;}\n";
    if (!strcmp(name, "root"))
        return "struct Identity { u64 uuid[2]; u32 pid; u32 parent; u32 "
        "execution_class; u32 cpu; u64 tls; u64 reserved[3]; };\n"
        "int main(){struct Identity me;if(identity(&me)<0)return "
        "1;if(me.execution_class!=1)return 2;write(1,\"Ring 1 authority "
        "active\\n\",24);return 0;}\n";
    if (!strcmp(name, "ring1check"))
        return "struct Identity { u64 uuid[2]; u32 pid; u32 parent; u32 execution_class; u32 cpu; "
        "u64 tls; u64 reserved[3]; };\n"
        "struct Request { u32 version; u32 size; u32 operation; u32 flags; u64 caller[2]; "
        "u64 target[2]; u32 pid; u32 resource; u64 address; u64 length; u64 value; u64 "
        "buffer; };\n"
        "int main() {\n"
        "    struct Identity me; struct Request r;\n"
        "    if(identity(&me)<0) return 1;\n"
        "    u8 *bytes=(u8*)&r; for(int i=0;i<sizeof(r);i++)bytes[i]=0;\n"
        "    r.version=1; r.size=sizeof(r); r.operation=3;\n"
        "    "
        "r.caller[0]=me.uuid[0];r.caller[1]=me.uuid[1];r.target[0]=me.uuid[0];r.target[1]="
        "me.uuid[1];r.pid=me.pid;\n"
        "    r.address=0x200000000000;r.length=4096;r.value=3;\n"
        "    if(me.execution_class==3)return admin(&r)==-11?0:2;\n"
        "    r.caller[0]^=1;if(admin(&r)!=-11)return 3;r.caller[0]^=1;\n"
        "    r.address=0;if(admin(&r)!=-1)return 4;\n"
        "    r.address=0x200000000000;r.value=7;if(admin(&r)!=-1)return 5;\n"
        "    r.value=3;if(admin(&r)!=0)return 6;\n"
        "    u64 *weird=(u64*)r.address;*weird=0x1234abcd;\n"
        "    if(*weird!=0x1234abcd)return 7;\n"
        "    if(admin(&r)!=-15)return 8;\n"
        "    return 0;\n"
        "}\n";
    if (!strcmp(name, "tlscheck"))
        return "_Thread_local u64 counter;\n"
        "int main() {\n"
        "    u64 *tcb=(u64*)tlsbase();\n"
        "    if(tcb[0]!=(u64)tcb || tcb[1]!=getpid() || counter!=0)return 1;\n"
        "    u64 original=getcpu();\n"
        "    for(int i=0;i<100;i++){ counter++; sleep(10); "
        "if(counter!=i+1||tcb[1]!=getpid()||getcpu()!=original)return 2; }\n"
        "    return 0;\n"
        "}\n";
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--example")) {
        const char *s = example(argv[2]);
        if (!s)
            die(0, "unknown example");
        fputs(s, stdout);
        return ferror(stdout) ? 1 : 0;
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: rief input.c -o output.rief\n       rief --example "
        "hello|worker|helper|client|root|ring1check|tlscheck\n");
        return 1;
    }
    const char *out = "a.rief";
    input_name = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else
            die(0, "unknown option");
    }
    if (!strcmp(input_name, out))
        die(0, "input and output must differ");
    FILE *f = fopen(input_name, "rb");
    if (!f)
        die(0, "cannot read input: %s", strerror(errno));
    char *source = alloc(MAX_DATA + 1);
    size_t n = fread(source, 1, MAX_DATA + 1, f);
    int failed = ferror(f);
    fclose(f);
    if (failed || n > MAX_DATA)
        die(0, "source read error or source over 4 MiB");
    for (size_t i = 0; i < n; ++i)
        if (!source[i])
            die(0, "NUL byte in source");
    source[n] = 0;
    tvoid = type_new(0, 1, 0);
    tu8 = type_new(1, 1, 1);
    tu16 = type_new(2, 2, 1);
    tu32 = type_new(4, 4, 1);
    ti64 = type_new(8, 8, 0);
    tu64 = type_new(8, 8, 1);
    lex(source);
    parse();
    generate();
    output(out);
    return 0;
}
