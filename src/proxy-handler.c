/*
 * Serve HTTP requests from another HTTP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.h"
#include "request.h"
#include "http-server.h"
#include "url-stream.h"

void
proxy_callback(struct request *request2)
{
    struct http_server_request *request = request2->request;
    const struct translate_response *tr = request2->translate.response;
    http_method_t method;
    off_t content_length;
    istream_t body;

    pool_ref(request->pool);

    if (response_dispatcher_wants_body(request2)) {
        method = HTTP_METHOD_GET;
        content_length = 0;
        body = NULL;
    } else {
        method = request->method;
        content_length = request->content_length;
        body = request->body;
        request2->body_consumed = 1;
    }

    url_stream_new(request->pool,
                   request2->http_client_stock,
                   method, tr->proxy, NULL,
                   content_length, body,
                   &response_handler, request2,
                   &request2->url_stream);
}
