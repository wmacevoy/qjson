/* libbf_shim.c — bridge libbf's dbuf_putc to QuickJS's __dbuf_putc.
 *
 * libbf.c compiled against QuickJS cutils.h gets the inline dbuf_putc,
 * but libbf.c compiled against its OWN cutils.h emits calls to dbuf_putc.
 * This shim is compiled against libbf's cutils.h only.
 */

#include <stddef.h>
#include <stdint.h>
#include "libbf/cutils.h"

/* Provide the dbuf_putc symbol that libbf expects */
int dbuf_putc(DynBuf *s, uint8_t c) {
    return dbuf_put(s, &c, 1);
}
