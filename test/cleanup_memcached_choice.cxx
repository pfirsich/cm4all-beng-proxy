#include "tcp_stock.hxx"
#include "tcp_balancer.hxx"
#include "balancer.hxx"
#include "hstock.hxx"
#include "address_list.hxx"
#include "address_resolver.hxx"
#include "memcached_stock.hxx"
#include "http_cache_choice.hxx"
#include "lease.hxx"
#include "async.hxx"
#include "strref.h"
#include "strmap.hxx"
#include "tpool.hxx"
#include "sink-impl.h"
#include "direct.h"
#include "fb_pool.hxx"

#include <socket/resolver.h>
#include <socket/util.h>

#include <glib.h>
#include <event.h>

#include <unistd.h>
#include <stdio.h>
#include <sys/signal.h>
#include <netdb.h>
#include <errno.h>

struct context {
    struct pool *pool;

    int fd;
    bool idle, reuse;

    struct istream *value;
    bool value_eof, value_abort;

    struct async_operation_ref async_ref;
};

static void
cleanup_callback(GError *error, gcc_unused void *ctx)
{
    if (error != NULL) {
        g_printerr("%s\n", error->message);
        g_error_free(error);
    }
}

/*
 * main
 *
 */

int main(int argc, char **argv) {
    struct addrinfo hints;
    struct event_base *event_base;
    struct pool *root_pool;
    static struct context ctx;
    struct memcached_stock *stock;

    if (argc != 3) {
        fprintf(stderr, "usage: cleanup-memcached-choice HOST[:PORT] URI\n");
        return 1;
    }

    /* initialize */

    signal(SIGPIPE, SIG_IGN);

    direct_global_init();
    event_base = event_init();
    fb_pool_init(false);

    root_pool = pool_new_libc(NULL, "root");
    tpool_init(root_pool);
    ctx.pool = pool_new_linear(root_pool, "test", 8192);

    AddressList address_list;
    address_list.Init();
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    GError *error = NULL;
    if (!address_list_resolve(ctx.pool, &address_list,
                              argv[1], 11211, &hints, &error)) {
        fprintf(stderr, "%s\n", error->message);
        g_error_free(error);
        return 1;
    }

    auto *tcp_stock = tcp_stock_new(ctx.pool, 0);
    struct tcp_balancer *tcp_balancer = tcp_balancer_new(*tcp_stock,
                                                         *balancer_new(*ctx.pool));
    stock = memcached_stock_new(*ctx.pool, tcp_balancer, &address_list);

    /* send memcached request */

    http_cache_choice_cleanup(*ctx.pool, *stock, argv[2],
                              cleanup_callback, &ctx,
                              ctx.async_ref);

    pool_unref(ctx.pool);
    pool_commit();

    event_dispatch();

    tcp_balancer_free(tcp_balancer);
    hstock_free(tcp_stock);

    /* cleanup */

    tpool_deinit();
    pool_commit();

    pool_unref(root_pool);
    pool_commit();
    pool_recycler_clear();

    fb_pool_deinit();
    event_base_free(event_base);
    direct_global_deinit();

    return ctx.value_eof ? 0 : 2;
}
