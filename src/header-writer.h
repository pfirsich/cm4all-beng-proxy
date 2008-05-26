/*
 * Write HTTP headers into a fifo_buffer_t.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_HEADER_WRITER_H
#define __BENG_HEADER_WRITER_H

#include "growing-buffer.h"

struct strmap;

void
header_write(growing_buffer_t gb, const char *key, const char *value);

void
headers_copy(struct strmap *in, growing_buffer_t out, const char *const* keys);

void
headers_copy_all(struct strmap *in, growing_buffer_t out);

growing_buffer_t
headers_dup(pool_t pool, struct strmap *in);

#endif
