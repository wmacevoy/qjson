/* ============================================================
 * qjson_lex.h — QJSON lexer: byte stream → tokens
 *
 * Hand-written tokenizer for QJSON.  Handles whitespace/comments,
 * numbers (with N/M/L suffixes), strings, blobs (0j), unbound
 * variables (?name), bare identifiers, and structural tokens.
 *
 * Zero-copy where possible — token values point into source buffer.
 * Decoded strings/blobs are arena-allocated.
 *
 * Token type IDs (TK_NULL, TK_NUMBER, etc.) come from the
 * Lemon-generated qjson_parse.h.
 * ============================================================ */

#ifndef QJSON_LEX_H
#define QJSON_LEX_H

#include "qjson.h"
#include "qjson_parse.h"       /* Lemon-generated token IDs */
#include "qjson_parse_ctx.h"   /* qjson_token typedef */

#define TK_EOF 0

/* Lexer state */
typedef struct {
    const char  *src;
    int          pos;
    int          end;
    qjson_arena *arena;
} qjson_lexer;

/* Initialize lexer over source buffer */
void qjson_lex_init(qjson_lexer *lex, const char *src, int len, qjson_arena *arena);

/* Get next token.  Returns token type (also stored in tok->type). */
int qjson_lex(qjson_lexer *lex, qjson_token *tok);

#endif
