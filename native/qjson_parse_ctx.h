/* ============================================================
 * qjson_parse_ctx.h — Parse context for Lemon semantic actions
 *
 * Value stack + arena allocation.  Used by the generated parser.
 * A NULL sentinel on the stack marks the start of each container.
 * ============================================================ */

#ifndef QJSON_PARSE_CTX_H
#define QJSON_PARSE_CTX_H

#include <string.h>
#include "qjson.h"

/* Token value passed from lexer to parser.
   Must match the lexer's qjson_token layout. */
typedef struct {
    int         type;
    const char *start;
    int         len;
    double      num;
    const char *decoded;
    int         decoded_len;
} qjson_token;

#define QJSON_PARSE_STACK_MAX 1024

typedef struct {
    qjson_arena *arena;
    qjson_val   *stack[QJSON_PARSE_STACK_MAX];
    int          top;        /* index of next free slot */
    int          error;
} qjson_parse_ctx;

static inline void qjson_ctx_init(qjson_parse_ctx *ctx, qjson_arena *arena) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = arena;
}

static inline qjson_val *qjson_ctx_alloc_val(qjson_parse_ctx *ctx, qjson_type t) {
    qjson_val *v = (qjson_val *)qjson_arena_alloc(ctx->arena, sizeof(qjson_val));
    if (v) { memset(v, 0, sizeof(*v)); v->type = t; }
    else ctx->error = 1;
    return v;
}

static inline void qjson_ctx_push(qjson_parse_ctx *ctx, qjson_val *v) {
    if (ctx->top < QJSON_PARSE_STACK_MAX)
        ctx->stack[ctx->top++] = v;
    else
        ctx->error = 1;
}

static inline qjson_val *qjson_ctx_pop(qjson_parse_ctx *ctx) {
    if (ctx->top > 0)
        return ctx->stack[--ctx->top];
    ctx->error = 1;
    return NULL;
}

/* Push NULL sentinel to mark start of a container's contents */
static inline void qjson_ctx_push_mark(qjson_parse_ctx *ctx) {
    qjson_ctx_push(ctx, NULL);
}

/* Count items from top of stack back to the NULL sentinel */
static inline int qjson_ctx_count_to_mark(qjson_parse_ctx *ctx) {
    int n = 0;
    for (int i = ctx->top - 1; i >= 0; i--) {
        if (ctx->stack[i] == NULL) return n;
        n++;
    }
    ctx->error = 1;
    return 0;
}

static inline void qjson_ctx_push_array(qjson_parse_ctx *ctx) {
    /* Stack: ... MARK item0 item1 ... itemN */
    int n = qjson_ctx_count_to_mark(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_ARRAY);
    if (!v) return;
    v->arr.count = n;
    v->arr.items = (qjson_val **)qjson_arena_alloc(ctx->arena, n * sizeof(qjson_val *));
    if (n > 0 && !v->arr.items) { ctx->error = 1; return; }
    int base = ctx->top - n;
    for (int i = 0; i < n; i++)
        v->arr.items[i] = ctx->stack[base + i];
    ctx->top = base - 1; /* also pop the MARK */
    qjson_ctx_push(ctx, v);
}

static inline void qjson_ctx_push_object(qjson_parse_ctx *ctx) {
    /* Stack: ... MARK key0 val0 key1 val1 ... keyN valN */
    int items = qjson_ctx_count_to_mark(ctx);
    int n = items / 2;
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_OBJECT);
    if (!v) return;
    v->obj.count = n;
    v->obj.pairs = (qjson_kv *)qjson_arena_alloc(ctx->arena, n * sizeof(qjson_kv));
    if (n > 0 && !v->obj.pairs) { ctx->error = 1; return; }
    int base = ctx->top - items;
    for (int i = 0; i < n; i++) {
        v->obj.pairs[i].key = ctx->stack[base + 2*i];
        v->obj.pairs[i].val = ctx->stack[base + 2*i + 1];
    }
    ctx->top = base - 1; /* also pop the MARK */
    qjson_ctx_push(ctx, v);
}

/* ── View / Datalog semantic actions ──────────────────────── */

/* view_def: pattern WHERE condition → VIEW node
   stack: [..., pattern, condition] → [..., view] */
static inline void qjson_ctx_push_view(qjson_parse_ctx *ctx) {
    qjson_val *cond = qjson_ctx_pop(ctx);
    qjson_val *pattern = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_VIEW);
    if (v) { v->view.pattern = pattern; v->view.cond = cond; }
    qjson_ctx_push(ctx, v);
}

/* match_clause: pattern IN source → MATCH node
   stack: [..., pattern, source] → [..., match] */
static inline void qjson_ctx_push_match(qjson_parse_ctx *ctx) {
    qjson_val *source = qjson_ctx_pop(ctx);
    qjson_val *pattern = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_MATCH);
    if (v) { v->match.pattern = pattern; v->match.source = source; }
    qjson_ctx_push(ctx, v);
}

/* binop: condition AND/OR condition → BINOP node
   stack: [..., left, right] → [..., binop] */
static inline void qjson_ctx_push_binop(qjson_parse_ctx *ctx, const char *op, int op_len) {
    qjson_val *right = qjson_ctx_pop(ctx);
    qjson_val *left = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_BINOP);
    if (v) { v->binop.op = op; v->binop.op_len = op_len; v->binop.left = left; v->binop.right = right; }
    qjson_ctx_push(ctx, v);
}

/* notop: NOT condition → NOTOP node
   stack: [..., operand] → [..., notop] */
static inline void qjson_ctx_push_notop(qjson_parse_ctx *ctx) {
    qjson_val *operand = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_NOTOP);
    if (v) { v->notop.operand = operand; }
    qjson_ctx_push(ctx, v);
}

/* equation: expr = expr → EQUATION node */
static inline void qjson_ctx_push_equation(qjson_parse_ctx *ctx) {
    qjson_val *right = qjson_ctx_pop(ctx);
    qjson_val *left = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_EQUATION);
    if (v) { v->equation.left = left; v->equation.right = right; }
    qjson_ctx_push(ctx, v);
}

/* arith: expr op expr → ARITH node (reuses binop struct) */
static inline void qjson_ctx_push_arith(qjson_parse_ctx *ctx, const char *op, int op_len) {
    qjson_val *right = qjson_ctx_pop(ctx);
    qjson_val *left = qjson_ctx_pop(ctx);
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_ARITH);
    if (v) { v->binop.op = op; v->binop.op_len = op_len; v->binop.left = left; v->binop.right = right; }
    qjson_ctx_push(ctx, v);
}

#endif
