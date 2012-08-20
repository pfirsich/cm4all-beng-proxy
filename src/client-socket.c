/*
 * TCP client socket with asynchronous connect.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "client-socket.h"
#include "async.h"
#include "fd_util.h"
#include "stopwatch.h"
#include "pevent.h"

#include <inline/poison.h>
#include <socket/util.h>

#ifdef ENABLE_STOPWATCH
#include <socket/address.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

#include <event.h>

struct client_socket {
    struct async_operation operation;
    struct pool *pool;
    int fd;
    struct event event;

    const struct client_socket_handler *handler;
    void *handler_ctx;

#ifdef ENABLE_STOPWATCH
    struct stopwatch *stopwatch;
#endif
};


/*
 * async operation
 *
 */

static struct client_socket *
async_to_client_socket(struct async_operation *ao)
{
    return (struct client_socket*)(((char*)ao) - offsetof(struct client_socket, operation));
}

static void
client_socket_abort(struct async_operation *ao)
{
    struct client_socket *client_socket = async_to_client_socket(ao);

    assert(client_socket != NULL);
    assert(client_socket->fd >= 0);

    p_event_del(&client_socket->event, client_socket->pool);
    close(client_socket->fd);
    pool_unref(client_socket->pool);
}

static const struct async_operation_class client_socket_operation = {
    .abort = client_socket_abort,
};


/*
 * libevent callback
 *
 */

static void
client_socket_event_callback(int fd, short event gcc_unused, void *ctx)
{
    struct client_socket *client_socket = ctx;
    int ret;
    int s_err = 0;
    socklen_t s_err_size = sizeof(s_err);

    assert(client_socket->fd == fd);

    p_event_consumed(&client_socket->event, client_socket->pool);

    async_operation_finished(&client_socket->operation);

    if (event & EV_TIMEOUT) {
        close(fd);
        client_socket->handler->timeout(client_socket->handler_ctx);
        pool_unref(client_socket->pool);
        pool_commit();
        return;
    }

    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&s_err, &s_err_size);
    if (ret < 0)
        s_err = errno;

    if (s_err == 0) {
#ifdef ENABLE_STOPWATCH
        stopwatch_event(client_socket->stopwatch, "connect");
        stopwatch_dump(client_socket->stopwatch);
#endif

        client_socket->handler->success(fd, client_socket->handler_ctx);
    } else {
        close(fd);

        GError *error = g_error_new_literal(g_file_error_quark(), s_err,
                                            strerror(s_err));
        client_socket->handler->error(error, client_socket->handler_ctx);
    }

    pool_unref(client_socket->pool);
    pool_commit();
}


/*
 * constructor
 *
 */

void
client_socket_new(struct pool *pool,
                  int domain, int type, int protocol,
                  const struct sockaddr *addr, size_t addrlen,
                  unsigned timeout,
                  const struct client_socket_handler *handler, void *ctx,
                  struct async_operation_ref *async_ref)
{
    int fd, ret;
#ifdef ENABLE_STOPWATCH
    struct stopwatch *stopwatch;
#endif

    assert(addr != NULL);
    assert(addrlen > 0);
    assert(handler != NULL);

    fd = socket_cloexec_nonblock(domain, type, protocol);
    if (fd < 0) {
        GError *error = g_error_new_literal(g_file_error_quark(), errno,
                                            strerror(errno));
        handler->error(error, ctx);
        return;
    }

    if ((domain == PF_INET || domain == PF_INET6) && type == SOCK_STREAM) {
        if (!socket_set_nodelay(fd, true)) {
            GError *error = g_error_new_literal(g_file_error_quark(), errno,
                                                strerror(errno));
            close(fd);
            handler->error(error, ctx);
            return;
        }
    }

#ifdef ENABLE_STOPWATCH
    stopwatch = stopwatch_sockaddr_new(pool, addr, addrlen, NULL);
#endif

    ret = connect(fd, addr, addrlen);
    if (ret == 0) {
#ifdef ENABLE_STOPWATCH
        stopwatch_event(stopwatch, "connect");
        stopwatch_dump(stopwatch);
#endif

        handler->success(fd, ctx);
    } else if (errno == EINPROGRESS) {
        struct client_socket *client_socket;
        struct timeval tv = {
            .tv_sec = timeout,
            .tv_usec = 0,
        };

        pool_ref(pool);
        client_socket = p_malloc(pool, sizeof(*client_socket));
        client_socket->pool = pool;
        client_socket->fd = fd;
        client_socket->handler = handler;
        client_socket->handler_ctx = ctx;

#ifdef ENABLE_STOPWATCH
        client_socket->stopwatch = stopwatch;
#endif

        async_init(&client_socket->operation, &client_socket_operation);
        async_ref_set(async_ref, &client_socket->operation);

        event_set(&client_socket->event, client_socket->fd,
                  EV_WRITE|EV_TIMEOUT, client_socket_event_callback,
                  client_socket);
        p_event_add(&client_socket->event, &tv,
                    client_socket->pool, "client_socket_event");
    } else {
        GError *error = g_error_new_literal(g_file_error_quark(), errno,
                                            strerror(errno));
        close(fd);
        handler->error(error, ctx);
    }
}
