/* ============================================================
 * qjson_lex.c — QJSON lexer: byte stream → tokens
 *
 * Extracted from qjson.c recursive descent parser.
 * All character-level complexity lives here.
 * ============================================================ */

#include <string.h>
#include <stdlib.h>
#include "qjson_lex.h"

/* ── Helpers ────────────────────────────────────────────────── */

static char peek(qjson_lexer *lex) {
    return lex->pos < lex->end ? lex->src[lex->pos] : 0;
}

static void skip_ws(qjson_lexer *lex) {
    while (lex->pos < lex->end) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lex->pos++;
            continue;
        }
        if (c == '/' && lex->pos + 1 < lex->end) {
            if (lex->src[lex->pos + 1] == '/') {
                lex->pos += 2;
                while (lex->pos < lex->end && lex->src[lex->pos] != '\n')
                    lex->pos++;
                continue;
            }
            if (lex->src[lex->pos + 1] == '*') {
                lex->pos += 2;
                int depth = 1;
                while (lex->pos + 1 < lex->end && depth > 0) {
                    if (lex->src[lex->pos] == '/' && lex->src[lex->pos+1] == '*') {
                        depth++; lex->pos += 2;
                    } else if (lex->src[lex->pos] == '*' && lex->src[lex->pos+1] == '/') {
                        depth--; lex->pos += 2;
                    } else {
                        lex->pos++;
                    }
                }
                continue;
            }
        }
        break;
    }
}

static char *arena_strdup(qjson_arena *a, const char *s, int len) {
    char *p = qjson_arena_alloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$';
}

static int is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_kw(qjson_lexer *lex, int pos, const char *word, int wlen) {
    if (pos + wlen > lex->end) return 0;
    if (memcmp(lex->src + pos, word, wlen) != 0) return 0;
    if (pos + wlen < lex->end) {
        char nc = lex->src[pos + wlen];
        if (is_ident_cont(nc)) return 0;
    }
    return 1;
}

/* ── JS64 helpers (for blob lexing) ─────────────────────────── */

static int is_js64_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '$' || c == '_';
}

/* Forward declare from qjson.c / qjson.h */
extern int qjson_js64_decode(const char *js64, int js64_len, char *out, int out_cap);

/* ── Token scanners ─────────────────────────────────────────── */

static int lex_string(qjson_lexer *lex, qjson_token *tok) {
    lex->pos++; /* skip opening " */
    int start = lex->pos, escaped_len = 0;
    while (lex->pos < lex->end && lex->src[lex->pos] != '"') {
        if (lex->src[lex->pos] == '\\') lex->pos++;
        lex->pos++;
        escaped_len++;
    }
    if (lex->pos >= lex->end) return -1;
    int raw_end = lex->pos;
    lex->pos++; /* skip closing " */

    /* Unescape into arena */
    char *buf = qjson_arena_alloc(lex->arena, escaped_len + 1);
    if (!buf) return -1;
    int out = 0, i = start;
    while (i < raw_end) {
        if (lex->src[i] == '\\') {
            i++;
            switch (lex->src[i]) {
                case '"':  buf[out++] = '"'; break;
                case '\\': buf[out++] = '\\'; break;
                case '/':  buf[out++] = '/'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'u':  buf[out++] = '?'; i += 4; break;
                default:   buf[out++] = lex->src[i]; break;
            }
        } else {
            buf[out++] = lex->src[i];
        }
        i++;
    }
    buf[out] = '\0';

    tok->type = TK_STRING;
    tok->decoded = buf;
    tok->decoded_len = out;
    tok->start = buf;
    tok->len = out;
    return TK_STRING;
}

static int lex_blob(qjson_lexer *lex, qjson_token *tok) {
    /* pos is right after '0j' or '0J' */
    int start = lex->pos;
    int char_count = 0;
    while (lex->pos < lex->end) {
        char c = lex->src[lex->pos];
        if (is_js64_char(c)) { char_count++; lex->pos++; }
        else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { lex->pos++; }
        else break;
    }

    int blob_len = (char_count * 3) >> 2;
    char *data = qjson_arena_alloc(lex->arena, blob_len > 0 ? blob_len : 1);
    if (!data) return -1;

    int js64_span = lex->pos - start;
    int decoded = qjson_js64_decode(lex->src + start, js64_span, data, blob_len);
    if (decoded < 0) return -1;

    tok->type = TK_BLOB;
    tok->decoded = data;
    tok->decoded_len = decoded;
    return TK_BLOB;
}

