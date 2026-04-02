/* ============================================================
 * qjson_types.h — QJSON type IDs (bitmask layout)
 *
 * Shared between qjson (C) and libbfxx (C++).
 * Group bits encode type categories; numeric ordering
 * matches type widening (max of two type IDs = widened type).
 *
 *   (type & QJSON_BOOLEAN)   — FALSE or TRUE
 *   (type & QJSON_NUMERIC)   — NUMBER, BIGINT, BIGFLOAT, BIGDECIMAL
 *   (type & QJSON_CONTAINER) — ARRAY or OBJECT
 *   (type & 0x01) on BOOLEAN — truth value
 *   UNBOUND & any_type == any_type (matches everything)
 * ============================================================ */

#ifndef QJSON_TYPES_H
#define QJSON_TYPES_H

#include <stdint.h>

typedef enum {
    QJSON_NULL       = 0x000,  /* 0b0'0000'0000 */
    QJSON_FALSE      = 0x010,  /* 0b0'0001'0000 */
    QJSON_TRUE       = 0x011,  /* 0b0'0001'0001 */
    QJSON_NUMBER     = 0x021,  /* 0b0'0010'0001  IEEE double */
    QJSON_BIGINT     = 0x022,  /* 0b0'0010'0010  arbitrary-precision integer */
    QJSON_BIGFLOAT   = 0x024,  /* 0b0'0010'0100  arbitrary-precision base-2 */
    QJSON_BIGDECIMAL = 0x028,  /* 0b0'0010'1000  arbitrary-precision base-10 */
    QJSON_BLOB       = 0x040,  /* 0b0'0100'0000 */
    QJSON_STRING     = 0x081,  /* 0b0'1000'0001 */
    QJSON_ARRAY      = 0x101,  /* 0b1'0000'0001 */
    QJSON_OBJECT     = 0x102,  /* 0b1'0000'0010 */
    QJSON_VIEW       = 0x204,  /* 0b10'0000'0100  pattern WHERE condition */
    QJSON_MATCH      = 0x208,  /* 0b10'0000'1000  pattern IN source */
    QJSON_BINOP      = 0x210,  /* 0b10'0001'0000  AND / OR */
    QJSON_NOTOP      = 0x220,  /* 0b10'0010'0000  NOT condition */
    QJSON_EQUATION   = 0x240,  /* 0b10'0100'0000  expr = expr */
    QJSON_ARITH      = 0x280,  /* 0b10'1000'0000  expr op expr (+,-,*,/,^) */
    QJSON_UNBOUND    = 0x3FF,  /* 0b11'1111'1111 */

    /* Group masks */
    QJSON_BOOLEAN    = 0x010,
    QJSON_NUMERIC    = 0x020,
    QJSON_CONTAINER  = 0x100,
    QJSON_LOGIC      = 0x200   /* VIEW, MATCH, BINOP, NOTOP */
} qjson_type;

#endif
