/*
 * Objects in stock.  May be used for connection pooling.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "stock.h"
#include "async.h"

#include <assert.h>

struct stock {
    pool_t pool;
    const struct stock_class *class;
    void *class_ctx;
    const char *uri;

    unsigned num_idle;
    struct list_head idle;

    unsigned num_busy;
    struct list_head busy;
};


/*
 * constructor
 *
 */

struct stock *
stock_new(pool_t pool, const struct stock_class *class,
          void *class_ctx, const char *uri)
{
    struct stock *stock;

    assert(pool != NULL);
    assert(class != NULL);
    assert(class->item_size > sizeof(struct stock_item));
    assert(class->create != NULL);
    assert(class->borrow != NULL);
    assert(class->release != NULL);
    assert(class->destroy != NULL);

    pool = pool_new_linear(pool, "stock", 1024);
    stock = p_malloc(pool, sizeof(*stock));
    stock->pool = pool;
    stock->class = class;
    stock->class_ctx = class_ctx;
    stock->uri = uri == NULL ? NULL : p_strdup(pool, uri);
    stock->num_idle = 0;
    list_init(&stock->idle);
    stock->num_busy = 0;
    list_init(&stock->busy);

    return stock;
}


static void
stock_item_free(struct stock *stock, struct stock_item *item)
{
    assert(pool_contains(item->pool, item, stock->class->item_size));

    if (item->pool == stock->pool)
        p_free(stock->pool, item);
    else {
        pool_trash(item->pool);
        pool_unref(item->pool);
    }
}

static void
destroy_item(struct stock *stock, struct stock_item *item)
{
    assert(pool_contains(item->pool, item, stock->class->item_size));

    stock->class->destroy(stock->class_ctx, item);
    stock_item_free(stock, item);
}

void
stock_free(struct stock **stock_r)
{
    struct stock *stock;

    assert(stock_r != NULL);
    assert(*stock_r != NULL);

    stock = *stock_r;
    *stock_r = NULL;

    while (stock->num_idle > 0) {
        struct stock_item *item = (struct stock_item *)stock->idle.next;

        assert(!list_empty(&stock->idle));

        list_remove(&item->list_head);
        --stock->num_idle;

        destroy_item(stock, item);
    }

    assert(list_empty(&stock->idle));

    while (stock->num_busy > 0) {
        struct stock_item *item = (struct stock_item *)stock->busy.next;

        assert(!list_empty(&stock->busy));

        list_remove(&item->list_head);
        --stock->num_busy;

        destroy_item(stock, item);
    }

    assert(list_empty(&stock->busy));

    pool_unref(stock->pool);
}

void
stock_get(struct stock *stock, void *info,
          stock_callback_t callback, void *callback_ctx,
          struct async_operation_ref *async_ref)
{
    pool_t pool;
    struct stock_item *item;

    assert(stock != NULL);

    while (stock->num_idle > 0) {
        assert(!list_empty(&stock->idle));

        item = (struct stock_item *)stock->idle.next;
        list_remove(&item->list_head);
        --stock->num_idle;

        assert(item->is_idle);

        if (stock->class->borrow(stock->class_ctx, item)) {
            item->is_idle = false;

            list_add(&item->list_head, &stock->busy);
            ++stock->num_busy;

            callback(callback_ctx, item);
            return;
        }

        destroy_item(stock, item);
    }

    if (stock->class->pool == NULL)
        pool = stock->pool;
    else
        pool = stock->class->pool(stock->class_ctx, stock->pool, stock->uri);

    item = p_malloc(pool, stock->class->item_size);
    item->stock = stock;
    item->pool = pool;
    item->is_idle = false;
    item->callback = callback;
    item->callback_ctx = callback_ctx;

    stock->class->create(stock->class_ctx, item, stock->uri, info, async_ref);
}

void
stock_item_available(struct stock_item *item)
{
    struct stock *stock = item->stock;

    list_add(&item->list_head, &stock->busy);
    ++stock->num_busy;

    item->callback(item->callback_ctx, item);
}

void
stock_item_failed(struct stock_item *item)
{
    struct stock *stock = item->stock;

    item->callback(item->callback_ctx, NULL);
    stock_item_free(stock, item);
}

void
stock_item_aborted(struct stock_item *item)
{
    stock_item_free(item->stock, item);
}

void
stock_put(struct stock_item *item, bool destroy)
{
    struct stock *stock;

    assert(item != NULL);
    assert(!item->is_idle);

    stock = item->stock;

    assert(stock != NULL);
    assert(pool_contains(item->pool, item, stock->class->item_size));

    list_remove(&item->list_head);
    --stock->num_busy;

    destroy = !stock->class->release(stock->class_ctx, item) ||
        destroy || stock->num_idle >= 8;
    if (destroy) {
        destroy_item(stock, item);
    } else {
        item->is_idle = true;
        list_add(&item->list_head, &stock->idle);
        ++stock->num_idle;
    }
}

void
stock_del(struct stock_item *item)
{
    struct stock *stock;

    assert(item != NULL);
    assert(item->is_idle);

    stock = item->stock;

    assert(stock != NULL);
    assert(stock->num_idle > 0);
    assert(!list_empty(&stock->idle));
    assert(pool_contains(item->pool, item, stock->class->item_size));
    assert(item->list_head.next->prev == &item->list_head);
    assert(item->list_head.prev->next == &item->list_head);

    list_remove(&item->list_head);
    --stock->num_idle;

    destroy_item(stock, item);
}
