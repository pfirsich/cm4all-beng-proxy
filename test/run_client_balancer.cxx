#include "client_balancer.hxx"
#include "client-socket.h"
#include "pool.h"
#include "async.h"
#include "balancer.hxx"
#include "failure.hxx"
#include "address_list.hxx"

#include <socket/resolver.h>
#include <socket/util.h>

#include <glib.h>
#include <event.h>

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

struct context {
    struct balancer *balancer;

    enum {
        NONE, SUCCESS, TIMEOUT, ERROR,
    } result;

    int fd;
    GError *error;
};

/*
 * client_socket callback
 *
 */

static void
my_socket_success(int fd, void *_ctx)
{
    assert(fd >= 0);

    struct context *ctx = (struct context *)_ctx;

    ctx->result = context::SUCCESS;
    ctx->fd = fd;

    balancer_free(ctx->balancer);
}

static void
my_socket_timeout(void *_ctx)
{
    struct context *ctx = (struct context *)_ctx;

    ctx->result = context::TIMEOUT;

    balancer_free(ctx->balancer);
}

static void
my_socket_error(GError *error, void *_ctx)
{
    struct context *ctx = (struct context *)_ctx;

    ctx->result = context::ERROR;
    ctx->error = error;

    balancer_free(ctx->balancer);
}

static const struct client_socket_handler my_socket_handler = {
    .success = my_socket_success,
    .timeout = my_socket_timeout,
    .error = my_socket_error,
};

/*
 * main
 *
 */

int
main(int argc, char **argv)
{
    if (argc <= 1) {
        fprintf(stderr, "Usage: run-client-balancer ADDRESS ...\n");
        return EXIT_FAILURE;
    }

    /* initialize */

    struct event_base *event_base = event_init();

    struct pool *root_pool = pool_new_libc(nullptr, "root");
    struct pool *pool = pool_new_linear(root_pool, "test", 8192);

    failure_init(pool);

    struct context ctx;
    ctx.result = context::TIMEOUT;

    ctx.balancer = balancer_new(pool);

    struct address_list address_list;
    address_list.Init();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    for (int i = 1; i < argc; ++i) {
        const char *p = argv[i];

        struct addrinfo *ai;
        int ret = socket_resolve_host_port(p, 80, &hints, &ai);
        if (ret != 0) {
            fprintf(stderr, "Failed to resolve '%s': %s\n",
                    p, gai_strerror(ret));
            return EXIT_FAILURE;
        }

        for (struct addrinfo *j = ai; j != nullptr; j = j->ai_next)
            address_list.Add(pool, ai->ai_addr, ai->ai_addrlen);

        freeaddrinfo(ai);
    }

    /* connect */

    struct async_operation_ref async_ref;
    client_balancer_connect(pool, ctx.balancer,
                            false, nullptr, 0,
                            0, &address_list, 30,
                            &my_socket_handler, &ctx,
                            &async_ref);

    event_dispatch();

    assert(ctx.result != context::NONE);

    /* cleanup */

    failure_deinit();

    pool_unref(pool);
    pool_commit();

    pool_unref(root_pool);
    pool_commit();
    pool_recycler_clear();

    event_base_free(event_base);

    switch (ctx.result) {
    case context::NONE:
        break;

    case context::SUCCESS:
        close(ctx.fd);
        return EXIT_SUCCESS;

    case context::TIMEOUT:
        fprintf(stderr, "timeout\n");
        return EXIT_FAILURE;

    case context::ERROR:
        fprintf(stderr, "%s\n", ctx.error->message);
        g_error_free(ctx.error);
        return EXIT_FAILURE;
    }

    assert(false);
    return EXIT_FAILURE;
}
