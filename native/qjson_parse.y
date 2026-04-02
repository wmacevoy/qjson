/* ============================================================
 * qjson_parse.y — QJSON grammar for Lemon parser generator
 *
 * lemon qjson_parse.y → qjson_parse.c + qjson_parse.h
 *
 * Semantic actions push arena-allocated qjson_val nodes onto
 * a value stack in the parse context.  NULL sentinels mark
 * container boundaries.
 * ============================================================ */

%include {
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "qjson.h"
#include "qjson_parse_ctx.h"
}

%token_type { qjson_token }
%extra_argument { qjson_parse_ctx *ctx }

%syntax_error {
    ctx->error = 1;
}

%parse_failure {
    ctx->error = 1;
}

/* ── Precedence (low to high) ──────────────────────────────── */

%left TK_OR.
%left TK_AND.
%right TK_NOT.
%nonassoc TK_EQ TK_IN.
%left TK_PLUS TK_MINUS.
%left TK_STAR TK_SLASH.
%right TK_CARET.

/* ── Top-level ────────────────────────────────────────────── */

input ::= value.
input ::= view_def.

/* ── Atom values ──────────────────────────────────────────── */

value ::= TK_NULL. {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_NULL);
    qjson_ctx_push(ctx, v);
}

value ::= TK_TRUE. {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_TRUE);
    qjson_ctx_push(ctx, v);
}

value ::= TK_FALSE. {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_FALSE);
    qjson_ctx_push(ctx, v);
}

value ::= TK_NUMBER(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_NUMBER);
    if (v) v->num = T.num;
    qjson_ctx_push(ctx, v);
}

value ::= TK_NUMBER_N(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_BIGINT);
    if (v) { v->str.s = T.start; v->str.len = T.len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_NUMBER_M(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_BIGDECIMAL);
    if (v) { v->str.s = T.start; v->str.len = T.len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_NUMBER_L(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_BIGFLOAT);
    if (v) { v->str.s = T.start; v->str.len = T.len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_STRING(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_STRING);
    if (v) { v->str.s = T.decoded; v->str.len = T.decoded_len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_BLOB(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_BLOB);
    if (v) { v->blob.data = T.decoded; v->blob.len = T.decoded_len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_UNBOUND(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_UNBOUND);
    if (v) { v->str.s = T.decoded; v->str.len = T.decoded_len; }
    qjson_ctx_push(ctx, v);
}

value ::= TK_IDENT(T). {
    qjson_val *v = qjson_ctx_alloc_val(ctx, QJSON_STRING);
    if (v) { v->str.s = T.decoded; v->str.len = T.decoded_len; }
    qjson_ctx_push(ctx, v);
}

/* ── Array ────────────────────────────────────────────────── */

value ::= array.

array ::= arr_open TK_RBRACKET. {
    qjson_ctx_push_array(ctx);
}

array ::= arr_open elements opt_comma TK_RBRACKET. {
    qjson_ctx_push_array(ctx);
}

arr_open ::= TK_LBRACKET. {
    qjson_ctx_push_mark(ctx);
}

elements ::= value.
elements ::= elements TK_COMMA value.

/* ── Object ───────────────────────────────────────────────── */

value ::= object.

object ::= obj_open TK_RBRACE. {
    qjson_ctx_push_object(ctx);
}

object ::= obj_open entries opt_comma TK_RBRACE. {
    qjson_ctx_push_object(ctx);
}

obj_open ::= TK_LBRACE. {
    qjson_ctx_push_mark(ctx);
}

entries ::= entry.
entries ::= entries TK_COMMA entry.

/* key:value pair */
entry ::= value TK_COLON value.

/* set shorthand: value alone → {key: true} */
entry ::= value. {
    qjson_val *t = qjson_ctx_alloc_val(ctx, QJSON_TRUE);
    qjson_ctx_push(ctx, t);
}

/* key: view_def (e.g.  grandparents: {..} WHERE ..) */
entry ::= value TK_COLON view_def.

/* ── Trailing comma ───────────────────────────────────────── */

opt_comma ::= .
opt_comma ::= TK_COMMA.

/* ── View definition: pattern WHERE condition ─────────────── */

view_def ::= value TK_WHERE condition. {
    qjson_ctx_push_view(ctx);
}

/* ── Boolean condition ────────────────────────────────────── */

condition ::= match_clause.

condition ::= condition TK_AND condition. {
    qjson_ctx_push_binop(ctx, "and", 3);
}

condition ::= condition TK_OR condition. {
    qjson_ctx_push_binop(ctx, "or", 2);
}

condition ::= TK_NOT condition. {
    qjson_ctx_push_notop(ctx);
}

condition ::= TK_LPAREN condition TK_RPAREN.

/* ── Match clause: pattern IN source ──────────────────────── */

match_clause ::= value TK_IN value. {
    qjson_ctx_push_match(ctx);
}

/* ── Equation condition: expr = expr ──────────────────────── */

condition ::= expr TK_EQ expr. {
    qjson_ctx_push_equation(ctx);
}

/* ── Arithmetic expressions ───────────────────────────────── */

expr ::= value.
expr ::= expr TK_PLUS expr.  { qjson_ctx_push_arith(ctx, "+", 1); }
expr ::= expr TK_MINUS expr. { qjson_ctx_push_arith(ctx, "-", 1); }
expr ::= expr TK_STAR expr.  { qjson_ctx_push_arith(ctx, "*", 1); }
expr ::= expr TK_SLASH expr. { qjson_ctx_push_arith(ctx, "/", 1); }
expr ::= expr TK_CARET expr. { qjson_ctx_push_arith(ctx, "^", 1); }
expr ::= TK_MINUS expr. [TK_CARET] { /* unary minus = 0 - expr */
    qjson_val *zero = qjson_ctx_alloc_val(ctx, QJSON_NUMBER);
    if (zero) zero->num = 0;
    qjson_ctx_push(ctx, zero);  /* push zero under the expr */
    /* stack: [..., expr, zero] — need to swap */
    if (ctx->top >= 2) {
        qjson_val *e = ctx->stack[ctx->top - 2];
        ctx->stack[ctx->top - 2] = ctx->stack[ctx->top - 1];
        ctx->stack[ctx->top - 1] = e;
    }
    qjson_ctx_push_arith(ctx, "-", 1);
}
expr ::= TK_LPAREN expr TK_RPAREN.
