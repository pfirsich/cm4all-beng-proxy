/*
 * Delegate helper pooling.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_DELEGATE_STOCK_H
#define BENG_DELEGATE_STOCK_H

#include "stock.h"

struct pool;
struct child_options;

#ifdef __cplusplus
extern "C" {
#endif

struct hstock *
delegate_stock_new(struct pool *pool);

void
delegate_stock_get(struct hstock *delegate_stock, struct pool *pool,
                   const char *path,
                   const struct child_options *options,
                   const struct stock_get_handler *handler, void *handler_ctx,
                   struct async_operation_ref *async_ref);

void
delegate_stock_put(struct hstock *delegate_stock,
                   struct stock_item *item, bool destroy);

int
delegate_stock_item_get(struct stock_item *item);

#ifdef __cplusplus
}
#endif

#endif