static int lex_number(qjson_lexer *lex, qjson_token *tok) {
    int start = lex->pos;
    if (peek(lex) == '-') lex->pos++;

    /* Check for 0j/0J blob prefix */
    if (lex->pos < lex->end && lex->src[lex->pos] == '0' &&
        lex->pos + 1 < lex->end &&
        (lex->src[lex->pos + 1] == 'j' || lex->src[lex->pos + 1] == 'J')) {
        lex->pos += 2;
        return lex_blob(lex, tok);
    }

    while (lex->pos < lex->end && lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9')
        lex->pos++;
    if (lex->pos < lex->end && lex->src[lex->pos] == '.') {
        lex->pos++;
        while (lex->pos < lex->end && lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9')
            lex->pos++;
    }
    if (lex->pos < lex->end && (lex->src[lex->pos] == 'e' || lex->src[lex->pos] == 'E')) {
        lex->pos++;
        if (lex->pos < lex->end && (lex->src[lex->pos] == '+' || lex->src[lex->pos] == '-'))
            lex->pos++;
        while (lex->pos < lex->end && lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9')
            lex->pos++;
    }

    int raw_len = lex->pos - start;
    char *raw = arena_strdup(lex->arena, lex->src + start, raw_len);
    if (!raw) return -1;

    /* Check suffix */
    char suffix = (lex->pos < lex->end) ? lex->src[lex->pos] : 0;
    if (suffix == 'N' || suffix == 'n') {
        lex->pos++;
        tok->type = TK_NUMBER_N;
        tok->start = raw;
        tok->len = raw_len;
        return TK_NUMBER_N;
    }
    if (suffix == 'M' || suffix == 'm') {
        lex->pos++;
        tok->type = TK_NUMBER_M;
        tok->start = raw;
        tok->len = raw_len;
        return TK_NUMBER_M;
    }
    if (suffix == 'L' || suffix == 'l') {
        lex->pos++;
        tok->type = TK_NUMBER_L;
        tok->start = raw;
        tok->len = raw_len;
        return TK_NUMBER_L;
    }

    tok->type = TK_NUMBER;
    tok->start = raw;
    tok->len = raw_len;
    tok->num = atof(raw);
    return TK_NUMBER;
}

static int lex_unbound(qjson_lexer *lex, qjson_token *tok) {
    lex->pos++; /* skip '?' */

    if (lex->pos < lex->end && lex->src[lex->pos] == '"') {
        /* Quoted name: ?"Bob's Last Memo" */
        int r = lex_string(lex, tok);
        if (r < 0) return -1;
        tok->type = TK_UNBOUND;
        /* decoded/decoded_len already set by lex_string */
        return TK_UNBOUND;
    }

    /* Bare identifier or anonymous */
    int start = lex->pos;
    while (lex->pos < lex->end) {
        char c = lex->src[lex->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') lex->pos++;
        else break;
    }

    const char *name;
    int name_len;
    if (lex->pos == start) {
        name = arena_strdup(lex->arena, "", 0);
        name_len = 0;
    } else {
        name_len = lex->pos - start;
        name = arena_strdup(lex->arena, lex->src + start, name_len);
    }
    if (!name) return -1;

    tok->type = TK_UNBOUND;
    tok->decoded = name;
    tok->decoded_len = name_len;
    tok->start = name;
    tok->len = name_len;
    return TK_UNBOUND;
}

/* ── Public API ─────────────────────────────────────────────── */

void qjson_lex_init(qjson_lexer *lex, const char *src, int len, qjson_arena *arena) {
    lex->src = src;
    lex->pos = 0;
    lex->end = len;
    lex->arena = arena;
}

int qjson_lex(qjson_lexer *lex, qjson_token *tok) {
    memset(tok, 0, sizeof(*tok));
    skip_ws(lex);

    if (lex->pos >= lex->end) {
        tok->type = TK_EOF;
        return TK_EOF;
    }

    char c = lex->src[lex->pos];

    /* Structural tokens */
    switch (c) {
    case '[': lex->pos++; tok->type = TK_LBRACKET; return TK_LBRACKET;
    case ']': lex->pos++; tok->type = TK_RBRACKET; return TK_RBRACKET;
    case '{': lex->pos++; tok->type = TK_LBRACE;   return TK_LBRACE;
    case '}': lex->pos++; tok->type = TK_RBRACE;   return TK_RBRACE;
    case ',': lex->pos++; tok->type = TK_COMMA;    return TK_COMMA;
    case ':': lex->pos++; tok->type = TK_COLON;    return TK_COLON;
    case '(': lex->pos++; tok->type = TK_LPAREN;   return TK_LPAREN;
    case ')': lex->pos++; tok->type = TK_RPAREN;   return TK_RPAREN;
    default: break;
    }

    /* String */
    if (c == '"') return lex_string(lex, tok);

    /* Arithmetic operators */
    if (c == '+') { lex->pos++; tok->type = TK_PLUS;  return TK_PLUS; }
    if (c == '*') { lex->pos++; tok->type = TK_STAR;  return TK_STAR; }
    if (c == '/') {
        /* Not a comment start? Then it's division. */
        if (lex->pos + 1 < lex->end &&
            (lex->src[lex->pos+1] == '/' || lex->src[lex->pos+1] == '*')) {
            /* Comments are handled in skip_ws, shouldn't get here */
        } else {
            lex->pos++; tok->type = TK_SLASH; return TK_SLASH;
        }
    }
    if (c == '^') { lex->pos++; tok->type = TK_CARET; return TK_CARET; }
    if (c == '=') { lex->pos++; tok->type = TK_EQ;    return TK_EQ; }

    /* Number or blob: - is number start only if followed by digit */
    if (c >= '0' && c <= '9') return lex_number(lex, tok);
    if (c == '-') {
        if (lex->pos + 1 < lex->end && lex->src[lex->pos + 1] >= '0' && lex->src[lex->pos + 1] <= '9')
            return lex_number(lex, tok);
        lex->pos++; tok->type = TK_MINUS; return TK_MINUS;
    }

    /* Unbound */
    if (c == '?') return lex_unbound(lex, tok);

    /* Keywords and bare identifiers */
    if (is_ident_start(c)) {
        if (is_kw(lex, lex->pos, "true", 4)) {
            lex->pos += 4;
            tok->type = TK_TRUE;
            return TK_TRUE;
        }
        if (is_kw(lex, lex->pos, "false", 5)) {
            lex->pos += 5;
            tok->type = TK_FALSE;
            return TK_FALSE;
        }
        if (is_kw(lex, lex->pos, "null", 4)) {
            lex->pos += 4;
            tok->type = TK_NULL;
            return TK_NULL;
        }

        /* View keywords */
        if (is_kw(lex, lex->pos, "where", 5) || is_kw(lex, lex->pos, "WHERE", 5)) {
            lex->pos += 5; tok->type = TK_WHERE; return TK_WHERE;
        }
        if (is_kw(lex, lex->pos, "and", 3) || is_kw(lex, lex->pos, "AND", 3)) {
            lex->pos += 3; tok->type = TK_AND; return TK_AND;
        }
        if (is_kw(lex, lex->pos, "or", 2) || is_kw(lex, lex->pos, "OR", 2)) {
            lex->pos += 2; tok->type = TK_OR; return TK_OR;
        }
        if (is_kw(lex, lex->pos, "not", 3) || is_kw(lex, lex->pos, "NOT", 3)) {
            lex->pos += 3; tok->type = TK_NOT; return TK_NOT;
        }
        if (is_kw(lex, lex->pos, "in", 2) || is_kw(lex, lex->pos, "IN", 2)) {
            lex->pos += 2; tok->type = TK_IN; return TK_IN;
        }

        /* Bare identifier */
        int start = lex->pos;
        lex->pos++;
        while (lex->pos < lex->end && is_ident_cont(lex->src[lex->pos]))
            lex->pos++;
        int ilen = lex->pos - start;
        char *id = arena_strdup(lex->arena, lex->src + start, ilen);
        if (!id) return -1;

        tok->type = TK_IDENT;
        tok->start = id;
        tok->len = ilen;
        tok->decoded = id;
        tok->decoded_len = ilen;
        return TK_IDENT;
    }

    return -1; /* unexpected character */
}
