#include "http_server/http_server.hxx"
#include "http_server/Request.hxx"
#include "http_server/Handler.hxx"
#include "http_headers.hxx"
#include "direct.hxx"
#include "RootPool.hxx"
#include "pool.hxx"
#include "istream/istream.hxx"
#include "istream/istream_catch.hxx"
#include "fb_pool.hxx"
#include "system/fd_util.h"
#include "event/Event.hxx"

#include <stdio.h>
#include <stdlib.h>

struct Instance final : HttpServerConnectionHandler {
    /* virtual methods from class HttpServerConnectionHandler */
    void HandleHttpRequest(struct http_server_request &request,
                           struct async_operation_ref &async_ref) override;

    void LogHttpRequest(struct http_server_request &,
                        http_status_t, off_t,
                        uint64_t, uint64_t) override {}

    void HttpConnectionError(GError *error) override;
    void HttpConnectionClosed() override;
};

static GError *
catch_callback(GError *error, gcc_unused void *ctx)
{
    fprintf(stderr, "%s\n", error->message);
    g_error_free(error);
    return nullptr;
}

void
Instance::HandleHttpRequest(struct http_server_request &request,
                            gcc_unused struct async_operation_ref &async_ref)
{
    http_server_response(&request, HTTP_STATUS_OK, HttpHeaders(),
                         istream_catch_new(request.pool, *request.body,
                                           catch_callback, nullptr));
    http_server_connection_close(request.connection);
}

void
Instance::HttpConnectionError(GError *error)
{
    g_printerr("%s\n", error->message);
    g_error_free(error);
}

void
Instance::HttpConnectionClosed()
{
}

static void
test_catch(EventBase &event_base, struct pool *pool)
{
    pool = pool_new_libc(pool, "catch");

    int fds[2];
    if (socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair()");
        abort();
    }

    static constexpr char request[] =
        "POST / HTTP/1.1\r\nContent-Length: 1024\r\n\r\nfoo";
    send(fds[1], request, sizeof(request) - 1, 0);

    Instance instance;
    HttpServerConnection *connection;
    http_server_connection_new(pool, fds[0], FdType::FD_SOCKET,
                               nullptr, nullptr,
                               nullptr, nullptr,
                               true, instance,
                               &connection);
    pool_unref(pool);

    event_base.Dispatch();

    close(fds[1]);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    direct_global_init();
    EventBase event_base;
    fb_pool_init(false);

    test_catch(event_base, RootPool());

    fb_pool_deinit();
}
